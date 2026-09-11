/* ============================================================
 * shadow-0.4 rt/ — dict / hashmap / set（rt_extra.c）
 * 纯 C 链式哈希表（开放寻址 + 链表），仅依赖 rt_core 分配器。
 * 语义对齐 0.3 C++ ShadowDict/ShadowSet。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern void* __rt_shadow_malloc(int32_t n);
extern void __rt_shadow_free(void* p);
extern void rt_gc_set_type(void* data, uint32_t type_id);   /* rt_gc.c：标记 DICT 类型 */

/* ---------------- GC 写屏障 ----------------
 * dict/set 是纯 C 容器，直接对 kv->next / buckets[h] / kv->v.s 赋值，
 * **绕过了 __rt_shadow_store_ptr 这个统一咽喉**，因此必须在此手工插屏障。
 * 典型漏洞：rt_dict_grow 把已存在的（可能是白色的）kv 重挂到新分配的
 * buckets 数组上 —— 新数组「分配即黑」本轮不扫描，旧数组随即被释放，
 * 这些 kv 就成了无人引用的活对象，必被误回收。
 * WB(old, val)：删除屏障 shade(old) + 插入屏障 shade(val)。 */
extern int32_t shadow_gc_barrier_on;
extern void shadow_gc_barrier_slot(void* old, void* val);
#define WB(oldv, newv) \
    do { if (shadow_gc_barrier_on) shadow_gc_barrier_slot((void*)(oldv), (void*)(newv)); } while (0)

/* ---------------- Dict：key=char*，value=变体 ---------------- */
typedef enum {
    RT_V_STRING = 0,
    RT_V_INT = 1,
    RT_V_FLOAT = 2,
    RT_V_BOOL = 3,
    RT_V_PTR = 4
} rt_vtype;

typedef struct rt_kv {
    char* key;
    rt_vtype vtype;
    union {
        char* s;
        int64_t i;
        double f;
        int32_t b;
        void* p;
    } v;
    struct rt_kv* next;
} rt_kv;

typedef struct rt_dict {
    rt_kv** buckets;
    int32_t n_buckets;
    int32_t size;
} rt_dict;

