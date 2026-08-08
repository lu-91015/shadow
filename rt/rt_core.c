/* ============================================================
 * shadow-0.4 rt/ — 纯 C 系统调用层（rt_core.c）
 * ------------------------------------------------------------
 * 原则 3：仅依赖操作系统 API（Windows kernel32 + MSVCRT 最小部分），
 * 禁止 STL / std::string / 第三方库。
 *
 * 本文件：内存分配器（对标 Go mheap/mcentral/mcache）+ 基础字节/字符串原语 +
 * console I/O + 数值格式化/解析 + buf 缓冲 + 异常标志。
 *
 * 符号命名必须与 src/runtime/runtime_lib.shadow 的 @extern 声明一一对应。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------- 内存分配器 v2（对标 Go mheap/mcentral/mcache） ----------------
 * 三层结构（与 Go 运行时同构）：
 *   mcache   —— 每线程一个（__declspec(thread)），每 size class 一条空闲链表，
 *              分配走 mcache（无锁，最快路径，对标 Go 的 per-P mcache）。
 *   mcentral —— 每 size class 一个，持有该类的 spans（partial/empty 链表），加锁。
 *              mcache 空时从这里批量借入一整个 span 的空闲对象。
 *   mheap    —— 全局 span 注册表 + 向 OS 批量申请/归还。span = 一块连续 VirtualAlloc
 *              内存，切成同 size-class 的对象。整 span 空闲时 scavenge（MEM_DECOMMIT
 *              归还 OS，对标 Go 的 scavenge），进程 RSS 可真实回落。
 *
 * 对象头（与 rt_gc.c 共享 16 字节布局）：
 *   [size:u32@0][type_id:u32@4][mark:u32@8][span_id:u32@12]
 *   size/type/mark 由分配器/GC/运行时使用；偏移 12 存 span id（GC 不读该字段，安全复用）。
 *   大对象用哨兵 span_id = RT_SPAN_LARGE，free 时直接 VirtualFree 整块。
 */

#define RT_ALIGN 16u
#define RT_HEADER 16u
#define RT_SPAN_LARGE 0xFFFFFFFFu   /* 偏移 12 哨兵：大对象（整块一个，独立 VirtualAlloc） */

/* size-class 表（升序；数据区大小），对标 Go sizeclasses */
static const uint32_t rt_classes[] = {
    16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768,
    1024, 1536, 2048, 3072, 4096, 6144, 8192
};
#define RT_NCLASS (sizeof(rt_classes) / sizeof(rt_classes[0]))

#define RT_HDR_SIZE(blk)  (*(uint32_t*)((uintptr_t)(blk) + 0))
#define RT_HDR_SPAN(blk)  (*(uint32_t*)((uintptr_t)(blk) + 12))

/* GC 登记接口（rt_gc.c） */
void rt_gc_register(void* data);
void rt_gc_unregister(void* data);

/* GC 写屏障接口（rt_gc.c）。shadow_gc_barrier_on 仅在标记阶段为 1。 */
extern int32_t shadow_gc_barrier_on;
extern void shadow_gc_barrier_slot(void* old, void* val);
extern void shadow_gc_barrier_bulk(void* addr, int32_t n);

/* ---- mspan：一块连续 VirtualAlloc 内存切成的同 size-class 对象集 ---- */
typedef struct rt_span {
    struct rt_span* next;     /* mcentral 链表链接 */
    uint8_t* base;           /* span 内存基地址（VirtualAlloc 返回，页对齐） */
    uint32_t npages;         /* 页数 */
    int32_t  ci;             /* size class；-1 = 大对象 */
    uint32_t objsize;        /* 单对象总字节 = RT_HEADER + rt_classes[ci] */
    uint32_t nobj;           /* 对象总数 */
    uint32_t nfree;          /* 挂在 sp->free 链上的对象数（mcache 借出的不计入） */
    void*    free;          /* 空闲对象侵入式链表（每个空闲对象 data+0 存 next） */
    /* span 状态机（对标 Go mspan 的所有权协议）：
     *   0 empty       —— 全部对象空闲，挂在 mcentral.empty，可能已 scavenge
     *   1 partial     —— 有空闲对象，挂在 mcentral.partial，可被任意线程借走
     *   2 checked-out —— 被**某一个** mcache 独占，不挂任何 mcentral 链表
     *   3 full        —— 无主且无空闲对象（owner 用尽后交还），不挂任何链表
     * ⚠️ 不变式（违反即产生"两个 mcache 共享同一 span"，见 __rt_shadow_free）：
     *    state==2 的 span 绝不可被推回 partial —— 它的 owner 仍持有其对象。 */
    int32_t  state;
    int32_t  decommitted;    /* 1 = 页已被 MEM_DECOMMIT 归还 OS */
    uint32_t id;            /* 在 g_spans 注册表中的下标 */
} rt_span;

