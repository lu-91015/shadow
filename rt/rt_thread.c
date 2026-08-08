/* ============================================================
 * shadow-0.4 rt/ — 纯 C 系统调用层（rt_thread.c）
 * ------------------------------------------------------------
 * 原则 3：仅依赖操作系统 API（Windows kernel32 + MSVCRT 最小部分），
 * 禁止 STL / std::string / 第三方库。
 *
 * 本文件：spawn / await 真线程运行时（开发原则 §5.2.7）。
 *
 * 模型（与 0.3 一致，但去掉 C++ std::thread 依赖）：
 *   spawn <fn>        —— 立刻在独立 OS 线程运行函数体（真并行），返回 Future 句柄；
 *   await <fut>       —— 阻塞等待直至就绪，取出结果值。
 * Future 对运行时是不透明句柄（Shadow 侧类型为 any / i8*），值以 void* 承载。
 *
 * 协程体函数签名（由 codegen 决定）：
 *   nullary : void* fn(void)          —— spawn <funcname> 无参
 *   thunk   : void* fn(void* env)     —— spawn <funcname>(args)，codegen 生成 thunk
 *
 * ── 与 GC 的整合（§5.2.7 的要害，逐条对应）────────────────────
 *  1) 线程栈可见：线程体开始前 rt_gc_thread_attach() 把自己登记进 GC 线程表，
 *     GC 扫根时才能挂起本线程并扫描它的栈与寄存器；退出前 detach 注销，
 *     顺带把线程本地分配缓存（mcache）归还 mcentral。
 *  2) 跨线程传递的 GC 指针必须有根：
 *     · thunk 的 env 参数在「CreateThread 返回」到「新线程真正跑起来」之间，
 *       只被本文件的 ShadowTask（非 GC 内存）持有 —— 此窗口内 GC 扫不到它，
 *       会被当垃圾回收。故 spawn 时即登记为永久根，线程体结束后摘除。
 *     · 线程返回值同理：写进 Future 后、await 取走前，唯一持有者是 Future
 *       （非 GC 内存）。故先登记永久根再置 ready，顺序不可颠倒。
 *  3) Future / Task 结构体本身走进程堆（非 GC 堆）：GC 保守扫描遇到它们
 *     查对象表查不到，直接忽略，不会误标也不会误扫。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdlib.h>
#include <stdint.h>

/* ---- GC 侧接口（rt_gc.c）---- */
extern void rt_gc_thread_attach(void);
extern void rt_gc_thread_detach(void);
extern void shadow_gc_perm_root_add(void* p);
extern void shadow_gc_perm_root_remove(void* p);
/* 把某个内存槽登记为根：GC 扫根时读 *slot 的当前值。用于 Future 的结果槽。 */
extern int32_t shadow_gc_global_root_set(void* slot, void* val);

/* ---- Future：一次性事件 + 结果槽 ---- */
typedef struct ShadowFuture {
    CRITICAL_SECTION   mtx;     /* 保护 ready/value */
    CONDITION_VARIABLE cv;      /* await 等待就绪 */
    volatile LONG      ready;   /* 0=pending 1=ready */
    void*              value;   /* 结果（any / GC 指针） */
} ShadowFuture;

/* ---- Task：一个 spawn 对应一条 OS 线程 ---- */
typedef struct ShadowTask {
    void* (*fn_nullary)(void);
    void* (*fn_thunk)(void*);
    void*              arg;      /* thunk 的 env（GC 对象） */
    ShadowFuture*      fut;
    HANDLE             thread;
    struct ShadowTask* next;
} ShadowTask;

static CRITICAL_SECTION g_tasks_lock;
static ShadowTask*      g_tasks = NULL;   /* 全部 spawned 任务，sched_run 统一 join */
static LONG             g_thr_init_lock = 0;
static int              g_thr_init = 0;

static void shadow_sched_run_impl(void);
static void rt_thread_atexit(void) { shadow_sched_run_impl(); }

