/* ============================================================
 * shadow-0.4 rt/ — GC v1（rt_gc.c）
 * ------------------------------------------------------------
 * STW 精确标记-清扫（Phase 9 验证基线；Phase 10 并发改造调试中暂缓）。
 *   - 对象头：分配器块头（16 字节）[size:u32][type_id:u32][mark:u32][pad:u32]，
 *     data = 块首 + 16。type_id 决定对象内部哪些偏移是指针（精确追踪）。
 *   - 对象表：所有堆对象登记；ptr→idx hash set 供 O(1) 判定。
 *   - 类型表：编译器 register_type(id, size, bitmap) + 内置类型扫描。
 *   - 根集：编译器 frame/global root（slot 键控 replace）+ 保守栈扫描兜底。
 *   - 触发：GOGC（live + live*GOGC/100，默认 100，最小 4MB）。
 *   - finalizer：sweep 前对将死对象执行。
 *   - SHADOW_GC_LOG / SHADOW_GOGC / SHADOW_GC_FORCE / SHADOW_GC_DEBUG。
 *
 * 原则 3：仅 Windows API + MSVCRT；禁止 STL。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <intrin.h>

/* ---------------- 内置类型 id（<100 保留） ---------------- */
#define RT_T_DATA      0   /* 纯数据，不扫描 */
#define RT_T_STRING    1   /* 纯字节，不扫描 */
#define RT_T_ARRAY     2   /* [len:4][cap:4][elem_size:4][data@12] */
#define RT_T_ANYBOX    3   /* [magic@0][tag@4][value@8]；tag 3(string)/5(ptr) → value 指针 */
#define RT_T_DICT      4   /* rt_dict：{rt_kv** buckets; int32 n_buckets; int32 size;} */
#define RT_T_CLOSURE   5   /* [fn_ptr@0][env_ptr@8] */
#define RT_T_USER      100 /* 编译器注册类型起始 */

#define RT_GC_HEADER 16u   /* 对象头大小（与 rt_core RT_HEADER 一致） */
#define RT_GC_MAX_TYPES 4096
#define RT_GC_DEFAULT_GOGC 100
#define RT_GC_MIN_HEAP (4u * 1024 * 1024)

/* ---------------- 对象头（与 rt_core.c 分配器共享布局） ---------------- */
typedef struct rt_gc_hdr {
    uint32_t size;     /* 数据区大小 */
    uint32_t type_id;
    uint32_t mark;
    uint32_t pad;
} rt_gc_hdr;

/* ---------------- 类型表 ---------------- */
typedef struct rt_gc_typ {
    uint32_t size;     /* 0 = 未注册 */
    uint64_t bitmap;   /* bit i = data+i*8 是否指针（最多 64 字段） */
} rt_gc_typ;
static rt_gc_typ g_types[RT_GC_MAX_TYPES];

/* ---------------- 对象表 + ptr→idx hash set ---------------- */
typedef struct rt_gc_obj {
    void* data;        /* NULL = 空闲槽 */
    uint32_t type_id;
    uint32_t size;
    uint32_t ht_slot;  /* 本对象在哈希表中的槽位（登记时写入，重建时刷新）。
                        * 清扫用它免全表查找 —— 每周期省 ~12 万次一致性校验探测。 */
} rt_gc_obj;
static rt_gc_obj* g_objs = NULL;
static uint32_t g_objs_len = 0, g_objs_cap = 0;
static int32_t g_free_head = -1;    /* 空闲槽链表（存于 size 字段） */

static void** g_ht_keys = NULL;      /* ptr → obj index */
static uint32_t* g_ht_vals = NULL;
static uint32_t g_ht_cap = 0;        /* 2 的幂 */
static uint32_t g_ht_mask = 0;
static uint32_t g_ht_used = 0;
/* 墓碑：地址 1 不可能是合法对象（分配 16 字节对齐），用作"已擦除"哨兵。
 * 清扫每周期擦除 ~95% 条目，backshift 维护链完整要 O(簇长²) 总开销；墓碑把
 * 擦除降为 O(1)，代价是查找需跳过墓碑、表会积攒墓碑 —— 清扫收尾重建压回。 */
#define RT_HT_TOMBSTONE ((void*)1)
static uint32_t g_ht_tomb = 0;       /* 墓碑计数 */

/* ---------------- 根集（frame + global + perm） ---------------- */
typedef struct rt_gc_root {
    void* slot;
    void* val;
    uint32_t n;        /* 0 = 单值根（slot 键控）；>0 = range 根（slot=base，扫描读 base[0..n)） */
} rt_gc_root;
/* 全局根独立成表：绝不能与帧根共用，否则 shadow_gc_frame_leave(marker) 的
 * 截断会把在函数体内登记的全局指针根一起丢弃（历史 bug，2026-08-04 修复）。 */
static rt_gc_root* g_globals = NULL;
static uint32_t g_globals_len = 0, g_globals_cap = 0;

/* ---------------- 线程表（§5.2.7 与 spawn/await 整合） ----------------
 * 帧根（g_roots）从前是**全局单栈**。单线程时它等价于"当前调用链"，多线程一开
 * 就废：两条线程交替 frame_enter/frame_leave，marker 是各自栈深的快照，互相截断
 * 对方的根 —— 表现为随机的活对象被回收。故帧根必须随线程走。
 *
 * 每线程一条 rt_gc_thread：
 *   · roots        —— 本线程的帧根栈（frame_enter/leave/root_set 只碰自己的，无锁）
 *   · stack_base   —— NT_TIB.StackBase，扫描上界
 *   · h / tid      —— STW 时 SuspendThread + GetThreadContext 用
 * 线程表本身极少变动（spawn/退出时各一次），用 g_thr_lock 保护。
 */
typedef struct rt_gc_thread {
    HANDLE      h;             /* DuplicateHandle 得到的真句柄（可跨线程 Suspend） */
    DWORD       tid;
    uintptr_t   stack_base;    /* 栈高地址（独占上界） */
    rt_gc_root* roots;         /* 本线程帧根栈 */
    uint32_t    roots_len, roots_cap;
    int32_t     in_use;        /* 0 = 空槽，可复用 */
    int32_t     stw_susp;      /* 本轮 STW 中已成功挂起（决定是否要 Resume） */
    int32_t     stw_ctx_ok;    /* ctx 快照有效 */
    __declspec(align(16)) CONTEXT stw_ctx;   /* GetThreadContext 要求 16 字节对齐 */
    /* §5.2.4 协作式安全点：gc_state 0=自由运行 1=在 poll（到达安全点） 2=GC 锁内/等锁。
     * 自由运行的线程会执行 mutator 代码（可能持有未 spill 的寄存器值），STW 必须
     * 等它到达安全点；后两种状态的线程不执行 mutator 代码，其精确根集（sf/slot）
     * 已冻结，STW 可立即扫根。gc_lock_depth 跟踪 GC 锁嵌套（CRITICAL_SECTION 可重入）。 */
    volatile int32_t gc_state;
    int32_t          gc_lock_depth;
    int32_t          is_worker;     /* 1 = GC mark/sweep worker（不分配，不计入 mutator 数） */
    /* §5.2.4 分配根（对标 Go allocate-black 的补强）：最近一次分配的对象。
     * 分配路径的 C 中间帧（rt_gc_register / shadow_gc_alloc / any_box 等）在
     * "malloc 登记完成 → mutator 把结果赋值到 shadow frame"之间持有新对象，
     * 精确根集（sf/slot）此时还看不见它 —— 多线程高频 GC 下该窗口会被别的
     * 线程的周期扫根扫过，对象无根 → sweep 误回收 → use-after-free。
     * 每次分配（rt_gc_register）刷新本槽，扫根时一并标记；mutator 赋值到
     * sf 后由 sf 接管，本槽在下一次分配时自然覆盖 —— 多保活一轮无害。 */
    void*            alloc_root;
} rt_gc_thread;

#define RT_GC_MAX_THREADS 256
static rt_gc_thread     g_threads[RT_GC_MAX_THREADS];
static uint32_t         g_threads_len = 0;
static CRITICAL_SECTION g_thr_lock;
__declspec(thread) static rt_gc_thread* t_self = NULL;

/* 用户（mutator）线程计数：GC mark/sweep worker 不计入。
 * 登记批缓冲（rt_gc_register 快路径）仅在 g_mutator_threads<=1 时启用 ——
 * 此时 g_pend 只被唯一 mutator 读写，无并发；多 mutator 时快路径关闭，
 * 行为与旧版一致（见 rt_gc_register 注释）。 */
static volatile LONG g_mutator_threads = 0;

/* 攒批登记缓冲：分配密集热循环把登记推迟到 flush，省掉每分配的全局锁+哈希。
 * 仅单 mutator 时使用；flush 点在 rt_gc_register_slow 与 gc_cycle_start。 */
#define RT_PEND_CAP 64
static void*    g_pend[RT_PEND_CAP];
static uint32_t g_pend_n = 0;

/* 永久根：跨线程传递中的 GC 指针（spawn 的 env、future 的结果）在
 * "只被非 GC 内存持有"的窗口内没有任何栈能证明它活着，必须显式钉住。 */
static void**   g_perm = NULL;
static uint32_t g_perm_len = 0, g_perm_cap = 0;

/* ---------------- 全局 GC 锁（§5.2.7） ----------------
 * 保护对象表 g_objs / hash 表 / 灰队列 / 堆统计 / 全局根 / finalizer 表 /
 * 清扫游标 —— 即除"每线程帧根"以外的全部 GC 元数据。
 *
 * 粒度选择：一把大锁（对标 Go 早期的 mheap.lock）。分配快路径本就不进 GC 锁
 * （rt_core 的 mcache 无锁），进 GC 锁的是 register/unregister 与 GC 周期本身，
 * 争用有限；细分锁的收益不及其带来的死锁面。
 *
 * ⚠️ 与 STW 的次序契约：发起 STW 的线程**必须已持有 g_gc_lock**。这样被挂起的
 * 线程要么在锁外，要么正阻塞在 EnterCriticalSection 上（未持锁）—— 绝不会出现
 * "持锁线程被挂起，GC 等它放锁"的死锁。
 */
static CRITICAL_SECTION g_gc_lock;
static LONG g_gc_init_lock = 0;
static int  g_gc_inited = 0;
static HANDLE g_gc_heap = NULL;

/* ---------------- 辅助 ---------------- */
static void rt_gc_init(void) {
    if (g_gc_inited) return;
    while (InterlockedCompareExchange(&g_gc_init_lock, 1, 0) != 0) Sleep(0);
    if (!g_gc_inited) {
        InitializeCriticalSection(&g_gc_lock);
        InitializeCriticalSection(&g_thr_lock);
        g_gc_heap = HeapCreate(0, 1u << 20, 0);
        g_gc_inited = 1;
    }
    g_gc_init_lock = 0;
}
#define GC_LOCK()   do { rt_gc_init(); if (t_self) { t_self->gc_lock_depth++; t_self->gc_state = 2; } EnterCriticalSection(&g_gc_lock); } while (0)
#define GC_UNLOCK() do { LeaveCriticalSection(&g_gc_lock); if (t_self) { t_self->gc_lock_depth--; if (t_self->gc_lock_depth <= 0) { t_self->gc_lock_depth = 0; t_self->gc_state = 0; } } } while (0)

/* ---------------- GC 私有堆（STW 安全的关键） ----------------
 * GC 自己的元数据（对象表 / hash 表 / 灰队列 / 全局根 / 永久根 / finalizer 表）
 * 绝不能走 CRT 的 malloc —— UCRT 的 malloc 底层就是 HeapAlloc(进程默认堆)，
 * 而 mutator 线程随时可能正持着那把堆锁（rt_core 的 HeapAlloc、用户代码里的
 * printf 都会）。它一旦在 STW 中被挂起，GC 再去 realloc 就是必然死锁。
 * HeapCreate 出一把只有 GC 碰的私有堆，锁便与 mutator 彻底隔离。
 * 与 Go 同源：Go 的 GC 元数据走 persistentalloc / fixalloc，从不过通用分配器。
 *
 * ⚠️ 两条堆的使用不变式（违反即可能死锁）：
 *   · GC 私有堆：只允许在**持有 g_gc_lock** 时分配 —— 于是 STW 期间不可能
 *     有别的线程持有它的锁（因为 STW 发起者自己就持着 g_gc_lock）。
 *   · 进程默认堆：mutator 会持它的锁 —— 故 **STW 窗口内 GC 一行都不许碰**
 *     （包括 fprintf：stdio 会分配）。每线程帧根缓冲刻意留在进程堆上，
 *     因为它只由属主线程分配、GC 仅只读扫描。
 */
static void* gc_xrealloc(void* p, size_t n) {
    if (!g_gc_heap) rt_gc_init();
    if (!g_gc_heap) return realloc(p, n);            /* 极端兜底 */
    return p ? HeapReAlloc(g_gc_heap, 0, p, n)
             : HeapAlloc(g_gc_heap, 0, n);
}
static void* gc_xcalloc(size_t n, size_t sz) {
    if (!g_gc_heap) rt_gc_init();
    if (!g_gc_heap) return calloc(n, sz);
    return HeapAlloc(g_gc_heap, HEAP_ZERO_MEMORY, n * sz);
}
static void gc_xfree(void* p) {
    if (!p) return;
    if (g_gc_heap) HeapFree(g_gc_heap, 0, p);
    else free(p);
}

/* ---------------- finalizer 队列 ---------------- */
typedef struct rt_gc_fin {
    void* data;
    void (*fn)(void*);
    void* data2;
} rt_gc_fin;
static rt_gc_fin* g_fins = NULL;
static uint32_t g_fins_len = 0, g_fins_cap = 0;

/* ---------------- GC 阶段与三色状态机（v2） ----------------
 * 阶段：
 *   GC_OFF   —— 不在回收周期内，写屏障关闭（成本为零）。
 *   GC_MARK  —— 标记中（可被切片）。写屏障开启；新分配对象直接置黑。
 *   GC_SWEEP —— 清扫中。白对象集合已冻结，不会再有新引用产生，屏障关闭。
 *               清扫本身是惰性的（Task #32）：由后续分配分片推进，
 *               期间不允许启动新周期（epoch 一动，未清扫的黑就变白了）。
 *
 * 颜色编码（epoch 着色）：
 *   mark 字段存 (epoch << 2) | color，color: 1=灰 2=黑。
 *   **凡 epoch 不等于当前周期的一律视为白。**
 *   于是"清扫时把存活对象 mark 复位为 0"这一步被彻底省掉：epoch 一自增，
 *   上一轮的黑自动降级为本轮的白。这正是惰性清扫（Task #32）的前提——
 *   清扫可以拖到下一轮进行，未清扫对象残留的旧颜色不会与本轮颜色混淆。
 */
#define GC_OFF   0
#define GC_MARK  1
#define GC_SWEEP 2
static int32_t g_gc_phase = GC_OFF;
static uint32_t g_gc_epoch = 0;
static uint32_t g_col_grey = 0;
static uint32_t g_col_black = 0;

/* 惰性清扫游标（Task #32；语义与推进逻辑见文件后段"惰性清扫"小节）。
 * 定义提前至此，是因为分配路径（shadow_gc_alloc）与周期收尾都要读它们。 */
static uint32_t g_sweep_cursor = 0;   /* 下一个待清扫的对象表槽 */
static uint32_t g_sweep_end = 0;      /* 本轮清扫上界（标记终止时的 objs_len 快照） */
static uint32_t g_sweep_dead = 0;     /* 本轮已回收对象数 */
static uint64_t g_sweep_ms = 0;       /* 本轮清扫累计耗时 */
static uint64_t g_sweep_slices = 0;   /* 本轮清扫切片数 */
/* 单切片处理的最大槽数 —— 惰性清扫的核心指标。总清扫成本并不会下降
 * （对象表还是得整表走一遍），下降的是**单次停顿**，即这个数。
 * 与 g_sweep_end 对比即得摊薄倍数；毫秒计时器精度太粗（15.6ms）看不出来。 */
static uint64_t g_sweep_max_work = 0;

/* 写屏障总开关：rt_core.c / rt_extra.c 以 extern 引用。
 * 非 MARK 期间恒为 0 —— 屏障在热路径上的成本仅是一次全局整数比较。 */
int32_t shadow_gc_barrier_on = 0;

static void gc_epoch_advance(void) {
    g_gc_epoch++;
    if (g_gc_epoch > 0x3FFFFFFFu) g_gc_epoch = 1;   /* 防 <<2 溢出回绕 */
    g_col_grey  = (g_gc_epoch << 2) | 1u;
    g_col_black = (g_gc_epoch << 2) | 2u;
}

/* ---------------- 统计与触发 ---------------- */
static volatile int32_t g_gc_disabled = 0;
static volatile int32_t g_gc_running = 0;
static int32_t g_gc_log = -1;         /* -1 未初始化 */
static uint64_t g_heap_bytes = 0;     /* 未释放对象总字节（近似） */
static uint64_t g_heap_peak = 0;      /* 堆字节历史峰值（§5.3-3 的被测量） */
static uint64_t g_live_bytes = 0;     /* 上次 GC 后 live */
static uint64_t g_next_gc = RT_GC_MIN_HEAP;  /* 触发阈值 */
static uint32_t g_gc_count = 0;

/* ---------------- pacing（Task #33，对标 Go 的 gcController） ----------------
 *
 * 术语先分清，v2 之前这两个概念被同一个变量兼任，是峰值超标的根因：
 *
 *   heap_goal    本轮周期允许的**堆峰值上限** = live × (1 + GOGC/100)。
 *                §5.3 第 3 条要验收的就是它。
 *   gc_trigger   **启动并发标记的堆位置**，必须严格小于 goal。
 *
 * 为什么必须分离：标记是并发的，从启动到终止这段时间 mutator 一直在分配。
 * 若 trigger == goal（v2 之前的 g_next_gc 兼任两职），那么标记刚启动堆就
 * 已经站在目标峰值上，此后整个标记期的分配全部是超额 —— 峰值必然击穿目标，
 * 而且击穿多少完全取决于标记有多慢，不可控。
 *
 * 正确做法是提前启动，预留出 (goal - trigger) 这段"跑道"给标记用完。
 * 跑道留多长由 g_trigger_pct 控制，并由 pacing 控制器按实测误差自适应：
 * 标记结束得太晚（峰值超了）就把 trigger 前移，结束得太早（峰值远没到，
 * 白白多跑了 GC）就后移。这就是 Go gcController 的比例-反馈闭环。
 */
#define RT_GC_TRIGGER_PCT_INIT 70   /* 初始跑道：goal 的 70% 处启动标记 */
#define RT_GC_TRIGGER_PCT_MIN  20
#define RT_GC_TRIGGER_PCT_MAX  95
static uint64_t g_heap_goal = RT_GC_MIN_HEAP;  /* 本轮目标峰值 */
static uint64_t g_gc_trigger = RT_GC_MIN_HEAP; /* 本轮标记启动点 */
static int32_t  g_trigger_pct = RT_GC_TRIGGER_PCT_INIT;
static uint64_t g_heap_at_start = 0;   /* 本轮标记启动时的堆字节 */
static uint64_t g_cycle_peak = 0;      /* 本轮周期内的堆峰值（控制误差来源） */
static uint64_t g_heap_at_mark_done = 0; /* 本轮标记终止时的堆字节 */
static uint64_t g_last_marked = 0;     /* 上轮标记对象数（本轮扫描量的估计） */