/* ---- mcentral：每 size class 一个 ---- */
typedef struct {
    rt_span* partial;   /* 仍有空闲对象，可被 cacheSpan 取用 */
    rt_span* empty;     /* 全空闲（可能已 scavenge） */
    CRITICAL_SECTION lock;
} rt_central;
static rt_central g_central[RT_NCLASS];

/* ---- mcache：每线程一个（lock-free 分配快路径） ----
 * 必须同时记住空闲链**所属的 span**（对标 Go 的 mcache.alloc[] 存 *mspan）：
 * 没有这条回指，span 就不知道自己被谁独占，owner 用尽后也无从交还，
 * 于是只能靠 free 路径把 checked-out 的 span 硬拉回 partial —— 那正是
 * "两个 mcache 共享同一 span → nfree 计数错乱 → 误 DECOMMIT" 的病根。 */
typedef struct {
    void*    alloc[RT_NCLASS];   /* 每类一条空闲链表，从单个 span 整条借入 */
    rt_span* span[RT_NCLASS];    /* 该链所属的 span；用尽时据此交还 mcentral */
} rt_mcache;
__declspec(thread) static rt_mcache* t_mcache = NULL;

/* ---- mheap：span 注册表（free 时由对象指针反查 span） ---- */
static rt_span** g_spans = NULL;
static uint32_t g_spans_len = 0, g_spans_cap = 0;
static CRITICAL_SECTION g_span_lock;

static uint32_t g_pagesize = 4096;
static LONG g_init_lock = 0;
static int g_init = 0;

static void rt_alloc_init(void) {
    if (g_init) return;
    while (InterlockedCompareExchange(&g_init_lock, 1, 0) != 0) Sleep(0);
    if (!g_init) {
        SYSTEM_INFO si; GetSystemInfo(&si);
        g_pagesize = si.dwPageSize ? si.dwPageSize : 4096;
        for (int i = 0; i < (int)RT_NCLASS; i++) InitializeCriticalSection(&g_central[i].lock);
        InitializeCriticalSection(&g_span_lock);
        g_init = 1;
    }
    g_init_lock = 0;
}

static int rt_class_index(uint32_t n) {
    for (int i = 0; i < (int)RT_NCLASS; i++)
        if (n <= rt_classes[i]) return i;
    return -1;
}

/* span 注册：分配 id 并挂入 g_spans（极少增长，g_span_lock 保护） */
static uint32_t rt_span_register(rt_span* sp) {
    EnterCriticalSection(&g_span_lock);
    if (g_spans_len >= g_spans_cap) {
        uint32_t ncap = g_spans_cap ? g_spans_cap * 2 : 256;
        rt_span** np = (rt_span**)HeapAlloc(GetProcessHeap(), 0, ncap * sizeof(rt_span*));
        if (np) {
            if (g_spans) memcpy(np, g_spans, g_spans_len * sizeof(rt_span*));
            /* 旧数组不释放（运行时常驻，避免复杂生命周期）；量级很小，可接受 */
            g_spans = np; g_spans_cap = ncap;
        }
    }
    uint32_t id = g_spans_len++;
    g_spans[id] = sp;
    LeaveCriticalSection(&g_span_lock);
    return id;
}

/* 在 span 内铺空闲链表：每个对象 data+0 串成 next，并写入头(size/span_id) */
static void rt_span_build_freelist(rt_span* sp) {
    void* cur = (void*)(sp->base + RT_HEADER);
    sp->free = cur;
    for (uint32_t i = 0; i < sp->nobj; i++) {
        void* nxt = (i + 1 < sp->nobj)
            ? (void*)(sp->base + (uint64_t)(i + 1) * sp->objsize + RT_HEADER)
            : NULL;
        *(void**)cur = nxt;
        RT_HDR_SIZE((uintptr_t)cur - RT_HEADER) = sp->objsize - RT_HEADER; /* data size */
        RT_HDR_SPAN((uintptr_t)cur - RT_HEADER) = sp->id;
        cur = nxt;
    }
    sp->nfree = sp->nobj;
}