static uint32_t rt_hash(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static rt_dict* rt_dict_new(int32_t cap) {
    rt_dict* d = (rt_dict*)__rt_shadow_malloc(sizeof(rt_dict));
    if (!d) return NULL;
    rt_gc_set_type(d, 4);   /* RT_T_DICT：GC 按 dict 布局扫描 key/value */
    if (cap < 16) cap = 16;
    d->n_buckets = cap;
    d->size = 0;
    d->buckets = (rt_kv**)__rt_shadow_malloc((int32_t)(sizeof(rt_kv*) * (size_t)cap));
    if (!d->buckets) return NULL;
    memset(d->buckets, 0, sizeof(rt_kv*) * (size_t)cap);
    return d;
}

static void rt_dict_grow(rt_dict* d) {
    int32_t new_cap = d->n_buckets * 2;
    rt_kv** nb = (rt_kv**)__rt_shadow_malloc((int32_t)(sizeof(rt_kv*) * (size_t)new_cap));
    int32_t i;
    if (!nb) return;
    memset(nb, 0, sizeof(rt_kv*) * (size_t)new_cap);
    for (i = 0; i < d->n_buckets; i++) {
        rt_kv* kv = d->buckets[i];
        while (kv) {
            rt_kv* nx = kv->next;
            uint32_t h = rt_hash(kv->key) & (uint32_t)(new_cap - 1);
            WB(kv->next, nb[h]);
            kv->next = nb[h];
            WB(NULL, kv);
            nb[h] = kv;
            kv = nx;
        }
    }
    WB(d->buckets, nb);
    __rt_shadow_free(d->buckets);
    d->buckets = nb;
    d->n_buckets = new_cap;
}

static rt_kv* rt_dict_find(rt_dict* d, const char* key) {
    uint32_t h = rt_hash(key) & (uint32_t)(d->n_buckets - 1);
    rt_kv* kv = d->buckets[h];
    while (kv) {
        if (strcmp(kv->key, key) == 0) return kv;
        kv = kv->next;
    }
    return NULL;
}

static void rt_dict_set(rt_dict* d, const char* key, rt_vtype vt, void* raw) {
    rt_kv* kv;
    char* kdup;
    if (!d || !key) return;
    if (d->size >= d->n_buckets * 3 / 4) rt_dict_grow(d);
    kv = rt_dict_find(d, key);
    kdup = (char*)__rt_shadow_malloc((int32_t)strlen(key) + 1);
    if (!kdup) return;
    strcpy(kdup, key);
    if (kv) {
        __rt_shadow_free((void*)kv->key);
        WB(NULL, kdup);
        kv->key = kdup;
    } else {
        kv = (rt_kv*)__rt_shadow_malloc(sizeof(rt_kv));
        if (!kv) return;
        WB(NULL, kdup);
        kv->key = kdup;
        kv->next = NULL;
        {
            uint32_t h = rt_hash(kdup) & (uint32_t)(d->n_buckets - 1);
            WB(NULL, d->buckets[h]);
            kv->next = d->buckets[h];
            WB(NULL, kv);
            d->buckets[h] = kv;
        }
        d->size++;
    }
    /* 旧值若是堆指针需走删除屏障；新值若是堆指针需走插入屏障 */
    if (shadow_gc_barrier_on &&
        (kv->vtype == RT_V_STRING || kv->vtype == RT_V_PTR))
        shadow_gc_barrier_slot(kv->v.p, NULL);
    kv->vtype = vt;
    switch (vt) {
        case RT_V_STRING: WB(NULL, raw); kv->v.s = (char*)raw; break;
        case RT_V_INT:    kv->v.i = (int64_t)(intptr_t)raw; break;
        case RT_V_FLOAT: {
            int64_t bits = (int64_t)(intptr_t)raw;
            memcpy(&kv->v.f, &bits, sizeof(double));
            break;
        }
        case RT_V_BOOL:   kv->v.b = (int32_t)(intptr_t)raw; break;
        case RT_V_PTR:    WB(NULL, raw); kv->v.p = raw; break;
    }
}

/* ---------------- Dict API（0.3 兼容） ---------------- */
extern void* shadow_hashmap_new(void) {
    return rt_dict_new(16);
}

extern void shadow_hashmap_insert(void* map, const char* key, const char* value) {
    if (!map || !key) return;
    rt_dict_set((rt_dict*)map, key, RT_V_STRING, (void*)value);
}

extern void shadow_hashmap_insert_int(void* map, const char* key, int64_t value) {
    if (!map || !key) return;
    rt_dict_set((rt_dict*)map, key, RT_V_INT, (void*)(intptr_t)value);
}

extern const char* shadow_hashmap_get(void* map, const char* key) {
    extern char* __rt_shadow_int_to_cstr(int32_t v);
    extern char* __rt_shadow_long_to_cstr(int64_t v);
    extern char* __rt_shadow_float_to_cstr(double v);
    rt_kv* kv;
    if (!map || !key) return (const char*)(uintptr_t)0;
    kv = rt_dict_find((rt_dict*)map, key);
    if (!kv) return NULL;
    switch (kv->vtype) {
        case RT_V_STRING: return kv->v.s;
        case RT_V_INT:
            if (kv->v.i >= -2147483648LL && kv->v.i <= 2147483647LL)
                return __rt_shadow_int_to_cstr((int32_t)kv->v.i);
            return __rt_shadow_long_to_cstr(kv->v.i);
        case RT_V_FLOAT: return __rt_shadow_float_to_cstr(kv->v.f);
        case RT_V_BOOL:  return kv->v.b ? (const char*)(uintptr_t)1 : NULL;
        default:         return NULL;
    }
}

extern int shadow_hashmap_contains(void* map, const char* key) {
    if (!map || !key) return 0;
    return rt_dict_find((rt_dict*)map, key) != NULL ? 1 : 0;
}

extern int shadow_hashmap_remove(void* map, const char* key) {
    rt_dict* d;
    uint32_t h;
    rt_kv *kv, *prev;
    if (!map || !key) return 0;
    d = (rt_dict*)map;
    h = rt_hash(key) & (uint32_t)(d->n_buckets - 1);
    prev = NULL;
    kv = d->buckets[h];
    while (kv) {
        if (strcmp(kv->key, key) == 0) {
            /* 摘链：把（可能是白色的）后继写进 prev/buckets，需插入屏障 */
            WB(NULL, kv->next);
            if (prev) prev->next = kv->next;
            else d->buckets[h] = kv->next;
            __rt_shadow_free((void*)kv->key);
            __rt_shadow_free(kv);
            d->size--;
            return 1;
        }
        prev = kv;
        kv = kv->next;
    }
    return 0;
}

extern int shadow_hashmap_size(void* map) {
    if (!map) return 0;
    return ((rt_dict*)map)->size;
}

extern void shadow_hashmap_free(void* map) {
    rt_dict* d;
    int32_t i;
    if (!map) return;
    d = (rt_dict*)map;
    for (i = 0; i < d->n_buckets; i++) {
        rt_kv* kv = d->buckets[i];
        while (kv) {
            rt_kv* nx = kv->next;
            __rt_shadow_free((void*)kv->key);
            __rt_shadow_free(kv);
            kv = nx;
        }
    }
    __rt_shadow_free(d->buckets);
    __rt_shadow_free(d);
}

/* dict 语法：dict[key] = value（tag 见 codegen） */
extern void* shadow_dict_set(void* dict_ptr, void* key, int32_t tag, void* val) {
    if (!dict_ptr || !key) return dict_ptr;
    switch (tag) {
        case 0: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_INT, val); break;
        case 1: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_FLOAT, val); break;
        case 2: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_STRING, val); break;
        case 3: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_BOOL, val); break;
        case 4: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_PTR, val); break;
        default: rt_dict_set((rt_dict*)dict_ptr, (const char*)key, RT_V_INT, val); break;
    }
    return dict_ptr;
}