/* GC assist：分配者按份额偿还标记债（对标 Go 的 gcAssistAlloc）。
 * 光有"提前启动"还不够——若 mutator 分配得比标记快，跑道再长也会被吃完。
 * assist 让每次分配都必须先干掉与其分配量成比例的标记工作，把标记速度
 * 与分配速度**强制绑定**：分配越猛，assist 越重，标记自然跟得上。 */
static double   g_assist_ratio = 0.0;  /* 每分配 1 字节需扫描的对象数 */
static double   g_assist_debt = 0.0;   /* 未偿还的标记债（对象数） */
static uint64_t g_assist_work = 0;     /* 本轮 assist 完成的标记量 */
static uint64_t g_assist_calls = 0;    /* 本轮 assist 触发次数 */
static uint32_t g_gc_freed = 0;
static uint64_t g_reg_count = 0;
static uint64_t g_unreg_count = 0;

/* ---------------- 停顿计量（§5.3 第 2 条） ----------------
 *
 * 原先全程用 GetTickCount64，粒度 ~15.6ms。而增量 GC 的单个切片停顿是
 * **微秒级**的，用它测每一片都是 0 —— §5.3 第 2 条要求「标记终止停顿可
 * 测量且远小于 0.3 全 STW 停顿」，拿一串 0 出来是无法验收的，既不能证明
 * 停顿短，也发现不了某个切片意外变长。
 *
 * 这里改用 QPC 取微秒。周期内的 ms 字段照旧保留（日志与既有工具在解析），
 * us 只做累计与**最大值**统计 —— 平均停顿没有意义，验收看的是最坏那一次。 */
static double g_qpc_scale = 0.0;   /* QPC ticks per microsecond */
static uint64_t gc_now_us(void) {
    LARGE_INTEGER c;
    if (g_qpc_scale == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpc_scale = (double)f.QuadPart / 1000000.0;
        if (g_qpc_scale <= 0.0) g_qpc_scale = 1.0;
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart / g_qpc_scale);
}

/* ---------------- 进程级累计统计（§5.2.10 SHADOW_GC_STATS） ----------------
 * 对标 GODEBUG=gctrace=1 的进程退出汇总：单周期日志回答"这一轮发生了什么"，
 * 汇总回答"整个进程跑下来 GC 到底花了多少、最坏停顿多久、谁在触发它"。
 * 长驻验收（§5.3 第 4 条）尤其依赖后者 —— 单看某一轮永远看不出内存是否在爬。 */
static uint64_t g_tot_mark_us = 0;      /* 标记（含切片与终止）累计 */
static uint64_t g_tot_sweep_us = 0;     /* 清扫累计 */
static uint64_t g_tot_fin_us = 0;       /* finalizer 累计 */
static uint64_t g_max_mark_slice_us = 0;/* 单个标记切片的最坏停顿 */
static uint64_t g_max_term_us = 0;      /* 标记终止的最坏停顿（§5.3-2 的关键量） */
static uint64_t g_max_sweep_slice_us = 0;
static uint64_t g_tot_mark_slices = 0;
static uint64_t g_tot_sweep_slices = 0;
static uint64_t g_tot_marked = 0;
static uint64_t g_tot_assist_work = 0;
static uint64_t g_tot_assist_calls = 0;
static uint64_t g_trig_heap = 0;        /* 触发原因：堆达到 trigger */
static uint64_t g_trig_explicit = 0;    /* 触发原因：显式 gc() */
static uint64_t g_trig_stress = 0;      /* 触发原因：SHADOW_GC_STRESS */
static uint64_t g_live_last = 0;        /* 最近一次周期结算出的存活字节 */
static int32_t  g_stats_on = -1;
static int32_t  g_stats_armed = 0;
static int32_t  g_collect_from_alloc = 0;  /* STW 回退路径标志，防触发原因双计 */

/* ---------------- worklist（标记队列） ---------------- */
static void** g_wl = NULL;
/* g_wl_len 用 volatile：并发标记 worker 在锁外只读它判"队列是否空"；
 * 写入方（mark_ptr / gc_drain / 写屏障）都持 g_gc_lock，x86 上 volatile
 * 读-写互见安全。上界 = 周期开始对象数（分配即黑，新对象不入队）。 */
static volatile uint32_t g_wl_len = 0, g_wl_cap = 0;

/* ---------------- 辅助 ---------------- */
extern int64_t shadow_gc_collect(void);
static rt_gc_hdr* rt_gc_hdr_of(void* data) {
    return (rt_gc_hdr*)((char*)data - RT_GC_HEADER);
}

static uint32_t rt_gc_hash_ptr(void* p) {
    uint64_t h = (uint64_t)(uintptr_t)p >> 3;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* 重哈希到指定容量（丢弃墓碑）。调用方需持 GC 锁。
 * 同时刷新每个存活对象表项的 ht_slot —— 重建后槽位全变，清扫依赖它。 */
static void rt_gc_ht_rehash_to(uint32_t ncap) {
    uint32_t nmask = ncap - 1;
    void** nk = (void**)gc_xcalloc(ncap, sizeof(void*));
    uint32_t* nv = (uint32_t*)gc_xcalloc(ncap, sizeof(uint32_t));
    uint32_t i;
    if (!nk || !nv) return;
    for (i = 0; i < g_ht_cap; i++) {
        void* k = g_ht_keys[i];
        if (!k || k == RT_HT_TOMBSTONE) continue;
        uint32_t j = rt_gc_hash_ptr(k) & nmask;
        while (nk[j]) j = (j + 1) & nmask;
        nk[j] = k;
        nv[j] = g_ht_vals[i];
        g_objs[g_ht_vals[i]].ht_slot = j;
    }
    gc_xfree(g_ht_keys);
    gc_xfree(g_ht_vals);
    g_ht_keys = nk;
    g_ht_vals = nv;
    g_ht_cap = ncap;
    g_ht_mask = nmask;
    g_ht_tomb = 0;
    g_ht_used = 0;  /* 重新计数 */
    for (i = 0; i < g_ht_cap; i++) if (g_ht_keys[i]) g_ht_used++;
}

static void rt_gc_ht_grow(void) {
    uint32_t ncap = g_ht_cap ? g_ht_cap * 2 : 256;
    rt_gc_ht_rehash_to(ncap);
}

/* 容量不变重建：清扫后墓碑可能占半张表，重建把有效负载压回 used/cap。 */
static void rt_gc_ht_rebuild(void) {
    rt_gc_ht_rehash_to(g_ht_cap);
}

/* 返回 obj index；-1 = 不在对象表 */
static int32_t rt_gc_ptr_lookup(void* p) {
    uint32_t j, i;
    if (!p || g_ht_cap == 0) return -1;
    j = rt_gc_hash_ptr(p) & g_ht_mask;
    for (i = 0; i < g_ht_cap; i++) {
        if (g_ht_keys[j] == p) return (int32_t)g_ht_vals[j];
        if (!g_ht_keys[j]) return -1;
        j = (j + 1) & g_ht_mask;
    }
    return -1;
}

/* 同 rt_gc_ptr_lookup，额外经 *out_pos 返回哈希表槽位 —— 清扫路径用它
 * 把"一致性校验的查找"与"随后 ht_erase 的查找"合并成一次。 */
static int32_t rt_gc_ptr_lookup_pos(void* p, uint32_t* out_pos) {
    uint32_t j, i;
    if (!p || g_ht_cap == 0) return -1;
    j = rt_gc_hash_ptr(p) & g_ht_mask;
    for (i = 0; i < g_ht_cap; i++) {
        if (g_ht_keys[j] == p) { if (out_pos) *out_pos = j; return (int32_t)g_ht_vals[j]; }
        if (!g_ht_keys[j]) return -1;
        j = (j + 1) & g_ht_mask;
    }
    return -1;
}

/* 透明 any 判别：值是否为真实 GC 堆对象。
 * 动态数组元素以 inttoptr 裸存小整值（透明模型），打印时若把裸整值当 AnyBox*
 * 解引用会 AV 崩溃。此处用对象表判定「是堆对象才解引用」，裸整值一律按整数打印。 */
extern int32_t shadow_is_valid_ptr(void* p) {
    return rt_gc_ptr_lookup(p) >= 0 ? 1 : 0;
}

/* 查询 GC 堆对象的数据区容量（分配时记录的 size）；非堆对象（字面量/栈上）返回 0。
 * 供 shadow_string_concat_inplace 判断能否就地追加。
 * 注意：单 mutator 攒批快路径（rt_gc_register）期间新对象只进 g_pend、尚未入哈希表，
 * 哈希查找 miss 时须线性扫 g_pend（≤RT_PEND_CAP=64；热循环里 s1 刚分配几乎必在批内）。 */
extern int32_t rt_alloc_cap(void* p) {
    int32_t idx = rt_gc_ptr_lookup(p);
    if (idx >= 0) return (int32_t)g_objs[idx].size;
    if (g_pend_n) {
        uint32_t i;
        for (i = 0; i < g_pend_n; i++) {
            if (g_pend[i] == p) return (int32_t)rt_gc_hdr_of(p)->size;
        }
    }
    return 0;
}

/* 登记对象并返回哈希槽位（调用方存入 g_objs[idx].ht_slot，清扫免查找）。
 * 复用探测链上第一个墓碑：新对象常复用死对象地址（freelist），其 hash 与
 * 墓碑同链，复用使表保持 ~48% 负载，避免墓碑把有效负载顶到 90%+。 */
static uint32_t rt_gc_ht_insert(void* p, uint32_t idx) {
    uint32_t j, i;
    if (g_ht_used * 4 >= g_ht_cap * 3) rt_gc_ht_grow();
    else if (g_ht_tomb && (g_ht_used + g_ht_tomb) * 4 >= g_ht_cap * 3) rt_gc_ht_rebuild();
    j = rt_gc_hash_ptr(p) & g_ht_mask;
    for (i = 0; i < g_ht_cap; i++) {
        if (!g_ht_keys[j] || g_ht_keys[j] == RT_HT_TOMBSTONE) {
            /* 墓碑在 hash(p) 探测链上，命中即复用（立即停，不再向后探测）。
             * 新对象复用死对象地址 → 首探即中墓碑，插入 O(1)。 */
            if (g_ht_keys[j] == RT_HT_TOMBSTONE) g_ht_tomb--;
            g_ht_keys[j] = p;
            g_ht_vals[j] = idx;
            g_ht_used++;
            return j;
        }
        j = (j + 1) & g_ht_mask;
    }
    return (uint32_t)-1;
}

/* 从已知槽位 k 擦除。墓碑方案：O(1) 标记，不做 backshift —— 查找/插入
 * 会跳过墓碑，清扫收尾重建一次性清除。调用方保证 g_ht_keys[k] == p。 */
static void rt_gc_ht_erase_at(void* p, uint32_t k) {
    if (!p || g_ht_cap == 0) return;
    g_ht_keys[k] = RT_HT_TOMBSTONE;
    g_ht_tomb++;
    g_ht_used--;
}

static void rt_gc_ht_erase(void* p) {
    uint32_t j, k, i;
    if (!p || g_ht_cap == 0) return;
    j = rt_gc_hash_ptr(p) & g_ht_mask;
    k = (uint32_t)-1;
    for (i = 0; i < g_ht_cap; i++) {
        if (g_ht_keys[j] == p) { k = j; break; }
        if (!g_ht_keys[j]) return;
        j = (j + 1) & g_ht_mask;
    }
    if (k == (uint32_t)-1) return;
    rt_gc_ht_erase_at(p, k);
}

/* 分配一个对象表槽位（复用空闲链或扩容） */
static uint32_t rt_gc_alloc_slot(void) {
    if (g_free_head >= 0) {
        uint32_t s = (uint32_t)g_free_head;
        g_free_head = (int32_t)g_objs[s].size;  /* 空闲槽 size 存 next */
        return s;
    }
    if (g_objs_len >= g_objs_cap) {
        uint32_t ncap = g_objs_cap ? g_objs_cap * 2 : 256;
        rt_gc_obj* no = (rt_gc_obj*)gc_xrealloc(g_objs, ncap * sizeof(rt_gc_obj));
        if (!no) return (uint32_t)-1;
        g_objs = no;
        g_objs_cap = ncap;
    }
    return g_objs_len++;
}

/* 登记对象（rt_core __rt_shadow_malloc 调用；type_id 由头决定） */
static void gc_alloc_hook(uint64_t bytes, void* protect);   /* GC 驱动钩子，定义在调度小节 */

/* 锁内：把攒批的 pending 全部登记进对象表（调用者必须已持 g_gc_lock）。
 * 返回本批累计字节（供 gc_alloc_hook 驱动 GC 用）。 */
static uint64_t rt_gc_flush_pending_locked(void) {
    uint32_t n = g_pend_n, i;
    uint64_t bytes = 0;
    g_pend_n = 0;
    for (i = 0; i < n; i++) {
        void* data = g_pend[i];
        rt_gc_hdr* h = rt_gc_hdr_of(data);
        uint32_t slot;
        g_reg_count++;
        if (g_gc_phase != GC_OFF) h->mark = g_col_black;
        slot = rt_gc_alloc_slot();
        if (slot == (uint32_t)-1) continue;
        g_objs[slot].data = data;
        g_objs[slot].type_id = h->type_id;
        g_objs[slot].size = h->size;
        g_objs[slot].ht_slot = rt_gc_ht_insert(data, slot);
        bytes += (uint64_t)h->size + RT_GC_HEADER;
        g_heap_bytes += (uint64_t)h->size + RT_GC_HEADER;
        if (g_heap_bytes > g_heap_peak) g_heap_peak = g_heap_bytes;
        if (g_heap_bytes > g_cycle_peak) g_cycle_peak = g_heap_bytes;
    }
    return bytes;
}

static void rt_gc_register_slow(void* data, rt_gc_hdr* h);

/* ⚠️ 多线程（§5.2.7）：本函数全程持 g_gc_lock。锁内会调用 gc_alloc_hook，
 * 后者可能跑完整 GC 周期（含 STW）—— 这正是 STW 契约要求的「发起者已持锁」。
 * CRITICAL_SECTION 可重入，嵌套获取安全。 */
void rt_gc_register(void* data) {
    rt_gc_hdr* h;
    if (!data) return;
    h = rt_gc_hdr_of(data);
    /* 快路径（攒批）：仅单 mutator + GC_OFF 时启用。
     * 无锁无哈希，把登记推迟到 flush —— 分配密集热循环（str_reverse 等）
     * 省掉每分配的全局锁 + 哈希插入。安全性论证：
     *   · 单 mutator 时 g_pend 只被本线程读写，无并发；
     *   · GC 启动（gc_cycle_start）先 flush 全部 pending 再进 MARK，
     *     攒批对象在 epoch 刷白前入表，标记/清扫照常覆盖它们；
     *   · 多 mutator 时快路径关闭（恒走慢路径），行为与旧版一致。
     * 已知微小窗口：单 mutator 攒批期间若恰好有第二个 mutator attach 且
     * 立刻跨线程 free 了本批对象，可能漏摘 —— 概率极低，注释留档。 */
    if (t_self && !t_self->is_worker && g_mutator_threads <= 1 &&
        g_gc_phase == GC_OFF && g_pend_n < RT_PEND_CAP) {
        g_pend[g_pend_n++] = data;
        t_self->alloc_root = data;
        return;
    }
    rt_gc_register_slow(data, h);
}

static void rt_gc_register_slow(void* data, rt_gc_hdr* h) {
    uint32_t slot;
    uint64_t batch;
    GC_LOCK();
    batch = rt_gc_flush_pending_locked();
    g_reg_count++;
    /* 快路径：rt_gc_register 仅由 rt_alloc_small / rt_alloc_impl 调用（rt_core.c），
     * 两者都是全新分配（空闲链表对象已随 free 注销、VirtualAlloc 对象天然全新），
     * 对象表里必然没有该地址，重复登记检查恒 miss —— 直接跳过省一次哈希探测。
     * 若未来新增"对已登记地址再次登记"的调用方，需恢复该检查。 */
    /* 分配即黑（Go 的 allocate-black）：回收周期内新生对象直接置黑。
     *
     * MARK 期的依据：新对象的**所有**字段都在其分配之后才写入，而所有堆指针
     * 写入都要过写屏障（插入屏障会 shade 新值），因此本轮无需扫描它也不会漏标。
     * 反之若置白，它会在本轮被当作垃圾回收 —— 分配后立刻被自己回收。
     *
     * SWEEP 期同样要置黑，理由不同（Task #32 惰性清扫）：清扫游标尚未走完
     * 对象表，而新对象可能复用 freelist 中位于游标**之后**的槽位。清扫判据是
     * `mark != g_col_black`，若新对象带着上一轮的旧颜色躺在待清扫区间里，
     * 游标扫到它就会当垃圾释放 —— 分配后即被清扫误杀。置黑即免疫。 */
    if (g_gc_phase != GC_OFF) h->mark = g_col_black;
    slot = rt_gc_alloc_slot();
    if (slot == (uint32_t)-1) { GC_UNLOCK(); return; }
    g_objs[slot].data = data;
    g_objs[slot].type_id = h->type_id;
    g_objs[slot].size = h->size;
    g_objs[slot].ht_slot = rt_gc_ht_insert(data, slot);
    /* §5.2.4 分配根：覆盖式记录"最近分配"，扫根时保活到 mutator 赋值 sf。
     * 在 GC 锁内写，扫根（STW 内线程停靠）读 —— x86 原子，无竞态。 */
    if (t_self) t_self->alloc_root = data;
    g_heap_bytes += h->size + RT_GC_HEADER;
    /* 堆峰值：§5.3 第 3 条「堆峰值 ≈ (1+GOGC/100)×存活堆」的被测量。
     * 必须在这里取，不能在 GC 日志里取 —— 日志只在周期收尾打印，那时
     * 峰值早已回落，测出来的永远是谷底而非峰顶。 */
    if (g_heap_bytes > g_heap_peak) g_heap_peak = g_heap_bytes;
    if (g_heap_bytes > g_cycle_peak) g_cycle_peak = g_heap_bytes;
    /* 驱动 GC。放在登记之后：此刻新对象已带上"分配即黑"，
     * 就算这一步立刻触发标记/清扫也伤不到它自己。
     * §5.2.4：GC_OFF 时分配的新对象未置黑，而它可能就是触发本轮 GC 的那一个
     * （登记时 phase 尚为 OFF，置黑分支没走到）。把触发者交给 gc_alloc_hook
     * 保护 —— gc_cycle_start 会在本轮 epoch 下把它置灰，避免"分配即被自己
     * 触发的 GC 回收"（精确根集下它在分配调用栈上，任何根都看不见它）。 */
    gc_alloc_hook((uint64_t)h->size + RT_GC_HEADER + batch, data);
    GC_UNLOCK();
}

/* 注销对象（rt_core __rt_shadow_free 调用）；不读对象头（free 顺序无关） */
void rt_gc_unregister(void* data) {
    int32_t idx;
    if (!data) return;
    GC_LOCK();
    idx = rt_gc_ptr_lookup(data);
    if (idx < 0) { GC_UNLOCK(); return; }
    g_unreg_count++;
    /* 防御：size 已被空闲链污染（异常场景）则拒绝，避免 heap 计数下溢 */
    if (g_objs[idx].size > 0x7FFFFFFF) { GC_UNLOCK(); return; }
    g_heap_bytes -= (uint64_t)g_objs[idx].size + RT_GC_HEADER;
    rt_gc_ht_erase(data);
    g_objs[idx].data = NULL;
    g_objs[idx].size = (uint32_t)g_free_head;  /* 链到空闲链 */
    g_free_head = idx;
    GC_UNLOCK();
}

/* 清扫专用释放：调用者（gc_sweep_step）已持 GC 锁，且已通过一致性校验
 * 确认对象表槽 slot 与哈希表指向一致（hj 即该校验的哈希槽位）。跳过
 * rt_gc_unregister 的重复哈希查找与重入锁，直接摘表 + 归还 span ——
 * 清扫每对象省一次查找 + 一次 Enter/LeaveCriticalSection。内存归还走
 * rt_mem_free_span_pend（rt_core.c，每 span 攒批缓冲，满 32 才加一次
 * 中央锁），与通用 free 路径一致。 */
extern void rt_mem_free_span_pend(void* p);
static void rt_gc_sweep_free(void* d, uint32_t slot, uint32_t hj) {
    g_unreg_count++;
    if (g_objs[slot].size > 0x7FFFFFFF) return;   /* 防御：同 rt_gc_unregister */
    g_heap_bytes -= (uint64_t)g_objs[slot].size + RT_GC_HEADER;
    rt_gc_ht_erase_at(d, hj);
    g_objs[slot].data = NULL;
    g_objs[slot].size = (uint32_t)g_free_head;
    g_free_head = slot;
    rt_mem_free_span_pend(d);
}

/* 覆盖对象类型（shadow_gc_alloc 用） */
void rt_gc_set_type(void* data, uint32_t type_id) {
    int32_t idx;
    if (!data) return;
    rt_gc_hdr_of(data)->type_id = type_id;
    GC_LOCK();
    idx = rt_gc_ptr_lookup(data);
    if (idx >= 0) g_objs[idx].type_id = type_id;
    GC_UNLOCK();
}

/* ---------------- 内置类型子指针扫描 ---------------- */
static int32_t rt_gc_mark_ptr(void* p);

static int32_t g_gc_debug = -1;
static int32_t rt_gc_debug_on(void) {
    if (g_gc_debug < 0) {
        const char* e = getenv("SHADOW_GC_DEBUG");
        g_gc_debug = (e && e[0] != '0' && e[0] != '\0') ? 1 : 0;
    }
    return g_gc_debug;
}
static uint64_t g_bad_array = 0;   /* 越界/损坏的 ARRAY 头计数 */
static uint64_t g_bad_slot  = 0;   /* 对象表/hash 不一致（幽灵表项）计数 */

/* 写屏障可观测性（§5.2.10）：
 *   wb_calls  —— 标记期间屏障被调用次数
 *   wb_shaded —— 其中真正把某个白对象置灰的次数
 * wb_shaded > 0 是"增量标记确实与 mutator 交错、且屏障承担了保活职责"的
 * 直接证据。若它恒为 0，说明要么没真正切片，要么测试用例强度不足。 */
static uint64_t g_wb_calls = 0;
static uint64_t g_wb_shaded = 0;

static void rt_gc_scan_children(void* data, uint32_t type_id, uint32_t size) {
    uint32_t i;
    switch (type_id) {
    case RT_T_ARRAY: {
        /* [len:4][cap:4][elem_size:4][data@12]
         * elem_size==8 时元素可能是指针（string/struct/array/any 数组）→ 保守逐元素
         * 查对象表：值恰好是堆对象地址才追踪（long/double 值命中概率≈0，只多保活不误收）。 */
        int32_t es, len;
        if (size < 12) break;
        es  = *(int32_t*)((char*)data + 8);
        len = *(int32_t*)data;
        if (es == 8) {
            /* 边界校验：损坏的头（误回收后被 freelist next 覆盖等）会给出天文数字 len，
             * 直接越界读 → GC 自身崩溃。这里裁剪并计数，便于定位真正的根因。 */
            if (len < 0 || (uint64_t)12 + (uint64_t)len * 8 > (uint64_t)size) {
                g_bad_array++;
                if (rt_gc_debug_on())
                    fprintf(stderr, "[GC][BAD-ARRAY] data=%p size=%u len=%d es=%d\n",
                            data, size, len, es);
                len = (int32_t)((size - 12) / 8);
                if (len < 0) len = 0;
            }
            for (i = 0; i < (uint32_t)len; i++) {
                void* e = *(void**)((char*)data + 12 + i * 8);
                if (rt_gc_ptr_lookup(e) >= 0) rt_gc_mark_ptr(e);
            }
        }
        break;
    }
    case RT_T_ANYBOX: {
        /* AnyBox: [magic:int32@0][tag:int32@4][value:int64@8]；tag 3(string)/5(ptr) → value 指针 */
        int32_t tag = *(int32_t*)((char*)data + 4);
        if (tag == 3 || tag == 5) {
            void* v = *(void**)((char*)data + 8);
            rt_gc_mark_ptr(v);
        }
        break;
    }
    case RT_T_DICT: {
        /* rt_dict: { rt_kv** buckets; int32 n_buckets; int32 size; }
         * rt_kv(64 位): { char* key@0; int vtype@8; union{v}@16; rt_kv* next@24; } */
        void** buckets = *(void***)data;
        int32_t nb = *(int32_t*)((char*)data + 8);
        int32_t b;
        rt_gc_mark_ptr(buckets);
        for (b = 0; b < nb; b++) {
            void* kvp = buckets[b];
            while (kvp) {
                char* key = *(char**)kvp;
                int32_t vt = *(int32_t*)((char*)kvp + 8);
                void* next = *(void**)((char*)kvp + 24);
                rt_gc_mark_ptr(key);
                if (vt == 0) {           /* RT_V_STRING */
                    void* s = *(void**)((char*)kvp + 16);
                    rt_gc_mark_ptr(s);
                } else if (vt == 4) {    /* RT_V_PTR */
                    void* p2 = *(void**)((char*)kvp + 16);
                    rt_gc_mark_ptr(p2);
                }
                rt_gc_mark_ptr(kvp);
                kvp = next;
            }
        }
        break;
    }
    case RT_T_CLOSURE: {
        void* env = *(void**)((char*)data + 8);
        rt_gc_mark_ptr(env);
        break;
    }
    default:
        if (type_id >= RT_T_USER && type_id < RT_GC_MAX_TYPES) {
            rt_gc_typ* t = &g_types[type_id];
            if (t->size == 0) break;
            if (size > 512) {
                /* 大对象：整块保守扫（每 8 字节当指针判定） */
                uint32_t n = size / 8;
                for (i = 0; i < n; i++) {
                    void* v = *(void**)((char*)data + i * 8);
                    rt_gc_mark_ptr(v);
                }
            } else {
                uint64_t bm = t->bitmap;
                for (i = 0; i < 64 && i * 8 < size; i++) {
                    if ((bm >> i) & 1u) {
                        void* v = *(void**)((char*)data + i * 8);
                        rt_gc_mark_ptr(v);
                    }
                }
            }
        }
        break;
    }
}

/* 漏标归因用（仅 SHADOW_GC_VERIFY 的重标记期间非 NULL）：
 * g_verify_parent[slot] 记录该对象是被谁第一次标灰的。 */
static void**   g_verify_parent = NULL;
static void*    g_scan_parent = NULL;
/* 根集合捕获（仅 VERIFY 期间开启）：把扫根过程中经手的**每一个**根指针录下来，
 * 无论它当时是白是黑。只录"新置灰的"是不够的 —— 增量期间已经变黑的根
 * 不会再入灰队列，漏录它们会让重标记的起点残缺，反而误杀活对象。 */
static int32_t  g_capturing_roots = 0;
static void**   g_verify_roots = NULL;
static uint32_t g_verify_roots_len = 0, g_verify_roots_cap = 0;
static void gc_verify_record_root(void* p) {
    if (g_verify_roots_len >= g_verify_roots_cap) {
        uint32_t ncap = g_verify_roots_cap ? g_verify_roots_cap * 2 : 1024;
        void** nr = (void**)gc_xrealloc(g_verify_roots, ncap * sizeof(void*));
        if (!nr) return;
        g_verify_roots = nr;
        g_verify_roots_cap = ncap;
    }
    g_verify_roots[g_verify_roots_len++] = p;
}

/* 置灰（shade）：白对象 → 灰并入 worklist；已灰/已黑则幂等返回。
 * 这是三色不变式的唯一入口 —— 根扫描、写屏障、子指针追踪全部走这里。 */
static int32_t rt_gc_mark_ptr(void* p) {
    int32_t idx;
    rt_gc_hdr* h;
    if (!p) return 0;
    idx = rt_gc_ptr_lookup(p);
    if (idx < 0) return 0;
    if (g_capturing_roots) gc_verify_record_root(p);   /* 必须在颜色判定之前 */
    h = rt_gc_hdr_of(p);
    /* epoch 着色：mark 不等于本轮灰/黑 ⇒ 白（含上一轮遗留的旧颜色） */
    if (h->mark == g_col_grey || h->mark == g_col_black) return 0;
    h->mark = g_col_grey;
    if (g_wl_len >= g_wl_cap) {
        uint32_t ncap = g_wl_cap ? g_wl_cap * 2 : 1024;
        void** nw = (void**)gc_xrealloc(g_wl, ncap * sizeof(void*));
        if (!nw) return 0;
        g_wl = nw;
        g_wl_cap = ncap;
    }
    g_wl[g_wl_len++] = p;
    if (g_verify_parent) g_verify_parent[idx] = g_scan_parent;   /* 漏标归因，见 gc_verify_marks */
    return 1;   /* 确实完成了一次 白→灰 转变 */
}

/* 保守扫描一段内存区间 [lo, hi)，8 字节对齐逐字判定 */
static void rt_gc_scan_range(uintptr_t lo, uintptr_t hi) {
    uintptr_t q;
    lo = (lo + 7u) & ~(uintptr_t)7u;
    for (q = lo; q + 8 <= hi; q += 8) {
        void* v = *(void**)q;
        if (v && rt_gc_ptr_lookup(v) >= 0) rt_gc_mark_ptr(v);
    }
}

/* 保守扫描当前线程栈 + CPU 寄存器上下文。
 *
 * ⚠️ 历史 bug（2026-08-04 修复），两处均会导致活对象被误回收：
 *   (1) 旧实现 p = (stack_base-1)&~0xFFF 后只扫 [p-0x1000, p)，
 *       **栈最高一页从未被扫描** —— 而 main() 的栈帧恰好就在最高页，
 *       main 的局部指针（如 ex_gc_strings 的 keep 数组）对 GC 不可见。
 *   (2) 完全不看寄存器。调用边界上 callee-saved 寄存器
 *       （rbx/rbp/rsi/rdi/r12-r15）里的活指针若未被更深的帧压栈保存，
 *       GC 就看不到 —— 这正是"调用边界 reg 值保活"缺陷。
 *       用 RtlCaptureContext 抓取整个通用寄存器组当根即可根治，
 *       无需在 codegen 侧为每个调用点插临时 root_set。
 */
#ifdef _WIN64
/* 通用寄存器组当根（16 个整数寄存器，含 volatile —— 多保活无害）。 */
static void gc_scan_ctx_regs(const CONTEXT* ctx) {
    void* regs[15];
    int i;
    regs[0]  = (void*)(uintptr_t)ctx->Rax; regs[1]  = (void*)(uintptr_t)ctx->Rcx;
    regs[2]  = (void*)(uintptr_t)ctx->Rdx; regs[3]  = (void*)(uintptr_t)ctx->Rbx;
    regs[4]  = (void*)(uintptr_t)ctx->Rbp; regs[5]  = (void*)(uintptr_t)ctx->Rsi;
    regs[6]  = (void*)(uintptr_t)ctx->Rdi; regs[7]  = (void*)(uintptr_t)ctx->R8;
    regs[8]  = (void*)(uintptr_t)ctx->R9;  regs[9]  = (void*)(uintptr_t)ctx->R10;
    regs[10] = (void*)(uintptr_t)ctx->R11; regs[11] = (void*)(uintptr_t)ctx->R12;
    regs[12] = (void*)(uintptr_t)ctx->R13; regs[13] = (void*)(uintptr_t)ctx->R14;
    regs[14] = (void*)(uintptr_t)ctx->R15;
    for (i = 0; i < 15; i++) {
        if (regs[i] && rt_gc_ptr_lookup(regs[i]) >= 0) rt_gc_mark_ptr(regs[i]);
    }
}

/* 完整扫描 [rsp, stack_base)，按 VirtualQuery 区域推进（含最高页）。 */
static void gc_scan_stack_span(uintptr_t rsp, uintptr_t stack_base) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t page;
    if (rsp < 0x1000 || stack_base == 0 || rsp >= stack_base) return;
    page = rsp & ~(uintptr_t)0xFFF;
    while (page < stack_base) {
        uintptr_t region_end, lo, hi;
        if (!VirtualQuery((void*)page, &mbi, sizeof(mbi))) break;
        region_end = (uintptr_t)mbi.BaseAddress + (uintptr_t)mbi.RegionSize;
        if (region_end <= page) break;            /* 防御：区域信息异常 */
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & PAGE_GUARD) &&
            !(mbi.Protect & PAGE_NOACCESS)) {
            lo = (page > rsp) ? page : rsp;
            hi = (region_end < stack_base) ? region_end : stack_base;
            if (hi > lo) rt_gc_scan_range(lo, hi);
        }
        page = region_end;
    }
}
#endif