/* 新建一个 size class 的 span（约 32KB 对象量，向上取整到整页） */
static rt_span* rt_span_new(int ci) {
    uint32_t objsize = RT_HEADER + rt_classes[ci];
    uint32_t nobj = 32768u / objsize;
    if (nobj < 1) nobj = 1;
    if (nobj > 256) nobj = 256;
    uint32_t bytes = nobj * objsize;
    uint32_t npages = (bytes + g_pagesize - 1) / g_pagesize;
    uint8_t* base = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)npages * g_pagesize,
                                           MEM_COMMIT, PAGE_READWRITE);
    if (!base) return NULL;
    rt_span* sp = (rt_span*)HeapAlloc(GetProcessHeap(), 0, sizeof(rt_span));
    if (!sp) { VirtualFree(base, 0, MEM_RELEASE); return NULL; }
    sp->next = NULL; sp->base = base; sp->npages = npages; sp->ci = ci;
    sp->objsize = objsize; sp->nobj = nobj;
    sp->free = NULL; sp->state = 2; sp->decommitted = 0;
    sp->id = rt_span_register(sp);
    rt_span_build_freelist(sp);
    return sp;
}

/* mcentral：借出一个 span（partial 优先，其次 empty 重新提交，最后新建）。
 * ⚠️ 摘取空闲链（sp->free → *out_list）必须在**锁内**完成并原子地把 span 置为
 * checked-out：早先的实现让调用方在解锁后才做 `mc->alloc=sp->free; sp->nfree=0`，
 * 那段无锁写与 __rt_shadow_free 的加锁写同时改同一批 span 字段，本身就是数据竞争。 */
static rt_span* rt_central_cache_span(int ci, void** out_list) {
    rt_central* c = &g_central[ci];
    EnterCriticalSection(&c->lock);
    rt_span* sp = c->partial;
    if (sp) {
        c->partial = sp->next;
    } else {
        sp = c->empty;
        if (sp) {
            c->empty = sp->next;
            if (sp->decommitted) {
                if (!VirtualAlloc(sp->base, (SIZE_T)sp->npages * g_pagesize,
                                  MEM_COMMIT, PAGE_READWRITE)) {
                    sp->next = c->empty; c->empty = sp;  /* 放回 */
                    LeaveCriticalSection(&c->lock);
                    return NULL;
                }
                rt_span_build_freelist(sp);   /* 重新提交后页已零化，重建空闲链表 */
                sp->decommitted = 0;
            }
        } else {
            sp = rt_span_new(ci);
            if (!sp) { LeaveCriticalSection(&c->lock); return NULL; }
        }
    }
    /* 整条空闲链一次性交给调用者的 mcache，span 随即变为该 mcache 独占。
     * nfree 归零：此后 nfree 只统计"已还回 sp->free 的对象"。 */
    *out_list = sp->free;
    sp->free  = NULL;
    sp->nfree = 0;
    sp->state = 2; sp->next = NULL;   /* checked-out（借给 mcache） */
    LeaveCriticalSection(&c->lock);
    return sp;
}

static void rt_central_unlink_partial(int ci, rt_span* sp) {
    rt_span** pp = &g_central[ci].partial;
    while (*pp) {
        if (*pp == sp) { *pp = sp->next; return; }
        pp = &(*pp)->next;
    }
}

/* mcentral：收回一个 mcache 交还的 span（对标 Go 的 mcentral.uncacheSpan）。
 * 调用者必须已持有 g_central[ci].lock，且已把 mcache 里残留的空闲对象压回 sp->free。
 * 这是 span 从"被独占"回到"公共可借"的**唯一**出口 —— 只有走到这里，
 * nfree 才重新等于 span 的全部空闲对象数，nfree==nobj 的 scavenge 判定才成立。 */
static void rt_central_uncache_span_locked(int ci, rt_span* sp) {
    rt_central* c = &g_central[ci];
    if (sp->nfree == sp->nobj) {          /* 整 span 空闲 → 归还 OS（scavenge） */
        sp->next = c->empty;
        c->empty = sp;
        sp->state = 0;
        if (!sp->decommitted) {
            VirtualFree(sp->base, (SIZE_T)sp->npages * g_pagesize, MEM_DECOMMIT);
            sp->decommitted = 1;
        }
    } else if (sp->nfree > 0) {           /* 还有空闲 → 回 partial，供任意线程复用 */
        sp->next = c->partial;
        c->partial = sp;
        sp->state = 1;
    } else {                              /* 一个空闲都没有 → full，不挂链表；
                                           * 等它的对象被 free 时自会回到 partial */
        sp->next = NULL;
        sp->state = 3;
    }
}