/* 惰性初始化（自旋锁保护，与 rt_core 的 rt_alloc_init 同风格） */
static void rt_thread_init(void) {
    if (g_thr_init) return;
    while (InterlockedCompareExchange(&g_thr_init_lock, 1, 0) != 0) Sleep(0);
    if (!g_thr_init) {
        InitializeCriticalSection(&g_tasks_lock);
        /* 进程退出兜底：join 所有仍在运行的线程，避免主线程先跑完 atexit
         * 释放运行时资源后，后台线程还在访问已失效的 GC 结构。 */
        atexit(rt_thread_atexit);
        g_thr_init = 1;
    }
    g_thr_init_lock = 0;
}

static ShadowFuture* future_new(void) {
    ShadowFuture* f = (ShadowFuture*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                               sizeof(ShadowFuture));
    if (!f) return NULL;
    InitializeCriticalSection(&f->mtx);
    InitializeConditionVariable(&f->cv);
    f->ready = 0;
    f->value = NULL;
    return f;
}

/* 置位 Future 并唤醒全部等待者。
 * 结果值的保活：把 &f->value 这个**槽**登记为全局根，GC 扫根时读槽里的当前值。
 * 比"把值本身钉成永久根"更稳：
 *   · 键是槽而非值，重复 set 只占一条，不会随 spawn 次数累积重复项；
 *   · await 取走后再 await 依然安全（值一直有根，不存在摘早了的悬垂）。
 * 前提是 Future 永不释放 —— 本文件确实不释放（见 shadow_sched_run_impl 注释），
 * 否则槽会悬垂，扫根时读到野内存。 */
static void future_set(ShadowFuture* f, void* value) {
    if (!f) return;
    /* 先登记根、再进 f->mtx：避免 f->mtx → g_gc_lock 的嵌套持锁。
     * global_root_set 会同时缓存 val 并在扫根时一并标记，故"先登记后写槽"
     * 中间那一瞬也没有无根窗口。 */
    if (value) shadow_gc_global_root_set(&f->value, value);
    EnterCriticalSection(&f->mtx);
    f->value = value;
    f->ready = 1;
    LeaveCriticalSection(&f->mtx);
    WakeAllConditionVariable(&f->cv);
}

/* ---- 线程入口 ---- */
static DWORD WINAPI task_run(LPVOID p) {
    ShadowTask* t = (ShadowTask*)p;
    void* result;

    /* 先入 GC 线程表，再执行用户代码：线程体第一次分配就可能触发 GC，
     * 那时本线程的栈必须已经对 GC 可见，否则栈上的活对象会被误回收。 */
    rt_gc_thread_attach();

    if (t->fn_nullary) result = t->fn_nullary();
    else                result = t->fn_thunk(t->arg);

    /* future_set 内部先把 &fut->value 登记成根再置 ready —— 顺序不可颠倒：
     * 置 ready 之后 await 方随时可能取走结果并让本线程退出，若此时结果
     * 还没有根，中间任何一次 GC 都会把它回收掉。 */
    future_set(t->fut, result);

    /* env 的保护窗口到此为止：线程体已结束，env 不再被任何非 GC 内存独占。 */
    if (t->arg) shadow_gc_perm_root_remove(t->arg);

    /* 出表 + 归还 mcache（§5.2.7「线程终止时归还线程本地分配缓存」）。
     * 必须是本线程最后一个动作：detach 之后本线程栈对 GC 不再可见。 */
    rt_gc_thread_detach();
    return 0;
}

static void* spawn_common(void* (*fn0)(void), void* (*fn1)(void*), void* arg) {
    ShadowTask*   t;
    ShadowFuture* fut;
    rt_thread_init();
    fut = future_new();
    if (!fut) return NULL;
    t = (ShadowTask*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ShadowTask));
    if (!t) return fut;
    t->fn_nullary = fn0;
    t->fn_thunk   = fn1;
    t->arg        = arg;
    t->fut        = fut;

    /* env 在新线程 attach 之前处于「无根窗口」，先钉住（详见文件头 2）。 */
    if (arg) shadow_gc_perm_root_add(arg);

    /* CREATE_SUSPENDED 不是图省事，是为了消掉一个 use-after-free 窗口：
     * 若"先挂链表再 CreateThread"，就存在「t 已在 g_tasks 里、但 t->thread
     * 还是 NULL」的一瞬 —— 并发的 shadow_sched_run 会认为它没线程可 join，
     * 直接 HeapFree(t)，而线程随后启动仍在读 t。
     * 反过来"先建线程再挂链表"也不行：线程可能在挂链表之前就跑完，
     * 那一段它对 sched_run 不可见。
     * 故：挂起态建线程 → 挂链表（此刻 handle 已就绪）→ 放行。 */
    t->thread = CreateThread(NULL, 0, task_run, t, CREATE_SUSPENDED, NULL);

    EnterCriticalSection(&g_tasks_lock);
    t->next = g_tasks;
    g_tasks = t;
    LeaveCriticalSection(&g_tasks_lock);

    if (t->thread) {
        ResumeThread(t->thread);
    } else {
        /* 起线程失败：退化为同步执行，保证语义不丢（await 仍能拿到值）。 */
        void* r = fn0 ? fn0() : fn1(arg);
        future_set(fut, r);
        if (arg) shadow_gc_perm_root_remove(arg);
    }
    return fut;
}