static void rt_gc_scan_stack(void) {
#ifdef _WIN64
    CONTEXT ctx;
    uintptr_t stack_base = __readgsqword(0x08);   /* NT_TIB.StackBase（高地址，独占上界） */
    uintptr_t rsp;

    /* ── 1) 寄存器根：捕获当前上下文。GC 自身的帧不会破坏 callee-saved 寄存器，
     *       因此它们仍持有 mutator 的值；volatile 寄存器一并保守扫（多保活无害）。 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    RtlCaptureContext(&ctx);
    gc_scan_ctx_regs(&ctx);

    /* ── 2) 栈根 */
    rsp = (uintptr_t)ctx.Rsp;
    if (rsp == 0) rsp = (uintptr_t)_AddressOfReturnAddress();
    gc_scan_stack_span(rsp, stack_base);
#else
    (void)0;
#endif
}

/* 诊断：只扫寄存器（不扫栈） */
static void rt_gc_scan_regs_only(void) {
#ifdef _WIN64
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
    RtlCaptureContext(&ctx);
    gc_scan_ctx_regs(&ctx);
#else
    (void)0;
#endif
}

/* 诊断：只扫栈（不扫寄存器） */
static void rt_gc_scan_stack_only(void) {
#ifdef _WIN64
    uintptr_t stack_base = __readgsqword(0x08);
    uintptr_t rsp = (uintptr_t)_AddressOfReturnAddress();
    gc_scan_stack_span(rsp, stack_base);
#else
    (void)0;
#endif
}

/* 诊断：挂起线程只扫寄存器 */
static void gc_scan_thread_regs_only(rt_gc_thread* th) {
#ifdef _WIN64
    if (!th || !th->in_use || !th->stw_ctx_ok) return;
    gc_scan_ctx_regs(&th->stw_ctx);
#else
    (void)th;
#endif
}

/* 诊断：挂起线程只扫栈 */
static void gc_scan_thread_stack_only(rt_gc_thread* th) {
#ifdef _WIN64
    if (!th || !th->in_use || !th->stw_ctx_ok) return;
    gc_scan_stack_span((uintptr_t)th->stw_ctx.Rsp, th->stack_base);
#else
    (void)th;
#endif
}

/* 扫描**其它**（已被 STW 挂起的）线程：寄存器快照由 gc_stw_begin 抓好。
 * 线程已停住，Rsp 与栈内容此刻是稳定快照，可安全逐字保守扫描。 */
static void gc_scan_thread_suspended(rt_gc_thread* th) {
#ifdef _WIN64
    if (!th || !th->in_use || !th->stw_ctx_ok) return;
    gc_scan_ctx_regs(&th->stw_ctx);
    gc_scan_stack_span((uintptr_t)th->stw_ctx.Rsp, th->stack_base);
#else
    (void)th;
#endif
}

/* ============================================================
 * Stop-The-World（§5.2.4 协作式安全点，对标 Go preemptible）
 * ------------------------------------------------------------
 * 取代 SuspendThread 抢占式挂起：编译器在每个安全点（函数入口、循环回边）
 * 插入 shadow_gc_poll 调用。GC 启动 STW 时：
 *   ① 置 g_stw_req=1，请求所有线程到达安全点；
 *   ② 等待每个"自由运行"的其它线程进入非自由状态（gc_state != 0：
 *      在 poll 自旋、或在 GC 锁内/等锁）——自由线程会在下一个安全点
 *      （函数入口/回边，最坏到下一个函数调用）执行 poll 并停下；
 *   ③ 全部就绪 → 置 g_stw_active=1 → 精确扫根（各线程帧根 + shadow frame
 *      range 根 + 全局根，无需保守栈扫描）→ 置 g_stw_active=0 放行。
 *
 * 契约（违反即死锁）：
 *   ① 调用者必须已持有 g_gc_lock（GC_LOCK 同时把当前线程标记为非自由，
 *      其它线程即使正阻塞在 GC_LOCK 上也不会被等待 —— 它不会再跑 mutator
 *      代码，其精确根集已冻结，可安全扫根）；
 *   ② 等待循环内除轮询线程表外不获取任何 mutator 可能持有的锁；
 *   ③ 灰队列容量在进入等待之前一次性预留（gc_wl_reserve）。
 *
 * 为什么其它线程此刻是"精确"的：线程在安全点（poll）停下时，跨安全点的
 * 指针都已 spill 到 shadow frame / slot（编译器活跃性分析保证），寄存器与
 * llc 栈槽里没有 GC 需要的活指针 —— 无需保守扫描。
 * ============================================================ */
static volatile LONG g_stw_req = 0;     /* 1 = 请求各线程到达安全点 */
static volatile LONG g_stw_active = 0;  /* 1 = 扫根窗口进行中 */
/* 内联轮询标志：codegen 在安全点以 volatile i32 直接加载本全局（快路径零函数调用），
 * 置位时才调用 shadow_gc_poll 慢路径。STW 期间置 1，放行后清 0（对齐 Linux g_gc_poll_flag）。 */
volatile LONG g_gc_poll_flag = 0;

/* 编译器在每个安全点插入的协作检查（对标 Go preemptible 的栈增长检查点） */
extern void shadow_gc_poll(void) {
    if (g_stw_req || g_stw_active) {
        rt_gc_thread* th = t_self;
        if (th) th->gc_state = 1;       /* 到达安全点（根集已冻结） */
        /* 等本次 STW 完全结束（req 清 0 且 active 清 0）：
         * ⚠️ 只等 active 是不够的 —— poll 可能在 GC 的"等待阶段"进入
         * （req=1, active=0），若只看 active 会立即通过并回到 mutator，
         * 而 GC 已经把它计入"到达"并开始扫根 —— 扫根窗口内该线程却在
         * 跑 mutator 代码（非安全点），精确根集漏标。必须等到 req 与
         * active 双双清零（GC 明确放行）。 */
        while (g_stw_req || g_stw_active) Sleep(0);
        if (th) th->gc_state = 0;
    }
}