/* 每线程 mcache：首次使用时用非 GC 堆（HeapAlloc）分配，避免递归触发 GC */
static rt_mcache* rt_mcache_get(void) {
    rt_mcache* mc = (rt_mcache*)HeapAlloc(GetProcessHeap(), 0, sizeof(rt_mcache));
    if (mc) {
        for (int i = 0; i < (int)RT_NCLASS; i++) { mc->alloc[i] = NULL; mc->span[i] = NULL; }
        t_mcache = mc;
    }
    return mc;
}

/* 归还本线程 mcache 里尚未分配出去的空闲对象（对标 Go 的 mcache.releaseAll）。
 * 线程退出时由 rt_gc_thread_detach 调用。不归还的话，这些对象既不在任何 span 的
 * free 链上、也没有活对象引用，等于永久泄漏——spawn 密集的程序会持续涨内存。
 * 每个对象头里存着 span_id，所以可以逐个反查 span 压回去。 */
extern void rt_mcache_release(void) {
    rt_mcache* mc = t_mcache;
    if (!mc) return;
    t_mcache = NULL;                 /* 先摘掉：之后本线程若再分配会重建一个新的 */
    rt_alloc_init();
    for (int ci = 0; ci < (int)RT_NCLASS; ci++) {
        rt_span* own = mc->span[ci];
        void*    p   = mc->alloc[ci];
        mc->alloc[ci] = NULL;
        mc->span[ci]  = NULL;
        if (!own && !p) continue;
        EnterCriticalSection(&g_central[ci].lock);
        while (p) {                    /* 未分配出去的对象压回其 span 的空闲链 */
            void* nxt = *(void**)((char*)p + 0);
            rt_span* sp = g_spans[RT_HDR_SPAN((uintptr_t)p - RT_HEADER)];
            *(void**)((char*)p + 0) = sp->free;
            sp->free = p;
            sp->nfree++;
            p = nxt;
        }
        /* 交还独占的 span：到这一步本 mcache 已不再持有它的任何对象，
         * uncache 里的 nfree==nobj 判定才是真的"整 span 空闲"。 */
        if (own) rt_central_uncache_span_locked(ci, own);
        LeaveCriticalSection(&g_central[ci].lock);
    }
    HeapFree(GetProcessHeap(), 0, mc);
}

/* 小块分配：mcache 快路径 + mcentral 批量借入。
 * 借入的 span 归本 mcache 独占，用尽后必须先交还 mcentral 再借下一个（Go 的
 * mcache.refill 语义）。这条"先还后借"是多线程下同一 span 不会被两个 mcache
 * 同时持有的唯一保证 —— 旧实现允许 free 路径把 checked-out 的 span 推回 partial，
 * 于是另一线程借走后两边各自重置 nfree，最终误判整 span 空闲把页 DECOMMIT 掉。 */
static void* rt_alloc_small(int ci, uint32_t rec) {
    rt_mcache* mc = t_mcache;
    if (!mc) { mc = rt_mcache_get(); if (!mc) return NULL; }
    if (!mc->alloc[ci]) {
        rt_span* old = mc->span[ci];
        if (old) {                              /* 先交还用尽的旧 span */
            mc->span[ci] = NULL;
            EnterCriticalSection(&g_central[ci].lock);
            rt_central_uncache_span_locked(ci, old);
            LeaveCriticalSection(&g_central[ci].lock);
        }
        rt_span* sp = rt_central_cache_span(ci, &mc->alloc[ci]);
        if (!sp) return NULL;
        mc->span[ci] = sp;          /* 空闲链搬运与置 checked-out 已在锁内完成 */
    }
    void* data = mc->alloc[ci];
    mc->alloc[ci] = *(void**)((char*)data + 0);   /* pop（next 存于 data+0） */
    memset(data, 0, (size_t)rt_classes[ci]);       /* Go：分配即零化 */
    RT_HDR_SIZE((uintptr_t)data - RT_HEADER) = rec;
    /* span_id 在 span 切分时已写入，保留 */
    rt_gc_register(data);
    return data;
}