extern void* shadow_dict_get(void* dict_ptr, const char* key) {
    rt_kv* kv;
    if (!dict_ptr || !key) return NULL;
    kv = rt_dict_find((rt_dict*)dict_ptr, key);
    if (!kv) return NULL;
    if (kv->vtype == RT_V_PTR) return kv->v.p;
    if (kv->vtype == RT_V_STRING) return kv->v.s;
    return (void*)(intptr_t)kv->v.i;
}

extern int64_t shadow_dict_get_int(void* dict_ptr, const char* key) {
    rt_kv* kv;
    if (!dict_ptr || !key) return 0;
    kv = rt_dict_find((rt_dict*)dict_ptr, key);
    if (!kv) return 0;
    if (kv->vtype == RT_V_INT) return kv->v.i;
    if (kv->vtype == RT_V_BOOL) return kv->v.b;
    if (kv->vtype == RT_V_STRING) return (int64_t)(intptr_t)kv->v.s;
    return 0;
}

extern const char* shadow_dict_get_string(void* dict_ptr, const char* key) {
    rt_kv* kv;
    if (!dict_ptr || !key) return NULL;
    kv = rt_dict_find((rt_dict*)dict_ptr, key);
    if (!kv) return NULL;
    if (kv->vtype == RT_V_STRING) return kv->v.s;
    return NULL;
}

/* ---------------- Dict / Set 迭代支持 ----------------
 * for (k in dict) / for (k, v in dict) / for (x in set) 被 MIR 降级成
 * 「size + 索引访问」的计数循环（见 src/mir/mir.shadow 中 shadow_dict_size /
 * shadow_dict_key_at / shadow_set_at_string / shadow_set_at_int 的发射点）。
 * 0.3 用 std::unordered_map/unordered_set + std::advance(it, idx) 实现索引访问；
 * 0.4 的 rt_dict 是「桶数组 + 桶内单链表」，按桶下标升序、桶内链表序遍历
 * 即可给出稳定的线性索引（注意 rt_dict_set 是头插，故同桶内为逆插入序）。
 * 单次访问 O(n)，整个 for 循环 O(n^2)——与 0.3 的 std::advance 完全一致。 */