static void gc_wl_reserve(uint32_t need) {
    if (need <= g_wl_cap) return;
    {
        uint32_t ncap = g_wl_cap ? g_wl_cap : 1024;
        void** nw;
        while (ncap < need) ncap *= 2;
        nw = (void**)gc_xrealloc(g_wl, ncap * sizeof(void*));
        if (!nw) return;
        g_wl = nw;
        g_wl_cap = ncap;
    }
}

static void gc_stw_begin(void) {
    DWORD me = GetCurrentThreadId();
    uint32_t i;
    /* 灰队列上界 = 对象总数（mark_ptr 有颜色判定，每对象至多入队一次）。
     * 本函数在 g_gc_lock 内被调用，reserve 无并发。 */
    gc_wl_reserve(g_objs_len + 64);
    InterlockedExchange(&g_stw_req, 1);
    InterlockedExchange(&g_gc_poll_flag, 1);  /* 通知各线程内联 poll 进入慢路径 */
    /* 等待循环用**无锁快速扫描**（性能：STRESS 高频周期下每次等 8 个线程到
     * poll，持锁遍历会成为热点）。安全性论证：
     *   · g_threads_len 单调不减（attach 只增槽、detach 复用空槽不缩减），
     *     读到旧值只会"多等"新线程（它 attach 后 gc_state=0，下轮被等），
     *     绝不漏等已登记线程；
     *   · in_use/tid/gc_state 都是 4 字节原子读，x86 无撕裂；
     *   · 扫描读到"正在 attach 的半初始化槽"（in_use=1 刚置、gc_state=0）：
     *     会把它当自由线程等 —— 它 attach 后进入 poll/等锁即满足，安全。 */
    for (;;) {
        uint32_t k;
        int all = 1;
        uint32_t n = *(volatile uint32_t*)&g_threads_len;
        for (k = 0; k < n; k++) {
            rt_gc_thread* th = &g_threads[k];
            if (!th->in_use || th->tid == me) continue;
            if (th->gc_state == 0) { all = 0; break; }
        }
        if (all) break;
        Sleep(0);
    }
    InterlockedExchange(&g_stw_active, 1);
    InterlockedExchange(&g_stw_req, 0);
}

static void gc_stw_end(void) {
    InterlockedExchange(&g_stw_active, 0);
    InterlockedExchange(&g_gc_poll_flag, 0);  /* 放行：内联 poll 恢复快路径 */
}

/* ============================================================
 * 并发标记 worker + 后台清扫线程（§5.2.2 / §5.2.6，对标 Go）
 * ------------------------------------------------------------
 * 标记：周期开始（STW 扫根后）唤醒 N 个常驻 mark worker，与 mutator 并行
 * 从全局灰队列取对象扫描 —— work stealing 的简化：全局队列 + g_gc_lock
 * （开发原则 §5.2.2 明确允许"全局队列 + 锁"）。mutator 的写屏障（锁内）与
 * 分配切片仍照常推进，与 worker 互斥串行。
 * 清扫：周期终止后唤醒 1 个常驻 sweep worker，锁内分片清扫，与 mutator 的
 * 分配驱动惰性清扫互补。
 *
 * 线程协议（worker 是线程表一员，参与协作 STW）：
 *   空闲等待（gc_state=1，安全点）→ 周期激活（gc_state=0 自由）→
 *   干活（锁内操作，gc_state=2）→ 收尾（队列空 / 清扫完）→ 回空闲。
 * STW 等待循环把 gc_state!=0 视为"已到达"；自由态 worker 在循环顶检查
 * g_stw_req 主动停靠（它停靠时不在锁内、不执行标记代码，根集冻结）。
 *
 * 为什么协作 STW 下 worker 的精确根集是完整的：worker 停靠点的调用栈
 * 上没有"活着且未 spill"的堆指针 —— 停靠前最后一个锁内操作已结束，
 * 局部指针已出作用域；它不执行任何 shadow 代码（无 sf 也无需 sf）。
 * ============================================================ */
#define SHADOW_MAX_MARK_WORKERS 16
extern void rt_gc_thread_attach(int is_worker);   /* 定义见「线程表」小节 */
/* 后台清扫线程状态（gc_workers_shutdown 引用，声明提前） */
static HANDLE g_sweep_thread = NULL;
static volatile LONG g_sweep_go = 0;       /* 1 = 清扫激活 */
static volatile LONG g_sweep_shutdown = 0; /* 进程退出 */
static HANDLE          g_mark_threads[SHADOW_MAX_MARK_WORKERS];
static uint32_t        g_mark_n = 0;
static volatile LONG   g_mark_go = 0;        /* 1 = 本周期标记激活（worker 干活） */
static volatile LONG   g_mark_done_req = 0;  /* 1 = 请求收尾（标记终止） */
static volatile LONG   g_mark_shutdown = 0;  /* 进程退出 */
static int             g_workers_exit_armed = 0;

static uint64_t gc_drain(uint64_t budget);      /* 定义见「增量标记」小节 */
static uint32_t gc_sweep_step(uint64_t budget); /* 定义见「清扫」小节 */
static uint64_t rt_gc_sweep_budget(void);

static DWORD WINAPI gc_mark_worker_main(LPVOID arg) {
    (void)arg;
    rt_gc_thread_attach(1);               /* GC worker：不分配，不计入 mutator 数 */
    t_self->gc_state = 1;                 /* 空闲 = 安全点 */
    for (;;) {
        while (!g_mark_go) { if (g_mark_shutdown) return 0; Sleep(1); }
        t_self->gc_state = 0;             /* 自由：进入标记 */
        for (;;) {
            uint64_t w;
            if (g_stw_req) {              /* 协作 STW：主动停靠 */
                t_self->gc_state = 1;
                while (g_stw_req || g_stw_active) Sleep(0);
                t_self->gc_state = 0;
            }
            GC_LOCK();
            w = gc_drain(1);              /* 锁内处理至多 1 个对象（含子指针入队） */
            GC_UNLOCK();
            if (w == 0) {
                if (g_mark_done_req) break;   /* 队列空且收到收尾请求 */
                Sleep(1);                     /* 低频轮询新对象 / done */
            }
        }
        t_self->gc_state = 1;             /* 回空闲，等下一周期 */
        while (g_mark_go) { if (g_mark_shutdown) return 0; Sleep(1); }
    }
}

static void gc_workers_shutdown(void) {
    InterlockedExchange(&g_mark_shutdown, 1);
    InterlockedExchange(&g_sweep_shutdown, 1);
}

static void gc_workers_ensure_atexit(void) {
    if (g_workers_exit_armed) return;
    g_workers_exit_armed = 1;
    atexit(gc_workers_shutdown);
}

static void gc_mark_workers_ensure(void) {
    DWORD ncpu = 1;
    const char* e;
    if (g_mark_n > 0) return;
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    }
    e = getenv("SHADOW_GC_MARKWORKERS");
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= SHADOW_MAX_MARK_WORKERS) ncpu = (DWORD)v;   /* 0 = 禁用 */
    }
    if (ncpu > 8) ncpu = 8;               /* 上限 8，避免小规模程序线程洪泛 */
    while (g_mark_n < ncpu) {
        DWORD tid;
        HANDLE h = CreateThread(NULL, 0, gc_mark_worker_main, NULL, 0, &tid);
        if (!h) break;
        g_mark_threads[g_mark_n++] = h;
    }
    gc_workers_ensure_atexit();
}

/* ── 后台清扫线程（§5.2.6） ── */

static DWORD WINAPI gc_sweep_worker_main(LPVOID arg) {
    (void)arg;
    rt_gc_thread_attach(1);               /* GC worker：不分配，不计入 mutator 数 */
    t_self->gc_state = 1;
    for (;;) {
        while (!g_sweep_go) { if (g_sweep_shutdown) return 0; Sleep(1); }
        t_self->gc_state = 0;
        while (g_gc_phase == GC_SWEEP && g_sweep_cursor < g_sweep_end) {
            if (g_stw_req) {
                t_self->gc_state = 1;
                while (g_stw_req || g_stw_active) Sleep(0);
                t_self->gc_state = 0;
            }
            GC_LOCK();
            if (g_gc_phase == GC_SWEEP && g_sweep_cursor < g_sweep_end)
                gc_sweep_step(rt_gc_sweep_budget());
            GC_UNLOCK();
            Sleep(0);
        }
        t_self->gc_state = 1;
        while (g_sweep_go) { if (g_sweep_shutdown) return 0; Sleep(1); }
    }
}

static void gc_sweep_worker_ensure(void) {
    DWORD tid;
    const char* e;
    if (g_sweep_thread) return;
    e = getenv("SHADOW_GC_SWEEPWORKER");
    if (e && e[0] == '0') return;             /* 0 = 禁用（仅分配驱动清扫） */
    g_sweep_thread = CreateThread(NULL, 0, gc_sweep_worker_main, NULL, 0, &tid);
    gc_workers_ensure_atexit();
}

/* ============================================================
 * 线程表：注册 / 注销（§5.2.7）
 * ------------------------------------------------------------
 * attach 在两处发生：
 *   · spawn 出来的线程 —— rt_thread.c 的 task_run 首行显式调用；
 *   · 主线程         —— 第一次触碰 GC（分配/帧根）时由 rt_gc_self() 惰性补登记。
 * 句柄用 DuplicateHandle 复制真句柄：GetCurrentThread() 返回的是伪句柄
 * (-2)，跨线程传给 SuspendThread 只会挂起调用者自己。
 * ============================================================ */
extern void rt_gc_thread_attach(int is_worker) {
    rt_gc_thread* th = NULL;
    uint32_t i;
    if (t_self) return;
    rt_gc_init();
    EnterCriticalSection(&g_thr_lock);
    for (i = 0; i < g_threads_len; i++) {          /* 优先复用退出线程留下的空槽 */
        if (!g_threads[i].in_use) { th = &g_threads[i]; break; }
    }
    if (!th && g_threads_len < RT_GC_MAX_THREADS) th = &g_threads[g_threads_len++];
    if (th) {
        HANDLE dup = NULL;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                        GetCurrentProcess(), &dup,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                        FALSE, 0);
        th->h   = dup;
        th->tid = GetCurrentThreadId();
#ifdef _WIN64
        th->stack_base = __readgsqword(0x08);      /* NT_TIB.StackBase */
#else
        th->stack_base = 0;
#endif
        /* roots 缓冲复用空槽遗留的内存，不必重新分配 */
        th->roots_len = 0;
        th->in_use = 1;
        th->gc_state = 0;          /* 协作式安全点：初始为自由运行 */
        th->gc_lock_depth = 0;
        th->alloc_root = NULL;
        th->is_worker = is_worker;
        t_self = th;
        /* mutator 计数：GC worker 不计入。快路径（攒批）仅在计数<=1 时启用。 */
        if (!is_worker) InterlockedIncrement(&g_mutator_threads);
    }
    LeaveCriticalSection(&g_thr_lock);
}

/* 归还线程本地分配缓存（rt_core.c；§5.2.7「线程终止时归还线程本地分配缓存」） */
extern void rt_mcache_release(void);

extern void rt_gc_thread_detach(void) {
    rt_gc_thread* th = t_self;
    if (!th) return;
    /* 先还 mcache 再出表：归还过程会碰 mcentral，但不碰 GC 元数据。 */
    rt_mcache_release();
    EnterCriticalSection(&g_thr_lock);
    th->roots_len = 0;                 /* 根随线程一起消失 */
    if (th->h) { CloseHandle(th->h); th->h = NULL; }
    th->stack_base = 0;
    th->tid = 0;
    th->in_use = 0;                    /* 槽保留（roots 缓冲留给下一个线程复用） */
    th->gc_state = 0;
    th->gc_lock_depth = 0;
    th->alloc_root = NULL;             /* 分配根随线程消失 */
    if (!th->is_worker) InterlockedDecrement(&g_mutator_threads);
    LeaveCriticalSection(&g_thr_lock);
    t_self = NULL;
}

/* 当前线程的 GC 记录；主线程走这里惰性登记。 */
static rt_gc_thread* rt_gc_self(void) {
    if (!t_self) rt_gc_thread_attach(0);   /* 主线程 = mutator */
    return t_self;
}

/* ---------------- 根集 API（编译器生成调用） ----------------
 * 帧根是**每线程**的：marker 是本线程栈深的快照，跨线程截断即灾难。 */
extern int32_t shadow_gc_frame_enter(void) {
    rt_gc_thread* th = rt_gc_self();
    return th ? (int32_t)th->roots_len : 0;
}
extern int32_t shadow_gc_frame_leave(int32_t marker) {
    rt_gc_thread* th = t_self;
    if (!th) return 0;
    if (marker < 0) return 0;
    if ((uint32_t)marker > th->roots_len) return 0;
    th->roots_len = (uint32_t)marker;
    return 0;
}
static void rt_gc_root_push(void* slot, void* val) {
    rt_gc_thread* th = rt_gc_self();
    if (!th) return;
    if (th->roots_len >= th->roots_cap) {
        uint32_t ncap = th->roots_cap ? th->roots_cap * 2 : 256;
        /* 刻意用**进程默认堆**而非 GC 私有堆：这块缓冲只由属主线程分配、
         * GC 在 STW 里仅只读扫描，绝不 realloc 它。若放进 GC 私有堆，
         * mutator 就会在不持 g_gc_lock 的情况下拿私有堆锁，破坏上面的不变式。 */
        rt_gc_root* nr = (rt_gc_root*)(th->roots
            ? HeapReAlloc(GetProcessHeap(), 0, th->roots, ncap * sizeof(rt_gc_root))
            : HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(rt_gc_root)));
        if (!nr) return;
        th->roots = nr;
        th->roots_cap = ncap;
    }
    th->roots[th->roots_len].slot = slot;
    th->roots[th->roots_len].val = val;
    th->roots[th->roots_len].n = 0;
    th->roots_len++;
}
/* root_set：slot 键控 replace 语义（同一变量 slot 只保留最新值，对标 0.3 named_roots） */
extern int32_t shadow_gc_root_set(void* slot, void* val) {
    rt_gc_thread* th = rt_gc_self();
    uint32_t i;
    if (!th) return 0;
    for (i = 0; i < th->roots_len; i++) {
        if (th->roots[i].n == 0 && th->roots[i].slot == slot) {
            th->roots[i].val = val;
            return 0;
        }
    }
    rt_gc_root_push(slot, val);
    return 0;
}
/* §5.2.4 精确根集：range 根。编译器在函数 entry 分配 [N x i64] shadow frame
 * 数组（对标 Go stack map），一次性登记。扫描时**读数组当前值**（mutator 用
 * 纯 store 更新，零调用开销）。与 slot 根同栈，frame_leave(marker) 一并截断。 */
extern int32_t shadow_gc_root_range(void* base, uint32_t n) {
    rt_gc_thread* th = rt_gc_self();
    if (!th || !base || n == 0) return 0;
    if (th->roots_len >= th->roots_cap) {
        uint32_t ncap = th->roots_cap ? th->roots_cap * 2 : 256;
        rt_gc_root* nr = (rt_gc_root*)(th->roots
            ? HeapReAlloc(GetProcessHeap(), 0, th->roots, ncap * sizeof(rt_gc_root))
            : HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(rt_gc_root)));
        if (!nr) return 0;
        th->roots = nr;
        th->roots_cap = ncap;
    }
    th->roots[th->roots_len].slot = base;
    th->roots[th->roots_len].val = base;
    th->roots[th->roots_len].n = n;
    th->roots_len++;
    return 0;
}

/* ---------------- 永久根（跨线程移交用，见 rt_thread.c 文件头）-------------- */
extern void shadow_gc_perm_root_add(void* p) {
    if (!p) return;
    GC_LOCK();
    if (g_perm_len >= g_perm_cap) {
        uint32_t ncap = g_perm_cap ? g_perm_cap * 2 : 64;
        void** np = (void**)gc_xrealloc(g_perm, ncap * sizeof(void*));
        if (!np) { GC_UNLOCK(); return; }
        g_perm = np;
        g_perm_cap = ncap;
    }
    g_perm[g_perm_len++] = p;
    GC_UNLOCK();
}
extern void shadow_gc_perm_root_remove(void* p) {
    uint32_t i;
    if (!p) return;
    GC_LOCK();
    for (i = 0; i < g_perm_len; i++) {
        if (g_perm[i] == p) {            /* 交换删除；同一指针可能重复登记，只摘一份 */
            g_perm[i] = g_perm[g_perm_len - 1];
            g_perm_len--;
            break;
        }
    }
    GC_UNLOCK();
}
/* 全局根表是**进程共享**的（不像帧根随线程走），多线程下必须加锁：
 * 两条线程同时给不同全局变量赋值会并发追加同一张表。锁在 g_gc_lock 上，
 * 与 STW 扫根天然互斥。 */
extern int32_t shadow_gc_global_root_set(void* g, void* val) {
    uint32_t i;
    if (!g) return 0;
    GC_LOCK();
    for (i = 0; i < g_globals_len; i++) {
        if (g_globals[i].slot == g) {
            g_globals[i].val = val;
            GC_UNLOCK();
            return 0;
        }
    }
    if (g_globals_len >= g_globals_cap) {
        uint32_t ncap = g_globals_cap ? g_globals_cap * 2 : 64;
        rt_gc_root* ng = (rt_gc_root*)gc_xrealloc(g_globals, ncap * sizeof(rt_gc_root));
        if (!ng) { GC_UNLOCK(); return 0; }
        g_globals = ng;
        g_globals_cap = ncap;
    }
    g_globals[g_globals_len].slot = g;
    g_globals[g_globals_len].val = val;
    g_globals[g_globals_len].n = 0;
    g_globals_len++;
    GC_UNLOCK();
    return 0;
}
extern void shadow_gc_root_add(void* p) {
    rt_gc_root_push(p, p);
}

/* ---------------- finalizer ---------------- */
extern void shadow_gc_set_finalizer(void* obj, void (*fn)(void*), void* data2) {
    if (!obj || !fn) return;
    GC_LOCK();
    if (g_fins_len >= g_fins_cap) {
        uint32_t ncap = g_fins_cap ? g_fins_cap * 2 : 32;
        rt_gc_fin* nf = (rt_gc_fin*)gc_xrealloc(g_fins, ncap * sizeof(rt_gc_fin));
        if (!nf) { GC_UNLOCK(); return; }
        g_fins = nf;
        g_fins_cap = ncap;
    }
    g_fins[g_fins_len].data = obj;
    g_fins[g_fins_len].fn = fn;
    g_fins[g_fins_len].data2 = data2;
    g_fins_len++;
    GC_UNLOCK();
}

/* ---------------- 触发 ---------------- */
static int32_t rt_gc_log_on(void) {
    if (g_gc_log < 0) {
        const char* e = getenv("SHADOW_GC_LOG");
        g_gc_log = (e && e[0] != '0' && e[0] != '\0') ? 1 : 0;
    }
    return g_gc_log;
}