/* 内部分配：exact_size != 0 时 size 记精确字节数（供 rt_buf_len） */
static void* rt_alloc_impl(int32_t n, int exact_size) {
    if (n < 0) n = 0;
    uint32_t need = ((uint32_t)n + 7u) & ~7u;
    uint32_t rec = exact_size ? (uint32_t)n : need;
    int ci = rt_class_index(need);
    if (ci >= 0) return rt_alloc_small(ci, rec);
    /* 大对象：独立提交，span_id 哨兵，free 时整块释放 */
    uint32_t total = need + RT_HEADER;
    uint8_t* blk = (uint8_t*)VirtualAlloc(NULL, total, MEM_COMMIT, PAGE_READWRITE);
    if (!blk) return NULL;
    memset(blk, 0, RT_HEADER);
    RT_HDR_SIZE(blk) = rec;
    RT_HDR_SPAN(blk) = RT_SPAN_LARGE;
    void* data = blk + RT_HEADER;
    rt_gc_register(data);
    return data;
}

extern void* __rt_shadow_malloc(int32_t n) {
    rt_alloc_init();
    return rt_alloc_impl(n, 0);
}

/* SHADOW_GC_POISON=1：回收时把数据区填 0xDD，使"误回收后仍被使用"确定性暴露
 * （否则块进 freelist 后内容大多仍是旧数据，bug 表现为随机崩溃，极难定位）。 */
static int rt_poison = -1;
static int rt_poison_on(void) {
    if (rt_poison < 0) {
        const char* e = getenv("SHADOW_GC_POISON");
        rt_poison = (e && e[0] != '0' && e[0] != '\0') ? 1 : 0;
    }
    return rt_poison;
}

extern void __rt_shadow_free(void* p) {
    uintptr_t blk;
    uint32_t sid;
    if (!p) return;
    rt_gc_unregister(p);          /* 先注销 GC 对象表（不持锁，不会触发 GC） */
    rt_alloc_init();
    blk = (uintptr_t)p - RT_HEADER;
    sid = RT_HDR_SPAN(blk);
    if (sid == RT_SPAN_LARGE) {   /* 大对象：整块释放 */
        uint32_t size = RT_HDR_SIZE(blk);
        if (rt_poison_on() && size <= 0x7FFFFFFF) memset(p, 0xDD, (size_t)size);
        VirtualFree((void*)blk, 0, MEM_RELEASE);
        return;
    }
    rt_span* sp = g_spans[sid];
    int ci = sp->ci;
    EnterCriticalSection(&g_central[ci].lock);
    if (sp->decommitted) {        /* 防御：页已归还 OS，再碰就是 double free，直接吞掉 */
        LeaveCriticalSection(&g_central[ci].lock);
        return;
    }
    if (rt_poison_on()) memset(p, 0xDD, (size_t)rt_classes[ci]);
    *(void**)((char*)p + 0) = sp->free;   /* 压回 span 空闲链表 */
    sp->free = p;
    sp->nfree++;
    /* ⚠️ state==2（被某 mcache 独占）时**什么都不做**：这块内存的归属仍在那个
     * mcache 手里，它自会在 refill / release 时通过 uncache 把 span 交还。
     * 此处若强行推回 partial，另一线程就能借走同一个 span —— 这正是并发段错误的根。 */
    if (sp->state == 3) {                 /* full 且无主 → 有空闲了，回 partial */
        sp->next = g_central[ci].partial;
        g_central[ci].partial = sp;
        sp->state = 1;
    }
    if (sp->state == 1 && sp->nfree == sp->nobj) {
        /* 无主且整 span 空闲 → 归还 OS（scavenge）。state==2 不做此判定：
         * 那时 nfree 只统计"还回来的"，等于 nobj 也不代表没人用。 */
        rt_central_unlink_partial(ci, sp);
        sp->next = g_central[ci].empty;
        g_central[ci].empty = sp;
        sp->state = 0;
        VirtualFree(sp->base, (SIZE_T)sp->npages * g_pagesize, MEM_DECOMMIT);
        sp->decommitted = 1;
    }
    LeaveCriticalSection(&g_central[ci].lock);
}

/* ---------------- 基础字节原语 ---------------- */
/* 批量拷贝也要过屏障：数组扩容会把整段指针元素搬到新数组再释放旧数组，
 * 若不 shade，这些元素在增量标记下会被漏标（详见 shadow_gc_barrier_bulk 注释）。 */