/* ============================================================
 * 对外 ABI（符号名由 src/codegen/codegen.shadow 的 cg_resolve_extern_name 决定）
 * ============================================================ */

extern void shadow_sched_init(void) {
    rt_thread_init();
}

extern void* shadow_spawn_nullary(void* (*fn)(void)) {
    if (!fn) return NULL;
    return spawn_common(fn, NULL, NULL);
}

extern void* shadow_spawn_thunk(void* (*fn)(void*), void* arg) {
    if (!fn) return NULL;
    return spawn_common(NULL, fn, arg);
}

/* §5.2.4 协作式安全点：编译器在每个安全点插入的检查（定义于 rt_gc.c）。
 * await 的等待循环必须周期性调用它 —— 否则 GC 发起 STW 时，阻塞在 await
 * 里的线程无法到达安全点，GC 会死等（协作式 STW 的硬性前提：任何长阻塞
 * 都不能卡住安全点）。 */
extern void shadow_gc_poll(void);

extern void* shadow_future_await(void* fut_h) {
    ShadowFuture* f = (ShadowFuture*)fut_h;
    void* v;
    if (!f) return NULL;
    /* 轮询等待（不持锁阻塞）：持锁期间若被 GC 挂起，其它线程的
     * future_set 会卡在 f->mtx 上 → 它们无法到达安全点 → 死锁。
     * 先取锁读 ready，未就绪立即放锁，然后 poll + 短暂让出。 */
    for (;;) {
        EnterCriticalSection(&f->mtx);
        if (f->ready) {
            v = f->value;
            LeaveCriticalSection(&f->mtx);
            return v;
        }
        LeaveCriticalSection(&f->mtx);
        shadow_gc_poll();   /* 协作安全点：STW 请求时在此停下 */
        Sleep(1);           /* 让出时间片，避免忙等烧 CPU */
    }
}

/* 手动构造 / 完成 Future（async 函数返回 Future 用） */
extern void* shadow_future_new(void) {
    rt_thread_init();
    return future_new();
}

extern void shadow_future_complete(void* fut_h, void* value) {
    if (!fut_h) return;
    future_set((ShadowFuture*)fut_h, value);
}

/* 排空所有 spawned 线程（fire-and-forget 的收尾；atexit 亦调用）。
 * Future 本身不释放 —— Shadow 侧可能仍持有句柄，且进程即将退出。 */
static void shadow_sched_run_impl(void) {
    ShadowTask* list;
    if (!g_thr_init) return;
    EnterCriticalSection(&g_tasks_lock);
    list = g_tasks;
    g_tasks = NULL;
    LeaveCriticalSection(&g_tasks_lock);
    while (list) {
        ShadowTask* nx = list->next;
        if (list->thread) {
            /* 轮询 join（带超时）：无限阻塞会卡住协作式安全点 ——
             * 若此刻 GC 由其它线程触发，阻塞在 join 上的线程无法到达
             * 安全点，STW 死等。带超时 + poll 让出即可解除。 */
            while (WaitForSingleObject(list->thread, 50) == WAIT_TIMEOUT) {
                shadow_gc_poll();
            }
            CloseHandle(list->thread);
        }
        HeapFree(GetProcessHeap(), 0, list);
        list = nx;
    }
}

extern void shadow_sched_run(void) {
    shadow_sched_run_impl();
}