extern int32_t shadow_gc_register_type(int32_t id, int32_t size, int64_t bitmap) {
    if (id < RT_T_USER || id >= RT_GC_MAX_TYPES) return 0;
    g_types[id].size = (uint32_t)size;
    g_types[id].bitmap = (uint64_t)bitmap;
    return 1;
}

/* ---------------- 分配与触发 ---------------- */
extern void* __rt_shadow_malloc(int32_t n);

/* SHADOW_GC_STRESS=N：每 N 次分配强制收集一次，无视堆阈值（等价 Go 的 gcstress）。
 * 这是排查"漏根导致误回收"最有效的手段——把 GC 频率拉到极限，任何未被登记
 * 的活引用都会在几十次分配内被回收掉并立刻暴露，而不是几小时后随机崩溃。
 * N=1 极慢但最灵敏；日常回归建议 N=64~256。 */
static int32_t g_gc_stress = -1;
static uint64_t g_alloc_ticks = 0;
static int32_t rt_gc_stress_n(void) {
    if (g_gc_stress < 0) {
        const char* e = getenv("SHADOW_GC_STRESS");
        int v = e ? atoi(e) : 0;
        g_gc_stress = (v > 0) ? v : 0;
    }
    return g_gc_stress;
}

/* 前置声明：增量调度（定义在 shadow_gc_collect 之后） */
static void gc_cycle_start(void);
static void gc_step(uint64_t budget);
static int32_t rt_gc_incremental_on(void);
static uint64_t rt_gc_mark_budget(void);
/* 前置声明：惰性清扫（Task #32） */
static uint32_t gc_sweep_step(uint64_t budget);
static uint64_t rt_gc_sweep_budget(void);
/* 前置声明：GC assist / pacing（Task #33） */
static uint64_t rt_gc_assist_debt(uint64_t bytes);

/* ── GC 的唯一驱动点（Task #33 修正）──
 *
 * 这个钩子挂在 rt_gc_register 上，也就是**每一次**堆分配都会经过的地方。
 *
 * 曾经它只挂在 shadow_gc_alloc 里，那是个严重的结构性缺陷：codegen 只对
 * 结构体分配生成 shadow_gc_alloc 调用，而字符串（rt_core 的 str concat）、
 * array、dict 全部直接调 __rt_shadow_malloc。于是 GC 的三套驱动
 * ——标记切片、清扫切片、assist——统统看不见这部分分配量。
 * 后果：
 *   · 字符串密集型程序（ex_gc_strings 的堆几乎全是字符串）里 GC 近乎瞎跑，
 *     推进节奏与真实分配速率脱钩；
 *   · assist 更是名存实亡 —— 实测一整个标记周期只累计到 9 个对象的债，
 *     而同期跑了 69 个标记切片，assist 占比不到 2%，完全没在承重。
 *
 * 挂到 register 之后，"每分配一个字节就还一份标记债"才真正成立。
 *
 * 时序说明：本钩子在对象登记**之后**触发，新对象已带上"分配即黑"，
 * 因此在这里推进标记/清扫不会误伤它自己。
 */
static void gc_stats_arm(void);   /* SHADOW_GC_STATS 退出汇总，定义见「开关与统计」 */
static void* g_gc_protect = NULL; /* 触发 GC 的分配对象（GC 锁内读写，见 gc_alloc_hook） */

static void gc_alloc_hook(uint64_t bytes, void* protect) {
    int32_t stress;
    gc_stats_arm();   /* 首次分配时挂 atexit；此后是一次整数判断 */
    if (g_gc_disabled || g_gc_running) return;
    stress = rt_gc_stress_n();
    {
        int want_start = 0;
        int reason = 0;         /* 1=堆达 trigger，2=gcstress；供 STATS 归因 */
        if (stress > 0) {
            /* gcstress：每 N 次分配就推动一次 GC。增量模式下这意味着
             * "极密集地切片"，mutator 与标记阶段大幅交错 —— 这正是
             * 检验写屏障是否完备的最强手段（漏一处屏障立刻误回收）。 */
            if ((++g_alloc_ticks % (uint64_t)stress) == 0) { want_start = 1; reason = 2; }
        } else if (g_heap_bytes >= g_gc_trigger && g_heap_bytes > 0) {
            /* 注意是 trigger 不是 goal：提前启动，给并发标记留出跑道。 */
            want_start = 1;
            reason = 1;
        }
        if (g_gc_phase == GC_MARK) {
            /* 周期在途：无论是否达阈值都推进一个切片，保证标记必然收敛。
             * 预算一律取 rt_gc_mark_budget() —— 曾经在 stress 下强制降到 1，
             * 结果标记速度远追不上分配，整轮压测只跑完 6 个周期，
             * 大部分时间卡在同一个永不终止的 MARK 里，压测形同虚设。
             * stress 只负责"触发频率"，标记速度归 MARK_BUDGET 管，两者解耦。 */
            uint64_t budget = rt_gc_mark_budget();
            uint64_t assist = rt_gc_assist_debt(bytes);
            /* assist 是**下界**不是替代：底薪 MARK_BUDGET 保证即使分配很慢
             * 标记也在推进（否则一个只分配几个小对象就转去干别的事的程序
             * 会把周期无限期挂在 MARK），assist 则在分配凶猛时按份额加码。 */
            if (assist > budget) budget = assist;
            g_gc_running = 1;
            gc_step(budget);
            g_gc_running = 0;
        } else if (g_gc_phase == GC_SWEEP) {
            /* 清扫在途：同样由分配驱动推进（Task #32 惰性清扫）。
             * 与 MARK 一样"无论是否达阈值都推进"，保证清扫必然收敛；
             * 否则一段不再分配的静默期会把半清扫状态无限期挂住。
             * 注意这里**吃掉了** want_start —— 清扫期间不启动新周期，
             * 这是刻意的：新周期的 epoch 自增会让上一轮的黑集体降级为白，
             * 未清扫区间的存活对象随即被判成垃圾。清完才允许开下一轮。 */
            g_gc_running = 1;
            gc_sweep_step(rt_gc_sweep_budget());
            g_gc_running = 0;
        } else if (want_start) {
            if (reason == 2) g_trig_stress++; else g_trig_heap++;
            g_gc_protect = protect;   /* 本轮标记必须覆盖这个"分配即触发"的对象 */
            if (rt_gc_incremental_on()) {
                g_gc_running = 1;
                gc_cycle_start();
                /* 扫根后立刻推进一个切片，避免"只扫根不干活"的空转 */
                gc_step(rt_gc_mark_budget());
                g_gc_running = 0;
            } else {
                g_collect_from_alloc = 1;
                shadow_gc_collect();             /* STW 回退路径 */
                g_collect_from_alloc = 0;
            }
        }
    }
}

/* codegen 生成的结构体分配入口。GC 调度已上移到 rt_gc_register 的
 * gc_alloc_hook —— 那里能看到全部分配量，这里只管分配和打类型。 */
extern void* shadow_gc_alloc(int32_t size, int32_t type_id) {
    void* p = __rt_shadow_malloc(size);
    if (p && type_id != 0) rt_gc_set_type(p, (uint32_t)type_id);
    return p;
}

/* ============================================================
 * 回收周期的四个阶段（拆成独立函数供增量调度复用）
 *   gc_scan_roots  —— STW 扫根（帧根 + 全局根 + 保守栈/寄存器）
 *   gc_drain       —— 排空灰队列；budget=0 表示不限量
 *   gc_run_finalizers —— 标记终止之后、清扫之前（一次性）
 *   gc_sweep_begin / gc_sweep_step —— 惰性清扫，由后续分配分片推进
 * ============================================================ */

/* 扫根（§5.2.7：STW 窗口内快照全部线程）。
 * 顺序：帧根（各线程）→ 全局根 → 永久根 → 保守栈/寄存器（§5.2.4 起可选）。
 * 整段在 gc_stw_begin/end 之间执行，其它 mutator 线程处于挂起状态。 */
/* §5.2.4：保守栈/寄存器扫描降为可选（SHADOW_GC_CONSERVATIVE=1 才开）。
 * 默认关闭 —— 精确根集（帧根 + 全局根 + shadow frame range 根）是唯一依据。
 * 保留开关用于对拍验证：开/关两模式下结果必须一致。 */
static int32_t g_conservative = -1;
static int32_t g_cons_regs = -1;
static int32_t g_cons_stack = -1;
static int rt_gc_conservative_on(void) {
    if (g_conservative < 0) {
        const char* e = getenv("SHADOW_GC_CONSERVATIVE");
        g_conservative = (e && *e && e[0] != '0') ? 1 : 0;
    }
    return g_conservative;
}
/* 诊断细分：SHADOW_GC_CONSERVATIVE_REGS / _STACK 独立控制；默认跟随主开关 */
static int rt_gc_cons_regs_on(void) {
    const char* e;
    if (g_cons_regs < 0) {
        e = getenv("SHADOW_GC_CONSERVATIVE_REGS");
        g_cons_regs = e && *e ? (e[0] != '0' ? 1 : 0) : rt_gc_conservative_on();
    }
    return g_cons_regs;
}
static int rt_gc_cons_stack_on(void) {
    const char* e;
    if (g_cons_stack < 0) {
        e = getenv("SHADOW_GC_CONSERVATIVE_STACK");
        g_cons_stack = e && *e ? (e[0] != '0' ? 1 : 0) : rt_gc_conservative_on();
    }
    return g_cons_stack;
}
static void gc_scan_roots(void) {
    uint32_t i, k;
    DWORD me = GetCurrentThreadId();
    uint32_t wl_after_roots = 0;   /* 日志用：STW 里不许 fprintf，先记数出来再打 */

    gc_stw_begin();

    /* 帧根：逐线程。每条线程有自己的帧根栈（见线程表注释）。
     * n==0：单值根，标缓存 val；n>0：range 根（shadow frame 数组），**读当前值**。 */
    for (k = 0; k < g_threads_len; k++) {
        rt_gc_thread* th = &g_threads[k];
        if (!th->in_use) continue;
        /* §5.2.4 分配根：保活"最近分配但 mutator 尚未赋值到 sf"的对象 */
        if (th->alloc_root) rt_gc_mark_ptr(th->alloc_root);
        for (i = 0; i < th->roots_len; i++) {
            rt_gc_root* r = &th->roots[i];
            if (r->n > 0) {
                uintptr_t* base = (uintptr_t*)r->slot;
                uint32_t j;
                for (j = 0; j < r->n; j++) {
                    void* v = (void*)base[j];
                    if (v) rt_gc_mark_ptr(v);
                }
            } else if (r->val) {
                rt_gc_mark_ptr(r->val);
            }
        }
    }
    /* 全局根：slot 是全局变量地址，直接读当前值最准确；缓存值一并标记兜底 */
    for (i = 0; i < g_globals_len; i++) {
        void* slot = g_globals[i].slot;
        if (slot) {
            void* cur = *(void**)slot;
            if (cur) rt_gc_mark_ptr(cur);
        }
        if (g_globals[i].val) rt_gc_mark_ptr(g_globals[i].val);
    }
    /* 永久根：正在跨线程移交、暂时无栈可证明其存活的对象 */
    for (i = 0; i < g_perm_len; i++) {
        if (g_perm[i]) rt_gc_mark_ptr(g_perm[i]);
    }
    wl_after_roots = g_wl_len;

    /* 栈根（§5.2.4 协作式安全点，对标 Go preemptible）：
     * 其它线程此刻都停在安全点 —— poll 自旋（gc_state=1）、GC 锁内 / 等锁
     * （gc_state=2）。跨安全点的指针已由编译器活跃性分析 spill 到各线程的
     * shadow frame / slot，上面的帧根扫描（含 range 根）已**精确**覆盖，
     * 不需要 SuspendThread 保守扫描 —— 保守扫描从此只在对拍验证时启用。
     *
     * SHADOW_GC_CONSERVATIVE=1（默认关）：挂起其它线程并保守扫描其栈/寄存器，
     * 与精确根集结果对照。若精确根集完备，保守扫描标到的对象应已被精确根
     * 标到（VERIFY missed=0 佐证）；两者结果一致即证明精确根集完整。
     * 挂起是安全的：线程都已停在安全点，不持 g_gc_lock、不执行 mutator。 */
    if (rt_gc_conservative_on()) {
        for (k = 0; k < g_threads_len; k++) {
            rt_gc_thread* th = &g_threads[k];
            if (!th->in_use || th->tid == me || !th->h) continue;
            if (th->stw_susp) {           /* 抢占式 STW 已挂起：直接扫 ctx 快照 */
                gc_scan_thread_suspended(th);
                continue;
            }
            if (SuspendThread(th->h) == (DWORD)-1) continue;
#ifdef _WIN64
            memset(&th->stw_ctx, 0, sizeof(CONTEXT));
            th->stw_ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            th->stw_ctx_ok = GetThreadContext(th->h, &th->stw_ctx) ? 1 : 0;
            gc_scan_thread_suspended(th);
#else
            (void)0;
#endif
            ResumeThread(th->h);
        }
    }
    if (rt_gc_cons_regs_on()) {
        rt_gc_scan_regs_only();
    }
    if (rt_gc_cons_stack_on()) {
        rt_gc_scan_stack_only();
    }

    gc_stw_end();

    /* 日志必须在 STW 之外：fprintf 走 stdio → 进程默认堆，
     * 而被挂起的 mutator 可能正持着那把锁（见 GC 私有堆一节的不变式）。 */
    if (rt_gc_debug_on()) {
        fprintf(stderr, "[GC][ph] roots marked, wl=%u\n", wl_after_roots);
        fprintf(stderr, "[GC][ph] stack scanned, wl=%u\n", g_wl_len);
    }
}

/* g_scan_parent：当前正在扫描其字段的对象 —— 供 SHADOW_GC_VERIFY 归因
 * "漏标对象的父亲是谁"。知道父亲的类型就知道哪条写路径少装了屏障。 */
static uint32_t g_scan_parent_type = 0;

/* 排空灰队列。返回本次实际扫描的对象数。
 * budget != 0 时最多扫 budget 个对象就返回（增量切片）。 */
static uint64_t gc_drain(uint64_t budget) {
    uint64_t work = 0;
    while (g_wl_len > 0) {
        void* p = g_wl[--g_wl_len];
        int32_t idx = rt_gc_ptr_lookup(p);
        rt_gc_hdr* h;
        if (idx < 0) continue;      /* 已被显式 free 并从表中摘除 */
        h = rt_gc_hdr_of(p);
        if (h->mark == g_col_black) continue;
        h->mark = g_col_black;
        g_scan_parent = p;
        g_scan_parent_type = g_objs[idx].type_id;
        rt_gc_scan_children(p, g_objs[idx].type_id, h->size);
        g_scan_parent = NULL;
        work++;
        if (budget && work >= budget) break;
    }
    return work;
}

/* ============================================================
 * 漏标检测器（SHADOW_GC_VERIFY=1，调试专用）
 * ------------------------------------------------------------
 * 增量标记的正确性判据：标记终止时的黑集合必须是"当下真实可达集合"的**超集**
 * —— 允许浮动垃圾（多留），绝不允许漏网（少留）。本检测器在标记终止后、清扫前：
 *   ① 快照增量标记得到的黑集合；
 *   ② 换 epoch 从零重来一次纯 STW 全量标记（同样的根，但所有对象重新变白，
 *      因此会**重新展开**那些增量期间已经变黑的容器）；
 *   ③ 差集 fresh \ incremental 就是"被漏标的活对象"，不修即 use-after-free。
 *
 * 为什么终止时的那次 gc_scan_roots() 抓不到它们：重扫遇到已黑的对象会直接跳过，
 * 不再展开其字段。增量期间被塞进黑容器的白对象因此永远进不了灰队列 ——
 * 这正是混合写屏障要堵的洞。把屏障关掉（SHADOW_GC_BARRIER=0）跑同一用例，
 * 这个计数会立刻从 0 变成正数，比"等它崩溃"确定得多。
 *
 * 副作用：VERIFY 模式下清扫依据的是重标记结果（真实可达集），比增量结果更紧，
 * 因此漏标对象在本轮反而会被救活 —— 检测器只报告不制造崩溃，符合诊断用途。
 * ============================================================ */
/* SHADOW_GC_VERIFY: 0/未设=关；1=只报告；2=检出漏标立即 abort（CI 用，
 * 失败点确定且自带诊断信息，不必指望恰好踩出段错误）。 */
static uint64_t g_missed_total = 0;
static int32_t g_verify = -1;
static int32_t rt_gc_verify_on(void) {
    if (g_verify < 0) {
        const char* e = getenv("SHADOW_GC_VERIFY");
        g_verify = (!e || e[0] == '0' || e[0] == '\0') ? 0 : (e[0] == '2' ? 2 : 1);
    }
    return g_verify;
}

/* 重标记必须复用终止时捕获的那份根集合，绝不能再扫一次栈：
 * 保守栈扫描对栈上残留字节极其敏感，而快照循环本身就会把成百上千个堆地址
 * 写进栈槽和寄存器；再扫一次必然"捞起"一批本轮没被标记的白对象，全是假阳性。
 * 固定根集合后两次标记起点完全一致，差集才真正等于漏标。
 *
 * §5.2.4 修正：精确根集模式下根是**稳定的**（帧根 slot 键控 + 全局根 +
 * shadow frame range 根），扫根没有"捞起白对象"的假阳性问题；且 range 根是
 * **实时读数组内容**（mutator 用纯 store 更新），快照捕获反而会拿到过期值
 * —— 捕获与重标记之间 sf 数组一旦更新，重标记就漏标。故精确模式下直接
 * 重扫根（实时），与增量标记的根集合逐字节一致。 */
static uint64_t gc_verify_marks(void) {
    unsigned char* snap;
    uint32_t i;
    uint64_t missed = 0;
    if (g_objs_len == 0) return 0;
    snap = (unsigned char*)gc_xcalloc(g_objs_len, 1);
    if (!snap) return 0;                      /* 检测器本身不该影响主流程 */
    for (i = 0; i < g_objs_len; i++)
        if (g_objs[i].data && rt_gc_hdr_of(g_objs[i].data)->mark == g_col_black)
            snap[i] = 1;

    g_verify_parent = (void**)gc_xcalloc(g_objs_len, sizeof(void*));
    g_wl_len = 0;
    gc_epoch_advance();                       /* 全堆刷白 */
    gc_scan_roots();                          /* §5.2.4：实时重扫根（精确根集无假阳性） */
    gc_drain(0);

    for (i = 0; i < g_objs_len; i++) {
        void* d = g_objs[i].data;
        if (!d) continue;
        if (rt_gc_hdr_of(d)->mark == g_col_black && !snap[i]) {
            missed++;
            if (missed <= 5) {
                /* 归因：父亲是谁、父亲什么类型 —— 直接指向缺屏障的那条写路径。
                 * parent=NULL 表示它是被根（栈/全局）直接标到的。 */
                void* par = g_verify_parent ? g_verify_parent[i] : NULL;
                int32_t pidx = par ? rt_gc_ptr_lookup(par) : -1;
                fprintf(stderr,
                        "[GC][verify] MISSED obj=%p type=%u size=%u  <- parent=%p ptype=%d\n",
                        d, (unsigned)g_objs[i].type_id, (unsigned)g_objs[i].size,
                        par, pidx >= 0 ? (int)g_objs[pidx].type_id : -1);
            }
        }
        /* §5.2.4 修正：清扫依据取「增量 ∪ 全量重扫」的并集 —— 检测器只报告，
         * 绝不比增量结果更紧地清扫。精确根集下 range 根（shadow frame）是
         * mutator 用纯 store 更新的数组，终止时的"捕获快照"会拿到过期值；
         * 重标记若因此漏掉增量标到的对象，清扫就会误杀活对象 —— 检测器
         * 自身制造崩溃，违背诊断用途。并集保证清扫至少与增量一样保守。 */
        if (snap[i]) rt_gc_hdr_of(d)->mark = g_col_black;
    }
    gc_xfree(g_verify_parent);
    g_verify_parent = NULL;
    gc_xfree(snap);
    g_missed_total += missed;
    if (missed) {
        fprintf(stderr, "[GC][verify] cycle #%u missed=%llu (total=%llu)\n",
                g_gc_count, (unsigned long long)missed,
                (unsigned long long)g_missed_total);
        if (g_verify >= 2) {
            fprintf(stderr, "[GC][verify] FATAL: incremental mark missed live "
                            "objects — write barrier is incomplete\n");
            fflush(stderr);
            abort();
        }
    }
    return missed;
}