extern void __rt_shadow_memcpy(void* dst, const void* src, int32_t n) {
    if (dst && src && n > 0) {
        if (shadow_gc_barrier_on) {
            shadow_gc_barrier_bulk(dst, n);            /* 删除屏障：被覆盖的旧内容 */
            shadow_gc_barrier_bulk((void*)src, n);     /* 插入屏障：即将写入的新内容 */
        }
        memcpy(dst, src, (size_t)n);
    }
}
extern void __rt_shadow_memcpy_at(void* dst, int32_t doff, const void* src,
                                  int32_t soff, int32_t n) {
    if (dst && src && n > 0) {
        if (shadow_gc_barrier_on) {
            shadow_gc_barrier_bulk((char*)dst + doff, n);
            shadow_gc_barrier_bulk((char*)(void*)src + soff, n);
        }
        memcpy((char*)dst + doff, (const char*)src + soff, (size_t)n);
    }
}
extern void __rt_shadow_memset(void* dst, int32_t val, int32_t n) {
    if (dst && n > 0) memset(dst, (int)val, (size_t)n);
}
extern int32_t __rt_shadow_strlen(const char* s) {
    return s ? (int32_t)strlen(s) : 0;
}
extern int32_t shadow_get_byte(const void* p, int32_t off) {
    return ((const unsigned char*)p)[off];
}
extern void __rt_shadow_set_byte(void* p, int32_t off, int32_t val) {
    ((unsigned char*)p)[off] = (unsigned char)val;
}
extern int32_t __rt_shadow_load_int(const void* p, int32_t off) {
    int32_t v; memcpy(&v, (const char*)p + off, 4); return v;
}
extern void __rt_shadow_store_int(void* p, int32_t off, int32_t val) {
    memcpy((char*)p + off, &val, 4);
}
extern int64_t __rt_shadow_load_long(const void* p, int32_t off) {
    int64_t v; memcpy(&v, (const char*)p + off, 8); return v;
}
extern void __rt_shadow_store_long(void* p, int32_t off, int64_t val) {
    memcpy((char*)p + off, &val, 8);
}
extern double __rt_shadow_load_float(const void* p, int32_t off) {
    double v; memcpy(&v, (const char*)p + off, 8); return v;
}
extern void __rt_shadow_store_float(void* p, int32_t off, double val) {
    memcpy((char*)p + off, &val, 8);
}
extern void* __rt_shadow_load_ptr(const void* p, int32_t off) {
    void* v; memcpy(&v, (const char*)p + off, sizeof(void*)); return v;
}
/* 堆指针写的**唯一咽喉**：
 *   - Shadow 层字段/数组赋值 → runtime_lib rt_store_ptr → 这里
 *   - codegen 的 MIR_STORE_MEMBER / MIR_STORE_INDEX（cg_store_fn）→ 同样到这里
 * 因此混合写屏障装在这一处即可覆盖全部托管指针写，无需改动 codegen。 */
extern void __rt_shadow_store_ptr(void* p, int32_t off, void* val) {
    if (shadow_gc_barrier_on) {
        void* old;
        memcpy(&old, (char*)p + off, sizeof(void*));
        shadow_gc_barrier_slot(old, val);   /* 删除屏障(old) + 插入屏障(val) */
    }
    memcpy((char*)p + off, &val, sizeof(void*));
}
extern int32_t __rt_shadow_str_cmp(const char* a, const char* b) {
    if (!a || !b) return a == b ? 0 : (a ? 1 : -1);
    return strcmp(a, b);
}

/* ---------------- console I/O ---------------- */
extern int32_t __rt_shadow_puts(const char* s) {
    if (!s) s = "";
    puts(s);
    return 0;
}
extern int32_t __rt_shadow_print_long(int64_t v) {
    char buf[32];
    int n = sprintf(buf, "%lld", (long long)v);
    if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); }
    return 0;
}
extern int32_t __rt_shadow_print_float(double v) {
    char buf[64];
    int n = sprintf(buf, "%g", v);
    if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); }
    return 0;
}
extern int32_t shadow_print_int(int32_t v) {
    char buf[16];
    int n = sprintf(buf, "%d", v);
    if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); }
    return 0;
}
extern int32_t shadow_print_str(const char* s) {
    if (s) fwrite(s, 1, strlen(s), stdout);
    return 0;
}