static char* rt_str_dup(const char* s) {
    char* p;
    size_t n;
    if (!s) return NULL;
    n = strlen(s);
    p = (char*)__rt_shadow_malloc((int32_t)n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* 返回第 idx 个 key 的**内部指针**（不复制）；越界返回 NULL。 */
static const char* rt_dict_key_at(rt_dict* d, int32_t idx) {
    int32_t i;
    int32_t seen = 0;
    if (!d || idx < 0 || idx >= d->size) return NULL;
    for (i = 0; i < d->n_buckets; i++) {
        rt_kv* kv = d->buckets[i];
        while (kv) {
            if (seen == idx) return kv->key;
            seen++;
            kv = kv->next;
        }
    }
    return NULL;
}

/* dict 元素个数。不能复用 shadow_array_len——那读的是 Shadow 布局数组。 */
extern int32_t shadow_dict_size(void* dict_ptr) {
    if (!dict_ptr) return 0;
    return ((rt_dict*)dict_ptr)->size;
}

/* 第 idx 个 key，返回**副本**（对齐 0.3 的 strdup 语义：调用方可安全持有，
 * 不会因后续 hashmap_remove/free 释放内部 key 而悬空）。 */
extern const char* shadow_dict_key_at(void* dict_ptr, int32_t idx) {
    const char* k;
    if (!dict_ptr) return NULL;
    k = rt_dict_key_at((rt_dict*)dict_ptr, idx);
    if (!k) {
        fprintf(stderr, "error: dict index %d out of bounds (size=%d)\n",
                idx, ((rt_dict*)dict_ptr)->size);
        return NULL;
    }
    return rt_str_dup(k);
}

/* ---------------- Set：string 集合（int 元素序列化为十进制串） ---------------- */
extern void* shadow_set_new(void) {
    return rt_dict_new(16);
}

extern int shadow_set_add(void* set, const char* value) {
    if (!set || !value) return 0;
    rt_dict_set((rt_dict*)set, value, RT_V_BOOL, (void*)(intptr_t)1);
    return 0;
}

extern int shadow_set_contains(void* set, const char* value) {
    if (!set || !value) return 0;
    return rt_dict_find((rt_dict*)set, value) != NULL ? 1 : 0;
}

extern int shadow_set_remove(void* set, const char* value) {
    return shadow_hashmap_remove(set, value);
}

extern int shadow_set_size(void* set) {
    return shadow_hashmap_size(set);
}

extern void shadow_set_free(void* set) {
    shadow_hashmap_free(set);
}

extern int shadow_set_add_int(void* set, int64_t value) {
    extern char* __rt_shadow_long_to_cstr(int64_t v);
    char* s;
    if (!set) return 0;
    s = __rt_shadow_long_to_cstr(value);
    if (!s) return 0;
    rt_dict_set((rt_dict*)set, s, RT_V_BOOL, (void*)(intptr_t)1);
    return 0;
}

extern int shadow_set_contains_int(void* set, int64_t value) {
    extern char* __rt_shadow_long_to_cstr(int64_t v);
    char* s;
    int r;
    if (!set) return 0;
    s = __rt_shadow_long_to_cstr(value);
    if (!s) return 0;
    r = rt_dict_find((rt_dict*)set, s) != NULL ? 1 : 0;
    return r;
}

extern int shadow_set_remove_int(void* set, int64_t value) {
    extern char* __rt_shadow_long_to_cstr(int64_t v);
    char* s;
    int r;
    if (!set) return 0;
    s = __rt_shadow_long_to_cstr(value);
    if (!s) return 0;
    r = shadow_hashmap_remove(set, s);
    return r;
}

/* set 迭代：for (x in set<string>) —— 返回第 idx 个元素副本。
 * set 复用 rt_dict（元素存为 key），故直接走 rt_dict_key_at。 */
extern const char* shadow_set_at_string(void* set, int32_t idx) {
    const char* k;
    if (!set) return NULL;
    k = rt_dict_key_at((rt_dict*)set, idx);
    if (!k) {
        fprintf(stderr, "error: set index %d out of bounds (size=%d)\n",
                idx, ((rt_dict*)set)->size);
        return NULL;
    }
    return rt_str_dup(k);
}

/* set 迭代：for (x in set<int|long|bool|date|...>) —— 元素以十进制串存储，
 * 这里解析回 int64（对齐 0.3 的 std::stoll，解析失败返回 0）。 */
extern int64_t shadow_set_at_int(void* set, int32_t idx) {
    const char* k;
    if (!set) return 0;
    k = rt_dict_key_at((rt_dict*)set, idx);
    if (!k) {
        fprintf(stderr, "error: set index %d out of bounds (size=%d)\n",
                idx, ((rt_dict*)set)->size);
        return 0;
    }
    return (int64_t)strtoll(k, NULL, 10);
}

/* ============================================================
 * 杂项运行时符号（0.3 runtime_cpp/shadow_runtime_minimal.cpp 的等价物）
 * 0.4 重写 rt/ 层时遗漏了这批 @extern 目标，导致用户程序链接期报
 * "undefined symbol: shadow_putchar / shadow_abs / shadow_arena_create"。
 * 注意：本文件只进 rt_extra.o（用户程序链接行）；shadow.exe 自身链接的是
 * build/rt/runtime_for_selfhost.o，不含本文件，故无 duplicate symbol 风险。
 * ============================================================ */

extern int64_t shadow_abs(int64_t x) {
    return x < 0 ? -x : x;
}

extern void shadow_putchar(int32_t c) {
    putchar((int)c);
}

/* ---------------- Arena：bump 分配器（对齐 0.3 ShadowArena 语义） ----------------
 * 块列表 + 每块 used 游标；mark/rewind 以「累计 total_used」为坐标。
 * 内存用裸 malloc/free 管理（不进 GC）：arena 生命周期由调用方显式控制。 */
typedef struct rt_arena {
    void**  blocks;
    size_t* caps;
    size_t* useds;
    size_t  nblocks;
    size_t  cap_blocks;
    size_t  block_size;
    size_t  total_used;
    size_t  high_water;
} rt_arena;

static int rt_arena_reserve(rt_arena* ar) {
    size_t nc;
    void** nb;
    size_t* ncaps;
    size_t* nuse;
    if (ar->nblocks < ar->cap_blocks) return 1;
    nc = ar->cap_blocks ? ar->cap_blocks * 2 : 4;
    nb = (void**)realloc(ar->blocks, nc * sizeof(void*));
    if (!nb) return 0;
    ar->blocks = nb;
    ncaps = (size_t*)realloc(ar->caps, nc * sizeof(size_t));
    if (!ncaps) return 0;
    ar->caps = ncaps;
    nuse = (size_t*)realloc(ar->useds, nc * sizeof(size_t));
    if (!nuse) return 0;
    ar->useds = nuse;
    ar->cap_blocks = nc;
    return 1;
}

extern void* shadow_arena_create(int32_t block_size) {
    rt_arena* ar = (rt_arena*)calloc(1, sizeof(rt_arena));
    size_t bs;
    if (!ar) return NULL;
    bs = block_size > 0 ? (size_t)block_size : (size_t)4096;
    if (bs < 64) bs = 64;
    ar->block_size = bs;
    return ar;
}

extern void* shadow_arena_alloc(void* arena, int32_t size) {
    rt_arena* ar = (rt_arena*)arena;
    size_t n, cap, idx;
    char* p;
    if (!ar || size <= 0) return NULL;
    n = ((size_t)size + 7u) & ~((size_t)7u);
    if (ar->nblocks == 0 || ar->useds[ar->nblocks - 1] + n > ar->caps[ar->nblocks - 1]) {
        void* block;
        if (!rt_arena_reserve(ar)) return NULL;
        cap = n > ar->block_size ? n : ar->block_size;
        block = calloc(1, cap);
        if (!block) return NULL;
        ar->blocks[ar->nblocks] = block;
        ar->caps[ar->nblocks] = cap;
        ar->useds[ar->nblocks] = 0;
        ar->nblocks++;
    }
    idx = ar->nblocks - 1;
    p = (char*)ar->blocks[idx] + ar->useds[idx];
    ar->useds[idx] += n;
    ar->total_used += n;
    if (ar->total_used > ar->high_water) ar->high_water = ar->total_used;
    return p;
}

extern int32_t shadow_arena_mark(void* arena) {
    rt_arena* ar = (rt_arena*)arena;
    if (!ar) return 0;
    if (ar->total_used > 2147483647u) return 2147483647;
    return (int32_t)ar->total_used;
}

extern void shadow_arena_rewind(void* arena, int32_t mark) {
    rt_arena* ar = (rt_arena*)arena;
    size_t target, consumed, keep, i;
    if (!ar) return;
    target = mark > 0 ? (size_t)mark : 0u;
    if (target >= ar->total_used) return;
    consumed = 0;
    keep = 0;
    for (i = 0; i < ar->nblocks; i++) {
        size_t u = ar->useds[i];
        if (target <= consumed + u) {
            ar->useds[i] = target - consumed;
            keep = i + 1;
            break;
        }
        consumed += u;
    }
    for (i = keep; i < ar->nblocks; i++) {
        free(ar->blocks[i]);
        ar->blocks[i] = NULL;
    }
    ar->nblocks = keep;
    ar->total_used = target;
}

extern void shadow_arena_reset(void* arena) {
    shadow_arena_rewind(arena, 0);
}

extern void shadow_arena_destroy(void* arena) {
    rt_arena* ar = (rt_arena*)arena;
    size_t i;
    if (!ar) return;
    for (i = 0; i < ar->nblocks; i++) { free(ar->blocks[i]); }
    free(ar->blocks);
    free(ar->caps);
    free(ar->useds);
    free(ar);
}

extern int32_t shadow_arena_used(void* arena) {
    rt_arena* ar = (rt_arena*)arena;
    if (!ar) return 0;
    if (ar->total_used > 2147483647u) return 2147483647;
    return (int32_t)ar->total_used;
}

extern int32_t shadow_arena_high_water(void* arena) {
    rt_arena* ar = (rt_arena*)arena;
    if (!ar) return 0;
    if (ar->high_water > 2147483647u) return 2147483647;
    return (int32_t)ar->high_water;
}