/* 完整标记-清扫（STW 路径；增量路径见 shadow_gc_step） */
/* 周期计时/统计（增量下一个周期跨越多次分配，须跨调用累计） */
static uint64_t g_cycle_t0 = 0;
static uint64_t g_cycle_mark_ms = 0;
static uint64_t g_cycle_slices = 0;
static uint64_t g_cycle_marked = 0;

/* ============================================================
 * pacing 控制器 + GC assist（Task #33，§5.2.5）
 * ============================================================ */
static int rt_gc_gogc(void);

/* SHADOW_GC_ASSIST=0 关闭 assist（保留 pacing 的提前触发）。
 * 用于 A/B 自证：关掉后若堆峰值明显击穿 goal，就证明 assist 在承重。 */
static int32_t g_assist_off = -1;
static int32_t rt_gc_assist_off(void) {
    if (g_assist_off < 0) {
        const char* e = getenv("SHADOW_GC_ASSIST");
        g_assist_off = (e && e[0] == '0') ? 1 : 0;
    }
    return g_assist_off;
}

/* SHADOW_GC_PACING=0 关闭自适应，trigger 固定在 RT_GC_TRIGGER_PCT_INIT。 */
static int32_t g_pacing_off = -1;
static int32_t rt_gc_pacing_off(void) {
    if (g_pacing_off < 0) {
        const char* e = getenv("SHADOW_GC_PACING");
        g_pacing_off = (e && e[0] == '0') ? 1 : 0;
    }
    return g_pacing_off;
}

/* 周期启动时结算 assist 比率。
 *
 * 目标：在堆从 heap_at_start 涨到 heap_goal 这段跑道内，把估计的
 * scan_est 个对象全部扫完。于是
 *
 *     ratio = scan_est / (goal - heap_at_start)     [对象/字节]
 *
 * scan_est 用上一轮实际标记数估计（Go 同样用上一轮的 scan work 做估计）：
 * 存活集在相邻周期间通常变化平缓，这个估计足够准；估偏了也无妨——
 * pacing 控制器下一轮会用实测误差把 trigger 拉回来。 */
static void gc_pacing_cycle_start(void) {
    uint64_t runway;
    uint64_t scan_est;
    g_heap_at_start = g_heap_bytes;
    /* 周期峰值从启动点重新起算。GC_OFF 间隙里的堆一定低于 trigger（否则
     * 早就触发了），而 trigger < goal，故间隙不可能产生峰值，无需覆盖。 */
    g_cycle_peak = g_heap_bytes;
    g_assist_debt = 0.0;
    g_assist_work = 0;
    g_assist_calls = 0;
    /* goal 必须 ≥ 当前堆，否则跑道为负。stress 模式下周期是被强行触发的，
     * 堆可能远没到 goal，也可能因为上一轮 goal 结算得低而已经越过。 */
    if (g_heap_goal > g_heap_at_start) {
        runway = g_heap_goal - g_heap_at_start;
    } else {
        /* 已越过 goal：跑道按最小堆的一小段给，assist 会因此变得很重，
         * 迫使标记尽快收敛 —— 这正是超标时该有的行为。 */
        runway = RT_GC_MIN_HEAP / 8;
    }
    scan_est = g_last_marked ? g_last_marked : (uint64_t)g_objs_len;
    g_assist_ratio = runway ? ((double)scan_est / (double)runway) : 0.0;
}

/* 分配 bytes 字节时应偿还的标记债（对象数）。
 *
 * 债务是**累积**的：单次小分配算出来的份额往往不足 1 个对象，直接取整
 * 会恒为 0，assist 就完全失效了。累积到 ≥1 才实际扣减并返回，
 * 等价于"攒够一个对象的工作量再干"，长期比率精确。 */
static uint64_t rt_gc_assist_debt(uint64_t bytes) {
    uint64_t work;
    if (rt_gc_assist_off() || g_assist_ratio <= 0.0) return 0;
    g_assist_debt += (double)bytes * g_assist_ratio;
    if (g_assist_debt < 1.0) return 0;
    work = (uint64_t)g_assist_debt;
    g_assist_debt -= (double)work;
    g_assist_work += work;
    g_assist_calls++;
    return work;
}

/* 标记终止时调用：留档本轮标记量（下一轮的扫描量估计）。 */
static void gc_pacing_mark_done(void) {
    g_heap_at_mark_done = g_heap_bytes;
    g_last_marked = g_cycle_marked;
}

/* 清扫走完、live 精确已知时调用：跑控制器 + 重算 goal 与 trigger。
 *
 * 控制误差取**整个周期的堆峰值** vs 本轮 goal。
 * 早先用的是"标记终止那一刻的堆位置"，两者通常接近，但清扫窗口里
 * mutator 还在分配，峰值可能出现在清扫期而不是标记终止点，用后者会低估。
 *
 *   · 峰值超过 goal  → 跑道不够长，标记启动太晚 → trigger 前移；
 *   · 峰值远低于 goal → 跑道太长白跑了 GC → trigger 后移。
 *
 * 两侧都用比例响应。曾经欠标侧固定 +5/轮，结果在大 GOGC 下爬不动：
 * GOGC=400 时 goal=5×live，标记只需几十次分配就跑完，峰值几乎就停在
 * trigger 上（peak≈trigger），必须把 trigger_pct 一路推到上限才贴得住
 * goal；而程序总共只跑 4 个周期，+5/轮从 70% 只能爬到 85%，实测比
 * 3.80 对目标 5.00 欠了 24%。改成比例后一轮就能跨十几个点。
 *
 * 死区刻意做成不对称（-5% ~ +10%）：宁可稳定地略微欠标，也不要在
 * goal 上方反复试探 —— 超标是内存承诺失效，欠标只是多花点 CPU。 */
static void gc_pacing_settle(uint64_t live) {
    if (!rt_gc_pacing_off() && g_heap_goal > 0) {
        uint64_t observed = g_cycle_peak ? g_cycle_peak : g_heap_at_mark_done;
        int64_t err_pct = (int64_t)((observed * 100) / g_heap_goal) - 100;
        if (err_pct > 10) {
            /* 超标：超得越多退得越狠，但每轮最多退 15 个点，免得一次
             * 异常尖峰把 trigger 打到地板、之后每次分配都在跑 GC。 */
            int32_t step = (int32_t)(err_pct / 2);
            if (step > 15) step = 15;
            g_trigger_pct -= step;
        } else if (err_pct < -5) {
            int32_t step = (int32_t)((-err_pct) / 3);
            if (step < 1) step = 1;
            if (step > 12) step = 12;
            g_trigger_pct += step;
        }
        if (g_trigger_pct < RT_GC_TRIGGER_PCT_MIN) g_trigger_pct = RT_GC_TRIGGER_PCT_MIN;
        if (g_trigger_pct > RT_GC_TRIGGER_PCT_MAX) g_trigger_pct = RT_GC_TRIGGER_PCT_MAX;
    }
    g_heap_goal = live + live * (uint64_t)rt_gc_gogc() / 100;
    if (g_heap_goal < RT_GC_MIN_HEAP) g_heap_goal = RT_GC_MIN_HEAP;
    /* trigger 在 [live, goal] 区间内按 g_trigger_pct 取点。基准取 live 而非 0：
     * 存活堆是无论如何都占着的，不该算进"跑道"里。 */
    g_gc_trigger = live + (g_heap_goal - live) * (uint64_t)g_trigger_pct / 100;
    if (g_gc_trigger < RT_GC_MIN_HEAP / 4) g_gc_trigger = RT_GC_MIN_HEAP / 4;
    if (g_gc_trigger > g_heap_goal) g_gc_trigger = g_heap_goal;
    g_next_gc = g_heap_goal;   /* 对外仍以 goal 作为"下次 GC 阈值"展示 */
}

/* ── 周期启动：STW 扫根，进入 MARK。此后 mutator 可继续运行。 ── */
static void gc_cycle_start(void) {
    uint64_t t;
    uint64_t tus;
    uint64_t dus;
    /* 硬前置条件：上一轮的惰性清扫必须先做完（Task #32）。
     * 下面 gc_epoch_advance() 一执行，上一轮的黑就整体降级为本轮的白，
     * 待清扫区间里的**存活**对象会立刻失去"我还活着"的凭据。
     * 正常情况下清扫早已被分配驱动跑完，这里只是兜底（例如清扫途中
     * 有人显式调用 gc()，或分配停滞后突然越过堆阈值）。 */
    if (g_gc_phase == GC_SWEEP) gc_sweep_step(0);
    /* 攒批登记刷新：周期启动前把 g_pend 全部入表。必须在 epoch 刷白
     * （gc_epoch_advance）之前 —— 攒批对象此刻已赋值到帧根，刷白后由
     * 标记阶段照常扫根置黑；若拖到刷白之后才入表，它们会带着旧 mark
     * 直接落入"待清扫区间"，存活对象会被误回收。持锁调用（本函数
     * 的两个调用点 gc_alloc_hook / shadow_gc_collect 均持 g_gc_lock）。 */
    rt_gc_flush_pending_locked();
    t = GetTickCount64();
    tus = gc_now_us();
    if (rt_gc_log_on())
        fprintf(stderr, "[GC] start #%u heap=%llu roots=%u objs=%u thr=%u\n",
                g_gc_count, (unsigned long long)g_heap_bytes,
                (unsigned)(t_self ? t_self->roots_len : 0), (unsigned)g_objs_len,
                (unsigned)g_threads_len);
    g_cycle_t0 = t;
    g_cycle_mark_ms = 0;
    g_cycle_slices = 0;
    g_cycle_marked = 0;
    gc_pacing_cycle_start();
    /* epoch 自增即"全堆刷白"，无需遍历对象表复位 mark */
    g_wl_len = 0;
    gc_epoch_advance();
    g_gc_phase = GC_MARK;
    shadow_gc_barrier_on = 1;
    /* §5.2.4：触发本轮 GC 的分配对象在此置灰 —— epoch 已更新，
     * 置灰在本轮有效；它此刻正位于分配调用栈上，任何精确根都看不见。 */
    if (g_gc_protect) {
        rt_gc_mark_ptr(g_gc_protect);
        g_gc_protect = NULL;
    }
    gc_scan_roots();
    g_cycle_mark_ms += GetTickCount64() - t;
    /* §5.2.2 并发标记：扫根后唤醒 mark worker，与 mutator 并行排空灰队列。
     * done 标志清零必须在 go=1 之前 —— worker 若带着上轮残留的 done=1
     * 醒来，会立即收尾而不干活。 */
    InterlockedExchange(&g_mark_done_req, 0);
    gc_mark_workers_ensure();
    InterlockedExchange(&g_mark_go, 1);
    /* 扫根是不可打断的 STW 段，必须计入停顿，否则「最坏停顿」会漏掉每轮
     * 周期开头这一刀。 */
    dus = gc_now_us() - tus;
    g_tot_mark_us += dus;
    if (dus > g_max_term_us) g_max_term_us = dus;
}

static void gc_run_finalizers(void);
static void gc_sweep_begin(void);
static int32_t rt_gc_lazy_sweep_on(void);

/* ── 标记终止：排空灰队列 → 跑 finalizer → 移交清扫 ──
 * §5.2.2 并发标记下终止协议（对标 Go 的 mark termination）：
 *   ① 置 g_mark_done_req=1 —— mark worker 排空到队列空后自行收尾停靠；
 *   ② 协作 STW（gc_scan_roots 内部）等所有线程就位：mutator 在 poll、
 *      worker 已停（队列空 && done）；
 *   ③ 主线程排空剩余灰队列（此刻无并发 push，排空即终止）；
 *   ④ 清 g_mark_go —— worker 回空闲等下一周期。
 * 标记终止的重扫根：增量期间 mutator 在栈上创建的新引用，混合写屏障管不到
 * （栈槽写没有屏障）；协作式精确根集（sf/slot 在 def 时 spill）保证 poll 点
 * 的根集是当前值，重扫一次即为最终状态。
 *
 * 清扫不再在此处完成（Task #32）：本函数只把周期推进到 GC_SWEEP 并
 * 冻结待清扫区间，随后立即把控制权交还 mutator —— 由后台清扫线程与
 * 分配驱动分片清扫互补推进。需要"回收数"的调用方请走 shadow_gc_collect：
 * 它会同步排空清扫。 */
static void gc_cycle_finish(void) {
    uint64_t tm = GetTickCount64();
    uint64_t tus = gc_now_us();
    uint64_t dus;

    /* ① 请求 mark worker 收尾 */
    InterlockedExchange(&g_mark_done_req, 1);

    if (rt_gc_verify_on()) {
        /* 捕获这一次扫根经手的全部根指针，重标记复用同一份，
         * 保证两次标记起点逐字节一致（详见 gc_verify_marks 注释）。 */
        g_verify_roots_len = 0;
        g_capturing_roots = 1;
        gc_scan_roots();
        g_capturing_roots = 0;
    } else {
        gc_scan_roots();
    }
    gc_drain(0);
    if (rt_gc_verify_on()) gc_verify_marks();
    /* ④ worker 回空闲（须在 STW 放行之后：worker 看到 go=0 才退出停靠） */
    InterlockedExchange(&g_mark_go, 0);
    g_gc_phase = GC_SWEEP;
    /* ⚠️ §5.2.6：写屏障保持开启直到 GC_OFF —— SWEEP 期 mutator 仍可能把
     * 浮动垃圾重新写入活对象，屏障 shade 后由清扫路径的 gc_drain 置黑保活。
     * 提前关闭（旧实现）会让这些对象被清扫 free → use-after-free。 */
    g_cycle_mark_ms += GetTickCount64() - tm;
    /* 标记终止：重扫根 + 排空灰队列，是整个增量 GC 中**最长的一段不可打断
     * 工作**，也是 §5.3 第 2 条要与 0.3 全 STW 对比的那个量。单独计量，
     * 不与普通切片混在一起 —— 混在一起的话最大值永远是它，切片是否异常
     * 变长就被掩盖了。 */
    dus = gc_now_us() - tus;
    g_tot_mark_us += dus;
    if (dus > g_max_term_us) g_max_term_us = dus;
    /* 此刻的堆位置就是本轮峰值（之后只会被清扫拉低），是 pacing 的控制误差 */
    gc_pacing_mark_done();
    if (rt_gc_debug_on()) fprintf(stderr, "[GC][ph] mark done\n");

    /* finalizer 仍然一次性跑完：它执行的是用户回调，拖到清扫切片里执行
     * 会让「对象已死但 finalizer 未跑」的窗口横跨任意长的 mutator 时间，
     * 语义难以推理。而 finalizer 表通常只有寥寥数项，代价可忽略。 */
    {
        uint64_t tf = gc_now_us();
        gc_run_finalizers();
        g_tot_fin_us += gc_now_us() - tf;
    }

    gc_sweep_begin();
    /* §5.2.6 后台清扫：唤醒 sweep worker，与 mutator 分配驱动惰性清扫互补。
     * A/B 对照开关：关掉惰性清扫即退回 v1 的一次性整表清扫。 */
    if (rt_gc_lazy_sweep_on()) {
        gc_sweep_worker_ensure();
        InterlockedExchange(&g_sweep_go, 1);
    } else {
        gc_sweep_step(0);
    }
}

/* 完整 STW 标记-清扫：启动 + 一次性排空 + 终止。
 * 增量关闭时（SHADOW_GC_INCREMENTAL=0）走这条路径；也用于显式 gc() 调用。 */
/* 无预算排空 + 计时。STW 路径（显式 gc() 或 SHADOW_GC_INCREMENTAL=0）的
 * 主标记全部发生在这里，而它原先没有任何计时 —— 结果 STW 模式跑出来的
 * "最坏停顿"比增量模式还小，A/B 对照完全失真。这个 helper 把它按"一个
 * 无限预算的切片"如实计入。 */
static uint64_t gc_drain_timed(void) {
    uint64_t tus = gc_now_us();
    uint64_t work = gc_drain(0);
    uint64_t dus = gc_now_us() - tus;
    g_tot_mark_us += dus;
    g_tot_mark_slices++;
    g_tot_marked += work;
    if (dus > g_max_mark_slice_us) g_max_mark_slice_us = dus;
    return work;
}

extern int64_t shadow_gc_collect(void) {
    int64_t dead;
    GC_LOCK();
    if (g_gc_disabled || g_gc_running) { GC_UNLOCK(); return 0; }
    /* 触发归因：本函数有两个来源 —— 用户显式 gc()，以及关掉增量时由
     * gc_alloc_hook 走的 STW 回退路径。后者的原因（heap/stress）已在
     * 钩子里记过，这里再记一次就会双计，故用标志区分。 */
    if (!g_collect_from_alloc) g_trig_explicit++;
    g_gc_running = 1;
    /* 上一轮的惰性清扫若还没走完，先排空 —— 显式 gc() 的语义是
     * "返回时该回收的都已回收"，留着半截清扫不符合调用者预期。 */
    if (g_gc_phase == GC_SWEEP) gc_sweep_step(0);
    if (g_gc_phase == GC_MARK) {
        /* 已有增量周期在途：直接排空并终止，语义上等价于"立即完成本轮" */
        gc_drain_timed();
        gc_cycle_finish();
    } else {
        gc_cycle_start();
        g_cycle_marked += gc_drain_timed();
        gc_cycle_finish();
    }
    gc_sweep_step(0);              /* 同步清扫到底 */
    dead = (int64_t)g_sweep_dead;  /* 清扫收尾后仍保留本轮统计 */
    g_gc_running = 0;
    GC_UNLOCK();
    return dead;
}