/* ---------------- 数值 → 字符串（dup 到分配器） ---------------- */
static char* rt_dup_str(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)__rt_shadow_malloc((int32_t)(n + 1));
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}
extern char* __rt_shadow_int_to_cstr(int32_t v) {
    char buf[16];
    sprintf(buf, "%d", v);
    return rt_dup_str(buf);
}
extern char* __rt_shadow_long_to_cstr(int64_t v) {
    char buf[32];
    sprintf(buf, "%lld", (long long)v);
    return rt_dup_str(buf);
}
extern char* __rt_shadow_float_to_cstr(double v) {
    char buf[64];
    sprintf(buf, "%g", v);
    return rt_dup_str(buf);
}

/* ---------------- 字符串 → 数值解析（失败置异常） ---------------- */
static int g_parse_err = 0;   /* 解析失败标志 */
extern void shadow_throw_str(const char* msg);   /* 见 rt_err.c */

extern int32_t __rt_shadow_parse_int(const char* s) {
    char* end = NULL;
    long long v;
    if (!s) { g_parse_err = 1; shadow_throw_str("parse_int: null input"); return 0; }
    v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') {
        g_parse_err = 1;
        shadow_throw_str("parse_int: invalid number");
        return 0;
    }
    return (int32_t)v;
}
extern int64_t __rt_shadow_parse_long(const char* s) {
    char* end = NULL;
    long long v;
    if (!s) { g_parse_err = 1; shadow_throw_str("parse_long: null input"); return 0; }
    v = strtoll(s, &end, 10);
    if (end == s || *end != '\0') {
        g_parse_err = 1;
        shadow_throw_str("parse_long: invalid number");
        return 0;
    }
    return (int64_t)v;
}
extern double __rt_shadow_parse_double(const char* s) {
    char* end = NULL;
    double v;
    if (!s) { g_parse_err = 1; shadow_throw_str("parse_float: null input"); return 0.0; }
    v = strtod(s, &end);
    if (end == s || *end != '\0') {
        g_parse_err = 1;
        shadow_throw_str("parse_float: invalid number");
        return 0.0;
    }
    return v;
}
extern int32_t __rt_shadow_parse_bool(const char* s) {
    if (!s) { g_parse_err = 1; shadow_throw_str("parse_bool: null input"); return 0; }
    if (strcmp(s, "true") == 0) return 1;
    if (strcmp(s, "false") == 0) return 0;
    g_parse_err = 1;
    shadow_throw_str("parse_bool: invalid value");
    return 0;
}

/* ---------------- buf 缓冲（rt_buf_alloc/rt_buf_len） ----------------
 * ⚠️ 历史 bug（2026-08-04 修复）：旧实现用 [len:int32@0][pad@4][data@8...] 布局，
 *    向 GC 登记的是块 data 指针 p，却把 p+8 返回给 Shadow 层。GC 的 ptr→idx 查表
 *    按**精确地址**匹配，内部指针 p+8 永远查不到 → buf 对象对 GC 完全不可见，
 *    随时可能被误回收（mir_serialize 的 4MB 序列化缓冲首当其冲）。
 *
 * 现方案：buf 就是普通堆对象，长度直接记在对象头的 size 字段（精确字节数），
 *    返回的就是 GC 登记的 data 指针，无内部指针。
 */
extern void* rt_buf_alloc(int32_t n) {
    void* p;
    if (n <= 0) return NULL;
    p = rt_alloc_impl(n, 1);      /* exact_size：对象头 size 即 buf 长度 */
    if (!p) return NULL;
    memset(p, 0, (size_t)n);
    return p;
}
extern int32_t rt_buf_len(void* p) {
    if (!p) return 0;
    return (int32_t)RT_HDR_SIZE((uintptr_t)p - RT_HEADER);
}
extern int32_t rt_str_to_bytes(const char* s, void* buf, int32_t off) {
    int32_t slen;
    if (!s || !buf) return off;
    slen = (int32_t)strlen(s);
    memcpy((char*)buf + off, s, (size_t)slen);
    return off + slen;
}
extern char* rt_bytes_to_str(void* buf, int32_t off, int32_t slen) {
    char* p;
    if (!buf || slen < 0) return rt_dup_str("");
    p = (char*)__rt_shadow_malloc(slen + 1);
    if (!p) return NULL;
    memcpy(p, (char*)buf + off, (size_t)slen);
    p[slen] = '\0';
    return p;
}