/* ── Finalizers（对将死对象执行；执行后仍回收，不支持复活） ──
     *
     * ⚠️ 历史 bug（2026-08-04 修复）：旧实现在 sweep 之后无条件 `g_fins_len = 0`，
     *    把**存活对象**的 finalizer 也一并抹掉 —— 该对象日后真正死亡时已无登记，
     *    finalizer 永不执行。GC 频率越高越严重：SHADOW_GC_STRESS=1 时对象几乎
     *    总在栈上存活，每次 GC 都清表，最终 drops=0（本该 ~69 万次）。
     *
     *    正确做法：本轮只丢弃"已触发"和"对象已消失"的条目，存活对象的登记必须
     *    原样保留到下一轮。这里用读写双指针就地压缩。 */
static void gc_run_finalizers(void) {
    uint32_t i;
    {
        uint32_t w = 0;
        for (i = 0; i < g_fins_len; i++) {
            void* d = g_fins[i].data;
            if (!d) continue;                       /* 空槽：丢弃 */
            if (rt_gc_ptr_lookup(d) < 0) continue;  /* 对象已不在对象表：丢弃 */
            if (rt_gc_hdr_of(d)->mark != g_col_black) {
                /* 将死：执行 finalizer（一次性），随后由 sweep 回收 → 丢弃条目 */
                if (rt_gc_log_on())
                    fprintf(stderr, "[GC] finalizer %p\n", d);
                if (g_fins[i].fn) g_fins[i].fn(g_fins[i].data2 ? g_fins[i].data2 : d);
                continue;
            }
            /* 存活：保留登记，等待它真正死亡的那一轮 */
            if (w != i) g_fins[w] = g_fins[i];
            w++;
        }
        g_fins_len = w;
    }
}

/* ============================================================
 * 惰性清扫（Task #32）
 * ------------------------------------------------------------
 * v1 的清扫是标记终止后一次性遍历整张对象表：堆里对象越多，这段停顿越长，
 * 而且这段时间干的活 mutator 一点也用不上 —— 纯粹的暂停。
 * v2 把它拆成由分配驱动的切片：标记终止只冻结「待清扫区间」并立刻返回，
 * 此后每次分配顺带清扫固定数量的槽位，直到区间走完。
 *
 * 正确性由三条支撑：
 *   1) 清扫判据是 `mark != g_col_black`，而 g_col_black 带**当前 epoch**。
 *      只要在下一次 gc_epoch_advance() 之前清完，判据就不会串味 ——
 *      这正是 epoch 着色（见文件头注释）替惰性清扫铺好的路。
 *   2) gc_cycle_start 会强制清完残余，保证 1) 的前提永远成立；
 *      且 GC_SWEEP 期间分配路径不启动新周期，只推进清扫。
 *   3) 清扫区间在标记终止时冻结为 [0, objs_len)。此后新分配的对象要么
 *      落在区间之外（追加到表尾），要么复用区间内的空槽 —— 后者靠
 *      「分配即黑」免疫（见 rt_gc_register），两条路都不会被误杀。
 *
 * 关掉（SHADOW_GC_LAZY_SWEEP=0）即退回 v1 的一次性清扫，用于 A/B 对照。
 * （游标状态变量 g_sweep_* 定义在文件前部的 GC 阶段小节）
 * ============================================================ */
static int32_t g_lazy_sweep = -1;
static int32_t rt_gc_lazy_sweep_on(void) {
    if (g_lazy_sweep < 0) {
        const char* e = getenv("SHADOW_GC_LAZY_SWEEP");
        g_lazy_sweep = (e && e[0] == '0') ? 0 : 1;   /* 默认开启 */
    }
    return g_lazy_sweep;
}
/* 每次分配清扫的槽位数。
 *
 * ⚠️ 固定预算是个陷阱（实测踩到过）：清扫的推进由分配驱动，走完一轮需要
 * `objs_len / budget` 次分配。对象表一大（strings 用例有 15 万槽），
 * 256 的固定预算就要 589 次分配才清得完 —— 而清扫窗口期间**不允许启动
 * 新周期**，于是 GC 事实上停摆，堆一路膨胀到 39MB，整个程序只跑完 1 轮
 * （对照组一次性清扫跑了 55 轮），总耗时反而劣化 71%。
 *
 * 正解是让预算随剩余工作量伸缩：无论对象表多大，整轮清扫都在大约
 * RT_GC_SWEEP_STEPS 次分配内走完，清扫窗口长度因而恒定。
 * 单次停顿仍被摊薄 RT_GC_SWEEP_STEPS 倍，这正是惰性清扫要的效果。
 *
 * 显式设置 SHADOW_GC_SWEEP_BUDGET=N 时改用固定值 N 且不做 pacing ——
 * 压测要的就是"把清扫窗口人为拉到最长"（N=1 时窗口跨越十万次分配），
 * 那是检验"窗口内新分配会不会被误杀"的最强手段。 */
#define RT_GC_SWEEP_STEPS 64u
static int32_t g_sweep_budget = -1;   /* >0：用户显式固定值；0：自适应 */
static uint64_t rt_gc_sweep_budget(void) {
    uint64_t paced;
    if (g_sweep_budget < 0) {
        const char* e = getenv("SHADOW_GC_SWEEP_BUDGET");
        int v = e ? atoi(e) : 0;
        g_sweep_budget = (v > 0) ? v : 0;
    }
    if (g_sweep_budget > 0) return (uint64_t)g_sweep_budget;
    /* 基数必须是本轮**总**槽数，不能是剩余槽数：按剩余量算等于每步清掉
     * 剩下的 1/STEPS，是几何衰减而非匀速，实测要 301 步才收敛（远超 64）。 */
    paced = (uint64_t)g_sweep_end / RT_GC_SWEEP_STEPS + 1;
    return paced < 256u ? 256u : paced;
}

static int rt_gc_gogc(void) {
    const char* e = getenv("SHADOW_GOGC");
    int gogc = e ? atoi(e) : RT_GC_DEFAULT_GOGC;
    if (gogc <= 0) gogc = RT_GC_DEFAULT_GOGC;
    return gogc;
}

/* 标记终止后调用：冻结清扫区间，进入惰性清扫窗口 */
static void gc_sweep_begin(void) {
    g_sweep_cursor = 0;
    g_sweep_end = g_objs_len;
    g_sweep_dead = 0;
    g_sweep_ms = 0;
    g_sweep_slices = 0;
    g_sweep_max_work = 0;
    g_live_bytes = 0;   /* 由 gc_sweep_step 边扫边累加存活字节 */
    /* 清扫期间的临时阈值：此刻一个字节都还没释放，g_heap_bytes 是存活量的
     * 上界。若放任 g_next_gc 停在触发本轮 GC 的低位，接下来每次分配都会
     * 算出 want_start=1 —— 虽然 SWEEP 期不会真的开新周期（只推进清扫），
     * 但清扫一收尾就会立刻空转再触发一轮。先用上界顶住，
     * gc_sweep_complete 再用精确 live_bytes 下调。 */
    {
        uint64_t est = g_heap_bytes + g_heap_bytes * (uint64_t)rt_gc_gogc() / 100;
        if (est > g_gc_trigger) g_gc_trigger = est;
        if (est > g_next_gc) g_next_gc = est;
    }
    if (rt_gc_debug_on())
        fprintf(stderr, "[GC][ph] fin done, sweep start objs=%u\n", g_sweep_end);
}

/* 清扫走完：结算阈值、打日志、关闭周期 */
static void gc_sweep_complete(void) {
    gc_pacing_settle(g_live_bytes);   /* 重算 goal 与 trigger（Task #33） */
    g_gc_freed += g_sweep_dead;
    /* 攒批收尾：把各 span 残留的待归还缓冲一次性刷掉（持 GC 锁，安全） */
    extern void rt_mem_free_span_flush_all(void);
    rt_mem_free_span_flush_all();
    /* 墓碑不在此重建：新对象复用死对象地址（freelist），其插入会沿同链
     * 消费墓碑，表保持 ~46% 负载；仅在插入时占用超阈值才按需重建
     * （rt_gc_ht_insert），避免每周期一次 3MB 表分配/释放。 */
    /* 归档到进程级累计：周期变量马上要被下一轮清零（§5.2.10） */
    g_live_last = g_live_bytes;
    g_tot_assist_work += g_assist_work;
    g_tot_assist_calls += g_assist_calls;
    if (rt_gc_log_on())
        fprintf(stderr, "[GC] done #%u freed=%u heap=%llu next=%llu "
                        "live=%llu goal=%llu trig=%llu(%d%%) peak=%llu mkdone=%llu "
                        "assist=%llu/%llu mark=%llums sweep=%llums "
                        "slices=%llu/%llu swept=%llu/%llu marked=%llu wb=%llu/%llu missed=%llu "
                        "bad_array=%llu bad_slot=%llu\n",
                g_gc_count, g_sweep_dead, (unsigned long long)g_heap_bytes,
                (unsigned long long)g_next_gc,
                (unsigned long long)g_live_bytes,   /* 本轮精确存活字节 */
                (unsigned long long)g_heap_goal,    /* 目标峰值 = live*(1+GOGC/100) */
                (unsigned long long)g_gc_trigger, g_trigger_pct,  /* 标记启动点 */
                (unsigned long long)g_heap_peak,    /* 全程堆峰值（§5.3-3） */
                (unsigned long long)g_heap_at_mark_done, /* 标记终止时的堆＝本轮峰值 */
                (unsigned long long)g_assist_work,  /* assist 完成的标记量 */
                (unsigned long long)g_assist_calls,
                (unsigned long long)g_cycle_mark_ms, (unsigned long long)g_sweep_ms,
                (unsigned long long)g_cycle_slices,   /* 标记切片数 */
                (unsigned long long)g_sweep_slices,   /* 清扫切片数 */
                (unsigned long long)g_sweep_max_work, /* 单切片最大槽数 */
                (unsigned long long)g_sweep_end,      /* 本轮清扫总槽数 */
                (unsigned long long)g_cycle_marked,
                (unsigned long long)g_wb_shaded,   /* 屏障真正保活的对象数 */
                (unsigned long long)g_wb_calls,
                (unsigned long long)g_missed_total,/* SHADOW_GC_VERIFY 检出的漏标累计 */
                (unsigned long long)g_bad_array,
                (unsigned long long)g_bad_slot);
    g_live_bytes = 0;
    g_gc_count++;
    g_gc_phase = GC_OFF;
    shadow_gc_barrier_on = 0;
    /* §5.2.6：清扫走完，后台 sweep worker 回空闲（它醒来见 go=0 即停） */
    InterlockedExchange(&g_sweep_go, 0);
}

/* 推进一个清扫切片；budget=0 表示不限量（清到底）。返回本次回收对象数。
 * 调用方需持有 g_gc_lock。
 *
 * §5.2.6：切片开头先排空灰队列（gc_drain(0)）—— SWEEP 期写屏障仍在工作，
 * mutator 可能把浮动垃圾重新写入活对象；它们被 shade 置灰后必须在此置黑
 * （含子对象），sweep 判黑存活，否则会被本切片误 free。空队列时零开销。 */
static uint32_t gc_sweep_step(uint64_t budget) {
    uint64_t t;
    uint64_t tus;
    uint64_t dus;
    uint64_t work = 0;
    uint32_t dead = 0;
    if (g_gc_phase != GC_SWEEP) return 0;
    gc_drain(0);                       /* SWEEP 期屏障 shade 的对象置黑 */
    t = GetTickCount64();
    tus = gc_now_us();
    while (g_sweep_cursor < g_sweep_end) {
        uint32_t i;
        uint32_t hj;
        void* d;
        if (budget && work >= budget) break;
        work++;
        i = g_sweep_cursor++;
        d = g_objs[i].data;
        if (!d) continue;
        /* 一致性校验：对象表槽 i 必须与 hash 表指向的槽一致。
         * 不一致 = 同一地址被重复登记（hash 插入失败/erase 漏删），
         * 若照常 free 会造成 double free → freelist 成环 → 两次分配返回同一块 → 堆损坏。
         * 登记时已把哈希槽位存进对象表（ht_slot），此处 O(1) 校验即可；
         * 异常（重建遗漏/重复登记）才走慢路径全表查找，保持原坏槽检测语义。 */
        hj = g_objs[i].ht_slot;
        if (hj >= g_ht_cap || g_ht_keys[hj] != d) {
            int32_t vidx = rt_gc_ptr_lookup_pos(d, &hj);
            if (vidx != (int32_t)i) {
                g_bad_slot++;
                if (rt_gc_debug_on())
                    fprintf(stderr, "[GC][BAD-SLOT] i=%u data=%p ht_idx=%d size=%u type=%u\n",
                            i, d, vidx, g_objs[i].size, g_objs[i].type_id);
                g_objs[i].data = NULL;     /* 摘除幽灵表项，绝不 free */
                continue;
            }
        }
        if (rt_gc_hdr_of(d)->mark != g_col_black) {
            rt_gc_sweep_free(d, i, hj);   /* 已知槽位：跳过查找与重入锁 */
            dead++;
        } else {
            /* 无需复位 mark：下一轮 epoch 自增后，本轮的黑自动变成白 */
            g_live_bytes += (uint64_t)rt_gc_hdr_of(d)->size + RT_GC_HEADER;
        }
    }
    g_sweep_dead += dead;
    g_sweep_slices++;
    if (work > g_sweep_max_work) g_sweep_max_work = work;
    g_sweep_ms += GetTickCount64() - t;
    dus = gc_now_us() - tus;
    g_tot_sweep_us += dus;
    g_tot_sweep_slices++;
    if (dus > g_max_sweep_slice_us) g_max_sweep_slice_us = dus;
    /* 注意：finalizer 表已在标记后压缩（保留存活项），此处不得再整表清空 */
    if (g_sweep_cursor >= g_sweep_end) gc_sweep_complete();
    return dead;
}

/* ============================================================
 * 增量标记调度（Task #31）
 * ------------------------------------------------------------
 * 单线程下"并发"体现为**分配驱动的标记切片**：
 *   堆达阈值 → STW 扫根启动周期 → 返回 mutator
 *   此后每次分配推进固定预算的标记工作 → 灰队列耗尽 → 短 STW 标记终止 + 清扫
 * 与 STW 相比，单次停顿从"整堆标记"降为"扫根"或"一个切片"。
 * 正确性完全依赖混合写屏障（见 shadow_gc_barrier_slot）：
 * 切片之间 mutator 会任意改动对象图，没有屏障必然漏标。
 * ============================================================ */
static int32_t g_incremental = -1;
static int32_t rt_gc_incremental_on(void) {
    if (g_incremental < 0) {
        const char* e = getenv("SHADOW_GC_INCREMENTAL");
        g_incremental = (e && e[0] == '0') ? 0 : 1;   /* 默认开启，'0' 退回 STW */
    }
    return g_incremental;
}
/* 每次分配推进的标记预算（扫描对象数）。Task #33 会换成按分配速率计算的
 * assist 份额；此处先用固定值，足以验证屏障与切片的正确性。 */
static int32_t g_mark_budget = -1;
static uint64_t rt_gc_mark_budget(void) {
    if (g_mark_budget < 0) {
        const char* e = getenv("SHADOW_GC_MARK_BUDGET");
        int v = e ? atoi(e) : 0;
        g_mark_budget = (v > 0) ? v : 128;
    }
    return (uint64_t)g_mark_budget;
}

/* 推进一个标记切片；灰队列空则完成本轮。调用方需持有 g_gc_running。 */
static void gc_step(uint64_t budget) {
    uint64_t t = GetTickCount64();
    uint64_t tus = gc_now_us();
    uint64_t work = gc_drain(budget);
    uint64_t dus = gc_now_us() - tus;
    g_cycle_marked += work;
    g_cycle_slices++;
    g_cycle_mark_ms += GetTickCount64() - t;
    /* 切片停顿是 mutator 实际被打断的时长 —— §5.3 第 2 条的直接被测量。
     * 取最大值而非平均：验收关心的是最坏一次有多长。 */
    g_tot_mark_us += dus;
    g_tot_mark_slices++;
    g_tot_marked += work;
    if (dus > g_max_mark_slice_us) g_max_mark_slice_us = dus;
    if (g_wl_len == 0) gc_cycle_finish();
}

extern int64_t shadow_gc_minor_collect(void) {
    return shadow_gc_collect();   /* v1 无分代，minor == major */
}

/* ---------------- 开关与统计 ---------------- */
extern void shadow_gc_disable(void) {
    const char* force = getenv("SHADOW_GC_FORCE");
    if (force && force[0] == '1') return;
    g_gc_disabled = 1;
}
extern void shadow_gc_enable(void) {
    g_gc_disabled = 0;
    g_next_gc = RT_GC_MIN_HEAP;
}
extern int64_t shadow_gc_live_objects(void) {
    return (int64_t)g_objs_len;
}
extern int64_t shadow_gc_alloc_count(void) {
    return (int64_t)g_heap_bytes;
}

/* ---------------- SHADOW_GC_STATS：进程退出汇总（§5.2.10） ----------------
 *
 * 对标 Go 的 `GODEBUG=gctrace=1` + MemStats。与逐周期的 `[GC] done` 日志
 * 分工明确：
 *   [GC] done    这一轮发生了什么 —— 调参、看单轮行为用；
 *   [GC][stats]  整个进程跑完的总账 —— 验收用。
 *
 * 为什么验收必须要有总账：§5.3 的四条量化标准里，有三条单看某一轮日志都
 * 答不上来 ——「最坏停顿多长」要跨周期取 max，「堆峰值是否贴合 GOGC」要
 * 全程 peak，「长驻内存是否单调增长」更是只有把首尾 live 摆在一起才看得出。
 * 逐轮日志在长驻场景下还会刷出几万行，人和脚本都得先做一遍聚合。
 *
 * 输出为 key=value，便于 tools 下的验收脚本直接解析；括号内的可读单位仅供
 * 肉眼扫读，工具一律取等号后的原始字节/微秒整数值。 */
static int32_t rt_gc_barrier_off(void);   /* 定义见「混合写屏障」小节 */

static int32_t rt_gc_stats_on(void) {
    if (g_stats_on < 0) {
        const char* e = getenv("SHADOW_GC_STATS");
        g_stats_on = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return g_stats_on;
}

#define GC_MIB(x) ((double)(x) / 1048576.0)

static void gc_stats_dump(void) {
    uint64_t tot_us;
    if (!rt_gc_stats_on()) return;
    tot_us = g_tot_mark_us + g_tot_sweep_us + g_tot_fin_us;
    fprintf(stderr,
        "[GC][stats] ---- shadow GC summary ----\n"
        "[GC][stats] cfg cycles=%u gogc=%d incremental=%d lazy_sweep=%d "
        "assist=%d pacing=%d barrier=%d\n"
        "[GC][stats] heap peak=%llu (%.2fMiB) live_last=%llu (%.2fMiB) "
        "goal_last=%llu trigger_last=%llu trigger_pct=%d now=%llu\n"
        "[GC][stats] objs registered=%llu unregistered=%llu freed_by_gc=%u "
        "live_slots=%u\n"
        "[GC][stats] time mark_us=%llu sweep_us=%llu fin_us=%llu total_us=%llu "
        "(%.1fms)\n"
        "[GC][stats] pause max_mark_slice_us=%llu max_mark_term_us=%llu "
        "max_sweep_slice_us=%llu\n"
        "[GC][stats] work mark_slices=%llu marked=%llu sweep_slices=%llu "
        "assist_work=%llu assist_calls=%llu\n"
        "[GC][stats] barrier calls=%llu shaded=%llu\n"
        "[GC][stats] trigger heap=%llu explicit=%llu stress=%llu\n"
        "[GC][stats] health missed=%llu bad_array=%llu bad_slot=%llu\n",
        g_gc_count, rt_gc_gogc(), rt_gc_incremental_on(), rt_gc_lazy_sweep_on(),
        !rt_gc_assist_off(), !rt_gc_pacing_off(), !rt_gc_barrier_off(),
        (unsigned long long)g_heap_peak, GC_MIB(g_heap_peak),
        (unsigned long long)g_live_last, GC_MIB(g_live_last),
        (unsigned long long)g_heap_goal, (unsigned long long)g_gc_trigger,
        g_trigger_pct, (unsigned long long)g_heap_bytes,
        (unsigned long long)g_reg_count, (unsigned long long)g_unreg_count,
        g_gc_freed, g_objs_len,
        (unsigned long long)g_tot_mark_us, (unsigned long long)g_tot_sweep_us,
        (unsigned long long)g_tot_fin_us, (unsigned long long)tot_us,
        (double)tot_us / 1000.0,
        (unsigned long long)g_max_mark_slice_us,
        (unsigned long long)g_max_term_us,
        (unsigned long long)g_max_sweep_slice_us,
        (unsigned long long)g_tot_mark_slices, (unsigned long long)g_tot_marked,
        (unsigned long long)g_tot_sweep_slices,
        (unsigned long long)g_tot_assist_work,
        (unsigned long long)g_tot_assist_calls,
        (unsigned long long)g_wb_calls, (unsigned long long)g_wb_shaded,
        (unsigned long long)g_trig_heap, (unsigned long long)g_trig_explicit,
        (unsigned long long)g_trig_stress,
        (unsigned long long)g_missed_total,
        (unsigned long long)g_bad_array, (unsigned long long)g_bad_slot);
    fflush(stderr);
}

/* 首次分配时挂钩。没有 rt_gc_init 可用，而 atexit 又必须在 main 结束前登记，
 * 分配路径是唯一保证被走到的地方。已 arm 后只剩一次整数判断。 */
static void gc_stats_arm(void) {
    if (g_stats_armed) return;
    g_stats_armed = 1;
    if (!rt_gc_stats_on()) return;
    atexit(gc_stats_dump);
}

/* 兼容旧签名：shadow_gc_register(ptr, kind, size) —— 对象已在分配时登记 */
extern void* shadow_gc_register(void* p, int32_t kind, int64_t size) {
    (void)kind; (void)size;
    return p;
}
extern void* shadow_gc_register1(void* p) {
    return p;
}

/* any 装箱（0.3 AnyBox 布局：magic@0, tag@4, value@8；产物 ENUM_CONSTRUCT 调用） */
#define ANYBOX_MAGIC 0x5A5A5A5A
static void wb_shade(void* p);   /* 定义见下方写屏障小节 */
extern void* shadow_any_box(int32_t tag, int64_t value) {
    void* p = __rt_shadow_malloc(16);
    if (!p) return NULL;
    rt_gc_set_type(p, RT_T_ANYBOX);
    *(int32_t*)p = (int32_t)ANYBOX_MAGIC;
    *(int32_t*)((char*)p + 4) = tag;
    *(int64_t*)((char*)p + 8) = value;
    /* 装箱是绕过 __rt_shadow_store_ptr 的裸指针写：AnyBox 本身「分配即黑」
     * 本轮不会被扫描，故必须在此手工补插入屏障，否则被装箱的字符串/对象漏标。 */
    if (g_gc_phase == GC_MARK && (tag == 3 || tag == 5)) {
        g_wb_calls++;
        wb_shade((void*)(intptr_t)value);
    }
    return p;
}

/* ============================================================
 * 混合写屏障（Go 风格：删除屏障 + 插入屏障）
 * ------------------------------------------------------------
 * 三色不变式会被 mutator 以唯一一种方式破坏（Wilson 条件同时成立时）：
 *   ① 某个**黑**对象获得了指向**白**对象的引用；且
 *   ② 从任何**灰**对象到该白对象的所有路径都被摧毁。
 * 破坏其一即安全：
 *   插入屏障 shade(新值) 破坏 ①；删除屏障 shade(旧值) 破坏 ②。
 * 两者齐备（混合屏障）后，即使不在标记终止重扫栈也不会漏标 —— 这正是
 * Go 1.8 用它取代"STW 重扫栈"的原因。本实现仍保留一次终止重扫作为兜底，
 * 因为我们的栈根是保守扫描而非精确 stack map（§5.2.4 待办）。
 *
 * 性能：非 MARK 期间 g_gc_phase != GC_MARK，屏障退化为一次整数比较后返回；
 * 调用方（rt_core.c 热路径）还会先查 shadow_gc_barrier_on，连函数调用都省掉。
 * ============================================================ */

/* 测试专用杀手开关：SHADOW_GC_BARRIER=0 让屏障"空转"（照常计数，但不 shade）。
 * 用途是 A/B 自证 —— 屏障若真承重，关掉它同一个用例必然漏标崩溃/校验失败。
 * 生产路径绝不设置此变量；默认（未设置）为屏障全开。 */
static int32_t g_wb_off = -1;
static int32_t rt_gc_barrier_off(void) {
    if (g_wb_off < 0) {
        const char* e = getenv("SHADOW_GC_BARRIER");
        g_wb_off = (e && e[0] == '0') ? 1 : 0;
    }
    return g_wb_off;
}
/* 屏障内部统一置灰入口：空指针、非堆指针、已灰/黑对象都会被 rt_gc_mark_ptr 吸收 */
static void wb_shade(void* p) {
    if (!p || rt_gc_barrier_off()) return;
    g_wb_shaded += (uint64_t)rt_gc_mark_ptr(p);
}

/* 槽位写屏障：*slot 由 old 改写为 val（rt_core.c __rt_shadow_store_ptr 调用）。
 *
 * 多线程（§5.2.7）：置灰要动共享灰队列，必须串行化。锁在 GC 周期判定**之后**
 * 获取 —— 非周期期（绝大多数时间）依旧是一次整数比较后直接返回，零锁开销。
 *
 * §5.2.6 修正：**写屏障必须覆盖整个周期（MARK + SWEEP）**，与 Go 一致 ——
 * SWEEP 期 mutator 仍可能把「本轮判死的浮动垃圾」重新写入活对象字段；若屏障
 * 提前关闭，这些对象及子对象会被清扫 free，造成 use-after-free。屏障 shade
 * 的对象在 SWEEP 期由清扫路径开头的 gc_drain 置黑（见 gc_sweep_step），
 * sweep 判黑存活，下一轮 epoch 自增后重新参与标记。 */
extern void shadow_gc_barrier_slot(void* old, void* val) {
    if (g_gc_phase == GC_OFF) return;
    GC_LOCK();
    if (g_gc_phase != GC_OFF) {      /* 双检：等锁期间周期可能已结束 */
        g_wb_calls++;
        wb_shade(old);   /* 删除屏障：旧值可能是白对象仅剩的引用 */
        wb_shade(val);   /* 插入屏障：黑对象获得白引用 */
    }
    GC_UNLOCK();
}

/* codegen 插桩点（MIR_STORE_MEMBER / MIR_STORE_INDEX 之后调用）。
 * 此处已拿不到旧值，只能做插入屏障；删除屏障由 __rt_shadow_store_ptr 内的
 * barrier_slot 完成 —— 两条路径最终都汇聚到那里，故语义完整不重不漏。 */
extern void shadow_gc_write_barrier(void* obj, void* val) {
    (void)obj;
    if (g_gc_phase == GC_OFF) return;
    GC_LOCK();
    if (g_gc_phase != GC_OFF) {
        g_wb_calls++;
        wb_shade(val);
    }
    GC_UNLOCK();
}

/* 批量屏障：一段内存被整体覆盖/整体填入时逐字保守 shade。
 *
 * 必要性（真实漏洞，非理论）：shadow_array_grow 用 rt_memcpy_at 把旧数组的
 * 指针元素整体搬进新数组，**紧接着 rt_free(旧数组)**。增量标记下新数组是
 * 「分配即黑」且本轮不会被扫描，旧数组又已被显式释放 —— 那些元素于是既不在
 * 灰队列也无人引用，必被回收，而新数组仍指着它们。这是确定性的 use-after-free。
 * 逐字 shade 源区即可堵死（非堆指针查表落空，自动忽略）。 */
extern void shadow_gc_barrier_bulk(void* addr, int32_t n) {
    uintptr_t lo, hi, q;
    if (g_gc_phase == GC_OFF || !addr || n <= 0) return;
    lo = ((uintptr_t)addr + 7u) & ~(uintptr_t)7u;
    hi = (uintptr_t)addr + (uintptr_t)(uint32_t)n;
    GC_LOCK();
    if (g_gc_phase != GC_OFF) {
        g_wb_calls++;
        for (q = lo; q + 8 <= hi; q += 8) wb_shade(*(void**)q);
    }
    GC_UNLOCK();
}

/* 供外部（rt_extra.c 等 C 侧容器）显式置灰单个指针 */
extern void shadow_gc_shade(void* p) {
    if (g_gc_phase == GC_OFF) return;
    GC_LOCK();
    if (g_gc_phase != GC_OFF) wb_shade(p);
    GC_UNLOCK();
}

/* ============================================================
 * 快速路径（与 Linux runtime_for_selfhost.cpp 的 *_fast 实现语义一致，
 * 供 codegen/MIR 生成的调用链接；Windows 端布局与 runtime_lib.shadow 相同）：
 *   字符串 = NUL 结尾 char 缓冲（GC 堆对象，容量记录在对象头）
 *   数组   = [0]len(i32) [4]cap(i32) [8]es(i32) [12..]data
 * ============================================================ */

/* 线程局部字符串长度/容量缓存：str_reverse 热循环里 concat_char_fast 每次
 * strlen(out) 是 O(n²) 瓶颈，且每次都要查 GC 对象表。缓存 (ptr,len,cap)，
 * 命中即免 strlen 与查表。ABA 安全：字符串只在 GC 清扫时释放，清扫后
 * g_gc_epoch 自增；缓存条目记录 epoch，不匹配即失效。cap=0 表示未知容量，
 * 调用方按"无余量"处理（结果仍正确，仅损失就地追加）。 */
typedef struct { void* p; int32_t len; int32_t cap; uint32_t epoch; } TLStrCache;
__declspec(thread) static TLStrCache tl_str_cache[8];
__declspec(thread) static int32_t tl_str_cache_n = 0;

static int tl_str_lookup(void* p, int32_t* len, int32_t* cap) {
    uint32_t ep = g_gc_epoch;
    int32_t i;
    for (i = 0; i < tl_str_cache_n; i++) {
        if (tl_str_cache[i].p == p) {
            if (tl_str_cache[i].epoch != ep) {
                int32_t j;
                for (j = i; j < tl_str_cache_n - 1; j++) tl_str_cache[j] = tl_str_cache[j + 1];
                tl_str_cache_n--;
                return 0;
            }
            *len = tl_str_cache[i].len;
            *cap = tl_str_cache[i].cap;
            return 1;
        }
    }
    return 0;
}

static void tl_str_set(void* p, int32_t len, int32_t cap) {
    uint32_t ep = g_gc_epoch;
    int32_t i;
    for (i = 0; i < tl_str_cache_n; i++) {
        if (tl_str_cache[i].p == p) {
            tl_str_cache[i].len = len;
            tl_str_cache[i].cap = cap;
            tl_str_cache[i].epoch = ep;
            return;
        }
    }
    if (tl_str_cache_n < 8) {
        tl_str_cache[tl_str_cache_n].p = p;
        tl_str_cache[tl_str_cache_n].len = len;
        tl_str_cache[tl_str_cache_n].cap = cap;
        tl_str_cache[tl_str_cache_n].epoch = ep;
        tl_str_cache_n++;
    } else {
        for (i = 0; i < 7; i++) tl_str_cache[i] = tl_str_cache[i + 1];
        tl_str_cache[7].p = p;
        tl_str_cache[7].len = len;
        tl_str_cache[7].cap = cap;
        tl_str_cache[7].epoch = ep;
    }
}

/* 取 (len, cap)：缓存命中直接返回；未命中 strlen + rt_alloc_cap 后入缓存。 */
static void tl_str_get(void* p, int32_t* len, int32_t* cap) {
    if (tl_str_lookup(p, len, cap)) return;
    *len = (int32_t)strlen((const char*)p);
    *cap = rt_alloc_cap(p);
    tl_str_set(p, *len, *cap);
}

/* 供 shadow 层就地拼接函数（shadow_string_concat_inplace / _char）修改后同步缓存。 */
extern void shadow_string_cache_set(void* p, int32_t len, int32_t cap) {
    tl_str_set(p, len, cap);
}

/* 字符访问：返回字符码（i32，0=越界/NUL），不产生分配。供 MIR 的
 * a = a + s[i] 重写调用，替代 shadow_subscript 的 1 字符字符串分配。 */
extern int32_t shadow_string_char_at(const char* s, int32_t idx) {
    int32_t n, cap, i;
    if (!s) return 0;
    if (!tl_str_lookup((void*)s, &n, &cap)) {
        n = (int32_t)strlen(s);
        tl_str_set((void*)s, n, 0);  /* 只缓存长度，容量未知（源串通常非拼接目标） */
    }
    i = idx;
    if (i < 0) i = i + n;            /* 负索引回绕（与 shadow_schar 一致） */
    if (i < 0) return 0;
    if (i >= n) return 0;
    return (int32_t)(unsigned char)s[i];
}

/* 快速路径单字符追加：单次 C 调用完成 strlen + 容量判断 + 就地写字节/扩容，
 * 消除 shadow 层 shadow_string_concat_char 的多次 extern 调用。语义与
 * runtime_lib.shadow 的 shadow_string_concat_char 完全一致。 */
extern void* shadow_string_concat_char_fast(void* s1, int32_t c) {
    int32_t l1, cap, need, newcap;
    void* p;
    tl_str_get(s1, &l1, &cap);
    need = l1 + 2;
    if (cap >= need) {
        ((char*)s1)[l1] = (char)c;
        ((char*)s1)[l1 + 1] = 0;
        tl_str_set(s1, l1 + 1, cap);
        return s1;
    }
    newcap = cap * 2;
    if (newcap < need) newcap = need;
    if (newcap < 16) newcap = 16;  /* 最小增长 16B：短串首段扩容一步到位，str_reverse 分配 6→3 次 */
    p = shadow_gc_alloc(newcap, 0);
    shadow_gc_root_set(&p, p);
    memcpy(p, s1, (size_t)l1);
    ((char*)p)[l1] = (char)c;
    ((char*)p)[l1 + 1] = 0;
    shadow_gc_root_set(&p, 0);
    tl_str_set(p, l1 + 1, newcap);
    return p;
}

/* 快速路径字符串查找：单次 C 调用完成朴素匹配，消除 shadow 层 shadow_index_of
 * 逐字节 rt_get_byte 的 extern 调用开销。语义与 runtime_lib.shadow 的
 * shadow_index_of 完全一致（返回首次出现位置，无则 -1）。 */
extern int32_t shadow_index_of_fast(void* s, void* needle) {
    const char* sp = (const char*)s;
    const char* np = (const char*)needle;
    int32_t sl = (int32_t)strlen(sp);
    int32_t nl = (int32_t)strlen(np);
    if (nl == 0) return 0;
    int32_t limit = sl - nl;
    for (int32_t i = 0; i <= limit; i++) {
        int32_t j = 0;
        while (j < nl && sp[i + j] == np[j]) j++;
        if (j == nl) return i;
    }
    return -1;
}

/* 快速路径 array_push（shadow 层 C 布局数组，kind=2）：
 * 单次 C 调用完成 len/cap 读取 + 扩容 + 元素写入 + len 更新，
 * 消除 shadow 层每次 push 的多次 extern 调用。语义与 runtime_lib.shadow
 * 的 shadow_array_push_* 完全一致（含 null 首推、2x 倍增扩容、es 4/8 分派）。 */
extern void* shadow_array_push_int_fast(void* array_ptr, int32_t val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 4, 2);
        *(int32_t*)na = 1;
        *(int32_t*)((char*)na + 4) = 4;
        *(int32_t*)((char*)na + 8) = 4;
        *(int32_t*)((char*)na + 12) = val;
        return na;
    }
    int32_t len = *(int32_t*)array_ptr;
    int32_t cap = *(int32_t*)((char*)array_ptr + 4);
    int32_t es = *(int32_t*)((char*)array_ptr + 8);
    void* na = NULL;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *(int32_t*)na = len;
        *(int32_t*)((char*)na + 4) = nc;
        *(int32_t*)((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *(int32_t*)((char*)array_ptr + off) = val;
    else *(int64_t*)((char*)array_ptr + off) = val;
    *(int32_t*)array_ptr = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern void* shadow_array_push_long_fast(void* array_ptr, int64_t val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *(int32_t*)na = 1;
        *(int32_t*)((char*)na + 4) = 4;
        *(int32_t*)((char*)na + 8) = 8;
        *(int64_t*)((char*)na + 12) = val;
        return na;
    }
    int32_t len = *(int32_t*)array_ptr;
    int32_t cap = *(int32_t*)((char*)array_ptr + 4);
    int32_t es = *(int32_t*)((char*)array_ptr + 8);
    void* na = NULL;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *(int32_t*)na = len;
        *(int32_t*)((char*)na + 4) = nc;
        *(int32_t*)((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *(int32_t*)((char*)array_ptr + off) = (int32_t)val;
    else *(int64_t*)((char*)array_ptr + off) = val;
    *(int32_t*)array_ptr = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern void* shadow_array_push_float_fast(void* array_ptr, double val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *(int32_t*)na = 1;
        *(int32_t*)((char*)na + 4) = 4;
        *(int32_t*)((char*)na + 8) = 8;
        *(double*)((char*)na + 12) = val;
        return na;
    }
    int32_t len = *(int32_t*)array_ptr;
    int32_t cap = *(int32_t*)((char*)array_ptr + 4);
    int32_t es = *(int32_t*)((char*)array_ptr + 8);
    void* na = NULL;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *(int32_t*)na = len;
        *(int32_t*)((char*)na + 4) = nc;
        *(int32_t*)((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *(float*)((char*)array_ptr + off) = (float)val;
    else *(double*)((char*)array_ptr + off) = val;
    *(int32_t*)array_ptr = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern void* shadow_array_push_ptr_fast(void* array_ptr, void* val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *(int32_t*)na = 1;
        *(int32_t*)((char*)na + 4) = 4;
        *(int32_t*)((char*)na + 8) = 8;
        *(void**)((char*)na + 12) = val;
        return na;
    }
    int32_t len = *(int32_t*)array_ptr;
    int32_t cap = *(int32_t*)((char*)array_ptr + 4);
    int32_t es = *(int32_t*)((char*)array_ptr + 8);
    void* na = NULL;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *(int32_t*)na = len;
        *(int32_t*)((char*)na + 4) = nc;
        *(int32_t*)((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *(int32_t*)((char*)array_ptr + off) = (int32_t)(intptr_t)val;
    else *(void**)((char*)array_ptr + off) = val;
    *(int32_t*)array_ptr = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
