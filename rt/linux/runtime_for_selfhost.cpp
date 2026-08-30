// shadow_runtime.cpp - Shadow Language Runtime Library
// Provides file, network, string and JSON operations

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#include <io.h>
#include <fcntl.h>
#else
#include <curl/curl.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <execinfo.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <cctype>
#include <sstream>
#include <emmintrin.h>
#include <atomic>
#include <cstdarg>
#include <variant>
#include <sys/stat.h>

// Forward declarations: ShadowDict/ShadowArray 完整定义在下方（line ~617），
// 此处提前声明以供 shadow_hashmap_*（line ~412）使用 —— hashmap_* 已与 dict 统一表示。
// 由于 hashmap_* 需要访问成员（new ShadowDict、m->data 等），
// 必须把完整 struct 定义移到此处（不能仅 forward declare）。
// DictValue: pure C tag+union layout for W3 in-band header
struct DictValue {
    int32_t tag; // 0=int64, 1=double, 2=string, 3=bool, 4=ptr
    union { int64_t i; double d; const char* s; int32_t b; void* p; } val;
    
    DictValue() : tag(0), val{} {}
    DictValue(int64_t v) : tag(0), val{} { val.i = v; }
    DictValue(double v) : tag(1), val{} { val.d = v; }
    DictValue(const char* v) : tag(2), val{} { val.s = v ? strdup(v) : nullptr; }
    DictValue(std::string v) : tag(2), val{} { val.s = strdup(v.c_str()); }
    DictValue(bool v) : tag(3), val{} { val.b = v ? 1 : 0; }
    DictValue(void* v) : tag(4), val{} { val.p = v; }
    DictValue& operator=(int64_t v) { tag=0; val.i=v; return *this; }
    DictValue& operator=(double v) { tag=1; val.d=v; return *this; }
    DictValue& operator=(const char* v) { tag=2; free((void*)val.s); val.s=v?strdup(v):nullptr; return *this; }
    DictValue& operator=(bool v) { tag=3; val.b=v?1:0; return *this; }
    DictValue& operator=(void* v) { tag=4; val.p=v; return *this; }
};
struct ShadowDict;
struct ShadowArray;
// Forward declarations for static helpers (defined below)
static std::string value_to_string(const DictValue& v);
static std::string value_to_string_dict(ShadowDict* d);
static std::string value_to_string_array(ShadowArray* a);

// Forward declaration: GC registration (defined in the GC section below).
// Needed early because shadow_string_split/join/format (line ~453) create
// ShadowArray objects that must be GC-tracked.
extern "C" void shadow_gc_register(void* ptr, int32_t kind, int64_t size);
// 前向声明：段分配器（定义于文件后部 GC 段）；RFS 数组/AnyBox 创建处使用。
extern "C" void* shadow_gc_alloc(int32_t size, int32_t kind);

// ShadowDict: 纯 C 开放寻址哈希表（W3：替换 std::unordered_map<std::string, DictValue>）。
// 布局纯 POD {type_tag, head, tail, cap, count, used, slots} —— 带内对象头的前置条件。
// 方案：开放寻址 + 线性探测 + 墓碑删除，容量 2 的幂，FNV-1a 哈希；负载因子 0.7
// （按 count+tombstones 计），墓碑膨胀时原地重建、活条目多时倍增。
// 迭代序 = 插入序（slots 内嵌 prev/next 索引链），确定且跨平台一致。
// 所有权：表拥有 key（strdup）与 val.tag==2 的字符串（DictValue 构造时 strdup）；
// 覆盖/擦除/释放时先释放旧字符串 —— 顺带修复旧 unordered_map erase 泄漏 val.s。
#define SDICT_TOMBSTONE ((char*)(uintptr_t)1)
struct SDictEntry {
    char* key;      // nullptr=空槽; SDICT_TOMBSTONE=已删; 其它=活条目（strdup 拥有）
    uint64_t h;     // 缓存全量哈希（重建时免重算）
    int32_t next;   // 插入序链：下一索引，-1=尾
    int32_t prev;   // 插入序链：上一索引，-1=无
    DictValue val;  // tag==2 的 val.s 由表拥有
};
struct ShadowDict {
    uint32_t type_tag; // 0=array, 1=dict
    int32_t head;      // 插入序链头，-1=空
    int32_t tail;      // 插入序链尾，-1=空
    size_t cap;        // 槽位数（2 的幂），0=空表
    size_t count;      // 活条目数
    size_t used;       // count + 墓碑数（探测占用）
    SDictEntry* slots;
    ShadowDict() : type_tag(1), head(-1), tail(-1), cap(0), count(0), used(0), slots(nullptr) {}
    ~ShadowDict();
};
static uint64_t sdict_hash(const char* s) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a 64
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}
// 重建表：新容量插入（清除墓碑），保持插入序链不变。
static void sdict_rehash(ShadowDict* d, size_t new_cap) {
    SDictEntry* old = d->slots;
    SDictEntry* ns = (SDictEntry*)calloc(new_cap, sizeof(SDictEntry));
    size_t mask = new_cap - 1;
    int32_t new_head = -1, new_prev = -1;
    for (int32_t idx = d->head; idx >= 0; idx = old[idx].next) {
        SDictEntry& oe = old[idx];
        size_t i = (size_t)oe.h & mask;
        while (ns[i].key) i = (i + 1) & mask;
        ns[i].key = oe.key;
        ns[i].h = oe.h;
        ns[i].val = oe.val;
        ns[i].prev = new_prev;
        ns[i].next = -1;
        if (new_prev >= 0) ns[new_prev].next = (int32_t)i;
        else new_head = (int32_t)i;
        new_prev = (int32_t)i;
    }
    free(old);
    d->slots = ns;
    d->cap = new_cap;
    d->head = new_head;
    d->tail = new_prev;
    d->used = d->count;
}
// 查找：命中返回条目指针，未命中返回 nullptr。
static SDictEntry* sdict_lookup(const ShadowDict* d, const char* key, uint64_t h) {
    if (!d->slots) return nullptr;
    size_t mask = d->cap - 1;
    size_t i = (size_t)h & mask;
    for (;;) {
        SDictEntry& e = d->slots[i];
        if (!e.key) return nullptr; // 空槽 → 不存在
        if (e.key != SDICT_TOMBSTONE && e.h == h && strcmp(e.key, key) == 0) return &e;
        i = (i + 1) & mask;
    }
}
static DictValue* sdict_get(ShadowDict* d, const char* key) {
    if (!d || !key) return nullptr;
    SDictEntry* e = sdict_lookup(d, key, sdict_hash(key));
    return e ? &e->val : nullptr;
}
// 插入或定位：返回可写的值指针；键不存在则插入（默认值 int 0）。
static DictValue* sdict_put(ShadowDict* d, const char* key) {
    uint64_t h = sdict_hash(key);
    SDictEntry* e = sdict_lookup(d, key, h);
    if (e) return &e->val;
    if (d->cap == 0 || (d->used + 1) * 10 >= d->cap * 7) {
        size_t nc = d->cap ? d->cap : 8;
        while ((d->count + 1) * 10 >= nc * 7) nc *= 2; // 活条目驱动倍增；墓碑膨胀则等容重建
        sdict_rehash(d, nc);
    }
    size_t mask = d->cap - 1;
    size_t i = (size_t)h & mask;
    size_t tomb = SIZE_MAX;
    while (d->slots[i].key) {
        if (d->slots[i].key == SDICT_TOMBSTONE && tomb == SIZE_MAX) tomb = i;
        i = (i + 1) & mask;
    }
    size_t ins = (tomb != SIZE_MAX) ? tomb : i;
    if (d->slots[ins].key == SDICT_TOMBSTONE) d->used--; // 复用墓碑槽
    d->slots[ins].key = strdup(key);
    d->slots[ins].h = h;
    d->slots[ins].val = DictValue();
    d->slots[ins].prev = d->tail;                        // 追加到插入序链尾
    d->slots[ins].next = -1;
    if (d->tail >= 0) d->slots[d->tail].next = (int32_t)ins;
    else d->head = (int32_t)ins;
    d->tail = (int32_t)ins;
    d->count++;
    d->used++;
    return &d->slots[ins].val;
}
// 覆盖写入：先释放被覆盖的旧字符串，再做浅拷贝（v.val.s 所有权转归表）。
static void sdict_set(ShadowDict* d, const char* key, const DictValue& v) {
    DictValue* slot = sdict_put(d, key);
    if (slot->tag == 2 && slot->val.s) free((void*)slot->val.s);
    *slot = v;
}
// 擦除：命中返回 1；释放 key 与 tag==2 的字符串。
static int sdict_erase(ShadowDict* d, const char* key) {
    if (!d || !key) return 0;
    SDictEntry* e = sdict_lookup(d, key, sdict_hash(key));
    if (!e) return 0;
    if (e->prev >= 0) d->slots[e->prev].next = e->next;
    else d->head = e->next;
    if (e->next >= 0) d->slots[e->next].prev = e->prev;
    else d->tail = e->prev;
    free(e->key);
    if (e->val.tag == 2) free((void*)e->val.val.s);
    e->key = SDICT_TOMBSTONE;
    e->h = 0;
    e->prev = e->next = -1;
    e->val = DictValue();
    d->count--;
    return 1;
}
// 释放表内全部资源（键、字符串值、槽位数组）。
static void sdict_release(ShadowDict* d) {
    if (!d->slots) return;
    for (size_t i = 0; i < d->cap; i++) {
        SDictEntry& e = d->slots[i];
        if (e.key && e.key != SDICT_TOMBSTONE) {
            free(e.key);
            if (e.val.tag == 2) free((void*)e.val.val.s);
        }
    }
    free(d->slots);
    d->slots = nullptr;
    d->cap = d->count = d->used = 0;
    d->head = d->tail = -1;
}
inline ShadowDict::~ShadowDict() { sdict_release(this); }

struct ShadowArray {
    uint32_t type_tag; // 0=array, 1=dict, 2=set
    DictValue* data;
    size_t len;
    size_t cap;
    ShadowArray() : type_tag(0), data(nullptr), len(0), cap(0) {}
    ~ShadowArray() { free(data); }
    size_t size() const { return len; }
    void push_back(const DictValue& v) {
        if (len >= cap) { size_t nc = cap == 0 ? 8 : cap * 2; data = (DictValue*)realloc(data, nc * sizeof(DictValue)); cap = nc; }
        data[len++] = v;
    }
    DictValue& operator[](size_t i) { return data[i]; }
    const DictValue& operator[](size_t i) const { return data[i]; }
    void resize(size_t n) { if (n > cap) { data = (DictValue*)realloc(data, n * sizeof(DictValue)); cap = n; } len = n; }
    bool empty() const { return len == 0; }
    DictValue& back() { return data[len - 1]; }
    void pop_back() { if (len > 0) len--; }
};

// ShadowSet: 纯 C 字符串集（W3：替换 std::unordered_set<std::string>）。
// 与 ShadowDict 同构的开放寻址表，仅存键；迭代序 = 插入序。
struct SSetEntry {
    char* key;      // nullptr=空槽; SDICT_TOMBSTONE=已删; 其它=活元素（strdup 拥有）
    uint64_t h;
    int32_t next;   // 插入序链：下一索引，-1=尾
    int32_t prev;   // 插入序链：上一索引，-1=无
};
struct ShadowSet {
    uint32_t type_tag; // 0=array, 1=dict, 2=set
    int32_t head;      // 插入序链头，-1=空
    int32_t tail;      // 插入序链尾，-1=空
    size_t cap;        // 槽位数（2 的幂），0=空表
    size_t count;      // 活元素数
    size_t used;       // count + 墓碑数（探测占用）
    SSetEntry* slots;
    ShadowSet() : type_tag(2), head(-1), tail(-1), cap(0), count(0), used(0), slots(nullptr) {}
    ~ShadowSet();
};
static void sset_rehash(ShadowSet* s, size_t new_cap) {
    SSetEntry* old = s->slots;
    SSetEntry* ns = (SSetEntry*)calloc(new_cap, sizeof(SSetEntry));
    size_t mask = new_cap - 1;
    int32_t new_head = -1, new_prev = -1;
    for (int32_t idx = s->head; idx >= 0; idx = old[idx].next) {
        SSetEntry& oe = old[idx];
        size_t i = (size_t)oe.h & mask;
        while (ns[i].key) i = (i + 1) & mask;
        ns[i].key = oe.key;
        ns[i].h = oe.h;
        ns[i].prev = new_prev;
        ns[i].next = -1;
        if (new_prev >= 0) ns[new_prev].next = (int32_t)i;
        else new_head = (int32_t)i;
        new_prev = (int32_t)i;
    }
    free(old);
    s->slots = ns;
    s->cap = new_cap;
    s->head = new_head;
    s->tail = new_prev;
    s->used = s->count;
}
static SSetEntry* sset_lookup(const ShadowSet* s, const char* key, uint64_t h) {
    if (!s->slots) return nullptr;
    size_t mask = s->cap - 1;
    size_t i = (size_t)h & mask;
    for (;;) {
        SSetEntry& e = s->slots[i];
        if (!e.key) return nullptr; // 空槽 → 不存在
        if (e.key != SDICT_TOMBSTONE && e.h == h && strcmp(e.key, key) == 0) return &e;
        i = (i + 1) & mask;
    }
}
static void sset_add(ShadowSet* s, const char* key) {
    uint64_t h = sdict_hash(key); // 与 dict 共用 FNV-1a
    if (sset_lookup(s, key, h)) return; // 已存在
    if (s->cap == 0 || (s->used + 1) * 10 >= s->cap * 7) {
        size_t nc = s->cap ? s->cap : 8;
        while ((s->count + 1) * 10 >= nc * 7) nc *= 2;
        sset_rehash(s, nc);
    }
    size_t mask = s->cap - 1;
    size_t i = (size_t)h & mask;
    size_t tomb = SIZE_MAX;
    while (s->slots[i].key) {
        if (s->slots[i].key == SDICT_TOMBSTONE && tomb == SIZE_MAX) tomb = i;
        i = (i + 1) & mask;
    }
    size_t ins = (tomb != SIZE_MAX) ? tomb : i;
    if (s->slots[ins].key == SDICT_TOMBSTONE) s->used--;
    s->slots[ins].key = strdup(key);
    s->slots[ins].h = h;
    s->slots[ins].prev = s->tail;
    s->slots[ins].next = -1;
    if (s->tail >= 0) s->slots[s->tail].next = (int32_t)ins;
    else s->head = (int32_t)ins;
    s->tail = (int32_t)ins;
    s->count++;
    s->used++;
}
static int sset_erase(ShadowSet* s, const char* key) {
    if (!s || !key) return 0;
    SSetEntry* e = sset_lookup(s, key, sdict_hash(key));
    if (!e) return 0;
    if (e->prev >= 0) s->slots[e->prev].next = e->next;
    else s->head = e->next;
    if (e->next >= 0) s->slots[e->next].prev = e->prev;
    else s->tail = e->prev;
    free(e->key);
    e->key = SDICT_TOMBSTONE;
    e->h = 0;
    e->prev = e->next = -1;
    s->count--;
    return 1;
}
static void sset_release(ShadowSet* s) {
    if (!s->slots) return;
    for (size_t i = 0; i < s->cap; i++) {
        SSetEntry& e = s->slots[i];
        if (e.key && e.key != SDICT_TOMBSTONE) free(e.key);
    }
    free(s->slots);
    s->slots = nullptr;
    s->cap = s->count = s->used = 0;
    s->head = s->tail = -1;
}
inline ShadowSet::~ShadowSet() { sset_release(this); }

// ShadowArena: 纯 C 块列表（W3：替换 3 个 std::vector）。
struct ShadowArena {
    uint32_t type_tag; // 3=arena
    size_t block_size;
    size_t count;      // 块数
    size_t cap;        // blocks/caps/used 数组容量
    void** blocks;
    size_t* caps;
    size_t* used;
    size_t total_used;
    size_t high_water;
    ShadowArena(size_t bs) : type_tag(3), block_size(bs), count(0), cap(0),
                             blocks(nullptr), caps(nullptr), used(nullptr),
                             total_used(0), high_water(0) {}
    ~ShadowArena();
};
static void sarena_push(ShadowArena* ar, void* b, size_t c) {
    if (ar->count >= ar->cap) {
        size_t nc = ar->cap ? ar->cap * 2 : 8;
        ar->blocks = (void**)realloc(ar->blocks, nc * sizeof(void*));
        ar->caps = (size_t*)realloc(ar->caps, nc * sizeof(size_t));
        ar->used = (size_t*)realloc(ar->used, nc * sizeof(size_t));
        ar->cap = nc;
    }
    ar->blocks[ar->count] = b;
    ar->caps[ar->count] = c;
    ar->used[ar->count] = 0;
    ar->count++;
}
static void sarena_release(ShadowArena* ar) {
    for (size_t i = 0; i < ar->count; i++) free(ar->blocks[i]);
    free(ar->blocks);
    free(ar->caps);
    free(ar->used);
    ar->blocks = nullptr;
    ar->caps = nullptr;
    ar->used = nullptr;
    ar->count = ar->cap = 0;
    ar->total_used = 0;
}
inline ShadowArena::~ShadowArena() { sarena_release(this); }

// miniz: 用于 .spk 包的 DEFLATE 压缩（miniz.h 是 self-contained，会自动 include 它需要的一切）
// 头与源均在 bootstrap/（miniz.h + miniz.c），经 -I bootstrap 解析；
// 不得再引 build/linux/miniz-3.1.2/ —— build/ 被 gitignore，那会让本文件在干净检出上无法编译，
// 只能退回链接预编译 .o（曾因此发生源码已修、.o 陈旧的误判）。
#include "miniz.h"

// ── Crash diagnostics (no external debugger needed) ──
// Installs an UnhandledExceptionFilter that prints RIP, the containing
// module's load base, and the precise offset so we can map the crash to a
// generated function via `llvm-nm -n <exe>` / `llvm-objdump -d <o>`.
#ifdef _WIN32
static LONG WINAPI shadow_crash_filter(EXCEPTION_POINTERS* ep) {
    DWORD64 rip = ep->ContextRecord ? ep->ContextRecord->Rip : 0;
    HMODULE hMod = NULL;
    DWORD64 base = 0;
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          (LPCSTR)(uintptr_t)rip, &hMod)) {
        base = (DWORD64)hMod;
    }
    fprintf(stderr, "\nSHADOW-CRASH rip=0x%llx base=0x%llx offset=0x%llx code=0x%lx\n",
            (unsigned long long)rip, (unsigned long long)base,
            (unsigned long long)(rip - base),
            ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
    if (ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "  FAULTING_ADDR=0x%llx\n",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    void* frames[48];
    unsigned short n = CaptureStackBackTrace(0, 48, frames, NULL);
    char modbuf[MAX_PATH];
    for (unsigned short i = 0; i < n; i++) {
        DWORD64 fa = (DWORD64)frames[i];
        HMODULE fm = NULL;
        const char* modname = "?";
        if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                              (LPCSTR)(uintptr_t)fa, &fm)) {
            if (GetModuleFileNameA(fm, modbuf, MAX_PATH)) {
                const char* p = strrchr(modbuf, '\\');
                modname = p ? p + 1 : modbuf;
            }
        }
        fprintf(stderr, "  f%u=0x%llx %s+0x%llx\n", i,
                (unsigned long long)fa, modname,
                (unsigned long long)(fa - (DWORD64)fm));
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
struct ShadowCrashInstaller {
    ShadowCrashInstaller() { SetUnhandledExceptionFilter(shadow_crash_filter); }
};
static ShadowCrashInstaller g_crash_installer;
#else
// Linux: install a SIGSEGV handler for crash diagnostics.
#include <execinfo.h>
#include <ucontext.h>
static void shadow_crash_filter(int, siginfo_t* info, void* ctx) {
    ucontext_t* uc = (ucontext_t*)ctx;
    uintptr_t rip = uc->uc_mcontext.gregs[REG_RIP];
    fprintf(stderr, "\nSHADOW-CRASH sig=SIGSEGV fault=0x%llx RIP=0x%llx\n",
            (unsigned long long)(info ? info->si_addr : 0),
            (unsigned long long)rip);
    fflush(stderr);
    _exit(1);
}
struct ShadowCrashInstaller {
    ShadowCrashInstaller() {
        struct sigaction sa;
        sa.sa_sigaction = shadow_crash_filter;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
    }
};
static ShadowCrashInstaller g_crash_installer;
#endif

// AnyBox: heap-allocated box for scalar values stored in `any`.
// `magic` lets the runtime's any-dispatch (typeof/index/member/print/unbox)
// reliably tell a boxed scalar apart from a raw heap object — because
// ShadowArray.type_tag==0 / ShadowDict.type_tag==1 would otherwise collide
// with AnyBox.tag 0/1. See shadow-cpp README "自举编译器支持原则".
#define ANYBOX_MAGIC 0x5A5A5A5A
struct AnyBox { int32_t magic; int32_t tag; int64_t value; };

// Safe pointer validation: check if ptr points to committed, readable memory.
// Used before AnyBox magic checks to prevent access violations when a raw
// integer value (>= 10000) is misinterpreted as an AnyBox pointer.
static bool is_valid_ptr(void* ptr) {
    if (!ptr) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
#else
    (void)ptr;
    return true; // used only by conservative GC (off by default); safepoint GC doesn't call this
#endif
}

// L4 HashMap: 已与 ShadowDict 统一（2026-07-28）
// 历史上 shadow_hashmap_* 使用独立的 ShadowHashMap（string→string）；
// 现已统一到 ShadowDict（variant 值），使 hashmap_* API 与 dict<K,V> 语法共享同一运行时表示。
// ShadowDict 在下方定义（forward declaration 在 line ~610），其首字段 type_tag==1 供 shadow_free 识别。
// 旧的 ShadowHashMap struct 已删除，避免两套内存布局混淆。

// Ã¢ÂÂÃ¢ÂÂ Helper: duplicate string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
static char* dup_str(const char* s) {
    if (!s) return nullptr;
    size_t len = strlen(s);
    char* d = (char*)malloc(len + 1);
    if (!d) return nullptr;
    memcpy(d, s, len + 1);
    return d;
}
static char* dup_str(const std::string& s) {
    return dup_str(s.c_str());
}

// 前向声明：把路径中的 '\' 归一成 '/'（Linux 下 main_path_join 等产生的反斜杠路径也能打开）。
// 定义见文件下方 shadow_path_to_slashes。
static std::string shadow_path_to_slashes(const std::string& input);

// Ã¢ÂÂÃ¢ÂÂ File Operations Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

// Read entire file into a string
extern "C" const char* shadow_read_file(const char* path) {
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    FILE* f = fopen(np.c_str(), "rb");
    if (!f) return dup_str("");  // empty string, not nullptr — null would crash len()

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        fclose(f);
        return nullptr;
    }

    size_t read_len = fread(buf, 1, len, f);
    buf[read_len] = '\0';
    fclose(f);

    return buf;
}

// Write string to file
extern "C" int shadow_write_file(const char* path, const char* content) {
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    FILE* f = fopen(np.c_str(), "w");
    if (!f) return 0;

    fputs(content, f);
    fclose(f);
    return 1;
}

// Check if file exists
extern "C" int shadow_file_exists(const char* path) {
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    FILE* f = fopen(np.c_str(), "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

// Get file modification time (seconds since epoch).
// Returns -1 if the file does not exist or stat fails.
// Used by LSP polling-based file watcher (P10-6).
extern "C" int32_t shadow_file_mtime(const char* path) {
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    struct stat st;
    if (stat(np.c_str(), &st) != 0) { return -1; }
    return (int32_t)st.st_mtime;
}

// Delete file
extern "C" int shadow_delete_file(const char* path) {
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    return remove(np.c_str()) == 0 ? 1 : 0;
}

// Rename file (atomic on Windows via MoveFileExA with MOVEFILE_REPLACE_EXISTING)
// Used by .lu cache atomic write: write to .tmp, then rename to .lu
extern "C" int shadow_rename_file(const char* old_path, const char* new_path) {
#ifdef _WIN32
    return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
#else
    return rename(old_path, new_path) == 0 ? 1 : 0;
#endif
}

// List directory contents (semicolon-separated)
extern "C" const char* shadow_list_dir(const char* path) {
    // Normalize path separators first so backslash-joined paths produced by
    // main_path_join (always '\') also work on POSIX opendir().
    std::string np = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    std::string result;
#ifdef _WIN32
    std::string search_path = np + "/*";
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
                if (!result.empty()) result += ";";
                result += fd.cFileName;
            }
        } while (FindNextFile(hFind, &fd));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(np.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                if (!result.empty()) result += ";";
                result += entry->d_name;
            }
        }
        closedir(dir);
    }
#endif

    char* buf = (char*)malloc(result.size() + 1);
    if (buf) {
        strcpy(buf, result.c_str());
        return buf;
    }
    return nullptr;
}

// Ã¢ÂÂÃ¢ÂÂ Network Operations Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

// HTTP GET request
extern "C" const char* shadow_http_get(const char* url) {
    if (!url) return strdup("");
#ifdef _WIN32
    // Simple WinHTTP GET implementation
    std::string url_s(url);
    std::wstring wurl(url, url + strlen(url));
    
    HINTERNET hSession = WinHttpOpen(L"Shadow/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return strdup("");
    
    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    DWORD urlLen = (DWORD)wurl.size();
    wchar_t hostBuf[256] = {0}, pathBuf[1024] = {0}, schemeBuf[16] = {0};
    uc.lpszHostName = hostBuf;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = pathBuf;   uc.dwUrlPathLength  = 1024;
    uc.lpszScheme   = schemeBuf; uc.dwSchemeLength   = 16;
    
    if (!WinHttpCrackUrl(wurl.c_str(), urlLen, 0, &uc)) {
        WinHttpCloseHandle(hSession);
        return strdup("");
    }
    
    HINTERNET hConnect = WinHttpConnect(hSession, uc.lpszHostName,
        uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80), 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return strdup(""); }
    
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", 
        uc.lpszUrlPath ? uc.lpszUrlPath : L"/", NULL, NULL, NULL, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return strdup(""); }
    
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return strdup("");
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return strdup("");
    }
    
    std::string result;
    char buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0) {
                buf[bytesRead] = '\0';
        result += buf;
        bytesRead = 0;
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return strdup(result.c_str());
#else
    return strdup("");
#endif
}
extern "C" const char* shadow_http_post(const char* url, const char* body) {
    if (!url) return strdup("");
#ifdef _WIN32
    std::wstring wurl(url, url + strlen(url));
    
    HINTERNET hSession = WinHttpOpen(L"Shadow/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hSession) return strdup("");
    
    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    DWORD urlLen = (DWORD)wurl.size();
    wchar_t hostBuf[256] = {0}, pathBuf[1024] = {0}, schemeBuf[16] = {0};
    uc.lpszHostName = hostBuf;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = pathBuf;   uc.dwUrlPathLength  = 1024;
    uc.lpszScheme   = schemeBuf; uc.dwSchemeLength   = 16;
    
    if (!WinHttpCrackUrl(wurl.c_str(), urlLen, 0, &uc)) {
        WinHttpCloseHandle(hSession);
        return strdup("");
    }
    
    HINTERNET hConnect = WinHttpConnect(hSession, uc.lpszHostName,
        uc.nPort ? uc.nPort : (uc.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80), 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return strdup(""); }
    
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", 
        uc.lpszUrlPath ? uc.lpszUrlPath : L"/", NULL, NULL, NULL, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return strdup(""); }
    
    LPCSTR pBody = NULL;
    DWORD bodyLen = 0;
    std::string bodyStr;
    if (body) { bodyStr = body; pBody = bodyStr.c_str(); bodyLen = (DWORD)bodyStr.size(); }
    
    // Send request
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)pBody, bodyLen, bodyLen, 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return strdup("");
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return strdup("");
    }
    
    std::string result;
    char buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpReadData(hRequest, buf, sizeof(buf) - 1, &bytesRead) && bytesRead > 0) {
        buf[bytesRead] = '\0';
        result += buf;
        bytesRead = 0;
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return strdup(result.c_str());
#else
    return strdup("");
#endif
}
extern "C" const char* shadow_substring(const char* s, int64_t start, int64_t end_pos, int use_end) {
    if (!s) return dup_str("");
    std::string str(s);
    int64_t len = (int64_t)str.size();
    if (start < 0) start = std::max<int64_t>(0, len + start);
    if (start >= len) return dup_str("");
    if (use_end) {
        if (end_pos < 0) end_pos = std::max<int64_t>(0, len + end_pos);
        if (end_pos <= start) return dup_str("");
        end_pos = (end_pos < len) ? end_pos : len;
        return dup_str(str.substr(start, end_pos - start));
    }
    return dup_str(str.substr(start));
}

// L4：3 参数版本的 substr（包装 shadow_substring，use_end=1）
// 供 shadow 源码 substr2(s, start, end) 调用，避免修改 codegen 调用约定
extern "C" const char* shadow_string_substr(const char* s, int64_t start, int64_t end_pos) {
    return shadow_substring(s, start, end_pos, 1);
}

extern "C" int shadow_string_contains(const char* s, const char* needle) {
    if (!s || !needle) return 0;
    return std::string(s).find(needle) != std::string::npos ? 1 : 0;
}

extern "C" int64_t shadow_string_index_of(const char* s, const char* needle) {
    if (!s || !needle) return -1;
    auto pos = std::string(s).find(needle);
    return pos == std::string::npos ? -1 : (int64_t)pos;
}

extern "C" const char* shadow_string_replace(const char* s, const char* old_s, const char* new_s) {
    if (!s) return dup_str("");
    std::string result(s);
    std::string o(old_s ? old_s : "");
    std::string n_str(new_s ? new_s : "");
    if (o.empty()) return dup_str(result);
    size_t pos = 0;
    while ((pos = result.find(o, pos)) != std::string::npos) {
        result.replace(pos, o.size(), n_str);
        pos += n_str.size();
    }
    return dup_str(result);
}

extern "C" const char* shadow_string_trim(const char* s) {
    if (!s) return dup_str("");
    std::string str(s);
    size_t l = str.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return dup_str("");
    size_t r = str.find_last_not_of(" \t\r\n");
    return dup_str(str.substr(l, r - l + 1));
}

extern "C" const char* shadow_to_upper(const char* s) {
    if (!s) return dup_str("");
    std::string r(s);
    for (char& c : r) c = (char)std::toupper((unsigned char)c);
    return dup_str(r);
}

extern "C" const char* shadow_to_lower(const char* s) {
    if (!s) return dup_str("");
    std::string r(s);
    for (char& c : r) c = (char)std::tolower((unsigned char)c);
    return dup_str(r);
}

extern "C" const char* shadow_char_at(const char* s, int64_t idx) {
    if (!s) return dup_str("");
    std::string str(s);
    int64_t len = (int64_t)str.size();
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return dup_str("");
    return dup_str(std::string(1, str[idx]));
}

// ── String processing: split / join / format ──
// These functions use the shadow-lang array layout (NOT C++ ShadowArray):
//   [len:int32@0][cap:int32@4][elem_size:int32@8][data@12+]
// This matches what shadow_array_new / shadow_array_len / shadow_array_get_ptr
// in runtime_lib.shadow expect.

// shadow_string_split: split string by delimiter, return shadow-lang array of strings
// s: input string, delim: delimiter (must not be empty)
// returns: malloc'd shadow-lang array (elem_size=8, each element is char*)
extern "C" void* shadow_string_split(const char* s, const char* delim) {
    if (!s) s = "";
    if (!delim || delim[0] == '\0') {
        // empty delimiter: return array with the whole string as single element
        void* arr = malloc(12 + 8);
        *(int32_t*)((char*)arr + 0) = 1;   // length
        *(int32_t*)((char*)arr + 4) = 1;   // capacity
        *(int32_t*)((char*)arr + 8) = 8;   // elem_size (pointer)
        *(void**)((char*)arr + 12) = (void*)dup_str(s);
        return arr;
    }
    std::string str(s);
    std::string d(delim);
    // Count parts
    int count = 0;
    size_t start = 0;
    size_t end = str.find(d);
    while (end != std::string::npos) {
        count++;
        start = end + d.size();
        end = str.find(d, start);
    }
    count++;  // last part

    // Allocate shadow-lang array: header(12) + count * 8
    void* arr = malloc(12 + (size_t)count * 8);
    *(int32_t*)((char*)arr + 0) = count;   // length
    *(int32_t*)((char*)arr + 4) = count;   // capacity
    *(int32_t*)((char*)arr + 8) = 8;       // elem_size (pointer)

    // Store parts
    start = 0;
    end = str.find(d);
    int idx = 0;
    while (end != std::string::npos) {
        *(void**)((char*)arr + 12 + (size_t)idx * 8) =
            (void*)dup_str(str.substr(start, end - start));
        start = end + d.size();
        end = str.find(d, start);
        idx++;
    }
    *(void**)((char*)arr + 12 + (size_t)idx * 8) =
        (void*)dup_str(str.substr(start));

    return arr;
}

// shadow_string_join: join array of strings with separator
// arr_ptr: shadow-lang array (elements are char* pointers)
// sep: separator string
// returns: joined string (caller owns)
extern "C" const char* shadow_string_join(void* arr_ptr, const char* sep) {
    if (!arr_ptr) return dup_str("");
    // Shadow-lang array layout: [len@0][cap@4][elem_size@8][data@12+]
    int32_t arr_len = *(int32_t*)((char*)arr_ptr + 0);
    int32_t es = *(int32_t*)((char*)arr_ptr + 8);
    if (es == 0) es = 8;  // safety
    std::string sep_str(sep ? sep : "");
    std::string result;
    for (int32_t i = 0; i < arr_len; i++) {
        if (i > 0) result += sep_str;
        void* elem = *(void**)((char*)arr_ptr + 12 + (size_t)i * es);
        if (elem) result += std::string((const char*)elem);
    }
    return dup_str(result);
}

// shadow_string_format: format string with {0}, {1}, ... placeholders
// fmt: format string with {N} placeholders ({{ and }} for literal { })
// args_ptr: shadow-lang array of string elements (char* pointers)
// returns: formatted string (caller owns)
extern "C" const char* shadow_string_format(const char* fmt, void* args_ptr) {
    if (!fmt) return dup_str("");
    if (!args_ptr) return dup_str(fmt);
    // Shadow-lang array layout: [len@0][cap@4][elem_size@8][data@12+]
    int32_t arr_len = *(int32_t*)((char*)args_ptr + 0);
    int32_t es = *(int32_t*)((char*)args_ptr + 8);
    if (es == 0) es = 8;  // safety
    std::string f(fmt);
    std::string result;
    size_t i = 0;
    while (i < f.size()) {
        if (f[i] == '{') {
            // check for escape {{
            if (i + 1 < f.size() && f[i+1] == '{') {
                result += '{';
                i += 2;
                continue;
            }
            // find closing }
            size_t close = f.find('}', i + 1);
            if (close == std::string::npos) {
                result += f[i];
                i++;
                continue;
            }
            // parse index
            std::string idx_str = f.substr(i + 1, close - i - 1);
            int idx = 0;
            bool valid = true;
            for (char c : idx_str) {
                if (c >= '0' && c <= '9') {
                    idx = idx * 10 + (c - '0');
                } else {
                    valid = false;
                    break;
                }
            }
            if (valid && idx >= 0 && idx < arr_len) {
                void* elem = *(void**)((char*)args_ptr + 12 + (size_t)idx * es);
                if (elem) result += std::string((const char*)elem);
            } else {
                // invalid index: keep placeholder as-is
                result += f.substr(i, close - i + 1);
            }
            i = close + 1;
        } else if (f[i] == '}') {
            // check for escape }}
            if (i + 1 < f.size() && f[i+1] == '}') {
                result += '}';
                i += 2;
                continue;
            }
            result += f[i];
            i++;
        } else {
            result += f[i];
            i++;
        }
    }
    return dup_str(result);
}

// ── L4 HashMap Operations (string → string, dict handle) ──
// 独立生命周期：shadow_hashmap_new 分配，shadow_hashmap_free 释放
// 不参与 GC（调用方负责 free）；适合计数器、缓存、配置映射等场景
// 2026-07-28：与 dict<K,V> 语法统一运行时表示 —— hashmap_* 现操作 ShadowDict
// （值存为 DictValue 的 std::string 变体），与 dict[key] 索引访问共享同一内存布局。
// 这使得 `let d: dict<string, string> = hashmap_new(); d[k] = v;` 端到端可用。

extern "C" void* shadow_hashmap_new() {
    return new ShadowDict();
}

extern "C" void shadow_hashmap_insert(void* map, const char* key, const char* value) {
    if (!map || !key) return;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    sdict_set(m, key, DictValue(value ? value : ""));
}

// dict<K, int/long/bool/date/timestamp> 的 hashmap_insert 变体。
// codegen 按第 3 个实参的类型分派到此函数；否则 int 会被当作 const char*
// 解引用（崩溃）。值以 int64_t 变体存放，与 shadow_dict_set/get_int 共享布局。
extern "C" void shadow_hashmap_insert_int(void* map, const char* key, int64_t value) {
    if (!map || !key) return;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    sdict_set(m, key, DictValue(value));
}

extern "C" const char* shadow_hashmap_get(void* map, const char* key) {
    if (!map || !key) return dup_str("");
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    DictValue* v = sdict_get(m, key);
    if (!v) return dup_str("");
    // 仅当值为 string 变体时返回；非 string 变体返回其字符串表示（与原行为一致）
    if (v->tag == 2) {
        return dup_str(v->val.s);
    }
    return dup_str(value_to_string(*v).c_str());
}

extern "C" int shadow_hashmap_contains(void* map, const char* key) {
    if (!map || !key) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return sdict_get(m, key) != nullptr ? 1 : 0;
}

extern "C" int shadow_hashmap_remove(void* map, const char* key) {
    if (!map || !key) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return sdict_erase(m, key);
}

extern "C" int shadow_hashmap_size(void* map) {
    if (!map) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return (int)m->count;
}

extern "C" void shadow_hashmap_free(void* map) {
    if (!map) return;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    delete m;
}

// ── Set operations (ShadowSet: 纯 C 开放寻址字符串集) ──
// set<T> 容器的运行时实现。
// 统一存储为 string：string 元素直接存，int/long 元素序列化为 string。
// 这样 set<int> 和 set<string> 可以共存于同一容器类型。

extern "C" void* shadow_set_new() {
    return new ShadowSet();
}

extern "C" int shadow_set_add(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    sset_add(s, value);
    return 0;
}

extern "C" int shadow_set_contains(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return sset_lookup(s, value, sdict_hash(value)) != nullptr ? 1 : 0;
}

extern "C" int shadow_set_remove(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return sset_erase(s, value);
}

extern "C" int shadow_set_size(void* set) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return (int)s->count;
}

extern "C" void shadow_set_free(void* set) {
    if (!set) return;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    delete s;
}

// int 元素版本：序列化为十进制 string 存储
extern "C" int shadow_set_add_int(void* set, int64_t value) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    sset_add(s, buf);
    return 0;
}

extern "C" int shadow_set_contains_int(void* set, int64_t value) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return sset_lookup(s, buf, sdict_hash(buf)) != nullptr ? 1 : 0;
}

extern "C" int shadow_set_remove_int(void* set, int64_t value) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return sset_erase(s, buf);
}

// shadow_set_at_string: return string element at given index (strdup'd, caller owns).
// Used by for (x in set) iteration. Order is insertion order (deterministic).
extern "C" const char* shadow_set_at_string(void* set, int32_t idx) {
    if (!set) return nullptr;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    if (idx < 0 || (size_t)idx >= s->count) {
        fprintf(stderr, "error: set index %d out of bounds (size=%zu)\n",
                idx, s->count);
        return nullptr;
    }
    int32_t i = s->head;
    for (int32_t k = 0; k < idx && i >= 0; k++) i = s->slots[i].next;
    if (i < 0) return nullptr;
    return strdup(s->slots[i].key);
}

// shadow_set_at_int: return int64 element at given index (parsed from stored string).
// Used by for (x in set<int>) / for (x in set<long>) iteration.
extern "C" int64_t shadow_set_at_int(void* set, int32_t idx) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    if (idx < 0 || (size_t)idx >= s->count) {
        fprintf(stderr, "error: set index %d out of bounds (size=%zu)\n",
                idx, s->count);
        return 0;
    }
    int32_t i = s->head;
    for (int32_t k = 0; k < idx && i >= 0; k++) i = s->slots[i].next;
    if (i < 0) return 0;
    return (int64_t)strtoll(s->slots[i].key, nullptr, 10);
}

// Ã¢ÂÂÃ¢ÂÂ JSON Operations Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

static std::string json_get_raw(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t kpos = json.find(searchKey);
    if (kpos == std::string::npos) return "";
    size_t colon = json.find(':', kpos + searchKey.size());
    if (colon == std::string::npos) return "";
    size_t vstart = colon + 1;
    while (vstart < json.size() &&
           (json[vstart]==' ' || json[vstart]=='\t' ||
            json[vstart]=='\r' || json[vstart]=='\n')) {
        ++vstart;
    }
    if (vstart >= json.size()) return "";
    char first = json[vstart];
    if (first == '"') {
        size_t end = vstart + 1;
        while (end < json.size()) {
            if (json[end] == '\\') { end += 2; continue; }
            if (json[end] == '"') break;
            ++end;
        }
        return json.substr(vstart + 1, end - vstart - 1);
    }
    if (first == '{' || first == '[') {
        char open = first;
        char close = (first == '{') ? '}' : ']';
        int depth = 0;
        bool inStr = false;
        size_t end = vstart;
        while (end < json.size()) {
            char c2 = json[end];
            if (!inStr) {
                if (c2 == '"')      inStr = true;
                else if (c2 == open)  ++depth;
                else if (c2 == close) { if (--depth == 0) { ++end; break; } }
            } else {
                if (c2 == '\\') ++end;
                else if (c2 == '"') inStr = false;
            }
            ++end;
        }
        return json.substr(vstart, end - vstart);
    }
    size_t end = vstart;
    while (end < json.size() &&
           json[end] != ',' && json[end] != '}' &&
           json[end] != ']' && json[end] != ' ' && json[end] != '\n') {
        ++end;
    }
    return json.substr(vstart, end - vstart);
}

extern "C" double shadow_json_get_float(const char* json, const char* key) {
    std::string v = json_get_raw(json ? json : "", key ? key : "");
    if (v.empty()) return 0.0;
    try { return std::stod(v); } catch (...) { return 0.0; }
}

extern "C" int shadow_json_get_bool(const char* json, const char* key) {
    std::string v = json_get_raw(json ? json : "", key ? key : "");
    return (v == "true") ? 1 : 0;
}

extern "C" int64_t shadow_json_array_len(const char* json) {
    if (!json) return 0;
    std::string s(json);
    size_t st = s.find('[');
    if (st == std::string::npos) return 0;
    int depth = 0;
    bool inStr = false;
    int count = 0;
    bool saw = false;
    for (size_t i = st; i < s.size(); ++i) {
        char c = s[i];
        if (!inStr) {
            if (c == '"') {
                inStr = true;
                if (depth == 1) saw = true;
            } else if (c == '[' || c == '{') {
                if (++depth == 1) saw = false;
                else if (depth == 2) saw = true;
            } else if (c == ']' || c == '}') {
                if (c == ']' && depth == 1) { if (saw) ++count; break; }
                --depth;
            } else if (c == ',' && depth == 1) {
                ++count; saw = false;
            } else if (depth == 1 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                saw = true;
            }
        } else {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
        }
    }
    return count;
}

extern "C" const char* shadow_json_array_get(const char* json, int64_t target) {
    if (!json) return dup_str("");
    std::string s(json);
    size_t sp = s.find('[');
    if (sp == std::string::npos) return dup_str("");
    int depth = 0;
    bool inStr = false;
    int64_t idx = -1;
    size_t item_s = std::string::npos;
    for (size_t i = sp; i < s.size(); ++i) {
        char c = s[i];
        if (!inStr) {
            if (c == '"') {
                inStr = true;
                if (depth == 1 && item_s == std::string::npos) { ++idx; item_s = i; }
            } else if (c == '[' || c == '{') {
                ++depth;
                if (depth == 2 && item_s == std::string::npos) { ++idx; item_s = i; }
            } else if (c == ']' || c == '}') {
                if (c == ']' && depth == 1) {
                    if (idx == target && item_s != std::string::npos) {
                        std::string it = s.substr(item_s, i - item_s);
                        if (!it.empty() && it.front() == '"' && it.back() == '"')
                            it = it.substr(1, it.size() - 2);
                        return dup_str(it);
                    }
                    break;
                }
                --depth;
            } else if (c == ',' && depth == 1) {
                if (idx == target && item_s != std::string::npos) {
                    std::string it = s.substr(item_s, i - item_s);
                    size_t e = it.find_last_not_of(" \t\r\n");
                    if (e != std::string::npos) it = it.substr(0, e + 1);
                    if (!it.empty() && it.front() == '"' && it.back() == '"')
                        it = it.substr(1, it.size() - 2);
                    return dup_str(it);
                }
                item_s = std::string::npos;
            } else if (depth == 1 && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                if (item_s == std::string::npos) { ++idx; item_s = i; }
            }
        } else {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
        }
    }
    return dup_str("");
}

// ---- LSP server I/O (JSON-RPC over stdio) ----
// 这些函数让 shadow 程序能作为长驻 LSP 服务器运行：从 stdin 按字节读取
// Content-Length 帧，并把响应以相同格式写回 stdout。stdin 设为二进制模式，
// 避免 Windows 把 \r\n 翻译成 \n 导致帧体长度错位。
static bool g_lsp_stdin_binary = false;
static void lsp_ensure_stdin_binary() {
#ifdef _WIN32
    if (!g_lsp_stdin_binary) {
        _setmode(_fileno(stdin), _O_BINARY);
        g_lsp_stdin_binary = true;
    }
#else
    (void)g_lsp_stdin_binary; // Linux stdin is already binary
#endif
}

// 读取一行（遇到 \n 结束，剥掉 \r\n）。EOF 且无内容时返回 ""。
extern "C" const char* shadow_stdin_read_line() {
    lsp_ensure_stdin_binary();
    std::string line;
    char c;
    int r;
#ifdef _WIN32
    while ((r = _read(_fileno(stdin), &c, 1)) == 1) {
#else
    while ((r = (int)read(fileno(stdin), &c, 1)) == 1) {
#endif
        if (c == '\r') continue;
        if (c == '\n') break;
        line.push_back(c);
    }
    if (r != 1 && line.empty()) return dup_str("");
    return dup_str(line);
}

// 精确读取 n 个字节（二进制）。EOF 提前到达时返回已读部分。
extern "C" const char* shadow_stdin_read_n(int64_t n) {
    lsp_ensure_stdin_binary();
    if (n <= 0) return dup_str("");
    std::string buf;
    buf.resize((size_t)n);
    size_t got = 0;
    while (got < (size_t)n) {
#ifdef _WIN32
        int r = _read(_fileno(stdin), &buf[0] + got, (unsigned int)((size_t)n - got));
#else
        int r = (int)read(fileno(stdin), &buf[0] + got, (size_t)n - got);
#endif
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf.resize(got);
    return dup_str(buf);
}

// 原样写字节到 stdout（不附加换行），并 flush。
// 注意：Windows 下 stdout 默认文本模式，会把 '\n' 翻译成 '\r\n'，
// 破坏 LSP 帧头（应为裸 '\r\n'）。首次写入前把 stdout 切到二进制模式。
extern "C" void shadow_stdout_write_raw(const char* s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len > 0) {
#ifdef _WIN32
        static bool stdout_binary_set = false;
        if (!stdout_binary_set) {
            _setmode(_fileno(stdout), _O_BINARY);
            stdout_binary_set = true;
        }
#endif
        fwrite(s, 1, len, stdout);
    }
    fflush(stdout);
}

// 从 JSON 文本中按 key 取出原始值片段（字符串已去引号；对象/数组返回 {…}/[…]）。
// 暴露内部静态 helper json_get_raw，供 shadow 侧做嵌套字段链式提取。
extern "C" const char* shadow_json_get_raw(const char* json, const char* key) {
    std::string v = json_get_raw(json ? json : "", key ? key : "");
    return dup_str(v);
}

// Ã¢ÂÂÃ¢ÂÂ Dict/Array Operations Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

#include <unordered_map>
#include <variant>

// Shadow Value types (simplified for runtime)
// DictValue / ShadowDict / ShadowArray 的完整定义已移到文件顶部（line ~33），
// 以便 shadow_hashmap_*（line ~412）能访问其成员。
// 此处仅保留 value_to_string 系列静态函数的实现。

// Forward declaration: GC registration (defined in the GC section below).
// Used by shadow_array_create_ints so runtime-created arrays are GC-tracked.
extern "C" void shadow_gc_register(void* ptr, int32_t kind, int64_t size);

// Helper: convert Shadow value to string
static std::string value_to_string(const DictValue& v) {
    if (v.tag == 0) {
        return std::to_string(v.val.i);
    }
    if (v.tag == 1) {
        std::ostringstream ss;
        ss << v.val.d;
        return ss.str();
    }
    if (v.tag == 3) {
        return v.val.b ? "true" : "false";
    }
    if (v.tag == 2) {
        return std::string(v.val.s ? v.val.s : "");
    }
    if (v.tag == 4) {
        ShadowDict* d = reinterpret_cast<ShadowDict*>(v.val.p);
        ShadowArray* a = reinterpret_cast<ShadowArray*>(v.val.p);
        if (d) return value_to_string_dict(d);
        if (a) return value_to_string_array(a);
    }
    return "null";
}

static std::string value_to_string_dict(ShadowDict* d) {
    if (!d) return "{}";
    std::string result = "{";
    bool first = true;
    for (int32_t _i = d->head; _i >= 0; _i = d->slots[_i].next) {
        SDictEntry& _e = d->slots[_i];
        if (!first) result += ", ";
        result += "\"" + std::string(_e.key) + "\": ";
        result += value_to_string(_e.val);
        first = false;
    }
    result += "}";
    return result;
}

static std::string value_to_string_array(ShadowArray* a) {
    if (!a) return "[]";
    std::string result = "[";
    for (size_t i = 0; i < a->size(); ++i) {
        if (i > 0) result += ", ";
        result += value_to_string((*a)[i]);
    }
    result += "]";
    return result;
}

// Helper: parse simple JSON value
static DictValue parse_json_value(const std::string& json, size_t& pos);

// Parse JSON string value
static std::string parse_json_string(const std::string& json, size_t& pos) {
    if (pos >= json.size() || json[pos] != '"') return "";
    ++pos; // skip opening quote
    std::string result;
    while (pos < json.size()) {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            ++pos;
            char esc = json[pos];
            switch (esc) {
                case 'n': result += '\n'; break;
                case 't': result += '\t'; break;
                case 'r': result += '\r'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default: result += esc; break;
            }
        } else if (c == '"') {
            ++pos;
            break;
        } else {
            result += c;
        }
        ++pos;
    }
    return result;
}

// Skip whitespace
static void skip_ws(const std::string& json, size_t& pos) {
    while (pos < json.size() && std::isspace(json[pos])) ++pos;
}

// Parse JSON object
static ShadowDict* parse_json_object(const std::string& json, size_t& pos) {
    ShadowDict* d = new ShadowDict();
    if (pos >= json.size() || json[pos] != '{') {
        return d;
    }
    ++pos; // skip '{'
    skip_ws(json, pos);
    if (pos < json.size() && json[pos] == '}') {
        ++pos;
        return d;
    }
    while (pos < json.size()) {
        skip_ws(json, pos);
        if (pos >= json.size() || json[pos] != '"') break;
        std::string key = parse_json_string(json, pos);
        skip_ws(json, pos);
        if (pos >= json.size() || json[pos] != ':') break;
        ++pos; // skip ':'
        skip_ws(json, pos);
        DictValue val = parse_json_value(json, pos);
        sdict_set(d, key.c_str(), val);
        skip_ws(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] == '}') {
            ++pos;
            break;
        }
        if (json[pos] == ',') {
            ++pos;
        }
    }
    return d;
}

// Parse JSON array
static ShadowArray* parse_json_array(const std::string& json, size_t& pos) {
    ShadowArray* a = new ShadowArray();
    if (pos >= json.size() || json[pos] != '[') {
        return a;
    }
    ++pos; // skip '['
    skip_ws(json, pos);
    if (pos < json.size() && json[pos] == ']') {
        ++pos;
        return a;
    }
    while (pos < json.size()) {
        skip_ws(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] == ']') {
            ++pos;
            break;
        }
        a->push_back(parse_json_value(json, pos));
        skip_ws(json, pos);
        if (pos >= json.size()) break;
        if (json[pos] == ']') {
            ++pos;
            break;
        }
        if (json[pos] == ',') {
            ++pos;
        }
    }
    return a;
}

// Parse JSON primitive value
static DictValue parse_json_value(const std::string& json, size_t& pos) {
    skip_ws(json, pos);
    if (pos >= json.size()) return (int64_t)0;
    char c = json[pos];
    if (c == '"') {
        return parse_json_string(json, pos);
    }
    if (c == '{') {
        ShadowDict* d = parse_json_object(json, pos);
        return (void*)d;
    }
    if (c == '[') {
        ShadowArray* a = parse_json_array(json, pos);
        return (void*)a;
    }
    // Number or boolean
    size_t start = pos;
    bool is_double = false;
    if (c == '-' || c == '+') ++pos;
    while (pos < json.size() && (std::isdigit(json[pos]) || json[pos] == '.' ||
                                 json[pos] == 'e' || json[pos] == 'E' ||
                                 json[pos] == '+' || json[pos] == '-')) {
        if (json[pos] == '.') is_double = true;
        ++pos;
    }
    std::string num_str = json.substr(start, pos - start);
    skip_ws(json, pos);
    // Check for true/false/null
    if (num_str == "true") return (bool)true;
    if (num_str == "false") return (bool)false;
    if (num_str == "null") return (int64_t)0;
    if (is_double) {
        try { return std::stod(num_str); } catch (...) {}
    } else {
        try { return (DictValue)(int64_t)std::stoll(num_str); } catch (...) {}
    }
    return (int64_t)0;
}

// Create Dict from key-value pairs (variadic)
extern "C" void* shadow_dict_create(int count, ...) {
    ShadowDict* d = new ShadowDict();
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; ++i) {
        const char* key = va_arg(args, const char*);
        int64_t val_type = va_arg(args, int); // 0=int, 1=float, 2=string, 3=bool
        DictValue val;
        switch (val_type) {
            case 0: val = va_arg(args, int64_t); break;
            case 1: {
                double f = va_arg(args, double);
                val = f;
                break;
            }
            case 2: val = std::string(va_arg(args, const char*)); break;
            case 3: val = (bool)(va_arg(args, int)); break;
            case 4:
            case 5: {
                // Struct/array/any value: store as a transparent void* pointer
                // (e.g. an LLVM handle boxed into 'any'). Required for the
                // self-hosted codegen, which keeps opaque handles in dicts.
                val = va_arg(args, void*);
                break;
            }
            default: val = (int64_t)0; break;
        }
        if (key) sdict_set(d, key, val);
    }
    va_end(args);
    return d;
}

// Get Dict value by key - returns pointer to the value
// NOTE: For int/float/bool, we store them in a static buffer and return that pointer.
// These are extern so shadow_any_print can detect them by address.
thread_local int64_t g_any_int_buf;
thread_local double g_any_float_buf;
thread_local bool g_any_bool_buf;
// Dynamic buffer for dict/array string values — no size limit (was g_str_buf[1024]).
// thread_local std::string reuses its heap across calls; .c_str() pointer is stable
// until the next assignment. shadow_any_print doesn't check string by address, only
// int/float/bool buffers, so we can safely return .c_str() here.
static thread_local std::string g_str_buf;

extern "C" void* shadow_dict_get(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return nullptr;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    DictValue* val = sdict_get(d, key);
    if (!val) return nullptr;
    if (val->tag == 0) {
        g_any_int_buf = val->val.i;
        return &g_any_int_buf;
    }
    if (val->tag == 1) {
        g_any_float_buf = val->val.d;
        return &g_any_float_buf;
    }
    if (val->tag == 3) {
        g_any_bool_buf = val->val.b;
        return &g_any_bool_buf;
    }
    if (val->tag == 2) {
        g_str_buf = std::string(val->val.s ? val->val.s : "");
        return const_cast<char*>(g_str_buf.c_str());
    }
    if (val->tag == 4) {
        void* p = val->val.p;
        // Nested dict/array
        ShadowArray* nested_a = reinterpret_cast<ShadowArray*>(p);
        if (nested_a && nested_a->type_tag == 0) {
            // This is an array
            return nested_a;
        }
        ShadowDict* nested_d = reinterpret_cast<ShadowDict*>(p);
        if (nested_d) {
            return nested_d;
        }
        return p;
    }
    return nullptr;
}

// Get all keys as array of strings (semicolon-separated)
extern "C" void* shadow_dict_keys(void* dict_ptr) {
    if (!dict_ptr) return new ShadowArray();
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    ShadowArray* a = new ShadowArray();
    for (int32_t _i = d->head; _i >= 0; _i = d->slots[_i].next) {
        a->push_back(DictValue(d->slots[_i].key));
    }
    return a;
}

// shadow_dict_size: return number of entries in dict (C++ ShadowDict)
// Used by for (k in dict) iteration to avoid layout mismatch with
// Shadow runtime's shadow_array_len (which reads Shadow-layout arrays).
extern "C" int32_t shadow_dict_size(void* dict_ptr) {
    if (!dict_ptr) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    return (int32_t)d->count;
}

// shadow_dict_key_at: return key string at given index (C++ ShadowDict)
// Used by for (k in dict) iteration. Returns strdup'd string (caller owns).
extern "C" const char* shadow_dict_key_at(void* dict_ptr, int32_t idx) {
    if (!dict_ptr) return nullptr;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    if (idx < 0 || (size_t)idx >= d->count) {
        fprintf(stderr, "error: dict index %d out of bounds (size=%zu)\n",
                idx, d->count);
        return nullptr;
    }
    int32_t i = d->head;
    for (int32_t k = 0; k < idx && i >= 0; k++) i = d->slots[i].next;
    if (i < 0) return nullptr;
    return strdup(d->slots[i].key);
}

// Get all values as array (semicolon-separated, values as strings)
extern "C" const char* shadow_dict_values(void* dict_ptr) {
    if (!dict_ptr) return dup_str("");
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    std::string result;
    bool first = true;
    for (int32_t _i = d->head; _i >= 0; _i = d->slots[_i].next) {
        if (!first) result += ";";
        result += value_to_string(d->slots[_i].val);
        first = false;
    }
    return dup_str(result);
}

// Check if dict has key
extern "C" int shadow_dict_has_key(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    return sdict_get(d, key) != nullptr ? 1 : 0;
}

// Create Array from values (variadic)
extern "C" void* shadow_array_create(int count, ...) {
    ShadowArray* a = new ShadowArray();
    a->resize(count);
    va_list args;
    va_start(args, count);
    for (int i = 0; i < count; ++i) {
        int64_t val_type = va_arg(args, int); // 0=int, 1=float, 2=string, 3=bool
        DictValue val;
        switch (val_type) {
            case 0: val = va_arg(args, int64_t); break;
            case 1: {
                double f = va_arg(args, double);
                val = f;
                break;
            }
            case 2: {
                const char* s = va_arg(args, const char*);
                val = std::string(s);
                break;
            }
            case 3: val = (bool)(va_arg(args, int)); break;
            case 4:
            case 5: {
                // Struct/array/any value: store as void* pointer.
                // shadow_array_get handles retrieval via holds_alternative<void*>.
                val = va_arg(args, void*);
                break;
            }
            default: val = (int64_t)0; break;
        }
        (*a)[i] = val;
    }
    va_end(args);
    return a;
}

// Split string by delimiter (single char)
extern "C" void* shadow_split(const char* str, const char* delim) {
    if (!str) return new ShadowArray();
    std::string s(str);
    char d = (delim && delim[0]) ? delim[0] : ',';
    
    ShadowArray* arr = new ShadowArray();
    if (d == '\0') {
        // Empty delimiter: split into characters
        for (char c : s) {
            arr->push_back(std::string(1, c));
        }
    } else {
        size_t pos = 0;
        while (true) {
            size_t next = s.find(d, pos);
            if (next == std::string::npos) {
                arr->push_back(s.substr(pos));
                break;
            }
            arr->push_back(s.substr(pos, next - pos));
            pos = next + 1;
        }
    }
    return arr;
}

// Set Array element by index (functional approach: returns new array)
extern "C" void* shadow_array_set(void* array_ptr, int32_t idx, int32_t type_tag, ...) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    ShadowArray* result = new ShadowArray();
    // Copy all elements from a to result
    for (size_t i = 0; i < a->size(); ++i) {
        result->push_back((*a)[i]);
    }
    
    int64_t adjusted_idx = idx;
    if (adjusted_idx < 0) adjusted_idx = (int64_t)result->size() + adjusted_idx;
    if (adjusted_idx < 0 || (size_t)adjusted_idx >= result->size()) return result;
    
    va_list args;
    va_start(args, type_tag);
    DictValue val;
    switch (type_tag) {
        case 0: val = (int64_t)va_arg(args, int64_t); break;
        case 1: val = (double)va_arg(args, double); break;
        case 2: val = std::string(va_arg(args, const char*)); break;
        case 3: val = (bool)(va_arg(args, int) != 0); break;
        case 4:
        case 5: val = va_arg(args, void*); break;
        default: val = (int64_t)va_arg(args, int64_t); break;
    }
    va_end(args);
    
    (*result)[adjusted_idx] = val;
    return result;
}

// Push element to array (destructive append)
extern "C" void* shadow_array_push(void* array_ptr, int32_t type_tag, ...) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    
    va_list args;
    va_start(args, type_tag);
    DictValue val;
    switch (type_tag) {
        case 0: val = (int64_t)va_arg(args, int64_t); break;
        case 1: val = (double)va_arg(args, double); break;
        case 2: val = std::string(va_arg(args, const char*)); break;
        case 3: val = (bool)(va_arg(args, int) != 0); break;
        case 4:
        case 5: val = va_arg(args, void*); break;
        default: val = (int64_t)va_arg(args, int64_t); break;
    }
    va_end(args);
    
    a->push_back(val);
    return a;
}

// Get Array element by index Ã¢ÂÂ return boxed value
extern "C" void* shadow_array_get(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) return nullptr;
    DictValue& val = (*a)[idx];
    if (val.tag == 0) {
        // Transparent 'any' model: return the raw int64 value, not a box pointer.
        // The self-hosted codegen (and shadowc's compiled code) treat 'any' as the
        // raw value (e.g. an LLVM handle stored as i64), so returning &g_any_int_buf
        // would hand a box address where a raw handle is expected.
        return (void*)(intptr_t)val.val.i;
    }
    if (val.tag == 1) {
        g_any_float_buf = val.val.d;
        return &g_any_float_buf;
    }
    if (val.tag == 2) {
        const char* c = val.val.s;
        return (void*)c;
    }
    if (val.tag == 3) {
        g_any_bool_buf = val.val.b;
        return &g_any_bool_buf;
    }
    if (val.tag == 4) {
        void* vp = val.val.p;
        return vp;
    }
    return nullptr;
}

// Get Array length
extern "C" int32_t shadow_array_len(void* array_ptr) {
    if (!array_ptr) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    int32_t n = (int32_t)a->size();
    return n;
}

// Create array from int buffer (non-variadic, for shadow-lang codegen)
// P3：段分配 + 带内头（kind=1，扫描语义与原注册对象一致）。
extern "C" void* shadow_array_create_ints(int32_t count, int32_t* values) {
    ShadowArray* a = (ShadowArray*)shadow_gc_alloc((int32_t)sizeof(ShadowArray), 1);
    if (!a) return nullptr;
    new (a) ShadowArray();
    a->resize(count);
    for (int32_t i = 0; i < count; i++) {
        (*a)[i] = (int64_t)values[i];
    }
    return a;
}

// Create array from pointer array (string/any/nested-array element buffers)
extern "C" void* shadow_array_create_ptrs(int32_t count, void** values) {
    ShadowArray* a = (ShadowArray*)shadow_gc_alloc((int32_t)sizeof(ShadowArray), 1);
    if (!a) return nullptr;
    new (a) ShadowArray();
    a->resize(count);
    for (int32_t i = 0; i < count; i++) {
        (*a)[i] = (DictValue)values[i];
    }
    return a;
}

// String content comparison (strcmp semantics: returns <0, 0, >0)
extern "C" int32_t shadow_string_compare(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return (int32_t)strcmp(a, b);
}

// String subscript: s[i] -> single-char string
extern "C" const char* shadow_string_subscript(const char* s, int32_t idx) {
    if (!s) return dup_str("");
    int32_t len = (int32_t)strlen(s);
    if (idx < 0 || idx >= len) return dup_str("");
    char buf[2] = { s[idx], '\0' };
    return dup_str(buf);
}

// P20-6: 无分配字符访问 — s[i] 编译为 shadow_string_subscript，每次 dup_str
// 裸 malloc 永不释放，循环里 `src[i] == "\n"` 是纯泄漏（长驻 LSP 进程最严重）。
// 此函数返回字符码（i32，0 = 越界/NUL），调用方与字面量字符码比较即可，
// 不产生任何分配。shadow-lang 0.2 不识别它，需在 Shadow 侧 @extern 声明使用。
static inline int tl_str_lookup(void* p, int32_t* len, int32_t* cap);  // defined later in TU
static inline void tl_str_set(void* p, int32_t len, int32_t cap);      // defined later in TU
extern "C" int32_t shadow_string_char_at(const char* s, int32_t idx) {
    if (!s) return 0;
    int32_t n = 0, cap = 0;
    if (!tl_str_lookup((void*)s, &n, &cap)) {
        n = (int32_t)strlen(s);
        tl_str_set((void*)s, n, 0);  // 只缓存长度，容量未知（源串通常非拼接目标）
    }
    int32_t i = idx;
    if (i < 0) i = i + n;            // 负索引回绕（与 shadow_schar 一致）
    if (i < 0) return 0;
    if (i >= n) return 0;
    return (int32_t)(unsigned char)s[i];
}

// Absolute value
extern "C" int64_t shadow_abs(int64_t x) {
    return x < 0 ? -x : x;
}

// Putchar wrapper
extern "C" void shadow_putchar(int32_t c) {
    putchar(c);
}

// Join array elements with delimiter
extern "C" char* shadow_join(void* array_ptr, const char* delim) {
    if (!array_ptr || !delim) return dup_str("");
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    std::string d(delim);
    std::string result;
    for (size_t i = 0; i < a->size(); ++i) {
        if (i > 0) result += d;
        result += value_to_string((*a)[i]);
    }
    return dup_str(result.c_str());
}

// shadow_string_array_join: join elements of a ShadowArray of string pointers
// (the layout produced by shadow_array_create_ptrs + shadow_array_push_ptr —
// i.e. what shadow-lang 0.2's `string[]` compiles to).
// arr_ptr: ShadowArray* whose elements are raw char* (void* variants)
// sep: separator string
// returns: joined string (caller owns)
extern "C" const char* shadow_string_array_join(void* arr_ptr, const char* sep) {
    if (!arr_ptr) return dup_str("");
    ShadowArray* a = reinterpret_cast<ShadowArray*>(arr_ptr);
    std::string d(sep ? sep : "");
    std::string result;
    for (size_t i = 0; i < a->size(); i++) {
        if (i > 0) result += d;
        const DictValue& v = (*a)[i];
        if (v.tag == 4) {
            void* p = v.val.p;
            if (p) result += (const char*)p;
        } else if (v.tag == 2) {
            result += std::string(v.val.s ? v.val.s : "");
        } else {
            result += value_to_string(v);
        }
    }
    return dup_str(result);
}

// Convert any value to string
extern "C" const char* shadow_to_string_any(void* val_ptr) {
    if (!val_ptr) return dup_str("()");
    uintptr_t u = (uintptr_t)val_ptr;
    // Boolean true is stored as intptr_t 1
    if (u == 1) {
        return dup_str("true");
    }
    // Small pointer optimization: small values are boxed integers (except 0 and 1 which are booleans)
    if (u > 1 && u < 10000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)(intptr_t)val_ptr);
        return dup_str(buf);
    }
    ShadowArray* a = reinterpret_cast<ShadowArray*>(val_ptr);
    if (a && a->type_tag == 0) {
        if (!a->empty()) {
            return dup_str(value_to_string_array(a).c_str());
        }
        return dup_str("[]");
    }
    ShadowDict* d = reinterpret_cast<ShadowDict*>(val_ptr);
    if (d && d->type_tag == 1) {
        if (d->count > 0) {
            return dup_str(value_to_string_dict(d).c_str());
        }
        return dup_str("{}");
    }
    // Assume string
    return dup_str((const char*)val_ptr);
}

// Parse JSON string to Dict
extern "C" void* shadow_parse_json(const char* json_str) {
    if (!json_str) return new ShadowDict();
    std::string json(json_str);
    size_t pos = 0;
    skip_ws(json, pos);
    if (pos < json.size() && json[pos] == '{') {
        ShadowDict* d = parse_json_object(json, pos);
        return d;
    }
    if (pos < json.size() && json[pos] == '[') {
        ShadowArray* a = parse_json_array(json, pos);
        return a;
    }
    return new ShadowDict();
}

// JSON value accessors operate on the parsed ShadowDict* returned by
// shadow_parse_json (NOT on a raw JSON string Ã¢ÂÂ the codegen passes the dict
// pointer). This matches the API backend's call contract.
extern "C" const char* shadow_json_get(const char* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return dup_str("");
    ShadowDict* d = reinterpret_cast<ShadowDict*>(const_cast<char*>(dict_ptr));
    if (!d || d->type_tag != 1) return dup_str("");
    DictValue* v = sdict_get(d, key);
    if (!v) return dup_str("");
    if (v->tag == 2)
        return dup_str(v->val.s);
    // Fall back to stringifying other variants (int/bool/etc.).
    return dup_str(value_to_string(*v).c_str());
}

extern "C" int64_t shadow_json_get_int(const char* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(const_cast<char*>(dict_ptr));
    if (!d || d->type_tag != 1) return 0;
    DictValue* v = sdict_get(d, key);
    if (!v) return 0;
    if (v->tag == 0) return v->val.i;
    if (v->tag == 1) return (int64_t)v->val.d;
    if (v->tag == 3) return v->val.b ? 1 : 0;
    if (v->tag == 2) {
        try { return std::stoll(std::string(v->val.s ? v->val.s : "")); } catch (...) { return 0; }
    }
    return 0;
}

// Get length of any container (Dict/Array) or string
extern "C" int32_t shadow_len_any(void* val_ptr) {
    if (!val_ptr) return 0;
    uintptr_t u = (uintptr_t)val_ptr;
    // Small pointer optimization: small values are boxed integers, not pointers
    if (u > 0 && u < 10000) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(val_ptr);
    // Use type_tag to distinguish: 0 for arrays
    if (a && a->type_tag == 0) {
        return (int32_t)a->size();
    }
    ShadowDict* d = reinterpret_cast<ShadowDict*>(val_ptr);
    if (d && d->type_tag == 1) {
        return (int32_t)d->count;
    }
    // Fallback: treat as string
    return (int32_t)strlen((const char*)val_ptr);
}

// Index access on any type: runtime dispatches based on type_tag.
// When the compiler cannot determine the static type of the object
// (e.g. it is stored in an `any` field), this function checks the
// runtime type and dispatches to the correct indexing operation.
extern "C" void* shadow_index_any(void* obj, int32_t idx) {
    if (!obj) return nullptr;
    uintptr_t u = (uintptr_t)obj;
    // Small pointer optimization: small values are boxed integers (interpreter)
    if (u > 0 && u < 10000) return nullptr;
    if (!is_valid_ptr(obj)) return nullptr;
    AnyBox* b = reinterpret_cast<AnyBox*>(obj);
    if (b->magic == ANYBOX_MAGIC) {
        // boxed string: index into the character data
        if (b->tag == 2) {
            return (void*)shadow_char_at(reinterpret_cast<const char*>(b->value), (int64_t)idx);
        }
        // boxed array: unbox the pointer and index into the array
        if (b->tag == 3) {
            void* raw_ptr = (void*)(intptr_t)b->value;
            return shadow_array_get(raw_ptr, idx);
        }
        return nullptr;  // int/float/bool/struct can't be indexed
    }
    // raw heap object
    ShadowArray* a = reinterpret_cast<ShadowArray*>(obj);
    if (a && a->type_tag == 0) {
        // Array access
        return shadow_array_get(obj, idx);
    }
    ShadowDict* d = reinterpret_cast<ShadowDict*>(obj);
    if (d && d->type_tag == 1) {
        // Dict access with integer key - not supported, return null
        return nullptr;
    }
    // Fallback: treat as raw string
    return (void*)shadow_char_at(reinterpret_cast<const char*>(obj), (int64_t)idx);
}

// Member access on an any-typed value at runtime.
// Dispatches based on type_tag:
//   - dict  (type_tag==1) -> shadow_dict_get
//   - array (type_tag==0) -> nullptr (arrays have no member access)
//   - other               -> nullptr (strings/structs: use type annotations)
// This prevents calling shadow_dict_get on a non-dict pointer,
// which would cause memory corruption.
// Struct field registry: maps type_name -> (field_names, field_offsets, field_tags)
// Populated by codegen via shadow_struct_register()
// field_tags: 0=int/char/short, 1=float/double, 2=string, 3=array, 4=struct/any, 5=long
struct StructFieldInfo {
    std::vector<std::string> names;
    std::vector<int32_t> offsets;
    std::vector<int32_t> tags;
};
static std::unordered_map<std::string, StructFieldInfo> g_struct_fields;

extern "C" void shadow_struct_register(const char* type_name, const char* field_names_csv) {
    StructFieldInfo info;
    std::string cur;
    for (const char* p = field_names_csv; *p; ++p) {
        if (*p == ',') {
            if (!cur.empty()) { info.names.push_back(cur); cur.clear(); }
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) info.names.push_back(cur);
    g_struct_fields[type_name] = info;
}

extern "C" void shadow_struct_register_ex(const char* type_name, const char* field_names_csv,
                                          const int32_t* field_offsets, const int32_t* field_tags, int32_t num_fields) {
    StructFieldInfo info;
    std::string cur;
    for (const char* p = field_names_csv; *p; ++p) {
        if (*p == ',') {
            if (!cur.empty()) { info.names.push_back(cur); cur.clear(); }
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) info.names.push_back(cur);
    for (int32_t i = 0; i < num_fields; ++i) {
        info.offsets.push_back(field_offsets[i]);
        info.tags.push_back(field_tags[i]);
    }
    g_struct_fields[type_name] = info;
}

extern "C" void* shadow_any_box(int32_t tag, int64_t value);  // forward declaration

// Member access on an any-typed value at runtime.
//
// IMPORTANT (开发原则.md §13.4.1 "Any 的显式边界" / issue #3 "Any滥用"):
// we must NEVER do a global, cross-struct field-name lookup. Two different
// structs can share a field name (e.g. `Token.line` vs `Location.line`); a
// global lookup that picks the first match by iteration order would apply the
// WRONG struct's field index to `obj`, reading a small integer as a pointer and
// segfaulting inside shadow_str_cmp. So resolution is now strictly scoped:
//   - `type_name` is the compile-time struct type (may be empty for genuine any)
//   - if `type_name` is given, we resolve ONLY within that struct's field list
//   - if `type_name` is empty, we resolve ONLY when exactly ONE registered
//     struct has the field (unambiguous); otherwise we return null (safe),
//     rather than guessing and risking a crash.
extern "C" void* shadow_member_any(void* obj, const char* type_name, const char* key) {
    if (!obj || !key) return nullptr;
    uintptr_t u = (uintptr_t)obj;
    if (u > 0 && u < 10000) return nullptr;
    if (!is_valid_ptr(obj)) return nullptr;
    AnyBox* b = reinterpret_cast<AnyBox*>(obj);
    if (b->magic == ANYBOX_MAGIC) {
        // boxed value: tag=4 means boxed struct pointer
        if (b->tag == 4) {
            obj = (void*)(intptr_t)b->value;
        } else {
            return nullptr;  // other boxed types have no members
        }
    }
    // Member access (`.field`) is struct-only. `obj` here is either an
    // AnyBox(tag 4) unwrapped to the struct pointer above, or a transparent
    // raw struct pointer (struct `as any` is a raw-pointer passthrough in
    // codegen). Arrays/dicts use index access (shadow_index_any), never
    // `.field`, so we must NOT reinterpret `obj` as array/dict here -- a raw
    // struct pointer whose first field happens to equal a type_tag would
    // otherwise be misidentified (e.g. Point.x==1 looks like Dict.type_tag).
    // Struct: resolve field using registered offsets and tags
    const StructFieldInfo* info = nullptr;
    if (type_name && type_name[0] != '\0') {
        auto it = g_struct_fields.find(type_name);
        if (it != g_struct_fields.end()) info = &it->second;
    } else {
        // Genuine `any`: only resolve when the field name is unambiguous
        int matches = 0;
        const StructFieldInfo* cand = nullptr;
        for (const auto& kv : g_struct_fields) {
            const auto& names = kv.second.names;
            for (size_t i = 0; i < names.size(); ++i) {
                if (names[i] == key) { matches++; cand = &kv.second; break; }
            }
        }
        if (matches == 1) info = cand;
    }
    if (!info) return nullptr;
    for (size_t i = 0; i < info->names.size(); ++i) {
        if (info->names[i] == key) {
            int32_t offset = info->offsets[i];
            int32_t tag = info->tags[i];
            char* field_ptr = (char*)obj + offset;
            switch (tag) {
                case 0: {  // int/char/short: box as any and return
                    int32_t v = *(int32_t*)field_ptr;
                    return shadow_any_box(0, (int64_t)v);
                }
                case 1: {  // float/double: box and return
                    double v = *(double*)field_ptr;
                    return shadow_any_box(1, *(int64_t*)&v);
                }
                case 2:   // string
                case 3:   // array
                case 4: { // struct/any: pointer types
                    return *(void**)field_ptr;
                }
                case 5: { // long: box and return
                    int64_t v = *(int64_t*)field_ptr;
                    return shadow_any_box(0, v);
                }
                default:
                    return nullptr;
            }
        }
    }
    return nullptr;
}


// Free Dict/Array memory
// 使用 type_tag 区分容器类型，避免双重释放 UB：
//   ShadowArray.type_tag == 0  → delete as ShadowArray
//   ShadowDict.type_tag  == 1  → delete as ShadowDict
//   ShadowSet.type_tag   == 2  → delete as ShadowSet
//   其他值 → 不释放（未知类型，可能是 GC 管理的对象）
// 注：ShadowArray/ShadowDict/ShadowSet 的首个字段是 uint32_t type_tag，
//     reinterpret_cast 到任一结构体后读 type_tag 即可区分。
// 前向声明：GC 登记对象删除辅助（定义于文件后部 GC 状态之后）
extern "C" int32_t shadow_gc_forget(void* ptr);
// 前向声明：诊断用——指针是否在 GC meta 中（定义于文件后部）
extern "C" int32_t shadow_gc_meta_contains(void* p);
// 前向声明：W3/P1 段对象显式释放（置死标记，不能 free 段中内存；定义于文件后部）
static int gc_hdr_release(void* p);

extern "C" void shadow_free(void* ptr) {
    if (!ptr) return;
    // P2：段对象不在 meta 表，必须先判段头——置死标记（+ 回空闲链表），
    // 段中内存绝不能 free()。
    if (gc_hdr_release(ptr)) return;
    // GC 登记对象（new+register 的 C++ 对象）：从 meta 删除后 free。
    // （C 布局对象头不是 RFS 的 type_tag，走原 type_tag 判断会错删/泄漏。）
    if (shadow_gc_forget(ptr)) {
        free(ptr);
        return;
    }
    uint32_t tag = *reinterpret_cast<uint32_t*>(ptr);
    if (tag == 0) {
        // ShadowArray
        delete reinterpret_cast<ShadowArray*>(ptr);
    } else if (tag == 1) {
        // ShadowDict
        delete reinterpret_cast<ShadowDict*>(ptr);
    } else if (tag == 2) {
        // ShadowSet
        delete reinterpret_cast<ShadowSet*>(ptr);
    } else if (tag == 3) {
        delete reinterpret_cast<ShadowArena*>(ptr);
    }
    // 其他 type_tag 值：不释放（可能是 GC 管理的对象）
}

extern "C" void* shadow_arena_create(int32_t block_size) {
    size_t bs = block_size > 0 ? (size_t)block_size : (size_t)4096;
    if (bs < 64) bs = 64;
    return new ShadowArena(bs);
}

extern "C" void* shadow_arena_alloc(void* arena, int32_t size) {
    if (!arena || size <= 0) return nullptr;
    auto* ar = reinterpret_cast<ShadowArena*>(arena);
    size_t n = ((size_t)size + 7u) & ~((size_t)7u);
    if (ar->count == 0 || ar->used[ar->count - 1] + n > ar->caps[ar->count - 1]) {
        size_t c = n > ar->block_size ? n : ar->block_size;
        void* block = calloc(1, c);
        if (!block) return nullptr;
        sarena_push(ar, block, c);
    }
    size_t idx = ar->count - 1;
    char* p = reinterpret_cast<char*>(ar->blocks[idx]) + ar->used[idx];
    ar->used[idx] += n;
    ar->total_used += n;
    if (ar->total_used > ar->high_water) ar->high_water = ar->total_used;
    return p;
}

extern "C" int32_t shadow_arena_mark(void* arena) {
    if (!arena) return 0;
    auto* ar = reinterpret_cast<ShadowArena*>(arena);
    if (ar->total_used > 2147483647u) return 2147483647;
    return (int32_t)ar->total_used;
}

extern "C" void shadow_arena_rewind(void* arena, int32_t mark) {
    if (!arena) return;
    auto* ar = reinterpret_cast<ShadowArena*>(arena);
    size_t target = mark > 0 ? (size_t)mark : 0u;
    if (target >= ar->total_used) return;
    size_t consumed = 0;
    size_t keep_blocks = 0;
    for (size_t i = 0; i < ar->count; ++i) {
        size_t u = ar->used[i];
        if (target <= consumed + u) {
            ar->used[i] = target - consumed;
            keep_blocks = i + 1;
            break;
        }
        consumed += u;
    }
    for (size_t i = keep_blocks; i < ar->count; ++i) {
        free(ar->blocks[i]);
    }
    ar->count = keep_blocks; // 容量不收缩（与原 vector::resize 语义一致）
    ar->total_used = target;
}

extern "C" void shadow_arena_reset(void* arena) {
    shadow_arena_rewind(arena, 0);
}

extern "C" void shadow_arena_destroy(void* arena) {
    shadow_free(arena);
}

extern "C" int32_t shadow_arena_used(void* arena) {
    if (!arena) return 0;
    auto* ar = reinterpret_cast<ShadowArena*>(arena);
    if (ar->total_used > 2147483647u) return 2147483647;
    return (int32_t)ar->total_used;
}

extern "C" int32_t shadow_arena_high_water(void* arena) {
    if (!arena) return 0;
    auto* ar = reinterpret_cast<ShadowArena*>(arena);
    if (ar->high_water > 2147483647u) return 2147483647;
    return (int32_t)ar->high_water;
}

// Terminate the process with an exit code
extern "C" void shadow_exit(int code) {
    std::exit(code);
}

// panic(msg): print "panic: <msg>" to stderr and terminate with exit code 101.
// P0 fix: STMT_PANIC was silently dropped in HIR/MIR; parser now desugars
// panic(msg) into a call to this function (bound as rt_panic_msg).
extern "C" void shadow_panic_msg(const char* msg) {
    fprintf(stderr, "panic: %s\n", msg ? msg : "");
    fflush(stderr);
    std::exit(101);
}

// Ã¢ÂÂÃ¢ÂÂ CLI Args Support Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
static ShadowArray* g_cli_args = nullptr;

// Set CLI args from main.cpp (called before program execution)
extern "C" void shadow_set_cli_args(int argc, const char** argv) {
    if (g_cli_args) delete g_cli_args;
    g_cli_args = new ShadowArray();
    for (int i = 0; i < argc; ++i) {
        g_cli_args->push_back(std::string(argv[i] ? argv[i] : ""));
    }
}

// Get CLI args as ShadowArray*
extern "C" void* shadow_sys_args() {
    if (!g_cli_args) return new ShadowArray();
    // Return a copy to avoid mutation issues
    ShadowArray* result = new ShadowArray();
    result->data = g_cli_args->data;
    return result;
}

// Get CLI args count (used by the self-hosted compiler's argument parsing,
// which cannot rely on len(any) since the bootstrap compiler dispatches
// len(any) to shadow_string_len).
extern "C" int32_t shadow_sys_args_len() {
    if (!g_cli_args) return 0;
    return (int32_t)g_cli_args->size();
}

// Ã¢ÂÂÃ¢ÂÂ TCP Socket Operations Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

// Winsock auto-initialization helper
static int winsock_ready() {
    static bool initialized = false;
    if (!initialized) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return -1;
#endif
        initialized = true;
    }
    return 0;
}

// tcp_bind(port: int) -> socket_handle: int
// Creates a TCP server socket, binds to port, listens for connections.
// Returns a socket handle (intptr_t cast to int64) or -1 on error.
extern "C" int64_t shadow_tcp_bind(int32_t port) {
    if (winsock_ready() != 0) return -1;

#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    // Allow address reuse
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }

    // Listen with backlog 10
    if (listen(s, 10) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }

    return (int64_t)(intptr_t)s;
#else
    // Unix-style socket (POSIX)
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }

    if (listen(s, 10) < 0) {
        close(s);
        return -1;
    }

    return (int64_t)s;
#endif
}

// tcp_accept(server_socket: int) -> client_socket: int
// Accepts a connection. Returns client socket handle, or -1 on error.
extern "C" int64_t shadow_tcp_accept(int64_t server_handle) {
    if (server_handle == -1) return -1;

#ifdef _WIN32
    SOCKET server_sock = (SOCKET)(intptr_t)server_handle;
    SOCKET client = accept(server_sock, nullptr, nullptr);
    if (client == INVALID_SOCKET) return -1;
    return (int64_t)(intptr_t)client;
#else
    int client = accept((int)server_handle, nullptr, nullptr);
    return (int64_t)client;
#endif
}

// tcp_recv(socket: int, max_size: int) -> string
// Receives up to max_size bytes from the socket. Returns empty string on error.
extern "C" const char* shadow_tcp_recv(int64_t socket_handle, int32_t max_size) {
    if (socket_handle == -1 || max_size <= 0) return dup_str("");

    char* buf = (char*)malloc(max_size + 1);
    if (!buf) return dup_str("");

    int recv_len = 0;
#ifdef _WIN32
    recv_len = recv((SOCKET)(intptr_t)socket_handle, buf, max_size, 0);
#else
    recv_len = (int)recv((int)socket_handle, buf, (size_t)max_size, 0);
#endif

    if (recv_len <= 0) {
        free(buf);
        return dup_str("");
    }
    buf[recv_len] = '\0';
    // Return the buffer Ã¢ÂÂ caller MUST free it (Shadow runtime will copy it)
    return buf;
}

// tcp_send(socket: int, data: string) -> int
// Sends data to the socket. Returns number of bytes sent, or -1 on error.
extern "C" int32_t shadow_tcp_send(int64_t socket_handle, const char* data) {
    if (socket_handle == -1 || !data) return -1;

    int len = (int)strlen(data);
    if (len == 0) return 0;

#ifdef _WIN32
    int sent = send((SOCKET)(intptr_t)socket_handle, data, len, 0);
#else
    int sent = (int)send((int)socket_handle, data, (size_t)len, 0);
#endif
    return (int32_t)sent;
}

// tcp_close(socket: int) -> int
// Closes the socket. Returns 1 on success, 0 on error.
extern "C" int32_t shadow_tcp_close(int64_t socket_handle) {
    if (socket_handle == -1) return 0;
#ifdef _WIN32
    int result = closesocket((SOCKET)(intptr_t)socket_handle);
    return (result == 0) ? 1 : 0;
#else
    int result = close((int)socket_handle);
    return (result == 0) ? 1 : 0;
#endif
}


extern "C" const char* shadow_any_to_string(void* val, const char* type_name) {
    if(!val)return "0";
    if(strcmp(type_name,"int")==0||strcmp(type_name,"bool")==0){char b[32];snprintf(b,32,"%lld",(long long)(intptr_t)val);return dup_str(b);}
    if(strcmp(type_name,"string")==0)return (const char*)val;
    return "?";
}
// 线程安全：返回 malloc 拷贝（对齐 Windows rt_dup_str 语义）。
// 旧实现返回静态缓冲，多线程 spawn 下互覆盖导致字符串内容错乱。
extern "C" const char* shadow_int_to_str(void* val) {
    char b[32]; snprintf(b,32,"%lld",(long long)(intptr_t)val); return dup_str(b);
}
extern "C" int64_t shadow_any_to_int(void* val) { return (int64_t)(intptr_t)val; }
extern "C" const char* shadow_string_concat(const char* a, const char* b) {
    if(!a&&!b)return 0; if(!a)return b?strdup(b):0; if(!b)return strdup(a);
    size_t la=strlen(a),lb=strlen(b); char* r=(char*)malloc(la+lb+1);
    if(r){memcpy(r,a,la);memcpy(r+la,b,lb);r[la+lb]=0;}return r;
}
// Null-safe string comparison used by generated `==` / `!=` / `<` / `>` etc.
// Behaves like libc strcmp but tolerates null pointers (a null pointer orders
// before any non-null string). This prevents a segfault when a comparison
// operand is null — e.g. `arr[i].name == "x"` where arr's element type is
// unknown at compile time and member access safely returns null instead of a
// string. Ordering: null==null -> 0, null < non-null -> -1, non-null > null -> +1.
extern "C" int32_t shadow_str_cmp(const char* a, const char* b) {
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return (int32_t)strcmp(a, b);
}
extern "C" void* shadow_array_append(void* arr, void* elem) { return arr; }
extern "C" void* shadow_array_concat(void* a, void* b) {
    ShadowArray* result = new ShadowArray();
    if (a) {
        uintptr_t ua = (uintptr_t)a;
        if (ua > 10000) {
            ShadowArray* arr_a = reinterpret_cast<ShadowArray*>(a);
            if (arr_a->type_tag == 0) {
                result->data = arr_a->data;
            }
        }
    }
    if (b) {
        uintptr_t ub = (uintptr_t)b;
        if (ub > 10000) {
            ShadowArray* arr_b = reinterpret_cast<ShadowArray*>(b);
            if (arr_b->type_tag == 0) {
                for (size_t _i = 0; _i < arr_b->size(); ++_i) {
                    result->push_back((*arr_b)[_i]);
                }
            }
        }
    }
    return result;
}
extern "C" const char* shadow_typeof(void* val) {
    if (!val) return "null";
    uintptr_t u = (uintptr_t)val;
    if (u > 0 && u < 10000) return "number";   // interpreter small-int optimization
    if (!is_valid_ptr(val)) return "number";
    AnyBox* b = reinterpret_cast<AnyBox*>(val);
    if (b->magic == ANYBOX_MAGIC) {
        // boxed scalar: report the static type name the bootstrap expects
        switch (b->tag) {
            case 0: return "number";   // int/long/short/char
            case 1: return "float";
            case 2: return "string";
            case 3: return "bool";
            default: return "unknown";
        }
    }
    // raw heap object (array/dict/set) or raw string pointer
    ShadowArray* a = reinterpret_cast<ShadowArray*>(val);
    if (a && a->type_tag == 0) return "array";
    ShadowDict* d = reinterpret_cast<ShadowDict*>(val);
    if (d && d->type_tag == 1) return "dict";
    ShadowSet* s = reinterpret_cast<ShadowSet*>(val);
    if (s && s->type_tag == 2) return "set";
    return "string";  // raw char*
}
extern "C" void* shadow_range(int32_t s, int32_t e, int32_t step) {
    ShadowArray* a = new ShadowArray();
    if (step == 0) step = 1;
    if (step > 0) {
        for (int64_t v = s; v < (int64_t)e; v += step)
            a->push_back(DictValue((int64_t)v));
    } else {
        for (int64_t v = s; v > (int64_t)e; v += step)
            a->push_back(DictValue((int64_t)v));
    }
    return a;
}
extern "C" int64_t shadow_struct_size(void* d) { return 0; }
extern "C" const char* shadow_string_copy(const char* s) {
    if(!s)return 0; size_t l=strlen(s); char* r=(char*)malloc(l+1); memcpy(r,s,l); r[l]=0; return r;
}

// Crash-safe debug marker: writes to stderr (unbuffered) and flushes, so the
// last marker survives a segfault (unlike stdout, which is fully buffered when
// piped and lost on crash). Used by the shadow-lang debug build to locate crashes.
extern "C" void shadow_dbg_mark(const char* s) {
    if (!s) s = "(null)";
    fprintf(stderr, "[DBG] %s\n", s);
    fflush(stderr);
}

// Ã¢ÂÂÃ¢ÂÂ shadow_dict_set Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
// `tag` follows the same convention as the codegen (tagFromType):
//   0=int/long/short/char  1=float/double  2=string/any  3=bool  4=struct/ptr
extern "C" void* shadow_dict_set(void* dict_ptr, void* key, int32_t tag, void* val) {
    if (!dict_ptr || !key) return dict_ptr;
    ShadowDict* d = (ShadowDict*)dict_ptr;
    DictValue dv;
    switch (tag) {
        case 0:  dv = DictValue((int64_t)(intptr_t)val); break;  // int family
        case 1: {                                                 // float/double
            int64_t bits = (int64_t)(intptr_t)val;
            double dval;
            std::memcpy(&dval, &bits, sizeof(double));
            dv = DictValue(dval);
            break;
        }
        case 2:  dv = DictValue(std::string((const char*)val)); break;  // string
        case 3:  dv = DictValue((bool)(intptr_t)val); break;            // bool
        case 4:  dv = DictValue((void*)val); break;                    // struct/ptr
        default: dv = DictValue((int64_t)(intptr_t)val); break;
    }
    sdict_set(d, (const char*)key, dv);
    return dict_ptr;
}

// Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
// ADDITIONAL RUNTIME FUNCTIONS Ã¢ÂÂ for codegen mode
// Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
#include <time.h>
#include <sys/stat.h>

extern "C" void shadow_sleep(int32_t ms) {
#ifdef _WIN32
    Sleep(ms);
#endif
}

extern "C" int32_t shadow_now() {
    return (int32_t)time(nullptr);
}

extern "C" int32_t shadow_today() {
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}

extern "C" const char* shadow_env_get(const char* name) {
    if (!name) return nullptr;
#ifdef _WIN32
    char buf[4096];
    DWORD r = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (r == 0 || r >= sizeof(buf)) return nullptr;
    return dup_str(buf);
#else
    return getenv(name);
#endif
}

#ifdef _WIN32
extern "C" const char* shadow_sys_exec(const char* cmd) {
    if (!cmd) return nullptr;
    // Windows: use _popen (POSIX popen equivalent in MSVC CRT)
    FILE* f = _popen(cmd, "r");
    if (!f) return strdup("");
    std::string result;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) result += buf;
    _pclose(f);
    return strdup(result.c_str());
}
#endif


extern "C" int32_t shadow_copy_file(const char* src, const char* dst) {
    if (!src || !dst) return 0;
#ifdef _WIN32
    return CopyFileA(src, dst, FALSE) ? 1 : 0;
#else
    return 0;
#endif
}

extern "C" int32_t shadow_mkdir(const char* path) {
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) ? 1 : 0;
#else
    // POSIX：mkdir -p 语义，逐层创建所有缺失的父目录（兼容 Windows 单层契约，
    // 同时让 a\b\c 这类多层级 dir_create 在 Linux 上也能一次建好）。
    if (!path || path[0] == '\0') return 0;
    std::string np = shadow_path_to_slashes(path);
    std::string cur;
    cur.reserve(np.size() + 1);
    for (size_t i = 0; i < np.size(); i++) {
        char c = np[i];
        cur.push_back(c);
        if (c == '/') {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return 0;
        }
    }
    // 末段（无尾斜杠时为最终目录名）
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(cur.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    }
    return 1;
#endif
}

extern "C" int32_t shadow_path_exists(const char* path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
#else
    return access(path, F_OK) == 0 ? 1 : 0;
#endif
}

static bool shadow_path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static bool shadow_path_is_absolute(const std::string& p) {
    if (p.empty()) return false;
    if (shadow_path_is_sep(p[0])) return true;
    return p.size() >= 3 && std::isalpha((unsigned char)p[0]) && p[1] == ':' && shadow_path_is_sep(p[2]);
}

static std::string shadow_path_to_slashes(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_sep = false;
    for (char ch : input) {
        char c = shadow_path_is_sep(ch) ? '/' : ch;
        if (c == '/') {
            if (last_sep) continue;
            last_sep = true;
        } else {
            last_sep = false;
        }
        out.push_back(c);
    }
    return out;
}

extern "C" const char* shadow_path_normalize(const char* path) {
    if (!path) return dup_str("");
    return dup_str(shadow_path_to_slashes(std::string(path)));
}

extern "C" const char* shadow_path_join(const char* base, const char* child) {
    std::string b = shadow_path_to_slashes(base ? std::string(base) : std::string(""));
    std::string c = shadow_path_to_slashes(child ? std::string(child) : std::string(""));
    if (b.empty()) return dup_str(c);
    if (c.empty()) return dup_str(b);
    if (shadow_path_is_absolute(c)) return dup_str(c);
    while (!b.empty() && b.back() == '/') b.pop_back();
    while (!c.empty() && c.front() == '/') c.erase(c.begin());
    return dup_str(b + "/" + c);
}

extern "C" const char* shadow_path_dirname(const char* path) {
    if (!path) return dup_str(".");
    std::string p = shadow_path_to_slashes(std::string(path));
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return dup_str(".");
    if (pos == 0) return dup_str("/");
    if (pos == 2 && p.size() >= 3 && p[1] == ':') return dup_str(p.substr(0, 3));
    return dup_str(p.substr(0, pos));
}

extern "C" const char* shadow_path_basename(const char* path) {
    if (!path) return dup_str("");
    std::string p = shadow_path_to_slashes(std::string(path));
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return dup_str(p);
    return dup_str(p.substr(pos + 1));
}

extern "C" int32_t shadow_rmdir(const char* path) {
#ifdef _WIN32
    return RemoveDirectoryA(path) ? 1 : 0;
#else
    if (!path) return 0;
    std::string np = shadow_path_to_slashes(path);
    return rmdir(np.c_str()) == 0 ? 1 : 0;
#endif
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_get_int Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" int64_t shadow_array_get_int(void* array_ptr, int32_t idx) {
    if (!array_ptr) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return 0;
    }
    if ((*a)[idx].tag == 0)
        return (*a)[idx].val.i;
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_get_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" const char* shadow_array_get_string(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return nullptr;
    }
    if ((*a)[idx].tag == 2) {
        const char* s = (*a)[idx].val.s;
        return s ? strdup(s) : nullptr;
    }
    return nullptr;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_get_bool Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" int32_t shadow_array_get_bool(void* array_ptr, int32_t idx) {
    if (!array_ptr) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return 0;
    }
    if ((*a)[idx].tag == 3)
        return (*a)[idx].val.b ? 1 : 0;
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_set_int Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

// shadow_array_get_ptr: get pointer-sized element (string, any, nested array)
extern "C" void* shadow_array_get_ptr(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return nullptr;
    }
    // Full variant coverage (mirrors shadow_array_get). The transparent-`any`
    // model expects raw handles for scalars: an int element is returned as
    // (void*)(intptr_t)value so shadow_any_as_int can peel it; previously
    // only string/void* were handled and scalars fell through to nullptr,
    // which made `a[i] as int` read 0 instead of the real element.
    DictValue& val = (*a)[idx];
    if (val.tag == 0)
        return (void*)(intptr_t)val.val.i;
    if (val.tag == 1) {
        g_any_float_buf = val.val.d;
        return &g_any_float_buf;
    }
    if (val.tag == 2)
        return (void*)strdup(val.val.s);
    if (val.tag == 3) {
        g_any_bool_buf = val.val.b;
        return &g_any_bool_buf;
    }
    if (val.tag == 4)
        return val.val.p;
    return nullptr;
}

// shadow_array_set_ptr: set pointer-sized element (string, any, nested array)
extern "C" void shadow_array_set_ptr(void* array_ptr, int32_t idx, void* val) {
    if (!array_ptr) return;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return;
    }
    (*a)[idx] = (DictValue)val;
}
extern "C" void* shadow_array_set_int(void* array_ptr, int32_t idx, int64_t val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return array_ptr;
    }
    (*a)[idx] = val;
    return array_ptr;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_set_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" void* shadow_array_set_string(void* array_ptr, int32_t idx, const char* val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->size() + idx;
    if (idx < 0 || (size_t)idx >= a->size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->size());
        return array_ptr;
    }
    (*a)[idx] = std::string(val ? val : "");
    return array_ptr;
}

// ── array_push: append an element to the end, return the (stable) array ptr ──
extern "C" void* shadow_array_push_int(void* array_ptr, int32_t val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->push_back((DictValue)(int64_t)val);
    return array_ptr;
}

// 快速路径 array_push（shadow 层 C 布局数组，kind=2）：
//   [0]=len(int32) [4]=cap(int32) [8]=elem_size(int32) [12..]=data
// 单次 C 调用完成 len/cap 读取 + 扩容 + 元素写入 + len 更新，
// 消除 shadow 层每次 push 的多次 extern 调用（__rt_shadow_load_int×3 +
// __rt_shadow_store_int×2 → 1 次调用）。语义与 runtime_lib.shadow 的
// shadow_array_push_int 完全一致（含 null 首推、2x 倍增扩容、es 4/8 分派）。
extern "C" int32_t shadow_gc_root_set(void* slot, void* val);  // defined later in TU
extern "C" void* shadow_array_push_int_fast(void* array_ptr, int32_t val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 4, 2);
        *reinterpret_cast<int32_t*>(na) = 1;
        *reinterpret_cast<int32_t*>((char*)na + 4) = 4;
        *reinterpret_cast<int32_t*>((char*)na + 8) = 4;
        *reinterpret_cast<int32_t*>((char*)na + 12) = val;
        return na;
    }
    int32_t len = *reinterpret_cast<int32_t*>(array_ptr);
    int32_t cap = *reinterpret_cast<int32_t*>((char*)array_ptr + 4);
    int32_t es = *reinterpret_cast<int32_t*>((char*)array_ptr + 8);
    void* na = nullptr;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        // Linux 无保守栈扫描：grow 后新数组须显式扎根，防并发 GC 在拷贝/存储前回收。
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *reinterpret_cast<int32_t*>(na) = len;
        *reinterpret_cast<int32_t*>((char*)na + 4) = nc;
        *reinterpret_cast<int32_t*>((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *reinterpret_cast<int32_t*>((char*)array_ptr + off) = val;
    else *reinterpret_cast<int64_t*>((char*)array_ptr + off) = val;
    *reinterpret_cast<int32_t*>(array_ptr) = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
// 同快速路径：long / float / ptr 元素（es=8），单次 C 调用完成 push。
extern "C" void* shadow_array_push_long_fast(void* array_ptr, int64_t val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *reinterpret_cast<int32_t*>(na) = 1;
        *reinterpret_cast<int32_t*>((char*)na + 4) = 4;
        *reinterpret_cast<int32_t*>((char*)na + 8) = 8;
        *reinterpret_cast<int64_t*>((char*)na + 12) = val;
        return na;
    }
    int32_t len = *reinterpret_cast<int32_t*>(array_ptr);
    int32_t cap = *reinterpret_cast<int32_t*>((char*)array_ptr + 4);
    int32_t es = *reinterpret_cast<int32_t*>((char*)array_ptr + 8);
    void* na = nullptr;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *reinterpret_cast<int32_t*>(na) = len;
        *reinterpret_cast<int32_t*>((char*)na + 4) = nc;
        *reinterpret_cast<int32_t*>((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *reinterpret_cast<int32_t*>((char*)array_ptr + off) = (int32_t)val;
    else *reinterpret_cast<int64_t*>((char*)array_ptr + off) = val;
    *reinterpret_cast<int32_t*>(array_ptr) = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern "C" void* shadow_array_push_float_fast(void* array_ptr, double val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *reinterpret_cast<int32_t*>(na) = 1;
        *reinterpret_cast<int32_t*>((char*)na + 4) = 4;
        *reinterpret_cast<int32_t*>((char*)na + 8) = 8;
        *reinterpret_cast<double*>((char*)na + 12) = val;
        return na;
    }
    int32_t len = *reinterpret_cast<int32_t*>(array_ptr);
    int32_t cap = *reinterpret_cast<int32_t*>((char*)array_ptr + 4);
    int32_t es = *reinterpret_cast<int32_t*>((char*)array_ptr + 8);
    void* na = nullptr;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *reinterpret_cast<int32_t*>(na) = len;
        *reinterpret_cast<int32_t*>((char*)na + 4) = nc;
        *reinterpret_cast<int32_t*>((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *reinterpret_cast<float*>((char*)array_ptr + off) = (float)val;
    else *reinterpret_cast<double*>((char*)array_ptr + off) = val;
    *reinterpret_cast<int32_t*>(array_ptr) = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern "C" void* shadow_array_push_ptr_fast(void* array_ptr, void* val) {
    if (!array_ptr) {
        void* na = shadow_gc_alloc(12 + 4 * 8, 2);
        *reinterpret_cast<int32_t*>(na) = 1;
        *reinterpret_cast<int32_t*>((char*)na + 4) = 4;
        *reinterpret_cast<int32_t*>((char*)na + 8) = 8;
        *reinterpret_cast<void**>((char*)na + 12) = val;
        return na;
    }
    int32_t len = *reinterpret_cast<int32_t*>(array_ptr);
    int32_t cap = *reinterpret_cast<int32_t*>((char*)array_ptr + 4);
    int32_t es = *reinterpret_cast<int32_t*>((char*)array_ptr + 8);
    void* na = nullptr;
    if (len >= cap) {
        int32_t nc = cap * 2;
        if (nc == 0) nc = 4;
        na = shadow_gc_alloc(12 + nc * es, 2);
        shadow_gc_root_set(&na, na);
        memcpy((char*)na + 12, (char*)array_ptr + 12, (size_t)len * es);
        *reinterpret_cast<int32_t*>(na) = len;
        *reinterpret_cast<int32_t*>((char*)na + 4) = nc;
        *reinterpret_cast<int32_t*>((char*)na + 8) = es;
        array_ptr = na;
    }
    int32_t off = 12 + len * es;
    if (es == 4) *reinterpret_cast<int32_t*>((char*)array_ptr + off) = (int32_t)(intptr_t)val;
    else *reinterpret_cast<void**>((char*)array_ptr + off) = val;
    *reinterpret_cast<int32_t*>(array_ptr) = len + 1;
    if (na) shadow_gc_root_set(&na, 0);
    return array_ptr;
}
extern "C" void* shadow_array_push_long(void* array_ptr, int64_t val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->push_back((DictValue)val);
    return array_ptr;
}
extern "C" void* shadow_array_push_float(void* array_ptr, double val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->push_back((DictValue)val);
    return array_ptr;
}
extern "C" void* shadow_array_push_ptr(void* array_ptr, void* val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->push_back((DictValue)val);
    return array_ptr;
}

// ── array_pop: remove and return the last element (transparent-any value) ──
// 返回裸值（小整数直接为 intptr 值；字符串/指针元素返回指针；double 装箱为 AnyBox）。
extern "C" void* shadow_array_pop(void* array_ptr) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (a->empty()) return nullptr;
    DictValue v = a->back();
    a->pop_back();
    if (v.tag == 0) return (void*)(intptr_t)v.val.i;
    if (v.tag == 3) return (void*)(intptr_t)(v.val.b ? 1 : 0);
    if (v.tag == 1) {
        AnyBox* b = (AnyBox*)shadow_gc_alloc((int32_t)sizeof(AnyBox), 3);
        if (!b) return nullptr;
        b->magic = ANYBOX_MAGIC;
        b->tag = 2;  // float
        b->value = 0;
        memcpy(&b->value, &v.val.d, sizeof(double));
        return b;
    }
    if (v.tag == 2) return (void*)strdup(v.val.s);
    return v.val.p;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_dict_get_int Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" int64_t shadow_dict_get_int(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    DictValue* v = sdict_get(d, key);
    if (v && v->tag == 0)
        return v->val.i;
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_dict_get_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" const char* shadow_dict_get_string(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return nullptr;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    DictValue* v = sdict_get(d, key);
    if (v && v->tag == 2)
        return strdup(v->val.s);
    return nullptr;
}

// Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
// EXCEPTION HANDLING Ã¢ÂÂ setjmp/longjmp helpers
// The codegen generates @setjmp/@longjmp calls directly in LLVM IR
// Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
#include <cstring>
#include <cstdint>

static thread_local char g_exception_msg[1024] = {0};
static thread_local int g_exception_flag = 0;

extern "C" void shadow_throw_str(const char* msg) {
    g_exception_flag = 1;
    if (msg) {
        strncpy(g_exception_msg, msg, sizeof(g_exception_msg) - 1);
        g_exception_msg[sizeof(g_exception_msg) - 1] = 0;
    } else {
        g_exception_msg[0] = 0;
    }
}

extern "C" void shadow_throw_int(int64_t val) {
    g_exception_flag = 1;
    snprintf(g_exception_msg, sizeof(g_exception_msg), "%lld", (long long)val);
}

extern "C" int32_t shadow_exception_occurred() {
    return g_exception_flag;
}

extern "C" const char* shadow_exception_value() {
    return g_exception_msg;
}

extern "C" void shadow_exception_clear() {
    g_exception_flag = 0;
    // Don't clear g_exception_msg Ã¢ÂÂ it's still valid until next throw
}

// Ã¢ÂÂÃ¢ÂÂ `any` type boxing helpers Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
// Stores (tag, value) pair in a heap buffer so the runtime can
// dispatch shadow_any_to_string correctly at print time.
//   tag: 0=int, 1=float, 2=string, 3=bool
//   value: int64 (or double for float, or pointer for string)
// AnyBox struct & ANYBOX_MAGIC are defined near the top of this file
// (see the block after the #includes) so that shadow_index_any /
// shadow_member_any / shadow_typeof can reference AnyBox before this point.
extern "C" void* shadow_any_box(int32_t tag, int64_t value) {
    AnyBox* b = (AnyBox*)shadow_gc_alloc((int32_t)sizeof(AnyBox), 3);
    if (!b) return nullptr;
    b->magic = ANYBOX_MAGIC;
    b->tag = tag; b->value = value;
    // P3：段分配 + 带内头（kind=3），GC 经头追踪 AnyBox.value → GC 对象，
    // 只被 `any` 变量引用的对象不再被过早回收。
    return b;
}
extern "C" void* shadow_any_box_ptr(int32_t tag, void* ptr) {
    AnyBox* b = (AnyBox*)shadow_gc_alloc((int32_t)sizeof(AnyBox), 3);
    if (!b) return nullptr;
    b->magic = ANYBOX_MAGIC;
    b->tag = tag;
    b->value = (int64_t)(intptr_t)ptr;
    return b;
}
// Box a raw C string pointer as an AnyBox (tag 3 = string).
extern "C" void* shadow_any_box_string(void* s) {
    AnyBox* b = (AnyBox*)shadow_gc_alloc((int32_t)sizeof(AnyBox), 3);
    if (!b) return nullptr;
    b->magic = ANYBOX_MAGIC;
    b->tag = 3;
    b->value = (int64_t)(intptr_t)s;
    return b;
}
// Box a double as an AnyBox (tag 2 = float). Bit-cast the double to int64 so
// the exact bit pattern survives (no precision loss through int conversion).
extern "C" void* shadow_any_box_double(double d) {
    int64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    AnyBox* b = (AnyBox*)shadow_gc_alloc((int32_t)sizeof(AnyBox), 3);
    if (!b) return nullptr;
    b->magic = ANYBOX_MAGIC;
    b->tag = 2;
    b->value = bits;
    return b;
}
// Peel a boxed pointer back to the raw struct/array pointer. Array slots that
// hold struct or array values store an AnyBox* (value = real object pointer).
// This reads AnyBox.value (offset 8) so the consumer gets the actual object.
extern "C" void* shadow_any_unbox_ptr(void* ptr) {
    if (!ptr) return nullptr;
    uintptr_t u = (uintptr_t)ptr;
    if (u > 0 && u < 10000) return ptr;  // small-int smi -> not a box
    if (!is_valid_ptr(ptr)) return ptr;
    AnyBox* b = reinterpret_cast<AnyBox*>(ptr);
    if (b->magic == ANYBOX_MAGIC) {
        return (void*)(intptr_t)b->value;
    }
    return ptr;  // not a box -> already a raw pointer
}
extern "C" int32_t shadow_any_as_int(void* ptr) {
    if (!ptr) return 0;
    uintptr_t u = (uintptr_t)ptr;
    if (u > 0 && u < 10000) return (int32_t)u;
    if (!is_valid_ptr(ptr)) return 0;
    AnyBox* b = reinterpret_cast<AnyBox*>(ptr);
    if (b->magic == ANYBOX_MAGIC) {
        return (int32_t)b->value;
    }
    return 0;
}
// Unbox an `any` to a C string pointer. tag 2 = string (pointer stored in
// value); other tags are formatted. Used by 0.3's codegen when casting any→string.
extern "C" const char* shadow_any_as_string(void* ptr) {
    if (!ptr) return "";
    uintptr_t u = (uintptr_t)ptr;
    if (u > 0 && u < 0x10000) return "";  // raw int handle, not an AnyBox
    if (!is_valid_ptr(ptr)) return "";
    AnyBox* b = reinterpret_cast<AnyBox*>(ptr);
    if (b->magic == ANYBOX_MAGIC) {
        // AnyBox tag 约定（与 runtime_lib.shadow 一致）：0=int,1=long,2=float,3=string,4=bool
        if (b->tag == 3) return (const char*)(intptr_t)b->value;  // string
        if (b->tag == 2) {  // float (double bit pattern)
            static char dbuf[64]; snprintf(dbuf, sizeof(dbuf), "%g", *(double*)&b->value); return dbuf;
        }
        // int / long / bool → 格式化为稳定缓冲区
        static char ibuf[64];
        if (b->tag == 1) snprintf(ibuf, sizeof(ibuf), "%lld", (long long)b->value);        // long
        else if (b->tag == 4) snprintf(ibuf, sizeof(ibuf), "%s", b->value ? "true" : "false"); // bool
        else snprintf(ibuf, sizeof(ibuf), "%lld", (long long)b->value);                     // int
        return ibuf;
    }
    // Not an AnyBox: transparent 'any' model. A string element (e.g. from
    // shadow_array_get) is stored as a raw C-string pointer, not a boxed AnyBox.
    // Treat it as a C string here. (Mirrors shadow_any_print's last-resort path.)
    const char* s = (const char*)ptr;
    if (s && s[0]) return s;
    return "";
}
extern "C" const char* shadow_any_print(void* ptr) {
    if (!ptr) return "0";
    // Check static buffers first: pointer may come from shadow_array_get/shadow_dict_get
    if (ptr == &g_any_int_buf) {
        char b[32]; snprintf(b, sizeof(b), "%lld", (long long)g_any_int_buf); return dup_str(b);
    }
    if (ptr == &g_any_bool_buf) {
        return g_any_bool_buf ? "true" : "false";
    }
    if (ptr == &g_any_float_buf) {
        char b[32]; snprintf(b, sizeof(b), "%g", g_any_float_buf); return dup_str(b);
    }
    // Raw int handle from shadow_array_get (transparent model):
    // shadow_array_get returns (void*)(intptr_t)value for ints. This is the
    // ptr value itself, NOT an AnyBox*.  On Windows, the lowest valid heap
    // address is well above 64KB, so values < 65536 are definitely raw handles.
    uintptr_t u = (uintptr_t)ptr;
    if (u > 0 && u < 0x10000) {
        char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)u); return dup_str(b);
    }
    // Check if it's an AnyBox* (identified by magic, NOT just tag range,
    // because raw ShadowArray/ShadowDict also use small type_tags 0/1)
    if (!is_valid_ptr(ptr)) return "number";
    AnyBox* b = reinterpret_cast<AnyBox*>(ptr);
    if (b->magic == ANYBOX_MAGIC && b->tag >= 0 && b->tag <= 3) {
        char buf[128];
        // AnyBox tag 约定（与 runtime_lib.shadow 一致）：0=int,1=long,2=float,3=string,4=bool
        switch (b->tag) {
            case 0: snprintf(buf, sizeof(buf), "%lld", (long long)b->value); return dup_str(buf);  // int
            case 1: snprintf(buf, sizeof(buf), "%lld", (long long)b->value); return dup_str(buf);  // long
            case 2: snprintf(buf, sizeof(buf), "%g", *(double*)&b->value); return dup_str(buf);    // float
            case 3: return (const char*)(intptr_t)b->value;                                        // string
            case 4: return b->value ? "true" : "false";                                           // bool
            default: return "?";
        }
    }
    // Last resort: treat as string pointer (from shadow_array_get for strings)
    const char* s = (const char*)ptr;
    if (s && s[0]) return dup_str(s);
    return "?";
}
extern "C" int shadow_println_any(void* ptr) {
    const char* s = shadow_any_print(ptr);
    printf("%s\n", s ? s : "");
    fflush(stdout);
    return 0;
}
extern "C" void* shadow_any_unbox(void* ptr) {
    if (!ptr) return nullptr;
    uintptr_t u = (uintptr_t)ptr;
    // Small integer optimization: if the value looks like a raw handle
    // (e.g. inttoptr (i64 N to ptr)), return it as-is.
    if (u > 0 && u < 10000) return ptr;
    if (!is_valid_ptr(ptr)) return ptr;
    AnyBox* b = reinterpret_cast<AnyBox*>(ptr);
    if (b->magic == ANYBOX_MAGIC) return (void*)(intptr_t)b->value;
    return ptr;  // already a raw value
}

// ── GC: generational STW mark-sweep with in-band headers (W3) ──
// P3：GCMetaMap 已退役。每个对象在载荷前 16B 带一个 ShadowHdr（epoch/kind/
//   gen/surv/size/req），由分段堆（GcSegment bump 分配）承载。存活判据 =
//   (hdr.epoch & HDR_EPOCH_MASK) == 当前轮；trace 位（HDR_TRACE_BIT）= 本轮已追。
//   - shadow_gc_alloc(size, kind): 段分配 + 写头（分配即黑 = 写当前 epoch）
//   - shadow_gc_register: 已空实现（仅留符号兼容；所有对象走段分配）
//   - shadow_gc_frame_enter(): snapshot current root count → returns marker
//   - shadow_gc_root_add(ptr): push a root (a live GC pointer)
//   - shadow_gc_root_set(slot, val): replace-semantics root (keyed by slot addr)
//   - shadow_gc_frame_leave(marker): pop roots back to marker (LIFO scope)
//   - shadow_gc_collect(): full mark-sweep (major GC)
//   - shadow_gc_minor_collect(): young-gen-only collection (minor GC)
//   - shadow_gc_set_finalizer(ptr, fn, data): register finalizer callback
//
// Generational model:
//   - Young gen (gen=0): newly allocated objects. Minor GC scans only young gen.
//   - Old gen (gen=1): objects surviving PROMOTION_THRESHOLD cycles.
//   - Remembered set: old→young references tracked via write barrier.
//
// Lock-free design:
//   - 头内 epoch 字用 std::atomic<uint32_t> CAS 完成去重+标记（无锁标记）。
//   - GC collect uses a spinlock (std::atomic_flag) to coordinate concurrent collect triggers.
//   - Root vectors use thread-local accumulation + atomic snapshot for mark phase.
//
// Finalizer:
//   - 落旁路表 g_finalizers（段对象不在任何登记表中）。
//   - During sweep, finalizers run in a separate pass BEFORE freeing.
//   - Finalizers may resurrect objects (add back to a root set) — objects with
//     finalizers that were dead get one extra cycle before actual free.
//
// kind = 类型 id（RT_T_*）。权威语义与分派逻辑见 gc_trace_object_children()：
//   0   = 未分类（rt_malloc 通用对象）→ 逐字保守扫描，兜住 AnyBox.value、字符串内嵌指针
//   1   = RT_T_STRING  纯字节，不扫内部
//   2   = RT_T_ARRAY   shadow 层 C 布局 [len:4][cap:4][elem_size:4][data@12]
//                      （runtime_lib.shadow_array_new 与各数组扩容路径传的就是 2，
//                       不是"字典"——见下方历史注记）
//   3   = RT_T_ANYBOX  [tag:4][value@4]；其 value 可能是 GC 指针，缺此类型会使
//                      global→AnyBox→ShadowArray 链断裂而提前释放
//   4   = RT_T_DICT    rt_dict
//   5   = RT_T_CLOSURE [fn_ptr@0][env_ptr@8]
//   ≥100= RT_T_USER    用户类型，按类型表 bitmap 精确追踪（>512B 退化为整块保守）
// 历史注记：此处曾写作 "0=raw, 1=ShadowArray, 2=ShadowDict, 3=AnyBox"，与实现不符，
// 已按代码更正。判断 kind 语义一律以 gc_trace_object_children 为准。

// ── 分配门控计数 ──
// P2：攒批机制已删除——分配即写带内头，记账内联原子完成，无需攒批/延迟登记。
// g_mutator_threads 仍用于空闲链表/无锁路径的单 mutator 门控。
static std::atomic<int32_t> g_mutator_threads{0};
// 前向声明：g_heap_bytes 定义在下方（GC 触发状态小节）。
extern std::atomic<int64_t> g_heap_bytes;

// ── 大小类空闲链表分配器（Linux 单 mutator 优化）──
// 清扫把死对象按大小类压入空闲链表（免 free()），分配从链表弹出（免 malloc）。
// 对象数据前 16 字节存 FLNode（next + size）：size 用于弹出时校验 ≥ 请求，
// 避免同类的较小对象被较大请求复用（类区间是 2 的幂开区间，同类可含多个
// 8 对齐尺寸）。仅 16B..64KB 的对象走链表（<16B 无节点空间、>64KB 大对象
// 直接 free/malloc），且仅 g_mutator_threads<=1 时启用（与攒批快路径同门控）。
#define FL_NUM_CLASS 14           // 8,16,...,65536
struct FLNode { FLNode* next; int32_t size; };
static void* g_fl_head[FL_NUM_CLASS] = {0};

static int fl_class_of(int32_t asize) {
    int c = 0;
    int32_t s = 8;
    while (s < asize && c < FL_NUM_CLASS - 1) { s <<= 1; c++; }
    return c;
}

static bool fl_enabled() {
    static int disabled = -1;
    if (disabled < 0) disabled = getenv("SHADOW_GC_NO_FL") ? 1 : 0;
    return disabled == 0;
}

static void fl_push(void* p, int32_t msize) {
    FLNode* n = (FLNode*)p;
    int c = fl_class_of(msize);
    n->next = (FLNode*)g_fl_head[c];
    n->size = msize;
    g_fl_head[c] = n;
}

static void* fl_pop(int32_t asize) {
    int c = fl_class_of(asize);
    FLNode** pp = (FLNode**)&g_fl_head[c];
    while (*pp) {
        FLNode* n = *pp;
        if (n->size >= asize) { *pp = n->next; return n; }
        pp = &n->next;
    }
    return nullptr;
}

// P3b：摘除落在 [lo, hi) 内的空闲链表节点（整段退役前必做——段内存即将
// 重置复用，悬空的复用槽会与新分配对象重叠）。
static void fl_purge_range(char* lo, char* hi) {
    for (int c = 0; c < FL_NUM_CLASS; c++) {
        FLNode** pp = (FLNode**)&g_fl_head[c];
        while (*pp) {
            char* np = (char*)(*pp);
            if (np >= lo && np < hi) *pp = (*pp)->next;  // 丢弃（不回收其它内存）
            else pp = &(*pp)->next;
        }
    }
}

// Lock-free collect coordination: spinlock prevents concurrent collect cycles.
// (gc_lock/gc_unlock defined after ThreadGCState — they mark stw_state while
// spinning so a concurrent collector is treated as blocked by the STW wait.)
static std::atomic_flag g_gc_collect_lock = ATOMIC_FLAG_INIT;

// Thread-local allocation counter for lock-free GC trigger.
static thread_local int64_t tl_gc_alloc_count = 0;
static std::atomic<int64_t> g_gc_total_alloc_count{0};

// ── 阶段计时（SHADOW_GC_PROFILE=1 时 atexit 打印分配/标记/清扫耗时）──
static int64_t g_prof_mark_ns = 0;
static int64_t g_prof_sweep_ns = 0;
static int64_t g_prof_gc_ns = 0;
static int64_t g_prof_gc_count = 0;
static int64_t g_prof_mark_reset_ns = 0;   // mark：epoch 推进/回绕处理
static int64_t g_prof_mark_roots_ns = 0;   // mark：根追踪
static int64_t g_prof_mark_wl_ns = 0;      // mark：worklist 展开
static int g_prof_on = -1;
static int prof_on(void) {
    if (g_prof_on < 0) { const char* e = getenv("SHADOW_GC_PROFILE"); g_prof_on = (e && e[0] == '1') ? 1 : 0; }
    return g_prof_on;
}

// ── 自动 GC 触发状态（对齐 Windows rt_gc.c 的 GOGC / SHADOW_GC_STRESS 模型）──
std::atomic<int64_t> g_heap_bytes{0};     // 未释放对象总字节（alloc+，sweep-；非 static 供 flush 前向声明）
static std::atomic<int64_t> g_gc_trigger{4 * 1024 * 1024};  // GOGC 触发点（初始最小堆 4MB）
static std::atomic<int>     g_gc_running{0};     // 防重入：collect 在途
static std::atomic<int64_t> g_alloc_ticks{0};    // STRESS 模式分配计数
static int64_t g_gc_gogc = -1;                   // SHADOW_GOGC（默认 100）
static int32_t g_gc_stress = -1;                 // SHADOW_GC_STRESS（0=关）
static int32_t g_gc_auto = -1;                   // SHADOW_GC_AUTO（0=禁用自动触发，供 reseed 种子）
static int64_t g_gc_min_heap = -1;               // SHADOW_GC_MIN_HEAP（最小触发堆字节，默认 4MB）
static uint32_t g_gc_epoch = 0;                  // 标记世代：visited 存 epoch，免每轮全量清 visited

static int64_t rt_gc_min_heap(void) {
    if (g_gc_min_heap < 0) {
        const char* e = getenv("SHADOW_GC_MIN_HEAP");
        g_gc_min_heap = (e && *e) ? atoll(e) : 4 * 1024 * 1024;
        if (g_gc_min_heap < 64 * 1024) g_gc_min_heap = 64 * 1024;
    }
    return g_gc_min_heap;
}

// 前向声明：段堆统计（定义在下方带内头基础设施块）。
static void gc_seg_stats(size_t* nseg, int64_t* objs);

static void prof_atexit(void) {
    if (!prof_on()) return;
    fprintf(stderr, "[PROF] mark_ns=%lld sweep_ns=%lld gc_ns=%lld gc_count=%lld total_alloc=%lld heap=%lld trigger=%lld\n",
            (long long)g_prof_mark_ns, (long long)g_prof_sweep_ns,
            (long long)g_prof_gc_ns, (long long)g_prof_gc_count,
            (long long)g_gc_total_alloc_count.load(std::memory_order_relaxed),
            (long long)g_heap_bytes.load(std::memory_order_relaxed),
            (long long)g_gc_trigger.load(std::memory_order_relaxed));
    fprintf(stderr, "[PROF] mark_reset_ns=%lld mark_roots_ns=%lld mark_wl_ns=%lld\n",
            (long long)g_prof_mark_reset_ns, (long long)g_prof_mark_roots_ns, (long long)g_prof_mark_wl_ns);
    size_t _nseg = 0; int64_t _sobjs = 0;
    gc_seg_stats(&_nseg, &_sobjs);
    fprintf(stderr, "[PROF] segments=%zu seg_objs=%lld\n", _nseg, (long long)_sobjs);
}
struct ProfAtexit { ~ProfAtexit() { prof_atexit(); } };
static ProfAtexit _prof_atexit;

// ── 协作式 STW（对齐 Windows rt_gc.o 的 g_stw_req/g_stw_active）──
// collect 置 req=1 → 等所有 free-running 线程在 poll（安全点）置 stw_state=1
// 并自旋 → active=1 扫根 → 放行（req/active 清零）。blocked（state=2）线程在
// 等 g_gc_mutex，不跑 mutator，collect 无需等它。
static std::atomic<int> g_gc_stw_req{0};
static std::atomic<int> g_gc_stw_active{0};
// 单 poll 标志：STW 请求或 alloc 触发时置 1，poll 快路径只查它（sum_loop 等
// 无分配热循环免去每次 6+ 次原子/普通加载）。慢路径清 0 后做完整 STW/触发检查。
// 非 static + extern "C"：codegen 内联轮询以 volatile i32 直接加载本全局（快路径零函数调用）。
// extern "C" 保证 MSVC ABI 下符号不 mangle（Linux 全局变量本就不 mangle），
// 否则 lld-link 找不到未修饰的 g_gc_poll_flag 引用。
extern "C" std::atomic<int> g_gc_poll_flag{0};
static int rt_gc_auto_on(void) {
    if (g_gc_auto < 0) {
        const char* e = getenv("SHADOW_GC_AUTO");
        g_gc_auto = (e && *e && e[0] == '0') ? 0 : 1;
    }
    return g_gc_auto;
}
static int64_t rt_gc_gogc(void) {
    if (g_gc_gogc < 0) {
        const char* e = getenv("SHADOW_GOGC");
        g_gc_gogc = (e && *e) ? atoll(e) : 100;
        if (g_gc_gogc < 0) g_gc_gogc = 100;
    }
    return g_gc_gogc;
}
static int32_t rt_gc_stress_n(void) {
    if (g_gc_stress < 0) {
        const char* e = getenv("SHADOW_GC_STRESS");
        if (e && *e) {
            int64_t v = atoll(e);
            g_gc_stress = (v > 0) ? (int32_t)v : 0;
        } else {
            g_gc_stress = 0;
        }
    }
    return g_gc_stress;
}

// ── Per-thread GC root sets (thread-safe concurrency) ──
// Each OS thread that runs Shadow code gets its OWN root stack/frames so that
// concurrent spawn bodies don't corrupt each other's root discipline. The mark
// phase scans every live thread's roots via the global registry below.

// codegen 直接读写的 per-thread 锚点链表头指针。取代每帧一次的 helper 调用：
// enter/leave 变成一次 TLS load + store，无跨模块调用开销。
extern "C" __thread void* shadow_gc_tls_frame_head = nullptr;

struct ShadowFrameAnchor;   // 前向声明：ThreadGCState::frame_head 用指针，struct 定义见下文
struct ThreadGCState {
    std::mutex mtx;                                  // guards this thread's root containers
    std::vector<void*> roots;                        // frame-managed roots (popped by frame_leave)
    std::unordered_map<void*, void*> named_roots;    // slot_addr -> current value (replace semantics)
    std::vector<std::vector<void*>> named_frames;    // per-frame slot addrs (popped by frame_leave)
    // shadow frame range roots (对齐 Windows rt_gc.o 的 shadow_gc_root_range)：
    // codegen 在函数入口调用 shadow_gc_root_range(%sf, n) 把整个 shadow frame
    // 注册为精确根（n 个指针槽）。collect 扫描这些槽 —— 这是「跨安全点指针
    // 已 spill 到 shadow frame」这一编译器保证的运行时兑现：自动 GC 在任意
    // alloc 安全点触发时，帧内活指针都被覆盖，未扎根临时量不会误回收。
    // range_markers 记录每帧入口时 range_roots 的长度快照：帧严格 LIFO 且每帧
    // 恰好一个 range（函数入口注册 shadow frame），frame_leave 直接 resize 回退，
    // 免去旧实现的 O(n) 线性扫描删除（深递归 quicksort 的 O(n²) 开销来源）。
    std::vector<size_t> range_markers;               // per-frame range_roots length snapshot
    std::vector<std::pair<void*, uint32_t>> range_roots;                // flattened for scan
    ShadowFrameAnchor* frame_head = nullptr;   // 编译期栈映射：本线程帧锚点链表头（per-thread）
    void** head_slot = nullptr;                  // 指向 shadow_gc_tls_frame_head（codegen 直写此处）
    // 协作式 STW 状态（对齐 Windows rt_gc.o 的 rt_gc_thread.gc_state）：
    //   0 = free-running（mutator，可能在任意点）
    //   1 = at safepoint（shadow_gc_poll 自旋等待放行）
    //   2 = blocked on g_gc_mutex（等锁，不跑 mutator，collect 无需等待）
    std::atomic<int> stw_state{0};
};

// ── 编译期栈映射（compile-time stack map）的帧锚点（GC 重构）──
// 取代运行时 shadow-frame 根注册（shadow_gc_root_range / per-param root_set）。
// 每个 Shadow 函数序言把一个 [3 x i64] 锚点（frame_ptr, prev, frame_n）压入【本线程】的
// 帧锚点链表；收尾弹出。collect 标记阶段遍历 g_gc_thread_states，逐线程沿本线程链表
// 扫描整片连续 shadow frame（变量槽 + 寄存器 spill 区），逐槽 gc_trace_child —— 与旧
// root_range 逐槽精确扫描位等价，但彻底消除每调用 unordered_map 根注册开销（递归/列表
// 基准的 100~160× 差距根因），并修复 V1 单全局头在 spawn 多线程下交替 push/pop 互相截断
// 根导致的随机活对象被回收（ex_gc_spawn 专门捕获该 bug）。链表头存于 ThreadGCState::
// frame_head（per-thread），由 codegen 经 shadow_gc_thread_frame_head() 读写。
struct ShadowFrameAnchor {
    void* frame_ptr;                 // 连续 shadow frame 基址（[n_slots+sf_n x i64]）
    ShadowFrameAnchor* prev;         // 调用链上一帧锚点
    int32_t frame_n;                 // shadow frame 槽数（变量槽 + spill 槽）
};

static thread_local ThreadGCState* tl_gc_state = nullptr;
static std::vector<ThreadGCState*> g_gc_thread_states;   // registry, guarded by g_gc_mutex
static std::mutex g_gc_mutex;                            // guards g_gc_remembered, g_finalizers,
                                                        //   g_gc_perm_roots, g_gc_thread_states, 段注册表
static std::vector<void*> g_gc_perm_roots;     // permanent roots (popped by gc_perm_root_remove)

// ── W3：带内对象头 + 分段堆（设计见 W3_带内对象头方案.md）──
// shadow_gc_alloc 的每个对象都带 16B 前置头：头在 (载荷 - 16) 处。
// P3：带内头是唯一元数据（GCMetaMap 已退役）；分配/标记/清扫/屏障全走头。
// 不变式：所有 gc_alloc 载荷前都有 16B 可写头区（段槽位）；因此清扫/显式释放
// 绝不能对段槽位做 free()——只能置死标记（+ 回空闲链表），段内存由整段生命周期管理。
typedef struct ShadowHdr {
    std::atomic<uint32_t> epoch; // 最近一次「分配或标记」的 epoch（+trace 位）
    uint16_t kind;               // RT_T_*；HDR_KIND_DEAD = 死槽位标记
    uint8_t  gen;                // 0=young 1=old
    uint8_t  surv;               // 幸存计数（晋升用）
    uint32_t size;               // 对齐后载荷容量（= 旧 meta.size）
    uint32_t req;                // 请求字节（= 旧 meta.req，pacing 记账）
} ShadowHdr;
static_assert(sizeof(ShadowHdr) == 16, "ShadowHdr must be exactly 16 bytes");
#define HDR_KIND_DEAD 0xFFFFu    // 死槽位标记：gc_header_of 必须 miss
// epoch 字双义：低 31 位 = 最近「分配或标记」的 epoch；bit31 = 本轮被追踪到
// （trace 位）。存活判据 = (epoch & MASK) == 当前轮；pacing 活字节只认 trace 位
// （分配即黑但未扎根的残留不计入 live，防 trigger 正反馈放大——
// ex_gc_longrun 谷底漂移的历史根因）。
#define HDR_TRACE_BIT  0x80000000u
#define HDR_EPOCH_MASK 0x7FFFFFFFu

// 段对象计数（供 shadow_gc_live_objects / 诊断）。
static std::atomic<int64_t> g_seg_objs{0};

// ── finalizer 旁路表（段对象的 Drop/终结器；g_gc_mutex 保护）──
// codegen 为含 Drop trait 的结构体发 shadow_gc_set_finalizer；段对象不在任何
// 登记表里，终结器统一落此表。复活语义与旧实现一致：执行后写回当前 epoch
// （无 trace 位）→ 本轮存活，下轮未扎根才真正回收。
struct GcFinalizerRec { void(*fn)(void*); void* data; bool finalized; };
static std::unordered_map<void*, GcFinalizerRec> g_finalizers;

#define GC_SEG_CAP_DEFAULT (1024u * 1024u)   // 1MB
#define GC_SEG_MAX 8192                      // 段数上限 = 8GB 堆（工程不可达）
struct GcSegment { char* base; size_t cap; size_t used; };
static GcSegment* g_segments[GC_SEG_MAX];    // append-only（P3 才有段退役）
static std::atomic<size_t> g_segment_n{0};   // 写者持 g_gc_mutex；读者 acquire

// P2：段堆统计（prof_atexit 用，前向声明在本块之前）。
static void gc_seg_stats(size_t* nseg, int64_t* objs) {
    *nseg = g_segment_n.load(std::memory_order_relaxed);
    *objs = g_seg_objs.load(std::memory_order_relaxed);
}

static size_t gc_seg_cap(void) {
    static size_t cap = 0;
    if (!cap) {
        const char* e = getenv("SHADOW_GC_SEG_CAP");
        cap = (e && *e) ? (size_t)atoll(e) : GC_SEG_CAP_DEFAULT;
        if (cap < 64 * 1024) cap = 64 * 1024;
    }
    return cap;
}

// 新建段（调用者持 g_gc_mutex）。达上限/内存不足返回 nullptr。
static GcSegment* gc_segment_new_locked(size_t min_bytes) {
    size_t n = g_segment_n.load(std::memory_order_relaxed);
    if (n >= GC_SEG_MAX) return nullptr;
    size_t cap = gc_seg_cap();
    if (cap < min_bytes) cap = min_bytes;
    char* base = (char*)malloc(cap);
    if (!base) return nullptr;
    GcSegment* s = (GcSegment*)malloc(sizeof(GcSegment));
    if (!s) { free(base); return nullptr; }
    s->base = base; s->cap = cap; s->used = 0;
    g_segments[n] = s;
    g_segment_n.store(n + 1, std::memory_order_release);
    return s;
}

// 前向声明：持锁辅助（定义在下方，等锁期间标记 stw_state=2 供 STW 跳过）。
static void gc_lock_blocked();

// bump 分配：返回载荷指针（前置 16B 头区已预留）。
// 单 mutator 走线程本地段无锁 bump；多 mutator / 段满走锁内路径。
// 巨对象（槽 > 段容量）得专属段。段分配失败返回 nullptr（分配失败语义）。
static void* gc_seg_alloc(int32_t asize) {
    static thread_local GcSegment* tl_seg = nullptr;
    size_t slot = 16 + (((size_t)asize + 15) & ~(size_t)15);
    if (g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
        GcSegment* s = tl_seg;
        if (s && s->used + slot <= s->cap) {
            size_t off = s->used;
            s->used += slot;
            return s->base + off + 16;
        }
    }
    gc_lock_blocked();
    GcSegment* s = tl_seg;   // 锁内允许 bump 任意段（多 mutator 只走此路径）
    if (!(s && s->used + slot <= s->cap)) {
        s = nullptr;
        size_t n = g_segment_n.load(std::memory_order_relaxed);
        for (size_t i = n; i-- > 0;) {
            GcSegment* c = g_segments[i];
            if (c->used + slot <= c->cap) { s = c; break; }
        }
        if (!s) s = gc_segment_new_locked(slot);
        if (s) tl_seg = s;
    }
    void* p = nullptr;
    if (s) {
        size_t off = s->used;
        s->used += slot;
        p = s->base + off + 16;
    }
    g_gc_mutex.unlock();
    return p;
}

// 对象身份判定：p 为分段堆上的活对象 → 头指针；否则 nullptr。
// 段 append-only，新对象在尾部，逆序扫描通常 1–2 次命中。
// 三重过滤（段区间 / 16B 槽对齐 / DEAD 标记 + epoch 上界）挡住野候选。
static ShadowHdr* gc_header_of(void* p) {
    uintptr_t u = (uintptr_t)p;
    if (u < 16) return nullptr;
    size_t n = g_segment_n.load(std::memory_order_acquire);
    char* pc = (char*)p;
    uint32_t ep = g_gc_epoch;
    for (size_t i = n; i-- > 0;) {
        GcSegment* s = g_segments[i];
        char* base = s->base;
        if (pc < base + 16 || pc > base + s->used) continue;
        size_t off = (size_t)(pc - base);
        if ((off & 15) != 0) return nullptr;  // 段内但不在槽边界 → 非对象指针
        ShadowHdr* h = (ShadowHdr*)(pc - 16);
        if (h->kind == HDR_KIND_DEAD) return nullptr;             // 死槽位 = miss
        if ((h->epoch.load(std::memory_order_relaxed) & HDR_EPOCH_MASK) > ep) return nullptr; // 脏数据过滤
        return h;
    }
    return nullptr;
}

// P1：显式释放段对象 —— 置死标记（+ 回空闲链表）；段中段内存不能 free()。
// p 是段对象返回 1；否则 0（调用方走原 free 路径）。
// P2：heap 记账与对象计数同步递减（原由 shadow_gc_forget 负责）。
static int gc_hdr_release(void* p) {
    ShadowHdr* h = gc_header_of(p);
    if (!h) return 0;
    h->kind = HDR_KIND_DEAD;
    g_heap_bytes.fetch_sub((int64_t)h->req, std::memory_order_relaxed);
    g_seg_objs.fetch_sub(1, std::memory_order_relaxed);
    if (fl_enabled() && h->size >= 16 && h->size <= 65536 &&
        g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
        fl_push(p, (int32_t)h->size);
    }
    return 1;
}


// ── 线程局部字符串长度/容量缓存 ──
// str_reverse 热循环里 shadow_string_concat_char_fast 每次 strlen(out)（out 0→43 增长）
// 是 O(n²) 瓶颈，且每次都要查带内头（P3 前是 g_pend 扫描 + 哈希查找）。缓存
// (ptr, len, cap) 三元组：命中即免 strlen 和哈希查找。
// ABA 安全：字符串只会在 GC 清扫（collect）时被释放，collect 后 g_gc_epoch 递增；
// 缓存条目记录 epoch，epoch 不匹配即视为失效（地址复用不会误用旧长度）。
// cap=0 表示"未知容量"（非堆对象/未查表），调用方按"无余量"处理（分配新缓冲，
// 输出仍正确，仅损失就地追加优化）。所有就地修改字符串的函数必须维护本缓存。
struct TLStrCache { void* p; int32_t len; int32_t cap; uint32_t epoch; };
// 用 __thread 而非 thread_local：thread_local 的动态初始化守卫（std::string g_str_buf
// 等）会把 __cxa_thread_atexit 初始化代码内嵌进首个访问函数（tl_str_lookup），
// 膨胀到 800+ 字节导致 -O2 拒绝内联，热循环每轮多 2 次函数调用。
// __thread 仅支持 POD 常量初始化 → 无守卫 → 访问退化为 %fs:offset，可正常内联。
static __thread TLStrCache tl_str_cache[8];
static __thread int32_t tl_str_cache_n = 0;

static inline int tl_str_lookup(void* p, int32_t* len, int32_t* cap) {
    uint32_t ep = g_gc_epoch;
    for (int32_t i = 0; i < tl_str_cache_n; i++) {
        if (tl_str_cache[i].p == p) {
            if (tl_str_cache[i].epoch != ep) {
                for (int32_t j = i; j < tl_str_cache_n - 1; j++) tl_str_cache[j] = tl_str_cache[j + 1];
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

// 返回缓存条目下标（-1 = 未命中）。热路径（char_at 就地追加）用它把
// 「查找 len/cap」与「追加后更新 len」合并成一次扫描，省一次线性遍历。
static inline int32_t tl_str_find(void* p) {
    uint32_t ep = g_gc_epoch;
    for (int32_t i = 0; i < tl_str_cache_n; i++) {
        if (tl_str_cache[i].p == p) {
            if (tl_str_cache[i].epoch != ep) {
                for (int32_t j = i; j < tl_str_cache_n - 1; j++) tl_str_cache[j] = tl_str_cache[j + 1];
                tl_str_cache_n--;
                return -1;
            }
            return i;
        }
    }
    return -1;
}

static inline void tl_str_set(void* p, int32_t len, int32_t cap) {
    uint32_t ep = g_gc_epoch;
    for (int32_t i = 0; i < tl_str_cache_n; i++) {
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
        for (int32_t i = 0; i < 7; i++) tl_str_cache[i] = tl_str_cache[i + 1];
        tl_str_cache[7].p = p;
        tl_str_cache[7].len = len;
        tl_str_cache[7].cap = cap;
        tl_str_cache[7].epoch = ep;
    }
}

// 取 (len, cap)：缓存命中直接返回；未命中计算 strlen + 查容量后入缓存。
// 慢路径拆成独立函数：tl_str_get 只剩「lookup + 慢路径调用」→ 体积小可内联，
// 否则 strlen/g_pend 扫描/mutex 哈希查找的慢路径会让 -O2 拒绝内联整个函数，
// 热循环每轮多 1 次函数调用。
static void tl_str_get_slow(void* p, int32_t* len, int32_t* cap) {
    *len = (int32_t)strlen((const char*)p);
    *cap = 0;
    // P3：带内头直读；非段对象（字面量/栈上）cap=0（无就地余量，行为同旧 miss）
    ShadowHdr* h = gc_header_of(p);
    if (h) *cap = (int32_t)h->size;
    tl_str_set(p, *len, *cap);
}
static inline void tl_str_get(void* p, int32_t* len, int32_t* cap) {
    if (tl_str_lookup(p, len, cap)) return;
    tl_str_get_slow(p, len, cap);
}

// 供 shadow 层就地拼接函数（shadow_string_concat_inplace / _char）在修改后同步缓存。
extern "C" void shadow_string_cache_set(void* p, int32_t len, int32_t cap) {
    tl_str_set(p, len, cap);
}

// 查询 GC 堆对象的数据区容量（分配时记录的 size）；非堆对象（字面量/栈上）返回 0。
// 供 shadow_string_concat_inplace 判断能否就地追加（与 Windows rt_gc.c 的 rt_alloc_cap 对齐）。
// P3：带内头直读——无锁、免哈希，字符串拼接热循环直接受益。
extern "C" int32_t rt_alloc_cap(void* p) {
    if (!p) return 0;
    ShadowHdr* h = gc_header_of(p);
    return h ? (int32_t)h->size : 0;
}

// 快速路径单字符追加：单次 C 调用完成 strlen + alloc_cap 判断 + 就地写字节/扩容，
// 消除 shadow 层 shadow_string_concat_char 的 4 次 extern 调用（str_reverse 热循环
// 9M 次追加 × 4 = 36M 次 extern 调用 → 1 次）。语义与 runtime_lib.shadow 的
// shadow_string_concat_char 完全一致（cap 足够就地写，否则 2x 倍增扩容）。
extern "C" void* shadow_string_concat_char_fast(void* s1, int32_t c) {
    int32_t l1, cap;
    tl_str_get(s1, &l1, &cap);
    int32_t need = l1 + 2;
    if (cap >= need) {
        ((char*)s1)[l1] = (char)c;
        ((char*)s1)[l1 + 1] = 0;
        tl_str_set(s1, l1 + 1, cap);
        return s1;
    }
    int32_t newcap = cap * 2;
    if (newcap < need) newcap = need;
    if (newcap < 16) newcap = 16;  // 最小增长 16B：短串首段扩容一步到位，str_reverse 分配 6→3 次
    void* p = shadow_gc_alloc(newcap, 0);
    shadow_gc_root_set(&p, p);
    memcpy(p, s1, (size_t)l1);
    ((char*)p)[l1] = (char)c;
    ((char*)p)[l1 + 1] = 0;
    shadow_gc_root_set(&p, 0);
    tl_str_set(p, l1 + 1, newcap);
    return p;
}

// 单字符追加（源串下标版）：读取 s[idx] 并追加到 s1。等价于
// shadow_string_concat_char_fast(s1, shadow_string_char_at(s, idx))，但单次 C 调用
// 完成，消除 str_reverse 热循环每轮 2 次调用 → 1 次（8.6M 次追加省 8.6M 次调用）。
// 语义：s[idx] 负索引回绕（与 shadow_schar 一致），越界返回 0 字符；追加语义与
// shadow_string_concat_char_fast 完全一致（cap 足够就地写，否则 2x 倍增扩容）。
extern "C" void* shadow_string_concat_char_at(void* s1, const char* s, int32_t idx) {
    int32_t i1 = tl_str_find(s1);
    int32_t l1, cap;
    if (i1 >= 0) {
        l1 = tl_str_cache[i1].len;
        cap = tl_str_cache[i1].cap;
    } else {
        tl_str_get_slow(s1, &l1, &cap);
        i1 = tl_str_find(s1);  // slow 路径已入缓存，必命中
    }
    int32_t c = 0;
    int32_t n = 0, scap = 0;
    int32_t i = idx;
    int32_t s_miss = 0;  // s 查找未命中会 tl_str_set 左移缓存 → i1 下标失效，须回退 tl_str_set
    if (tl_str_lookup((void*)s, &n, &scap)) {
        if (i < 0) i = i + n;
        if (i >= 0 && i < n) c = (int32_t)(unsigned char)s[i];
    } else {
        s_miss = 1;
        n = (int32_t)strlen(s);
        tl_str_set((void*)s, n, 0);
        if (i < 0) i = i + n;
        if (i >= 0 && i < n) c = (int32_t)(unsigned char)s[i];
    }
    int32_t need = l1 + 2;
    if (cap >= need) {
        ((char*)s1)[l1] = (char)c;
        ((char*)s1)[l1 + 1] = 0;
        if (i1 >= 0 && s_miss == 0) {
            tl_str_cache[i1].len = l1 + 1;
            tl_str_cache[i1].epoch = g_gc_epoch;
        } else {
            tl_str_set(s1, l1 + 1, cap);
        }
        return s1;
    }
    int32_t newcap = cap * 2;
    if (newcap < need) newcap = need;
    if (newcap < 16) newcap = 16;  // 最小增长 16B：短串首段扩容一步到位
    // 反向逐字符构建启发：追加 s[i] 时结果至少 l1+i+1 长（反向构建中 l1+i 为不变量，
    // 恒等于源长-1），一步到位预分配——str_reverse 每轮 3 次扩容分配 → 1 次，
    // 直接削减分配/攒批 flush/GC 清扫三处成本。仅 i 越界内才生效，越界 hint=0 不放大。
    if (i >= 0 && i < n) {
        int32_t hint = l1 + i + 2;
        if (newcap < hint) newcap = hint;
    }
    void* p = shadow_gc_alloc(newcap, 0);
    shadow_gc_root_set(&p, p);
    memcpy(p, s1, (size_t)l1);
    ((char*)p)[l1] = (char)c;
    ((char*)p)[l1 + 1] = 0;
    shadow_gc_root_set(&p, 0);
    tl_str_set(p, l1 + 1, newcap);
    return p;
}

// 快速路径字符串查找：单次 C 调用完成朴素匹配，消除 shadow 层 shadow_index_of
// 逐字节 rt_get_byte 的 extern 调用开销（string_find 热循环 48M 次调用 → 300K 次）。
// 语义与 runtime_lib.shadow 的 shadow_index_of 完全一致（返回首次出现位置，无则 -1）。
// 优化：SSE2 16 字节并行定位首字节（对标 Go strings.Index 的 SIMD 首字节扫描），
// 同一遍同时检测空终止符——首字节扫描与 strlen 合并为单遍，消除独立 strlen(sp)
// 的第二次全串扫描；命中后再逐字节验证剩余 needle。
// 空终止符充当天然边界：末字节检查读到的 0 永不等于非空 needle 末字节，
// 中间逐字节检查也会在越界处被 0 截断，故不会产生越界假匹配。
static inline int32_t find_byte_sse2_nul(const char* s, char c) {
    const __m128i target = _mm_set1_epi8(c);
    const __m128i nul = _mm_setzero_si128();
    int32_t i = 0;
    for (;;) {
        __m128i c0 = _mm_loadu_si128((const __m128i*)(s + i));
        int32_t m0 = (int32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c0, target));
        int32_t n0 = (int32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c0, nul));
        if (n0 != 0) {
            int32_t lim = __builtin_ctz((unsigned)n0);
            int32_t m = m0 & ((1u << lim) - 1);
            return m ? i + __builtin_ctz((unsigned)m) : -1;
        }
        if (m0 != 0) return i + __builtin_ctz((unsigned)m0);
        __m128i c1 = _mm_loadu_si128((const __m128i*)(s + i + 16));
        int32_t m1 = (int32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c1, target));
        int32_t n1 = (int32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(c1, nul));
        if (n1 != 0) {
            int32_t lim = __builtin_ctz((unsigned)n1);
            int32_t m = m1 & ((1u << lim) - 1);
            return m ? i + 16 + __builtin_ctz((unsigned)m) : -1;
        }
        if (m1 != 0) return i + 16 + __builtin_ctz((unsigned)m1);
        i += 32;
    }
}

extern "C" int32_t shadow_index_of_fast(void* s, void* needle) {
    const char* sp = (const char*)s;
    const char* np = (const char*)needle;
    int32_t nl = (int32_t)strlen(np);
    if (nl == 0) return 0;
    if (nl == 1) return find_byte_sse2_nul(sp, np[0]);
    char first = np[0];
    char last = np[nl - 1];
    int32_t i = 0;
    for (;;) {
        int32_t pos = find_byte_sse2_nul(sp + i, first);
        if (pos < 0) return -1;
        pos += i;
        if (sp[pos + nl - 1] == last) {
            int32_t j = 1;
            while (j < nl - 1 && sp[pos + j] == np[j]) j++;
            if (j == nl - 1) return pos;
        }
        i = pos + 1;
    }
    return -1;
}

// Lock-free collect coordination: spinlock prevents concurrent collect cycles.
// 等 collect 锁期间标记 blocked（stw_state=2）：并发触发 collect 的线程在等锁
// 时不跑 mutator，collect 的 STW 等待无需等它（否则并发 collect 死锁）。
static inline void gc_lock() {
    ThreadGCState* s = tl_gc_state;
    while (g_gc_collect_lock.test_and_set(std::memory_order_acquire)) {
        if (s) s->stw_state.store(2, std::memory_order_release);
        std::this_thread::yield();
    }
    if (s) s->stw_state.store(0, std::memory_order_release);
}
static inline void gc_unlock() { g_gc_collect_lock.clear(std::memory_order_release); }

// Global variable roots: slot_addr → current value (replace semantics).
// Unlike named_roots (frame-scoped), global roots persist across function
// calls and are never popped by frame_leave. This is essential for global
// `any` variables that hold GC-managed objects — without global root
// registration, the GC would free objects still referenced by globals.
static std::unordered_map<void*, void*> g_gc_global_roots;

// Conservative root regions: memory areas scanned for potential GC pointers.
// Used when the compiler binary itself wasn't compiled with explicit GC root
// registration (e.g., s03.exe compiled by shadow-lang 0.2). The GC scans these
// regions for pointer-sized values that match tracked GC objects, treating
// matches as additional roots. This is safe because gc_trace_child checks
// gc_header_of() before tracing — non-GC pointers are silently ignored.
// Trade-off: stale pointers may cause false positives (keeping dead objects
// alive), but never crashes. Acceptable for LSP long-running mode.
static std::vector<std::pair<void*, int64_t>> g_gc_conservative_regions;

extern "C" void shadow_gc_add_conservative_root(void* start, int64_t size) {
    if (!start || size <= 0) return;
    std::lock_guard<std::mutex> lk(g_gc_mutex);
    g_gc_conservative_regions.push_back({start, size});
}

// Conservative scan helper: trace pointer-sized values in registered regions.
// Called during GC mark phase, after explicit roots are traced.
// Forward declaration: gc_trace_child is defined below (after ThreadGCState).
static inline void gc_trace_child(void* child, std::vector<void*>& worklist);
static void gc_trace_conservative_regions(std::vector<void*>& worklist) {
    for (auto& region : g_gc_conservative_regions) {
        char* base = reinterpret_cast<char*>(region.first);
        int64_t size = region.second;
        for (int64_t off = 0; off + (int64_t)sizeof(void*) <= size; off += sizeof(void*)) {
            void* cand;
            std::memcpy(&cand, base + off, sizeof(void*));
            gc_trace_child(cand, worklist);
        }
    }
}

// ── Process heap conservative scan ──
// Scans ALL allocated blocks in the process heap for pointer-like values.
// This finds references from malloc'd AST nodes (not GC-tracked) to GC-tracked
// objects (strings, arrays), preventing GC from sweeping live objects.
static void gc_scan_process_heap(std::vector<void*>& worklist) {
#ifdef _WIN32
    HANDLE heap = GetProcessHeap();
    if (!heap) return;
    PROCESS_HEAP_ENTRY entry = {0};
    entry.lpData = NULL;
    while (HeapWalk(heap, &entry)) {
        if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) && entry.cbData >= sizeof(void*)) {
            char* base = reinterpret_cast<char*>(entry.lpData);
            int64_t size = (int64_t)entry.cbData;
            for (int64_t off = 0; off + (int64_t)sizeof(void*) <= size; off += sizeof(void*)) {
                void* cand;
                std::memcpy(&cand, base + off, sizeof(void*));
                gc_trace_child(cand, worklist);
            }
        }
    }
    SetLastError(0);
#endif
}

// On Windows, register the main module's image as a conservative root region.
// This allows GC to find global variable pointers in .data/.bss without explicit
// root registration. Called once at LSP startup.
extern "C" void shadow_gc_init_conservative_globals() {
#if defined(_WIN32)
    HMODULE hModule = GetModuleHandleA(NULL);
    if (!hModule) return;
    // Parse PE headers to get image size (avoids needing psapi.h/GetModuleInformation).
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return;
    PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(hModule) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return;
    int64_t imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    if (imageSize <= 0) return;
    shadow_gc_add_conservative_root(hModule, imageSize);
#endif
}

// thread_local guard: unregister this thread's state at thread exit. We do NOT
// free it (another thread's in-flight collect may still hold the pointer).
// Declared BEFORE gc_get_thread_state so that function can force-construct it.
struct GCTLSGuard {
    ~GCTLSGuard() {
        if (tl_gc_state) {
            ThreadGCState* s = tl_gc_state;
            // 退出中：取 g_gc_mutex 前标记 blocked（stw_state=2），否则 collect
            // 的 STW 等待循环会把本线程当 free-running 无限等待 → 死锁。
            s->stw_state.store(2, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(g_gc_mutex);
                auto& v = g_gc_thread_states;
                for (size_t i = 0; i < v.size(); i++) {
                    if (v[i] == s) { v[i] = v.back(); v.pop_back(); break; }
                }
            }
            s->stw_state.store(0, std::memory_order_release);
            tl_gc_state = nullptr;
            g_mutator_threads.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};
static thread_local GCTLSGuard gc_tls_guard;   // forces construction in each thread

// 冷路径：每线程仅首次执行。必须 noinline + cold —— 否则其中的 operator new 与
// lock_guard 会迫使调用方（热路径 gc_get_thread_state）保存 8 个被调用者保存寄存器，
// 于是「每次用户函数调用」都要付一整套 push/pop 序言：反汇编可见该函数曾长达 137 条
// 指令，实测外提前它在 awfy_list_long 仍占 41.5%、awfy_towers_long 占 36.6%。
// 外提后热路径只剩一次 TLS 读 + 判空 + 返回。
static __attribute__((noinline, cold)) ThreadGCState* gc_thread_state_create() {
    (void)&gc_tls_guard;   // 强制构造本线程 TLS 守卫（线程退出时反注册本线程 state）
    ThreadGCState* s = new ThreadGCState();
    {
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        g_gc_thread_states.push_back(s);
    }
    g_mutator_threads.fetch_add(1, std::memory_order_relaxed);
    tl_gc_state = s;
    // 把 codegen 直写的 TLS 槽绑定到本线程 state 的 frame_head 字段。
    // codegen enter/leave 直接读写 shadow_gc_tls_frame_head，collect 通过
    // *s->head_slot 访问同一链表头，两者始终一致。
    s->head_slot = &shadow_gc_tls_frame_head;
    return s;
}

// 热路径。注意：thread_local 初始化守卫的引用只允许出现在上面的冷函数里 ——
// 放在此处会让每次调用都读一次守卫（Linux 是 fs: 字节比较；Windows/MSVC ABI 还要
// 经 CRT 导入跳板读 _Init_thread_epoch，贵一个数量级）。
static inline ThreadGCState* gc_get_thread_state() {
    ThreadGCState* s = tl_gc_state;
    if (__builtin_expect(s != nullptr, 1)) return s;
    return gc_thread_state_create();
}

// 主线程注册兜底：进程启动时立即创建状态，确保第一个帧 push 之前 state 已存在。
// spawned 线程由 task_run() 开头调 gc_get_thread_state() 兜底。
// 注意：必须放在 gc_get_thread_state() 定义之后（C++ 要求函数在使用前声明）。
static struct GcMainInit {
    GcMainInit() { gc_get_thread_state(); }
} g_gc_main_init;

// 编译期栈映射（per-thread）：返回【本线程】帧锚点链表头指针的地址（ShadowFrameAnchor**）。
// codegen 的 frame enter/leave 通过该指针读写本线程的头，彻底避免 V1 单全局头在
// spawn 多线程下交替 push/pop 互相截断根（ex_gc_spawn 专门捕获该 bug）。collect 经
// g_gc_thread_states 遍历每个 ThreadGCState::frame_head 完成扫描。
extern "C" void** shadow_gc_thread_frame_head() {
    ThreadGCState* s = gc_get_thread_state();
    return reinterpret_cast<void**>(&s->frame_head);
}

// 持锁辅助（mutator 路径）：等锁期间标记 blocked（stw_state=2），collect 扫根
// 时跳过（等锁 = 不跑 mutator，根集冻结）。对齐 Windows「阻塞在 GC_LOCK 上
// 的线程不会被等待」契约，避免 collect 等一个永远到不了安全点的等锁线程。
static void gc_lock_blocked() {
    ThreadGCState* s = gc_get_thread_state();
    while (!g_gc_mutex.try_lock()) {
        s->stw_state.store(2, std::memory_order_release);
        std::this_thread::yield();
    }
    s->stw_state.store(0, std::memory_order_release);
}

// ── 协作式 STW（对齐 Windows rt_gc.o 的 gc_stw_begin/gc_stw_end）──
// collect 持 g_gc_mutex 调用：置 req=1 → 等所有其它线程 stw_state!=0
// （1=安全点自旋，2=等锁阻塞）→ active=1、清 req → 精确扫根/清扫 →
// active=0 放行。等待循环只读原子 stw_state，不取 s->mtx（避免与 mutator
// 的 root_set 死锁）；g_gc_thread_states 在 g_gc_mutex 保护下稳定（collect
// 持锁期间无增删）。collect 自身（me）跳过 —— 它不跑 mutator，根集冻结。
static void gc_stw_begin() {
    g_gc_stw_req.store(1, std::memory_order_relaxed);
    g_gc_poll_flag.store(1, std::memory_order_relaxed);
    ThreadGCState* me = tl_gc_state;
    for (;;) {
        int all = 1;
        for (ThreadGCState* s : g_gc_thread_states) {
            if (s == me) continue;
            // acquire：与 mutator 在 poll 的 stw_state=1 release store 配对，
            // 建立「mutator 快路径根修改 → collect 扫根」的 happens-before 链
            // （无锁快路径依赖此序，见 shadow_gc_frame_enter 注释）。
            if (s->stw_state.load(std::memory_order_acquire) == 0) { all = 0; break; }
        }
        if (all) break;
        std::this_thread::yield();
    }
    g_gc_stw_active.store(1, std::memory_order_relaxed);
    g_gc_stw_req.store(0, std::memory_order_relaxed);
}
static void gc_stw_end() {
    g_gc_stw_active.store(0, std::memory_order_relaxed);
    g_gc_poll_flag.store(0, std::memory_order_relaxed);
}

static void gc_perm_root_add(void* p) {
    if (!p) return;
    gc_lock_blocked();
    g_gc_perm_roots.push_back(p);
    g_gc_mutex.unlock();
}
static void gc_perm_root_remove(void* p) {
    if (!p) return;
    gc_lock_blocked();
    for (size_t i = 0; i < g_gc_perm_roots.size(); i++) {
        if (g_gc_perm_roots[i] == p) { g_gc_perm_roots[i] = g_gc_perm_roots.back(); g_gc_perm_roots.pop_back(); break; }
    }
    g_gc_mutex.unlock();
}
// Global variable root registration (replace semantics, not frame-scoped).
// Called from generated IR when a global variable of pointer kind is assigned.
// The slot is the LLVM global's address; val is the new value (AnyBox* or
// raw GC pointer). A null val erases the slot's root.
extern "C" int32_t shadow_gc_global_root_set(void* slot, void* val) {
    if (!slot) return 0;
    std::lock_guard<std::mutex> lk(g_gc_mutex);
    if (val) {
        g_gc_global_roots[slot] = val;
    } else {
        g_gc_global_roots.erase(slot);
    }
    return 0;
}

// P12: Permanent GC root management for LSP cache entries.
// Shadow raw arrays (malloc'd buffers) are NOT traced by GC (only C++
// ShadowArray with std::vector is). When any values (AnyBox*) are stored
// in raw arrays, they must be registered as permanent roots to prevent
// GC from collecting the cached Program/Tokens/etc.
extern "C" void shadow_gc_perm_root_add(void* p) {
    gc_perm_root_add(p);
}
extern "C" void shadow_gc_perm_root_remove(void* p) {
    gc_perm_root_remove(p);
}

static int64_t g_gc_collect_count = 0;
static int64_t g_gc_minor_count = 0;       // minor GC count
static int64_t g_gc_major_count = 0;       // major GC count
static int64_t g_diag_kind0 = 0;

// Generational thresholds.
static const int64_t GC_COLLECT_THRESHOLD = 1024;
static const uint32_t PROMOTION_THRESHOLD = 3;   // survive N minor GCs → promote to old gen
static const int64_t MINOR_GC_THRESHOLD = 256;   // allocations before minor GC

// Remembered set: old-gen objects that may reference young-gen objects.
// Populated by the write barrier (shadow_gc_write_barrier).
static std::unordered_set<void*> g_gc_remembered;

// When set, shadow_gc_collect() becomes a no-op. Intended for short-lived
// batch programs (e.g. the Shadow self-hosting compiler) whose memory is
// reclaimed at process exit.
// ── Crash diagnostics: Vectored Exception Handler captures AV details ──
#ifdef _WIN32
static LONG WINAPI shadow_crash_handler(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005) {
        CONTEXT* ctx = ep->ContextRecord;
        fprintf(stderr, "[CRASH-DIAG] AV at 0x%llx fault=0x%llx RAX=0x%llx RCX=0x%llx RIP=0x%llx\n",
            (unsigned long long)ep->ExceptionRecord->ExceptionAddress,
            (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
            (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rcx,
            (unsigned long long)ctx->Rip);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static struct ShadowCrashHandlerInit {
    ShadowCrashHandlerInit() { AddVectoredExceptionHandler(1, shadow_crash_handler); }
} g_crash_handler_init;
#else
static struct ShadowCrashHandlerInit {
    ShadowCrashHandlerInit() {}
} g_crash_handler_init;
#endif

// ── Conservative stack scan: treat pointer-sized values on the current ──
// ── thread's stack as GC roots. 对齐 Windows rt_gc.c（§5.2.4）：保守栈扫   ──
// ── 描默认关闭 —— 精确根集（frame root + global root + shadow frame range  ──
// ── 根 + slot 键控 replace）是唯一依据。无条件保守扫栈会把编辑循环等热  ──
// ── 路径栈上的残留旧指针当根 → 旧对象永不回收 → 长驻堆单调增长           ──
// ── （ex_gc_longrun heap floor grew 的根因）。SHADOW_GC_CONSERVATIVE=1    ──
// ── 才启用（对拍验证用，与 rt_gc.c 语义一致）。                            ──
static int g_gc_cons_stack_init = 0;
static int g_gc_cons_stack_on = 0;
static int gc_cons_stack_on(void) {
    if (!g_gc_cons_stack_init) {
        g_gc_cons_stack_init = 1;
        const char* e = getenv("SHADOW_GC_CONSERVATIVE");
        g_gc_cons_stack_on = (e && *e && e[0] != '0') ? 1 : 0;
    }
    return g_gc_cons_stack_on;
}
static void gc_scan_stack(std::vector<void*>& worklist) {
#ifdef _WIN32
    if (!gc_cons_stack_on()) return;
    void* sp = (void*)_AddressOfReturnAddress();
    void* teb = (void*)NtCurrentTeb();
    void* stack_base = *(void**)((char*)teb + 0x08);  // NT_TIB.StackBase
    if (sp && stack_base && sp < stack_base) {
        for (void** p = (void**)sp; p < (void**)stack_base; p++) {
            gc_trace_child(*p, worklist);
        }
    }
#endif
}

static int64_t g_gc_disabled = 0;
// SHADOW_GC_FORCE=1 makes shadow_gc_disable() a no-op, so the same binary can
// be run with GC forced ON for forensics without recompiling shadow source.
extern "C" void shadow_gc_disable() {
    if (getenv("SHADOW_GC_LOG")) fprintf(stderr, "[GC] DISABLE called\n");
    const char* f = getenv("SHADOW_GC_FORCE");
    if (f && f[0] == '1') return;
    g_gc_disabled = 1;
}
extern "C" void shadow_gc_enable()  {
    g_gc_disabled = 0;
}

// ── Heap pressure introspection (P20-5) ──
// A long-running host (the LSP server) needs to decide *when* to collect.
// Previously the only trigger was "a document was recompiled", so a steady-state
// IDE session — where every request hits the compile cache — never collected at
// all and the process grew without bound (measured: ~31 MB per
// semanticTokens/full request). These two read-only counters let the host
// implement a high-water-mark policy without guessing.
extern "C" int64_t shadow_gc_live_objects() {
    // P3：全部对象在段上
    return g_seg_objs.load(std::memory_order_relaxed);
}

extern "C" int64_t shadow_gc_alloc_count() {
    // 对齐 Windows rt_gc.c：返回**堆字节**（未释放对象总字节），
    // 供长驻验收用例（ex_gc_longrun 等）读谷底判断泄漏。
    // 旧实现返回累计分配计数 → 单调递增 → 任何存活集恒定的程序都误报泄漏。
    return (int64_t)g_heap_bytes.load(std::memory_order_relaxed);
}

// Trace a single candidate child pointer（P3：纯段对象，带内头判定）。
// CAS epoch 字 → 当前轮|trace位（一次原子操作完成去重+标记）；
// 已带 trace 位（本轮已追）直接跳过。
static inline void gc_trace_child(void* child, std::vector<void*>& worklist) {
    if (!child) return;
    ShadowHdr* h = gc_header_of(child);
    if (!h) return;
    uint32_t want = g_gc_epoch | HDR_TRACE_BIT;
    uint32_t v = h->epoch.load(std::memory_order_relaxed);
    if (v == want) return;  // 本轮已追踪
    if (!h->epoch.compare_exchange_strong(v, want, std::memory_order_acq_rel)) return;
    worklist.push_back(child);
}

// P3：取对象分代（带内头）；-1 = 非 GC 对象。
static int gc_obj_gen(void* p) {
    ShadowHdr* h = gc_header_of(p);
    return h ? (int)h->gen : -1;
}

// Write barrier: call when old-gen object `parent` gains a reference to `child`.
// If parent is old gen and child is young gen, add parent to remembered set.
extern "C" void shadow_gc_write_barrier(void* parent, void* child) {
    if (!parent || !child) return;
    if (gc_obj_gen(parent) != 1) return;
    if (gc_obj_gen(child) != 0) return;
    gc_lock_blocked();
    g_gc_remembered.insert(parent);
    g_gc_mutex.unlock();
}

// ── GC forensic diagnostics (enabled via env vars, zero cost otherwise) ──
// SHADOW_GC_LOG=1      : log every collect cycle and every swept object
// SHADOW_GC_WATCH=hex  : when this address is about to be swept, dump every
//                        tracked object that still references it (+mark state)
static int   g_gc_log_init = 0;
static int   g_gc_log_on = 0;
static void* g_gc_watch = nullptr;
static void gc_diag_init() {
    if (g_gc_log_init) return;
    g_gc_log_init = 1;
    const char* l = getenv("SHADOW_GC_LOG");
    if (l && l[0] == '1') g_gc_log_on = 1;
    const char* w = getenv("SHADOW_GC_WATCH");
    if (w && w[0]) g_gc_watch = (void*)(uintptr_t)strtoull(w, nullptr, 16);
}
// Set the watch target at runtime (callable from shadow code via @extern).
extern "C" void shadow_gc_watch(void* p) {
    gc_diag_init();
    g_gc_watch = p;
    fprintf(stderr, "[GCWATCH] now watching %p\n", p);
    fflush(stderr);
}
// Report every tracked object that references `target`, with its mark state.
static void gc_dump_referrers(void* target) {
    fprintf(stderr, "[GCWATCH] sweeping %p — searching referrers...\n", target);
    // Also check root sets
    for (void* r : g_gc_perm_roots)  if (r == target) fprintf(stderr, "[GCWATCH]   in PERM roots\n");
    for (auto& kv : g_gc_global_roots) if (kv.second == target)
        fprintf(stderr, "[GCWATCH]   in GLOBAL roots (slot=%p)\n", kv.first);
    for (ThreadGCState* s : g_gc_thread_states) {
        std::lock_guard<std::mutex> lk(s->mtx);
        for (void* r : s->roots) if (r == target) fprintf(stderr, "[GCWATCH]   in FRAME roots\n");
        for (auto& kv : s->named_roots) if (kv.second == target)
            fprintf(stderr, "[GCWATCH]   in NAMED roots (slot=%p)\n", kv.first);
    }
    // P3：全部对象在段上——按字保守扫描引用
    {
        size_t sn = g_segment_n.load(std::memory_order_acquire);
        for (size_t si = 0; si < sn; si++) {
            GcSegment* seg = g_segments[si];
            for (size_t off = 16; off <= seg->used; ) {
                ShadowHdr* h = (ShadowHdr*)(seg->base + off - 16);
                size_t stride = 16 + ((((size_t)(uint32_t)h->size) + 15) & ~(size_t)15);
                if (h->kind != HDR_KIND_DEAD) {
                    char* base = seg->base + off;
                    bool refs = false;
                    for (int64_t o = 0; o + 8 <= (int64_t)h->size; o += 8) {
                        void* cand; std::memcpy(&cand, base + o, 8);
                        if (cand == target) { refs = true; break; }
                    }
                    if (refs)
                        fprintf(stderr, "[GCWATCH]   referrer %p kind=%d size=%u epoch=%u (seg)\n",
                                (void*)base, (int)h->kind, (unsigned)h->size,
                                h->epoch.load(std::memory_order_relaxed));
                }
                off += stride;
            }
        }
    }
    fflush(stderr);
}

// Internal: trace object children (shared by major and minor GC).
// ── 用户类型表（对齐 Windows rt_gc.c 的 g_types / shadow_gc_register_type）──
// codegen 为每个用户结构体类型调用 shadow_gc_register_type(id, size, bitmap)，
// bitmap 位 i = 第 i 个 8 字节槽是否指针（与 Windows 一致）。trace 时按表精确
// 追踪；未注册（异常）回退保守扫描。
static std::mutex g_gc_types_mtx;
struct GcTypeInfo { int32_t size; int64_t bitmap; };
static std::vector<GcTypeInfo> g_gc_types;   // index = type_id

extern "C" int32_t shadow_gc_register_type(int32_t id, int32_t size, int64_t bitmap) {
    if (id < 0) return 0;
    std::lock_guard<std::mutex> lk(g_gc_types_mtx);
    if ((size_t)id >= g_gc_types.size()) g_gc_types.resize((size_t)id + 1);
    g_gc_types[(size_t)id] = GcTypeInfo{ size, bitmap };
    return 1;
}

// 对齐 Windows rt_gc.c 的 type_id 语义（RT_T_*）：
//   0 = 未分类（rt_malloc 通用对象：字符串/AnyBox/dict 桶等）→ 保守扫（兜底 AnyBox.value）
//   1 = RT_T_STRING 纯字节 → 不扫
//   2 = RT_T_ARRAY  [len:4][cap:4][elem_size:4][data@12]（C 布局，shadow 层数组）
//   3 = RT_T_ANYBOX [tag:4][value@4]（runtime_lib shadow 层 12 字节布局；tag 3=string,5=ptr）
//   4 = RT_T_DICT   rt_dict：{buckets**; n_buckets:4; size:4}
//   5 = RT_T_CLOSURE [fn_ptr@0][env_ptr@8]
//   ≥100 = RT_T_USER 用户类型：类型表 bitmap 精确追踪（>512B 整块保守）
static inline void gc_trace_object_children(void* obj, int32_t tid, int64_t size, std::vector<void*>& worklist) {
    if (tid == 0) {
        // 未分类对象：保守扫描（与 Windows 的"大对象保守"同精神；兜住
        // AnyBox.value / 字符串内嵌指针等无法精确判定的场景，只多保活不误收）。
        char* base = reinterpret_cast<char*>(obj);
        for (int64_t off = 0; off + (int64_t)sizeof(void*) <= size; off += sizeof(void*)) {
            void* cand;
            std::memcpy(&cand, base + off, sizeof(void*));
            gc_trace_child(cand, worklist);
        }
    } else if (tid == 2) {
        // RT_T_ARRAY（shadow 层 C 布局）：[len:4][cap:4][es:4][data@12]
        if (size < 12) return;
        int32_t es = 0, len = 0;
        std::memcpy(&es, (char*)obj + 8, 4);
        std::memcpy(&len, obj, 4);
        if (es == 8) {
            // 边界裁剪（对齐 Windows g_bad_array 保护：损坏头给出天文 len → 越界读）
            if (len < 0 || (int64_t)12 + (int64_t)len * 8 > size) {
                len = (int32_t)((size - 12) / 8);
                if (len < 0) len = 0;
            }
            for (int32_t i = 0; i < len; i++) {
                void* e;
                std::memcpy(&e, (char*)obj + 12 + (int64_t)i * 8, 8);
                gc_trace_child(e, worklist);
            }
        }
    } else if (tid == 3) {
        // RT_T_ANYBOX（RFS AnyBox：new + register kind=3，16 字节）
        // 布局：[magic:4@0][tag:4@4][value:8@8]；tag 3=string / 5=ptr → value 指针
        if (size < 16) return;
        int32_t tag = 0;
        std::memcpy(&tag, (char*)obj + 4, 4);
        if (tag == 3 || tag == 5) {
            void* v;
            std::memcpy(&v, (char*)obj + 8, 8);
            gc_trace_child(v, worklist);
        }
    } else if (tid == 4) {
        // RT_T_DICT：rt_dict { rt_kv** buckets@0; int32 n_buckets@8; int32 size@12; }
        if (size < 16) return;
        void* buckets = nullptr;
        std::memcpy(&buckets, obj, 8);
        int32_t nb = 0;
        std::memcpy(&nb, (char*)obj + 8, 4);
        gc_trace_child(buckets, worklist);
        if (!buckets || nb <= 0 || nb > 1 << 20) return;
        for (int32_t b = 0; b < nb; b++) {
            void* kvp = nullptr;
            std::memcpy(&kvp, (char*)buckets + (int64_t)b * 8, 8);
            while (kvp) {
                char* key = nullptr;
                std::memcpy(&key, kvp, 8);
                int32_t vt = 0;
                std::memcpy(&vt, (char*)kvp + 8, 4);
                void* next = nullptr;
                std::memcpy(&next, (char*)kvp + 24, 8);
                gc_trace_child(key, worklist);
                if (vt == 0 || vt == 4) {   // RT_V_STRING / RT_V_PTR
                    void* v;
                    std::memcpy(&v, (char*)kvp + 16, 8);
                    gc_trace_child(v, worklist);
                }
                gc_trace_child(kvp, worklist);
                kvp = next;
            }
        }
    } else if (tid == 5) {
        // RT_T_CLOSURE：[fn_ptr@0][env_ptr@8]
        if (size < 16) return;
        void* env = nullptr;
        std::memcpy(&env, (char*)obj + 8, 8);
        gc_trace_child(env, worklist);
    } else if (tid >= 100) {
        // RT_T_USER：类型表 bitmap 精确追踪；未注册/大对象 → 保守扫描
        std::lock_guard<std::mutex> lk(g_gc_types_mtx);
        if ((size_t)tid < g_gc_types.size() && g_gc_types[(size_t)tid].size > 0) {
            int64_t sz = g_gc_types[(size_t)tid].size;
            int64_t bm = g_gc_types[(size_t)tid].bitmap;
            if (size > 512) {
                char* base = reinterpret_cast<char*>(obj);
                for (int64_t off = 0; off + 8 <= size; off += 8) {
                    void* cand;
                    std::memcpy(&cand, base + off, 8);
                    gc_trace_child(cand, worklist);
                }
            } else {
                for (int64_t i = 0; i < 64 && i * 8 < sz; i++) {
                    if ((bm >> i) & 1) {
                        void* f;
                        std::memcpy(&f, (char*)obj + i * 8, 8);
                        gc_trace_child(f, worklist);
                    }
                }
            }
        } else {
            char* base = reinterpret_cast<char*>(obj);
            for (int64_t off = 0; off + 8 <= size; off += 8) {
                void* cand;
                std::memcpy(&cand, base + off, 8);
                gc_trace_child(cand, worklist);
            }
        }
    }
    // tid == 1（RT_T_STRING）及其它：不扫描
}

// Internal: run finalizers for dead objects, then reclaim them.
// Objects with finalizers that haven't run yet get one extra cycle (resurrection).
// P3/P3b：段对象清扫 —— 线性遍历，存活判据 = (epoch & MASK) == 当前轮。
// 活字节只累计带 trace 位的对象（pacing 口径对齐旧 visited==epoch：
// 分配即黑但未扎根的残留不计入 live）。返回回收对象数。
// 终结器走旁路表：未执行过 → 执行并复活一轮（写当前 epoch，无 trace 位）。
// P3b 整段退役：段内无任何活居民 → 摘除段内空闲链表节点、重置 used，
// 整段立即重新参与 bump 分配（成批分配-丢弃型负载的悬崖对策）。
static int64_t gc_sweep_segments(bool is_minor, int64_t* out_live) {
    int64_t freed = 0;
    uint32_t ep = g_gc_epoch;
    size_t n = g_segment_n.load(std::memory_order_acquire);
    std::vector<size_t> dead_offs;   // 本段新死槽位偏移（免二次全段扫描）
    for (size_t si = 0; si < n; si++) {
        GcSegment* seg = g_segments[si];
        if (seg->used == 0) continue;
        dead_offs.clear();
        size_t live_cnt = 0;         // 活居民（minor 下老年代算居民但不扫）
        // ── 单遍：存活判定 + 晋升 + 终结器 + 死槽收集 ──
        for (size_t off = 16; off <= seg->used; ) {
            ShadowHdr* h = (ShadowHdr*)(seg->base + off - 16);
            size_t stride = 16 + ((((size_t)(uint32_t)h->size) + 15) & ~(size_t)15);
            void* obj = seg->base + off;
            if (h->kind == HDR_KIND_DEAD) { off += stride; continue; }  // 已死（待复用/废弃）
            if (is_minor && h->gen != 0) { live_cnt++; off += stride; continue; }  // 老年代：居民，不扫
            uint32_t ew = h->epoch.load(std::memory_order_relaxed);
            if ((ew & HDR_EPOCH_MASK) == ep) {
                // 存活：晋升记账（年轻代）+ 活字节（只认 trace 位）
                live_cnt++;
                if (h->gen == 0) {
                    if (h->surv < 200) h->surv++;
                    if (h->surv >= PROMOTION_THRESHOLD) h->gen = 1;
                }
                if (ew & HDR_TRACE_BIT) *out_live += (int64_t)h->req;
                off += stride;
                continue;
            }
            // 死对象 —— 先查终结器（可能复活）
            auto fit = g_finalizers.find(obj);
            if (fit != g_finalizers.end() && !fit->second.finalized) {
                fit->second.finalized = true;
                fit->second.fn(fit->second.data ? fit->second.data : obj);
                h->epoch.store(ep, std::memory_order_relaxed);  // 复活一轮（无 trace 位）
                live_cnt++;
                off += stride;
                continue;
            }
            if (fit != g_finalizers.end()) g_finalizers.erase(fit);
            if (g_gc_watch && obj == g_gc_watch) gc_dump_referrers(obj);
            if (g_gc_log_on)
                fprintf(stderr, "[GCLOG] sweep #%lld free %p kind=%d size=%u gen=%u (seg)\n",
                        (long long)g_gc_collect_count, obj, (int)h->kind,
                        (unsigned)h->size, (unsigned)h->gen);
            if (h->kind == 0) g_diag_kind0++;
            dead_offs.push_back(off);
            off += stride;
        }
        // ── 整段退役：无活居民（新死槽 + 既有 DEAD 槽）→ 摘 fl + 重置 ──
        if (live_cnt == 0) {
            int64_t dead_req = 0;
            for (size_t off : dead_offs) {
                ShadowHdr* h = (ShadowHdr*)(seg->base + off - 16);
                void* obj = seg->base + off;
                if (h->kind == 1) reinterpret_cast<ShadowArray*>(obj)->~ShadowArray();
                if (is_minor) g_gc_remembered.erase(obj);
                for (auto git = g_gc_global_roots.begin(); git != g_gc_global_roots.end(); ) {
                    if (git->second == obj) git = g_gc_global_roots.erase(git);
                    else ++git;
                }
                dead_req += (int64_t)h->req;
            }
            fl_purge_range(seg->base, seg->base + seg->cap);
            seg->used = 0;   // 重置：段整体重新参与 bump（头在分配时重写）
            g_heap_bytes.fetch_sub(dead_req, std::memory_order_relaxed);
            g_seg_objs.fetch_sub((int64_t)dead_offs.size(), std::memory_order_relaxed);
            freed += (int64_t)dead_offs.size();
            if (g_gc_log_on)
                fprintf(stderr, "[GCLOG] sweep #%lld retire segment #%zu (%lld objs)\n",
                        (long long)g_gc_collect_count, si, (long long)dead_offs.size());
            continue;
        }
        if (dead_offs.empty()) continue;
        // ── 有活居民：逐槽回收死对象 ──
        for (size_t off : dead_offs) {
            ShadowHdr* h = (ShadowHdr*)(seg->base + off - 16);
            void* obj = seg->base + off;
            // P3：类型化载荷回收前终结——kind==1（迁移自注册的 RFS ShadowArray）
            // 的 data 缓冲是独立 malloc 内存，须先析构释放。
            if (h->kind == 1) {
                reinterpret_cast<ShadowArray*>(obj)->~ShadowArray();
            }
            if (is_minor) g_gc_remembered.erase(obj);
            for (auto git = g_gc_global_roots.begin(); git != g_gc_global_roots.end(); ) {
                if (git->second == obj) git = g_gc_global_roots.erase(git);
                else ++git;
            }
            h->kind = HDR_KIND_DEAD;   // 置死标记；段中内存绝不 free()
            g_heap_bytes.fetch_sub((int64_t)h->req, std::memory_order_relaxed);
            g_seg_objs.fetch_sub(1, std::memory_order_relaxed);
            if (h->size >= 16 && h->size <= 65536 && fl_enabled() &&
                g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
                fl_push(obj, (int32_t)h->size);
            }
            freed++;
        }
    }
    return freed;
}

static int64_t gc_sweep_dead(bool is_minor, int64_t* out_live) {
    // P3：GCMetaMap 已退役——全部对象在段上，清扫 = 纯线性遍历。
    return gc_sweep_segments(is_minor, out_live);
}

// ── Major GC: full mark-sweep over ALL objects (both generations) ──
extern "C" int64_t shadow_gc_collect() {
    if (g_gc_disabled) return 0;
    // P20-5: debug trace must honour g_gc_log_on — an unconditional fprintf here
    // pollutes the LSP server's stderr on every collect.
    if (g_gc_log_on) { fprintf(stderr, "[GC] major START segments=%zu objs=%lld\n", g_segment_n.load(std::memory_order_relaxed), (long long)g_seg_objs.load(std::memory_order_relaxed)); fflush(stderr); }
    gc_lock();  // lock-free spinlock: prevent concurrent collect
    g_gc_running.store(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_gc_mutex);   // serialize GC metadata/root access
    gc_diag_init();
    // ── STW：请求所有其它线程到达安全点，冻结根集（多线程并发下防止
    //    扫根/清扫窗口内其它线程改 roots 或写刚分配对象 → 漏标 → 误回收）──
    gc_stw_begin();
    if (g_gc_log_on)
        fprintf(stderr, "[GCLOG] major collect #%lld segments=%zu objs=%lld perm=%zu global=%zu threads=%zu remembered=%zu conservative=%zu heap=%lld trigger=%lld\n",
                (long long)g_gc_major_count, g_segment_n.load(std::memory_order_relaxed),
                (long long)g_seg_objs.load(std::memory_order_relaxed), g_gc_perm_roots.size(),
                g_gc_global_roots.size(), g_gc_thread_states.size(), g_gc_remembered.size(),
                g_gc_conservative_regions.size(),
                (long long)g_heap_bytes.load(std::memory_order_relaxed),
                (long long)g_gc_trigger.load(std::memory_order_relaxed));

    // ── Mark phase: trace from all roots ──
    // P3：全部对象带内头——存活/去重由头内 epoch 字承担（trace 位 = 本轮已追）。
    // epoch 递增即「新一轮」，无需清任何标记；分配即黑 = 分配时写当前 epoch。
    auto _gc_t0 = prof_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto _gc_t0_mark = _gc_t0;
    auto _gc_t1 = _gc_t0;
    uint32_t epoch = g_gc_epoch + 1;
    if (epoch == 0) {
        // epoch 回绕（2^31 次 GC 一轮）：清全部段头后从 1 重新开始。
        size_t sn = g_segment_n.load(std::memory_order_relaxed);
        for (size_t si = 0; si < sn; si++) {
            GcSegment* seg = g_segments[si];
            for (size_t off = 16; off <= seg->used; ) {
                ShadowHdr* h = (ShadowHdr*)(seg->base + off - 16);
                size_t stride = 16 + ((((size_t)(uint32_t)h->size) + 15) & ~(size_t)15);
                if (h->kind != HDR_KIND_DEAD) h->epoch.store(0, std::memory_order_relaxed);
                off += stride;
            }
        }
        epoch = 1;
    }
    g_gc_epoch = epoch;
    if (prof_on()) {
        g_prof_mark_reset_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t1).count();
        _gc_t1 = std::chrono::steady_clock::now();
    }
    std::vector<void*> worklist;
    for (void* root : g_gc_perm_roots) { gc_trace_child(root, worklist); }
    for (auto& kv : g_gc_global_roots) { gc_trace_child(kv.second, worklist); }
    for (ThreadGCState* s : g_gc_thread_states) {
        std::lock_guard<std::mutex> lk(s->mtx);
        for (void* root : s->roots) gc_trace_child(root, worklist);
        for (auto& kv : s->named_roots) gc_trace_child(kv.second, worklist);
        // shadow frame range roots：逐槽精确追踪（对齐 Windows rt_gc.o 扫根）。
        for (auto& rg : s->range_roots) {
            char* base = reinterpret_cast<char*>(rg.first);
            uint32_t n = rg.second;
            for (uint32_t i = 0; i < n; i++) {
                void* slot_val;
                std::memcpy(&slot_val, base + (size_t)i * sizeof(void*), sizeof(void*));
                gc_trace_child(slot_val, worklist);
            }
        }
    }
    // 编译期栈映射：逐线程沿【本线程】帧锚点链表扫描整片连续 shadow frame（变量槽 + spill 区）。
    // 取代旧 root_range（位等价、更全面，且无每调用根注册开销）。链表头存于 ThreadGCState::
    // head_slot（per-thread，codegen 直写此处），彻底修复 V1 单全局头在 spawn 多线程下
    // 根截断（ex_gc_spawn）。注意：必须读 *s->head_slot 而非 s->frame_head —— codegen
    // enter/leave 直接读写 shadow_gc_tls_frame_head，两者始终一致。
    for (ThreadGCState* s : g_gc_thread_states) {
        std::lock_guard<std::mutex> lk(s->mtx);
        ShadowFrameAnchor* head = s->head_slot ? reinterpret_cast<ShadowFrameAnchor*>(*s->head_slot) : nullptr;
        for (ShadowFrameAnchor* a = head; a != nullptr; a = a->prev) {
            if (a->frame_n <= 0) continue;
            char* base = reinterpret_cast<char*>(a->frame_ptr);
            uint32_t n = (uint32_t)a->frame_n;
            for (uint32_t i = 0; i < n; i++) {
                void* slot_val;
                std::memcpy(&slot_val, base + (size_t)i * sizeof(void*), sizeof(void*));
                gc_trace_child(slot_val, worklist);
            }
        }
    }
    // Conservative scan: trace pointer-sized values in registered memory regions
    // (e.g., process data segment for binaries without explicit GC root registration).
    gc_trace_conservative_regions(worklist);
    /* §5.2.4：精确 GC（默认，SHADOW_GC_CONSERVATIVE 未设/0）只用精确根集
     * （perm/global/thread roots/named_roots/range_roots）。保守栈/进程堆扫描
     * 会把死对象内部或残留指针当 root 复活整图 → 零回收 → 堆谷底单调增长
     * （ex_gc_longrun 漂移根因）。仅 SHADOW_GC_CONSERVATIVE=1 启用对拍。 */
    if (gc_cons_stack_on()) {
        gc_scan_stack(worklist);
        gc_scan_process_heap(worklist);
    }
    if (prof_on()) {
        g_prof_mark_roots_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t1).count();
        _gc_t1 = std::chrono::steady_clock::now();
    }

    while (!worklist.empty()) {
        void* obj = worklist.back(); worklist.pop_back();
        ShadowHdr* h = gc_header_of(obj);
        if (!h) continue;
        gc_trace_object_children(obj, (int32_t)h->kind, (int64_t)h->size, worklist);
    }
    if (prof_on()) {
        g_prof_mark_wl_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t1).count();
        g_prof_mark_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        _gc_t0_mark = _gc_t0;
        _gc_t0 = std::chrono::steady_clock::now();
    }

    if (g_gc_log_on) { fprintf(stderr, "[GC] major MARK done sweeping...\n"); fflush(stderr); }
    // ── Sweep phase ──
    g_gc_collect_count++;
    g_gc_major_count++;

    // Run finalizers + free dead objects（纯段遍历）。
    int64_t live = 0;   // 活字节由段清扫累计（只认 trace 位）
    int64_t freed = gc_sweep_dead(false, &live);
    if (prof_on()) {
        g_prof_sweep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        g_prof_gc_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0_mark).count();
        g_prof_gc_count++;
    }

    // ── GOGC pacing ──
    // live = 本轮真实可达堆（trace 位对象）。分配即黑未扎根的残留不计入，
    // 防 trigger 正反馈放大（ex_gc_longrun 谷底漂移的历史根因）。
    // Clear remembered set after major GC (all references re-traced).
    g_gc_remembered.clear();
    {
        int64_t gogc = rt_gc_gogc();
        int64_t next = live + live * gogc / 100;
        int64_t minheap = rt_gc_min_heap();
        if (next < minheap) next = minheap;
        g_gc_trigger.store(next, std::memory_order_relaxed);
        if (g_gc_log_on)
            fprintf(stderr, "[GCLOG] major settle live=%lld trigger=%lld gogc=%lld\n",
                    (long long)live, (long long)next, (long long)gogc);
    }

    gc_stw_end();
    g_gc_running.store(0, std::memory_order_relaxed);
    gc_unlock();
    return freed;
}

// ── Minor GC: collect only young-gen objects ──
// Roots = stack roots + remembered set (old→young refs).
// Old-gen objects are NOT swept (assumed alive unless a major GC runs).
extern "C" int64_t shadow_gc_minor_collect() {
    if (g_gc_disabled) return 0;
    if (g_gc_log_on) { fprintf(stderr, "[GC] minor START segments=%zu objs=%lld\n", g_segment_n.load(std::memory_order_relaxed), (long long)g_seg_objs.load(std::memory_order_relaxed)); fflush(stderr); }
    gc_lock();
    std::lock_guard<std::mutex> lk(g_gc_mutex);   // serialize GC metadata/root access
    gc_diag_init();
    gc_stw_begin();
    if (g_gc_log_on)
        fprintf(stderr, "[GCLOG] minor collect #%lld objs=%lld remembered=%zu\n",
                (long long)g_gc_minor_count,
                (long long)g_seg_objs.load(std::memory_order_relaxed), g_gc_remembered.size());

    // ── Mark phase: trace from roots, but only mark young-gen objects ──
    // Old-gen objects are treated as alive (not swept in minor GC —— 段清扫
    // 按 h->gen 跳过，无需显式置活)。P2：段对象标记走头内 epoch 字。
    auto _gc_t0 = prof_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto _gc_t0_mark = _gc_t0;
    uint32_t epoch = g_gc_epoch + 1;
    if (epoch == 0) epoch = 1;   // 回绕：段头清零由 major 负责（此处罕见且无害）
    g_gc_epoch = epoch;
    std::vector<void*> worklist;

    // Trace from roots: only young-gen objects get newly marked.
    for (void* root : g_gc_perm_roots) { gc_trace_child(root, worklist); }
    for (auto& kv : g_gc_global_roots) { gc_trace_child(kv.second, worklist); }
    for (ThreadGCState* s : g_gc_thread_states) {
        std::lock_guard<std::mutex> lk(s->mtx);
        for (void* root : s->roots) gc_trace_child(root, worklist);
        for (auto& kv : s->named_roots) gc_trace_child(kv.second, worklist);
        for (auto& rg : s->range_roots) {
            char* base = reinterpret_cast<char*>(rg.first);
            uint32_t n = rg.second;
            for (uint32_t i = 0; i < n; i++) {
                void* slot_val;
                std::memcpy(&slot_val, base + (size_t)i * sizeof(void*), sizeof(void*));
                gc_trace_child(slot_val, worklist);
            }
        }
    }
    // Conservative scan: trace pointer-sized values in registered memory regions.
    gc_trace_conservative_regions(worklist);
    gc_scan_stack(worklist);
    // Note: heap scan only in major GC (too slow for minor GC frequency)

    // Trace from remembered set: old-gen objects referencing young-gen.
    for (void* parent : g_gc_remembered) {
        ShadowHdr* h = gc_header_of(parent);
        if (!h) continue;
        gc_trace_object_children(parent, (int32_t)h->kind, (int64_t)h->size, worklist);
    }

    // Expand worklist: trace children of marked young-gen objects.
    while (!worklist.empty()) {
        void* obj = worklist.back(); worklist.pop_back();
        ShadowHdr* h = gc_header_of(obj);
        if (!h) continue;
        gc_trace_object_children(obj, (int32_t)h->kind, (int64_t)h->size, worklist);
    }
    if (prof_on()) {
        g_prof_mark_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        _gc_t0_mark = _gc_t0;
        _gc_t0 = std::chrono::steady_clock::now();
    }

    // ── Sweep phase: only sweep young-gen objects that are unmarked ──
    g_gc_collect_count++;
    g_gc_minor_count++;

    // Run finalizers + free dead young-gen objects（纯段遍历）。
    int64_t _minor_live = 0;
    int64_t freed = gc_sweep_dead(true, &_minor_live);
    if (prof_on()) {
        g_prof_sweep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        g_prof_gc_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0_mark).count();
        g_prof_gc_count++;
    }

    gc_stw_end();
    gc_unlock();
    return freed;
}

// Register a finalizer callback for a GC-managed object.
// The finalizer fn(void* data) will be called BEFORE the object is freed.
// If the finalizer re-roots the object (adds it back to a root set),
// the object survives this cycle (resurrection).
// P3：段对象（唯一对象来源）落旁路表。
extern "C" void shadow_gc_set_finalizer(void* ptr, void(*fn)(void*), void* data) {
    if (!ptr || !fn) return;
    gc_lock_blocked();
    g_finalizers[ptr] = GcFinalizerRec{fn, data, false};
    g_gc_mutex.unlock();
}

// GC 登记对象删除辅助：shadow_free 在段释放（gc_hdr_release）之后调用。
// P3：登记表已退役，此处恒 0（段对象已被前一步处理）；保留符号兼容。
extern "C" int32_t shadow_gc_forget(void* ptr) {
    (void)ptr;
    return 0;
}

// 诊断：指针是否是活 GC 对象（0=已回收/非堆对象）。P3：查带内头。
extern "C" int32_t shadow_gc_meta_contains(void* p) {
    if (!p) return 0;
    return gc_header_of(p) ? 1 : 0;
}

// Allocate a raw GC-managed block (used by codegen for `new Struct{...}`).
// Allocations go to young gen (gen=0).
//
// 自动 GC 触发（对齐 Windows rt_gc.c 的 gc_alloc_hook）：//   ① SHADOW_GC_STRESS=N：每 N 次分配触发一次（无视堆阈值，Go gcstress 等价）；
//   ② 否则 GOGC 模型：g_heap_bytes >= g_gc_trigger 触发（trigger = live×(1+GOGC/100)，
//      最小 4MB）。分配即安全点：调用 alloc 时活指针已 spill 到 shadow frame
//      （root_range 注册），同步 collect 不会误回收未扎根临时量。
// 防重入：g_gc_running 在 collect 在途时置 1，触发检查跳过。
extern "C" void* shadow_gc_alloc(int32_t size, int32_t kind) {
    // 对齐 Windows rt_alloc_impl：分配 8 字节对齐容量并记录对齐后大小，
    // 使 rt_alloc_cap 返回真实可用容量（如 5 字节字符串 → 8），
    // shadow_string_concat_inplace 可复用对齐余量就地追加（匹配 Windows 效率）。
    if (size < 0) size = 0;
    int32_t asize = (size + 7) & ~7;
    if (asize < size) asize = size;   // overflow guard
    // 触发检查（P2：记账已内联，无攒批字节）。
    if (g_gc_disabled == 0 && rt_gc_auto_on() && g_gc_running.load(std::memory_order_relaxed) == 0) {
        int32_t stress = rt_gc_stress_n();
        int64_t want = 0;
        if (stress > 0) {
            int64_t t = g_alloc_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((t % (int64_t)stress) == 0) want = 1;
        } else {
            int64_t hb = g_heap_bytes.load(std::memory_order_relaxed);
            if (hb >= g_gc_trigger.load(std::memory_order_relaxed) && hb > 0) want = 1;
        }
        if (want) {
            g_gc_poll_flag.store(1, std::memory_order_relaxed);  // 提示 poll 兜底复查
            shadow_gc_collect();
        }
    }
    // 空闲链表快路径：单 mutator 时优先复用已死对象（免段 bump）。
    // 所有槽位（复用/新分配）载荷前都有 16B 头区。
    void* p = nullptr;
    if (fl_enabled() && asize >= 16 && asize <= 65536 &&
        g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
        p = fl_pop(asize);
    }
    if (!p) {
        p = gc_seg_alloc(asize);
        if (!p) return nullptr;
    }
    {   // P3：带内头是唯一元数据。「分配即黑」= 写当前 epoch → 本轮清扫必然存活。
        ShadowHdr* h = (ShadowHdr*)((char*)p - 16);
        int32_t k = kind < 0 ? 0 : (kind > (int32_t)0xFFFE ? (int32_t)0xFFFE : kind);
        h->kind = (uint16_t)k;
        h->gen = 0;
        h->surv = 0;
        h->size = (uint32_t)asize;
        h->req = (uint32_t)size;
        h->epoch.store(g_gc_epoch, std::memory_order_relaxed);
    }
    // P2：记账内联——无锁、无哈希、无攒批。
    g_heap_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
    g_seg_objs.fetch_add(1, std::memory_order_relaxed);
    tl_gc_alloc_count++;
    g_gc_total_alloc_count.fetch_add(1, std::memory_order_relaxed);
    return p;
}

// Register an already-allocated C++ object (ShadowArray/ShadowDict) for GC.
// NOTE: previously this permanently rooted the object (g_gc_perm_roots), which
// made every array/dict immortal. That broke collection after reassignment
// (`a = []` could never reclaim the old array because the perm root lived
// forever). Rooting is now driven by the *variable slot* via
// shadow_gc_root_set (replace semantics): a variable's slot keys the current
// value, so reassigning the variable stops rooting the old value. Temporary
// arrays/dicts that are never bound to a slot are still kept alive by
// shadow_gc_root_add (frame roots) until the enclosing frame returns.
extern "C" void shadow_gc_register(void* ptr, int32_t kind, int64_t size) {
    // P3：GCMetaMap 已退役——所有 GC 对象走段分配 + 带内头，不再有「登记既有
    // 对象」路径。保留符号仅为 codegen 兼容（rt_gc_register 映射，无实际调用方）。
    (void)ptr; (void)kind; (void)size;
}

// ── Root management (LIFO frame discipline) ──
// These are called from generated IR; signatures use int32_t to match the
// codegen's i32 convention. The frame marker is a root-count snapshot.
//
// 无锁快路径：GC 收集只在安全点（poll/alloc）同步发生，STW 协议（gc_stw_begin）
// 保证 collect 扫根前所有其它线程已到达安全点（stw_state!=0）且不再改根——因此
// g_gc_running==0（无收集在途）时根容器只被本线程访问，可跳过 s->mtx 互斥锁；
// collect 在途时回退持锁慢路径。可见性由 stw_state 的 release/acquire 对保证：
// mutator 的根修改 → poll 置 stw_state=1（release）→ gc_stw_begin 读 1（acquire）
// → collect 扫根。基准（quicksort 等）GC 收集次数为 0，此快路径消除每函数
// 4 次互斥锁（frame_enter/root_range/root_set/frame_leave）的纯开销。
static inline bool gc_root_lock_free() {
    return g_gc_running.load(std::memory_order_relaxed) == 0;
}

extern "C" int32_t shadow_gc_frame_enter() {
    ThreadGCState* s = gc_get_thread_state();
    if (gc_root_lock_free()) {
        s->named_frames.push_back({});
        s->range_markers.push_back(s->range_roots.size());
        return (int32_t)s->roots.size();
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    s->named_frames.push_back({});
    s->range_markers.push_back(s->range_roots.size());
    return (int32_t)s->roots.size();
}
extern "C" int32_t shadow_gc_root_add(void* ptr) {
    ThreadGCState* s = gc_get_thread_state();
    if (gc_root_lock_free()) {
        s->roots.push_back(ptr);
        return 0;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    s->roots.push_back(ptr);
    return 0;
}
// Replace-semantics root: keyed by the variable's slot address. Repeated calls
// with the same slot overwrite the rooted value, so a reassignment stops
// rooting the previous value and the GC can reclaim it. A null `val` erases
// the slot's root (used on variable death / re-init).
extern "C" int32_t shadow_gc_root_set(void* slot, void* val) {
    if (!slot) return 0;
    ThreadGCState* s = gc_get_thread_state();
    if (gc_root_lock_free()) {
        auto it = s->named_roots.find(slot);
        if (it == s->named_roots.end() && !s->named_frames.empty()) {
            s->named_frames.back().push_back(slot);
        }
        if (val) {
            s->named_roots[slot] = val;
        } else {
            s->named_roots.erase(slot);
        }
        return 0;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    auto it = s->named_roots.find(slot);
    if (it == s->named_roots.end() && !s->named_frames.empty()) {
        s->named_frames.back().push_back(slot);
    }
    if (val) {
        s->named_roots[slot] = val;
    } else {
        s->named_roots.erase(slot);
    }
    return 0;
}
extern "C" int32_t shadow_gc_frame_leave(int32_t marker) {
    if (marker < 0) return 0;
    ThreadGCState* s = gc_get_thread_state();
    if (gc_root_lock_free()) {
        size_t m = (size_t)marker;
        if (m <= s->roots.size()) {
            s->roots.resize(m);
        }
        // Drop every replace-semantics root registered in this frame; the slot
        // addresses are about to become invalid as the frame's stack unwinds.
        if (!s->named_frames.empty()) {
            for (void* slot : s->named_frames.back()) {
                s->named_roots.erase(slot);
            }
            s->named_frames.pop_back();
        }
        // Drop this frame's shadow-frame range root：帧 LIFO，直接 resize 回退到
        // 帧入口快照（range_markers），免线性扫描（对齐 Windows rt_gc.o 的
        // frame discipline：ranges 是 LIFO，由 frame_leave 弹出）。
        if (!s->range_markers.empty()) {
            s->range_roots.resize(s->range_markers.back());
            s->range_markers.pop_back();
        }
        return 0;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    size_t m = (size_t)marker;
    if (m <= s->roots.size()) {
        s->roots.resize(m);
    }
    // Drop every replace-semantics root registered in this frame; the slot
    // addresses are about to become invalid as the frame's stack unwinds.
    if (!s->named_frames.empty()) {
        for (void* slot : s->named_frames.back()) {
            s->named_roots.erase(slot);
        }
        s->named_frames.pop_back();
    }
    // Drop this frame's shadow-frame range roots (aligned with Windows
    // rt_gc.o's frame discipline: ranges are LIFO, popped by frame_leave).
    if (!s->range_markers.empty()) {
        s->range_roots.resize(s->range_markers.back());
        s->range_markers.pop_back();
    }
    return 0;
}

// ── shadow frame range roots（§5.2.4 对齐 Windows rt_gc.o）──
// codegen 在函数入口调用 shadow_gc_root_range(%sf, n)：%sf 是 shadow frame
// （活跃变量的 spill 区，n 个指针槽）。注册到当前线程的 range 列表，collect
// 扫描时逐槽追踪。frame_leave 弹出（LIFO）。
// 此前 Linux 用户产物链接 shadow_gc_supplement.o 的 no-op 版 —— 帧槽不被
// 扫描，自动 GC 一开就误回收未扎根临时量。此处为真实现，配合 main_link_exe
// 去掉 supplement 后生效。
extern "C" int32_t shadow_gc_root_range(void* base, uint32_t n) {
    if (!base || n == 0) return 0;
    ThreadGCState* s = gc_get_thread_state();
    if (gc_root_lock_free()) {
        s->range_roots.push_back({base, n});
        return 0;
    }
    std::lock_guard<std::mutex> lk(s->mtx);
    s->range_roots.push_back({base, n});
    return 0;
}

// ── 协作式安全点（§5.2.4 对齐 Windows rt_gc.o 的 shadow_gc_poll）──
// codegen 在函数入口 / 循环回边插入。本实现：
//   ① STW 协作：collect 请求时到达安全点并自旋，等放行（多线程下防止
//      collect 扫根窗口内其它线程改 roots → 漏标 → 误回收）；
//   ② 触发检查：安全点处活指针已 spill 到 shadow frame，GOGC/STRESS 达标即
//      同步 collect（与 Windows 在 alloc hook 触发语义一致）。
extern "C" void shadow_gc_poll() {
    // 快路径：单原子加载。STW 请求或 alloc 触发时标志置 1，否则无分配热循环
    // （sum_loop 等）直接返回，免去每次 6+ 次加载 + rt_gc_auto_on 调用。
    if (g_gc_poll_flag.load(std::memory_order_relaxed) == 0) return;
    g_gc_poll_flag.store(0, std::memory_order_relaxed);
    // ① STW 协作：collect 请求时到达安全点并自旋，等放行。
    if (g_gc_stw_req.load(std::memory_order_relaxed) ||
        g_gc_stw_active.load(std::memory_order_relaxed)) {
        ThreadGCState* s = gc_get_thread_state();
        s->stw_state.store(1, std::memory_order_release);   // 到达安全点（根集已冻结）
        // 等本次 STW 完全结束（req 清 0 且 active 清 0）：
        // ⚠️ 只等 active 不够 —— poll 可能在 GC 的"等待阶段"进入（req=1,
        // active=0），只看 active 会立即通过并回到 mutator，而 GC 已把它
        // 计入"到达"并开始扫根 —— 扫根窗口内该线程却在跑 mutator 代码
        // （非安全点），精确根集漏标。必须等到 req 与 active 双双清零。
        while (g_gc_stw_req.load(std::memory_order_relaxed) ||
               g_gc_stw_active.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        s->stw_state.store(0, std::memory_order_release);
        return;
    }
    // ② 触发检查（安全点处活指针已 spill 到 shadow frame；GOGC/STRESS 达标即同步 collect）
    if (g_gc_disabled) return;
    if (!rt_gc_auto_on()) return;
    if (g_gc_running.load(std::memory_order_relaxed)) return;
    int32_t stress = rt_gc_stress_n();
    int64_t want = 0;
    if (stress > 0) {
        int64_t t = g_alloc_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((t % (int64_t)stress) == 0) want = 1;
    } else {
        int64_t hb = g_heap_bytes.load(std::memory_order_relaxed);
        if (hb >= g_gc_trigger.load(std::memory_order_relaxed) && hb > 0) want = 1;
    }
    if (want) shadow_gc_collect();
}

// ── Debug helper: detect corrupted (module-derived / low-32-zeroed) pointers ──
// Used by the shadow-0.3 self-host debugging to localize bad AST child pointers.
// Also called by runtime_lib.shadow_any_to_string to distinguish transparent raw
// int handles (inttoptr, e.g. array elements) from real heap pointers before
// dereferencing an AnyBox* — a raw int >= 65536 would otherwise be treated as a
// pointer and crash. VirtualQuery-based check mirrors the static is_valid_ptr.
extern "C" int32_t shadow_is_valid_ptr(void* p) {
    if (p == nullptr) return 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return 1;
#else
    // 用 mincore 判别地址是否已映射：裸整值（inttoptr，如动态数组 any 元素）指向
    // 未映射页 → mincore 返回 ENOMEM → 判定非堆指针，按整数打印，避免 AV。
    long page = sysconf(_SC_PAGESIZE);
    void* aligned = (void*)((uintptr_t)p & ~((uintptr_t)page - 1));
    unsigned char vec = 0;
    if (mincore(aligned, (size_t)page, &vec) != 0) return 0;
    return 1;
#endif
}

extern "C" void shadow_bad_node_report(void* e) {
    fprintf(stderr, "BAD_NODE ptr=%p\n", e);
#ifdef _WIN32
    void* frames[16];
    USHORT n = CaptureStackBackTrace(1, 16, frames, NULL);
    for (USHORT i = 0; i < n; i++) {
        fprintf(stderr, "  #%u %p\n", (unsigned)i, frames[i]);
    }
#else
    void* frames[16];
    int n = backtrace(frames, 16);
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  #%u %p\n", (unsigned)i, frames[i]);
    }
#endif
    fflush(stderr);
}

// NOTE: setjmp/longjmp are NOT wrapped here. The codegen declares them
// directly in LLVM IR using the native CRT names (_setjmp on Windows).
// The generated IR calls @_setjmp and @longjmp, which link against the C
// runtime. sizeof(jmp_buf) = 256 on x64 Windows (verified at runtime).
#include <csetjmp>
#include <cstring>
#include <llvm-c/Core.h>
static_assert(sizeof(jmp_buf) <= 256, "jmp_buf exceeds 256 bytes");

// Ã¢ÂÂÃ¢ÂÂ LLVM Bridge: Shadow Ã¢ÂÂ LLVM-C API (thread-local buffer for array params) Ã¢ÂÂÃ¢ÂÂ
static const int SHADOW_LLVM_BUF_MAX = 128;
thread_local void* g_shadow_llvm_buf[SHADOW_LLVM_BUF_MAX];
thread_local int g_shadow_llvm_buf_idx = 0;

extern "C" void shadow_llvm_buf_reset() { g_shadow_llvm_buf_idx = 0; }
extern "C" void shadow_llvm_buf_push(void* v) { if (g_shadow_llvm_buf_idx < SHADOW_LLVM_BUF_MAX) g_shadow_llvm_buf[g_shadow_llvm_buf_idx++] = v; }
extern "C" void shadow_llvm_buf_push_long(int64_t v) { if (g_shadow_llvm_buf_idx < SHADOW_LLVM_BUF_MAX) g_shadow_llvm_buf[g_shadow_llvm_buf_idx++] = (void*)v; }
extern "C" int shadow_llvm_buf_count() { return g_shadow_llvm_buf_idx; }

extern "C" void* shadow_llvm_fn_type(void* ret, int n, int va) {
    return LLVMFunctionType((LLVMTypeRef)ret, (LLVMTypeRef*)g_shadow_llvm_buf, n, va);
}
extern "C" void* shadow_llvm_build_call(void* b, void* t, void* fn, int n, const char* name) {
    return LLVMBuildCall2((LLVMBuilderRef)b, (LLVMTypeRef)t, (LLVMValueRef)fn, (LLVMValueRef*)g_shadow_llvm_buf, n, name);
}
extern "C" void shadow_llvm_struct_set_body(void* st, int n, int packed) {
    LLVMStructSetBody((LLVMTypeRef)st, (LLVMTypeRef*)g_shadow_llvm_buf, n, packed);
}
extern "C" void* shadow_llvm_build_gep2(void* b, void* et, void* ptr, int n, const char* name) {
    return LLVMBuildGEP2((LLVMBuilderRef)b, (LLVMTypeRef)et, (LLVMValueRef)ptr, (LLVMValueRef*)g_shadow_llvm_buf, n, name);
}
extern "C" void* shadow_llvm_append_block(void* fn, const char* name) {
    return LLVMAppendBasicBlock((LLVMValueRef)fn, name);
}
extern "C" void* shadow_llvm_const_int(void* t, int64_t v, int s) {
    return LLVMConstInt((LLVMTypeRef)t, (uint64_t)v, s);
}
extern "C" void* shadow_llvm_int1_type(void* ctx) { return LLVMInt1TypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_int8_type(void* ctx) { return LLVMInt8TypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_int16_type(void* ctx) { return LLVMInt16TypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_int32_type(void* ctx) { return LLVMInt32TypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_int64_type(void* ctx) { return LLVMInt64TypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_float_type(void* ctx) { return LLVMFloatTypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_double_type(void* ctx) { return LLVMDoubleTypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_void_type(void* ctx) { return LLVMVoidTypeInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_ptr_type(void* et, int as) { return LLVMPointerType((LLVMTypeRef)et, as); }
extern "C" void* shadow_llvm_array_type(void* et, int c) { return LLVMArrayType((LLVMTypeRef)et, (unsigned)c); }
extern "C" void* shadow_llvm_create_builder(void* ctx) { return LLVMCreateBuilderInContext((LLVMContextRef)ctx); }
extern "C" void* shadow_llvm_module_create_with_name(const char* name) {
    return LLVMModuleCreateWithName(name);
}
extern "C" void* shadow_llvm_get_named_function(void* m, const char* name) {
    return LLVMGetNamedFunction((LLVMModuleRef)m, name);
}
extern "C" void* shadow_llvm_add_global(void* m, void* t, const char* name) {
    return LLVMAddGlobal((LLVMModuleRef)m, (LLVMTypeRef)t, name);
}
extern "C" void shadow_llvm_set_initializer(void* g, void* v) {
    LLVMSetInitializer((LLVMValueRef)g, (LLVMValueRef)v);
}
// LLVMSetLinkage: 设置全局变量/函数的 linkage（如 LLVMPrivateLinkage=9
// 用于字符串字面量全局，避免 runtime.o 与用户 .ll 的 .str.N 在链接时冲突）
extern "C" void shadow_llvm_set_linkage(void* g, int linkage) {
    LLVMSetLinkage((LLVMValueRef)g, (LLVMLinkage)linkage);
}
extern "C" void shadow_llvm_set_global_constant(void* g, int is_const) {
    LLVMSetGlobalConstant((LLVMValueRef)g, is_const);
}
extern "C" void* shadow_llvm_const_string(const char* str, int len, int dont_null_terminate) {
    return LLVMConstString(str, (unsigned)len, dont_null_terminate);
}
// Float constant: parse string to double, create LLVMConstReal
extern "C" void* shadow_llvm_const_float(void* type_ref, const char* str) {
    double val = 0.0;
    if (str) val = strtod(str, nullptr);
    return LLVMConstReal((LLVMTypeRef)type_ref, val);
}
// Create a constant array from values previously pushed to the arg buffer.
extern "C" void* shadow_llvm_const_array(void* elem_ty, int count) {
    LLVMValueRef* vals = new LLVMValueRef[count];
    for (int i = 0; i < count && i < g_shadow_llvm_buf_idx; ++i) {
        vals[i] = (LLVMValueRef)g_shadow_llvm_buf[i];
    }
    LLVMValueRef arr = LLVMConstArray((LLVMTypeRef)elem_ty, vals, (unsigned)count);
    delete[] vals;
    return arr;
}
extern "C" void* shadow_llvm_add_function(void* m, const char* name, void* ft) {
    return LLVMAddFunction((LLVMModuleRef)m, name, (LLVMTypeRef)ft);
}
extern "C" void* shadow_llvm_get_named_global(void* m, const char* name) {
    return LLVMGetNamedGlobal((LLVMModuleRef)m, name);
}
// alwaysinline：给热点运行时函数（shadow_schar/shadow_subscript 等）加内联属性，
// 使 opt -O2 强制内联进热循环，消除调用开销（str_reverse 提升约 10%）。
extern "C" void shadow_llvm_set_alwaysinline(void* ctx, void* fn) {
    unsigned kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
    if (kind == 0) return;
    LLVMAttributeRef attr = LLVMCreateEnumAttribute((LLVMContextRef)ctx, kind, 0);
    LLVMAddAttributeAtIndex((LLVMValueRef)fn, LLVMAttributeFunctionIndex, attr);
}

// 把 LLVM 全局变量标记为 thread_local。codegen 用它声明 shadow_gc_tls_frame_head
// 为 TLS 全局，使 enter/leave 能直接读写该槽而无需每帧调用 helper。
extern "C" void shadow_llvm_set_tls(void* g) {
    LLVMSetThreadLocal((LLVMValueRef)g, 1);
    LLVMSetThreadLocalMode((LLVMValueRef)g, LLVMInitialExecTLSModel);
}

// ── Shadow-lang runtime I/O (used by user code compiled by shadow-lang) ─
extern "C" void shadow_println_str(const char* s) {
    if (s) printf("%s\n", s);
    else printf("(null)\n");
    fflush(stdout);
}
extern "C" void shadow_println_int(int32_t v) {
    printf("%d\n", v);
    fflush(stdout);
}
extern "C" int32_t shadow_print_str(const char* s) {
    if (s) printf("%s", s);
    fflush(stdout);
    return 0;
}
extern "C" void shadow_print_int(int32_t v) {
    printf("%d", v);
    fflush(stdout);
}
extern "C" int32_t shadow_string_len(const char* s) {
    if (!s) return 0;
    return (int32_t)strlen(s);
}
extern "C" const char* shadow_int_to_string(int32_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    return dup_str(buf);
}

// ── __rt_ prefixed aliases for shadow-0.3 runtime primitives ──
// shadow-0.3 declares extern functions with __rt_ prefix to avoid
// name collision with user-defined Shadow runtime functions.
// 对齐 Windows rt_core.c 的 rt_alloc_impl：所有 rt_malloc 分配（字符串、C 布局
// AnyBox、dict 桶等）都登记进 GC（type_id=0，不扫描子对象，仅保活/回收）。
// 这让字符串进入 GC 堆 → GOGC 触发模型生效（ex_gc_longrun 等长驻用例），
// 且与 Windows 的分配语义一致。GC disabled 时回退裸 malloc。
extern "C" void* __rt_shadow_malloc(int32_t n) {
    if (g_gc_disabled) return malloc(n);
    return shadow_gc_alloc(n, 0);
}
extern "C" void  __rt_shadow_free(void* p) { shadow_free(p); }
extern "C" void  __rt_shadow_memcpy(void* dst, const void* src, int32_t n) { memcpy(dst, src, n); }
extern "C" void  __rt_shadow_memcpy_at(void* dst, int32_t dst_off, const void* src, int32_t src_off, int32_t n) { memcpy((char*)dst+dst_off, (const char*)src+src_off, n); }
extern "C" void  __rt_shadow_memset(void* dst, int32_t val, int32_t n) { memset(dst, val, n); }
extern "C" int32_t __rt_shadow_strlen(const char* s) {
    return (int32_t)(s ? strlen(s) : 0);
}
extern "C" int32_t shadow_get_byte(void* p, int32_t off) { return (int32_t)(((unsigned char*)p)[off]); }
extern "C" int32_t __rt_shadow_get_byte(void* p, int32_t off) { return (int32_t)((unsigned char*)p)[off]; }
extern "C" void   __rt_shadow_set_byte(void* p, int32_t off, int32_t val) { ((char*)p)[off] = (char)val; }
extern "C" int32_t __rt_shadow_load_int(void* p, int32_t off) { return *(int32_t*)((char*)p+off); }
extern "C" void   __rt_shadow_store_int(void* p, int32_t off, int32_t val) { *(int32_t*)((char*)p+off) = val; }
extern "C" int64_t __rt_shadow_load_long(void* p, int32_t off) { return *(int64_t*)((char*)p+off); }
extern "C" void    __rt_shadow_store_long(void* p, int32_t off, int64_t val) { *(int64_t*)((char*)p+off) = val; }
extern "C" void*   __rt_shadow_load_ptr(void* p, int32_t off) { return *(void**)((char*)p+off); }
extern "C" void    __rt_shadow_store_ptr(void* p, int32_t off, void* val) { *(void**)((char*)p+off) = val; }
extern "C" double  __rt_shadow_load_float(void* p, int32_t off) { return *(double*)((char*)p+off); }
extern "C" void    __rt_shadow_store_float(void* p, int32_t off, double val) { *(double*)((char*)p+off) = val; }
extern "C" int32_t __rt_shadow_puts(const char* s) { if(s) puts(s); return 0; }
extern "C" int32_t __rt_shadow_print_str(const char* s) { shadow_print_str(s); return 0; }
extern "C" int32_t __rt_shadow_print_int(int32_t v) { shadow_print_int(v); return 0; }
extern "C" int32_t __rt_shadow_print_long(int64_t v) { printf("%lld", (long long)v); fflush(stdout); return 0; }
extern "C" int32_t __rt_shadow_print_float(double v) { printf("%g", v); fflush(stdout); return 0; }
extern "C" int32_t __rt_shadow_str_cmp(const char* a, const char* b) { return shadow_str_cmp(a, b); }
extern "C" const char* __rt_shadow_int_to_cstr(int32_t v) { return shadow_int_to_string(v); }
extern "C" const char* __rt_shadow_long_to_cstr(int64_t v) { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)v); return dup_str(buf); }
extern "C" const char* __rt_shadow_float_to_cstr(double v) { char buf[32]; snprintf(buf, sizeof(buf), "%g", v); return dup_str(buf); }

// ── String → Number parsing primitives ──
// 失败时调用 shadow_throw_str 设置全局异常标志并返回 0（依赖 shadow 的 try/catch 机制向上传播）。
// 严格语义：整串必须被消费（endptr 指向 '\0'）；空串或前导空白都视为非法。
extern "C" int32_t __rt_shadow_parse_int(const char* s) {
    if (!s || !s[0]) {
        shadow_throw_str("parse_int: empty string is not a valid integer");
        return 0;
    }
    char* endp = nullptr;
    errno = 0;
    long v = strtol(s, &endp, 10);
    if (endp == s || (endp && *endp != '\0')) {
        shadow_throw_str("parse_int: invalid integer format");
        return 0;
    }
    if (errno == ERANGE) {
        shadow_throw_str("parse_int: integer out of range");
        return 0;
    }
    return (int32_t)v;
}

extern "C" int64_t __rt_shadow_parse_long(const char* s) {
    if (!s || !s[0]) {
        shadow_throw_str("parse_long: empty string is not a valid integer");
        return 0;
    }
    char* endp = nullptr;
    errno = 0;
    int base = 10;
    const char* p = s;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    long long v = strtoll(p, &endp, base);
    if (endp == p || (endp && *endp != '\0')) {
        shadow_throw_str("parse_long: invalid integer format");
        return 0;
    }
    if (errno == ERANGE) {
        shadow_throw_str("parse_long: integer out of range");
        return 0;
    }
    return (int64_t)v;
}

extern "C" double __rt_shadow_parse_double(const char* s) {
    if (!s || !s[0]) {
        shadow_throw_str("parse_double: empty string is not a valid number");
        return 0.0;
    }
    char* endp = nullptr;
    errno = 0;
    double v = strtod(s, &endp);
    if (endp == s || (endp && *endp != '\0')) {
        shadow_throw_str("parse_double: invalid number format");
        return 0.0;
    }
    if (errno == ERANGE) {
        shadow_throw_str("parse_double: number out of range");
        return 0.0;
    }
    return v;
}

// parse_bool: 接受 "true"/"false"（大小写敏感，与 shadow 字面量一致）
extern "C" int32_t __rt_shadow_parse_bool(const char* s) {
    if (!s) {
        shadow_throw_str("parse_bool: null string");
        return 0;
    }
    if (strcmp(s, "true") == 0) { return 1; }
    if (strcmp(s, "false") == 0) { return 0; }
    shadow_throw_str("parse_bool: invalid bool format (expected 'true' or 'false')");
    return 0;
}
// Return the first byte of the c string, cast to int32_t.
// Used by the lexer to classify characters without shadow string comparisons.
extern "C" int32_t shadow_lexer_char_ord(const char* s) {
    if (!s || !s[0]) return -1;
    return (unsigned char)s[0];
}
extern "C" void __rt_shadow_exit(int32_t code) { shadow_exit(code); }

// ── Byte-buffer primitives for binary serialization (.lu format) ──
// shadow string is C null-terminated and cannot hold 0x00 bytes, so binary
// MIR serialization needs raw byte buffers. These primitives allocate byte
// buffers, track their lengths, and perform binary file I/O.
#include <unordered_map>
static std::unordered_map<void*, int32_t> g_buf_lens;

// Allocate a zero-initialized byte buffer of n bytes. Returns nullptr on failure.
extern "C" void* rt_buf_alloc(int32_t n) {
    if (n <= 0) return nullptr;
    void* p = malloc(n);
    if (p) {
        memset(p, 0, n);
        g_buf_lens[p] = n;
    }
    return p;
}

// Query the length of a buffer allocated by rt_buf_alloc.
// Returns 0 for unknown pointers (including nullptr).
extern "C" int32_t rt_buf_len(void* p) {
    if (!p) return 0;
    auto it = g_buf_lens.find(p);
    return it == g_buf_lens.end() ? 0 : it->second;
}

// Write n bytes from buffer p to file at path (binary mode).
// Returns 1 on success, 0 on failure.
extern "C" int32_t rt_write_bytes(const char* path, void* p, int32_t n) {
    if (!path || !p || n < 0) return 0;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    size_t written = fwrite(p, 1, n, f);
    fclose(f);
    return written == (size_t)n ? 1 : 0;
}

// Read entire file (binary mode) into a newly allocated byte buffer.
// Returns nullptr if file does not exist or is empty. Buffer length is
// queryable via rt_buf_len.
extern "C" void* rt_read_bytes(const char* path) {
    if (!path) return nullptr;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return nullptr; }
    void* buf = malloc(len);
    if (!buf) { fclose(f); return nullptr; }
    size_t read_len = fread(buf, 1, len, f);
    fclose(f);
    if (read_len != (size_t)len) { free(buf); return nullptr; }
    g_buf_lens[buf] = (int32_t)len;
    return buf;
}

// Copy string bytes into buffer at offset. Returns new offset.
// Uses strlen(s) — strings with embedded null bytes will be truncated.
extern "C" int32_t rt_str_to_bytes(const char* s, void* buf, int32_t off) {
    if (!s || !buf) return off;
    int32_t slen = (int32_t)strlen(s);
    memcpy((char*)buf + off, s, slen);
    return off + slen;
}

// Read len bytes from buffer at offset and construct a null-terminated string.
// Caller receives a malloc'd C string (ownership transferred).
extern "C" const char* rt_bytes_to_str(void* buf, int32_t off, int32_t slen) {
    if (!buf || slen <= 0) return dup_str("");
    char* s = (char*)malloc(slen + 1);
    if (!s) return dup_str("");
    memcpy(s, (char*)buf + off, slen);
    s[slen] = 0;
    return s;
}

// Return byte value of the first character of s (0-255), or -1 if empty.
extern "C" int32_t rt_char_ord(const char* s) {
    if (!s || !s[0]) return -1;
    return (unsigned char)s[0];
}

// ── .spk 打包原语（ZIP STORE 模式，无压缩） ──────────────────────────
// 实现标准 ZIP 格式但仅支持 STORE（compression_method=0），用于 shadow 发布包 .spk。
// 优点：无外部依赖、跨平台、约 250 行实现；文本源码压缩率约 30%。
// 缺点：无 deflate 压缩。后续可集成 miniz 升级为 deflate。
//
// ZIP 文件结构（小端字节序）：
//   [Local File Header 1][File Data 1]
//   [Local File Header 2][File Data 2]
//   ...
//   [Central Directory Entry 1]
//   [Central Directory Entry 2]
//   ...
//   [End of Central Directory Record]
#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

// CRC-32（IEEE 802.3，反射多项式 0xEDB88320）
static uint32_t rt_crc32(const uint8_t* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

// 写入小端 uint16/uint32 到 FILE*
static void rt_put_u16(FILE* f, uint16_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void rt_put_u32(FILE* f, uint32_t v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}
static uint16_t rt_get_u16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rt_get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 把 dir_path 目录下所有文件打包到 archive_path（.spk）
// 使用 DEFLATE 压缩（compression_method=8）；对压缩后反而变大的文件回退为 STORE
// 成功返回 1，失败返回 0
extern "C" int32_t rt_zip_pack_dir(const char* dir_path, const char* archive_path) {
    if (!dir_path || !archive_path) return 0;
    fs::path root(dir_path);
    if (!fs::exists(root) || !fs::is_directory(root)) return 0;

    FILE* out = fopen(archive_path, "wb");
    if (!out) return 0;

    struct Entry {
        std::string rel_path;    // ZIP 内部路径（用 /）
        std::string abs_path;     // 磁盘绝对路径
        uint32_t crc;
        uint32_t comp_size;       // 压缩后大小
        uint32_t uncomp_size;     // 压缩前大小
        uint16_t method;          // 0=STORE, 8=DEFLATE
        uint32_t local_offset;
    };
    std::vector<Entry> entries;

    // 收集所有文件（按字典序稳定输出）
    std::vector<fs::path> files;
    for (auto& it : fs::recursive_directory_iterator(root)) {
        if (it.is_regular_file()) files.push_back(it.path());
    }
    std::sort(files.begin(), files.end());

    // 写入每个文件的 Local Header + Data
    for (auto& fp : files) {
        Entry e;
        e.abs_path = fp.string();
        e.rel_path = fs::relative(fp, root).generic_string();
        // 跨平台统一用 /
        for (auto& c : e.rel_path) if (c == '\\') c = '/';

        // 读取文件内容
        FILE* in = fopen(e.abs_path.c_str(), "rb");
        if (!in) continue;
        fseek(in, 0, SEEK_END);
        long sz = ftell(in);
        fseek(in, 0, SEEK_SET);
        std::vector<uint8_t> buf(sz);
        size_t rd = fread(buf.data(), 1, sz, in);
        fclose(in);
        if ((long)rd != sz) continue;

        e.crc = rt_crc32(buf.data(), sz);
        e.uncomp_size = (uint32_t)sz;
        e.local_offset = (uint32_t)ftell(out);

        // DEFLATE 压缩：mz_compressBound 给出最坏情况输出缓冲区大小
        mz_ulong bound = mz_compressBound((mz_ulong)sz);
        std::vector<uint8_t> comp(bound);
        mz_ulong comp_len = bound;
        int rc = mz_compress(comp.data(), &comp_len, buf.data(), (mz_ulong)sz);
        // 压缩失败或压缩后更大 → 回退 STORE
        if (rc != MZ_OK || comp_len >= (mz_ulong)sz) {
            e.method = 0;  // STORE
            e.comp_size = (uint32_t)sz;
        } else {
            e.method = 8;  // DEFLATE
            e.comp_size = (uint32_t)comp_len;
        }

        // Local File Header (PK\03\04)
        rt_put_u32(out, 0x04034b50);
        rt_put_u16(out, 20);              // version needed
        rt_put_u16(out, 0);               // flags
        rt_put_u16(out, e.method);        // compression: 0=STORE / 8=DEFLATE
        rt_put_u16(out, 0);                // mod time
        rt_put_u16(out, 0);                // mod date
        rt_put_u32(out, e.crc);
        rt_put_u32(out, e.comp_size);      // compressed size
        rt_put_u32(out, e.uncomp_size);
        rt_put_u16(out, (uint16_t)e.rel_path.size());
        rt_put_u16(out, 0);               // extra length
        fwrite(e.rel_path.data(), 1, e.rel_path.size(), out);
        if (e.method == 8) {
            fwrite(comp.data(), 1, comp_len, out);
        } else {
            fwrite(buf.data(), 1, sz, out);
        }

        entries.push_back(std::move(e));
    }

    // Central Directory
    uint32_t cd_start = (uint32_t)ftell(out);
    for (auto& e : entries) {
        rt_put_u32(out, 0x02014b50);      // central dir signature
        rt_put_u16(out, 20);              // version made by
        rt_put_u16(out, 20);              // version needed
        rt_put_u16(out, 0);               // flags
        rt_put_u16(out, e.method);        // compression: 0=STORE / 8=DEFLATE
        rt_put_u16(out, 0); rt_put_u16(out, 0);  // time/date
        rt_put_u32(out, e.crc);
        rt_put_u32(out, e.comp_size);
        rt_put_u32(out, e.uncomp_size);
        rt_put_u16(out, (uint16_t)e.rel_path.size());
        rt_put_u16(out, 0);               // extra
        rt_put_u16(out, 0);               // comment
        rt_put_u16(out, 0);               // disk number start
        rt_put_u16(out, 0);               // internal attrs
        rt_put_u32(out, 0);               // external attrs
        rt_put_u32(out, e.local_offset);
        fwrite(e.rel_path.data(), 1, e.rel_path.size(), out);
    }
    uint32_t cd_size = (uint32_t)ftell(out) - cd_start;

    // End of Central Directory Record
    rt_put_u32(out, 0x06054b50);
    rt_put_u16(out, 0);                   // disk number
    rt_put_u16(out, 0);                   // disk with CD
    rt_put_u16(out, (uint16_t)entries.size());
    rt_put_u16(out, (uint16_t)entries.size());
    rt_put_u32(out, cd_size);
    rt_put_u32(out, cd_start);
    rt_put_u16(out, 0);                   // comment length

    fclose(out);
    return 1;
}

// 解压 archive_path 到 dest_dir
// 成功返回 1，失败返回 0
extern "C" int32_t rt_zip_extract_to_dir(const char* archive_path, const char* dest_dir) {
    if (!archive_path || !dest_dir) return 0;
    FILE* in = fopen(archive_path, "rb");
    if (!in) return 0;

    // 定位 End of Central Directory Record
    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    if (file_size < 22) { fclose(in); return 0; }

    // EOCD 在文件末尾，最多前 64KB 内查找（含 comment）
    long scan_start = (file_size > 65557) ? (file_size - 65557) : 0;
    fseek(in, scan_start, SEEK_SET);
    std::vector<uint8_t> tail(file_size - scan_start);
    size_t rd = fread(tail.data(), 1, tail.size(), in);
    if (rd != tail.size()) { fclose(in); return 0; }

    long eocd_off = -1;
    for (long i = (long)tail.size() - 22; i >= 0; i--) {
        if (rt_get_u32(tail.data() + i) == 0x06054b50) {
            eocd_off = scan_start + i;
            break;
        }
    }
    if (eocd_off < 0) { fclose(in); return 0; }

    fseek(in, eocd_off, SEEK_SET);
    uint8_t eocd[22];
    if (fread(eocd, 1, 22, in) != 22) { fclose(in); return 0; }
    uint16_t num_entries = rt_get_u16(eocd + 10);
    uint32_t cd_size = rt_get_u32(eocd + 12);
    uint32_t cd_off = rt_get_u32(eocd + 16);

    // 创建目标目录
    std::error_code ec;
    fs::create_directories(dest_dir, ec);

    // 读取 Central Directory
    fseek(in, cd_off, SEEK_SET);
    std::vector<uint8_t> cd(cd_size);
    if (fread(cd.data(), 1, cd_size, in) != cd_size) { fclose(in); return 0; }

    size_t p = 0;
    for (uint16_t i = 0; i < num_entries; i++) {
        if (p + 46 > cd.size()) break;
        if (rt_get_u32(cd.data() + p) != 0x02014b50) break;
        uint16_t method = rt_get_u16(cd.data() + p + 10);   // 0=STORE, 8=DEFLATE
        uint32_t crc = rt_get_u32(cd.data() + p + 16);
        uint32_t comp_size = rt_get_u32(cd.data() + p + 20);
        uint32_t uncomp_size = rt_get_u32(cd.data() + p + 24);
        uint16_t name_len = rt_get_u16(cd.data() + p + 28);
        uint16_t extra_len = rt_get_u16(cd.data() + p + 30);
        uint16_t comment_len = rt_get_u16(cd.data() + p + 32);
        uint32_t local_off = rt_get_u32(cd.data() + p + 42);
        if (p + 46 + name_len + extra_len + comment_len > cd.size()) break;
        std::string name((const char*)cd.data() + p + 46, name_len);
        p += 46 + name_len + extra_len + comment_len;

        // 跳过目录 entry（以 / 结尾）
        if (!name.empty() && name.back() == '/') continue;

        // 读取 Local File Header
        fseek(in, local_off, SEEK_SET);
        uint8_t lfh[30];
        if (fread(lfh, 1, 30, in) != 30) continue;
        if (rt_get_u32(lfh) != 0x04034b50) continue;
        uint16_t l_name_len = rt_get_u16(lfh + 26);
        uint16_t l_extra_len = rt_get_u16(lfh + 28);
        fseek(in, l_name_len + l_extra_len, SEEK_CUR);

        // 读取压缩数据
        std::vector<uint8_t> comp_buf(comp_size);
        if (comp_size > 0 && fread(comp_buf.data(), 1, comp_size, in) != comp_size) continue;

        // 根据压缩方法处理
        std::vector<uint8_t> data;
        if (method == 0) {
            // STORE：直接使用 comp_buf
            data = std::move(comp_buf);
        } else if (method == 8) {
            // DEFLATE：用 mz_uncompress 解压
            data.resize(uncomp_size);
            mz_ulong out_len = uncomp_size;
            int rc = mz_uncompress(data.data(), &out_len, comp_buf.data(), comp_size);
            if (rc != MZ_OK || out_len != uncomp_size) continue;
        } else {
            // 不支持的压缩方法
            continue;
        }

        // CRC 校验
        if (rt_crc32(data.data(), data.size()) != crc) continue;

        // 写入目标文件（创建父目录）
        fs::path out_path = fs::path(dest_dir) / name;
        fs::create_directories(out_path.parent_path(), ec);
        FILE* of = fopen(out_path.string().c_str(), "wb");
        if (!of) continue;
        if (!data.empty()) fwrite(data.data(), 1, data.size(), of);
        fclose(of);
    }

    fclose(in);
    return 1;
}

// 列出 .spk/ZIP 包中的文件名（换行分隔的字符串）
// 失败返回空字符串。用于 shadow 层查询包内容
extern "C" const char* rt_zip_list_files(const char* archive_path) {
    if (!archive_path) return dup_str("");
    FILE* in = fopen(archive_path, "rb");
    if (!in) return dup_str("");

    std::string result;
    fseek(in, 0, SEEK_END);
    long file_size = ftell(in);
    if (file_size < 22) { fclose(in); return dup_str(""); }

    long scan_start = (file_size > 65557) ? (file_size - 65557) : 0;
    fseek(in, scan_start, SEEK_SET);
    std::vector<uint8_t> tail(file_size - scan_start);
    if (fread(tail.data(), 1, tail.size(), in) != tail.size()) { fclose(in); return dup_str(""); }

    long eocd_off = -1;
    for (long i = (long)tail.size() - 22; i >= 0; i--) {
        if (rt_get_u32(tail.data() + i) == 0x06054b50) { eocd_off = scan_start + i; break; }
    }
    if (eocd_off < 0) { fclose(in); return dup_str(""); }

    fseek(in, eocd_off, SEEK_SET);
    uint8_t eocd[22];
    if (fread(eocd, 1, 22, in) != 22) { fclose(in); return dup_str(""); }
    uint16_t num_entries = rt_get_u16(eocd + 10);
    uint32_t cd_size = rt_get_u32(eocd + 12);
    uint32_t cd_off = rt_get_u32(eocd + 16);

    fseek(in, cd_off, SEEK_SET);
    std::vector<uint8_t> cd(cd_size);
    if (fread(cd.data(), 1, cd_size, in) != cd_size) { fclose(in); return dup_str(""); }

    size_t p = 0;
    for (uint16_t i = 0; i < num_entries; i++) {
        if (p + 46 > cd.size()) break;
        if (rt_get_u32(cd.data() + p) != 0x02014b50) break;
        uint16_t name_len = rt_get_u16(cd.data() + p + 28);
        uint16_t extra_len = rt_get_u16(cd.data() + p + 30);
        uint16_t comment_len = rt_get_u16(cd.data() + p + 32);
        if (p + 46 + name_len > cd.size()) break;
        std::string name((const char*)cd.data() + p + 46, name_len);
        if (!name.empty() && name.back() != '/') {
            if (!result.empty()) result += "\n";
            result += name;
        }
        p += 46 + name_len + extra_len + comment_len;
    }
    fclose(in);
    return dup_str(result);
}

// ── SPK 包管理符号（shadow 层 @extern 名称）──────────────────────────
// Linux/POSIX 运行时只用 std::filesystem 实现（rt_zip_*），但 shadow 编译器
// 通过 shadow_zip_unpack / shadow_zip_pack / shadow_zip_list / shadow_content_hash
// 这几个名字调用。Windows 版 rt/rt_zip.c 无法在 Linux 编译，故在此补齐薄包装：
// 先归一化路径分隔符（main.shadow 用 '\' 拼接），再转发到 POSIX 实现。
// 返回语义必须对齐 Windows 版：成功 0，失败 -1（main_spk_ensure 按 ok != 0 判失败；
// POSIX rt_zip_extract_to_dir/pack_dir 是「成功 1 失败 0」，故在此翻转）。
extern "C" int32_t shadow_zip_unpack(const char* spk, const char* out_dir) {
    std::string a = shadow_path_to_slashes(spk ? std::string(spk) : std::string(""));
    std::string b = shadow_path_to_slashes(out_dir ? std::string(out_dir) : std::string(""));
    int32_t rc = rt_zip_extract_to_dir(a.c_str(), b.c_str());
    return rc == 1 ? 0 : -1;
}
extern "C" int32_t shadow_zip_pack(const char* src_dir, const char* out_spk) {
    std::string a = shadow_path_to_slashes(src_dir ? std::string(src_dir) : std::string(""));
    std::string b = shadow_path_to_slashes(out_spk ? std::string(out_spk) : std::string(""));
    int32_t rc = rt_zip_pack_dir(a.c_str(), b.c_str());
    return rc == 1 ? 0 : -1;
}
extern "C" const char* shadow_zip_list(const char* spk) {
    std::string a = shadow_path_to_slashes(spk ? std::string(spk) : std::string(""));
    return rt_zip_list_files(a.c_str());
}
// FNV-1a 64 文件内容哈希（hex 16 字符），与 Windows rt/rt_zip.c 的 shadow_content_hash 等价。
extern "C" const char* shadow_content_hash(const char* path) {
    std::string p = shadow_path_to_slashes(path ? std::string(path) : std::string(""));
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return NULL;
    unsigned char buf[65536];
    size_t rd;
    uint64_t hsh = 0xcbf29ce484222325ULL;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t j = 0; j < rd; j++) { hsh ^= (uint64_t)buf[j]; hsh *= 0x100000001b3ULL; }
    }
    fclose(f);
    char* hex = (char*)malloc(17);
    if (!hex) return NULL;
    for (int i = 0; i < 8; i++) {
        sprintf(hex + i * 2, "%02llx", (unsigned long long)((hsh >> (56 - i * 8)) & 0xff));
    }
    hex[16] = '\0';
    return hex;
}

// Character classification helper used by the lexer.
// Takes an `any` box (pointer to {tag: i32, value: ptr})
// and checks if the boxed string is a valid identifier start char.
extern "C" int32_t shadow_lexer_is_id_char(void* box) {
    if (!box) return 0;
    char* ptr = *(char**)((char*)box + 4);  // any.value at offset 4
    if (!ptr) return 0;
    char c = ptr[0];
    if (c == '_') return 1;
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return 0;
}

// ── 时间类型运行时原语（shadow-0.3 新增） ──────────────────────────
// 对应 shadow 类型：date (i32, 天精度), timestamp (i64, 毫秒), nanotimestamp (i64, 纳秒)
//
// 编码约定（与 shadow 字面量 parse_time_digits 一致：数字位按序拼接）：
//   rt_date_now()         → int32  YYYYMMDD        （如 20260725）
//   rt_timestamp_now()     → int64  YYYYMMDDHHMMSSmmm（注意：0.2 long 是 32 位会截断）
//   rt_nanotimestamp_now() → int64  纳秒级精度（使用 chrono::steady_clock 自纪元起的纳秒数）
//
// 设计权衡：
//   - date 用 YYYYMMDD 整数编码便于比较和打印（与 date"YYYY.MM.DD" 字面量一致）
//   - timestamp 编码为 YYYYMMDDHHMMSSmmm，但 14+ 位数字超出 int32 范围，
//     0.2 编译器下 long 是 32 位会截断，所以 0.3 仅在 0.4 修复 long 后才有意义。
//     为保持 API 一致性，这里仍返回完整 i64 值，由调用方决定如何使用。
//   - nanotimestamp 使用 steady_clock 纳秒计数（不是墙钟），避免时区/闰秒复杂性；
//     字面量 nanotimestamp"..." 也是 parse_time_digits 得到的数字串。
//
// 格式化函数：
//   rt_date_to_str(d)          → "YYYY.MM.DD"
//   rt_timestamp_to_str(t)      → "YYYY.MM.DD HH:MM:SS.mmm"
//   rt_nanotimestamp_to_str(n)  → 数字字符串（纳秒计数）
extern "C" int32_t rt_date_now() {
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return (int32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
}

extern "C" int64_t rt_timestamp_now() {
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    // YYYYMMDDHHMMSS * 1000 + milliseconds
    // 注意：14 位数字 * 1000 + mmm = 17 位数字，超出 int64 正常可表示范围（~19 位），
    // 但远超 int32 范围。0.2 编译器 long 是 32 位会截断高位，所以 0.3 仅作类型检查验证。
    int64_t base = (int64_t)(lt.tm_year + 1900) * 10000000000LL
                 + (int64_t)(lt.tm_mon + 1) * 100000000LL
                 + (int64_t)lt.tm_mday * 1000000LL
                 + (int64_t)lt.tm_hour * 10000LL
                 + (int64_t)lt.tm_min * 100LL
                 + (int64_t)lt.tm_sec;
    // 毫秒部分（Windows 用 GetSystemTime，其他平台用 gettimeofday）
    int32_t ms = 0;
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    ms = st.wMilliseconds;
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    ms = (int32_t)(tv.tv_usec / 1000);
#endif
    return base * 1000 + ms;
}

extern "C" int64_t rt_nanotimestamp_now() {
#ifdef _WIN32
    // Windows: 使用 QueryPerformanceCounter 转换为纳秒
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    // 转换为纳秒：counter * 1e9 / freq
    return (int64_t)((int64_t)counter.QuadPart * 1000000000LL / freq.QuadPart);
#else
    // POSIX: 使用 clock_gettime(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

// 格式化函数：将时间整数编码转换为可读字符串
// rt_date_to_str(d) → "YYYY.MM.DD"
extern "C" const char* rt_date_to_str(int32_t d) {
    char buf[16];
    int32_t year = d / 10000;
    int32_t md = d % 10000;
    int32_t month = md / 100;
    int32_t day = md % 100;
    snprintf(buf, sizeof(buf), "%04d.%02d.%02d", year, month, day);
    return dup_str(buf);
}

// rt_timestamp_to_str(t) → "YYYY.MM.DD HH:MM:SS.mmm"
// 注意：t 的编码是 YYYYMMDDHHMMSSmmm（17 位数字），需正确解码
extern "C" const char* rt_timestamp_to_str(int64_t t) {
    char buf[32];
    // 从 17 位整数解码（但 t 可能被 0.2 long 截断，这里尽力解码）
    int64_t ms_part = t % 1000;
    int64_t sec_part = (t / 1000) % 100;
    int64_t min_part = (t / 100000) % 100;
    int64_t hour_part = (t / 10000000) % 100;
    int64_t day_part = (t / 1000000000LL) % 100;
    int64_t month_part = (t / 100000000000LL) % 100;
    int64_t year_part = t / 10000000000000LL;
    snprintf(buf, sizeof(buf), "%04lld.%02lld.%02lld %02lld:%02lld:%02lld.%03lld",
             (long long)year_part, (long long)month_part, (long long)day_part,
             (long long)hour_part, (long long)min_part, (long long)sec_part,
             (long long)ms_part);
    return dup_str(buf);
}

// rt_nanotimestamp_to_str(n) → 纳秒计数的数字字符串
extern "C" const char* rt_nanotimestamp_to_str(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    return dup_str(buf);
}

// ───────────────────────────────────────────────────────────────
// 并发运行时：真并行（C++ std::thread）
// ───────────────────────────────────────────────────────────────
// spawn 立即在独立 OS 线程运行协程体（真并行）；await 用 condition_variable 等待。
// Future<T> 运行时为不透明句柄 ShadowFuture*（i8*），值以 void* (any) 承载。
// 协程体函数签名：
//   - nullary : void* fn()            —— spawn <funcname> 的情况
//   - thunk   : void* fn(void* arg)   —— spawn(closure) 的情况（codegen 生成 thunk）
struct ShadowFuture {
    std::mutex              mtx;        // 保护 ready/value 与 cv 等待
    std::condition_variable cv;         // await 等待 Future 就绪
    int32_t                 ready;      // 0=pending, 1=ready
    void*                   value;      // 结果值（any/ptr）
};

struct ShadowTask {
    void*   (*fn_nullary)();
    void*   (*fn_thunk)(void*);
    void*   arg;
    ShadowFuture* fut;
    void*   result;
    std::thread thread;                 // 承载协程体的 OS 线程
};

static std::mutex              g_tasks_mtx;
static std::vector<ShadowTask*> g_tasks;   // 所有 spawned 任务，sched_run join

// 协程体线程入口：运行函数体，存结果，标记 Future 就绪并通知等待者。
static void task_run(ShadowTask* t) {
    // 注册本线程的 GC 根集合，使其在 mark 阶段可见（并发分配安全）。
    gc_get_thread_state();
    void* result;
    if (t->fn_nullary) result = t->fn_nullary();
    else result = t->fn_thunk(t->arg);
    t->result = result;
    {
        std::lock_guard<std::mutex> lk(t->fut->mtx);
        t->fut->value = result;
        t->fut->ready = 1;
    }
    // 结果作为 GC 永久根，防止并发 GC 在 await 前回收。
    gc_perm_root_add(result);
    t->fut->cv.notify_all();
}

extern "C" void shadow_sched_init() {
    // std::thread 模型无需初始化调度器（spawn 直接起线程）。
}

// spawn 一个 nullary 函数（kimo fn() -> any）—— codegen 解析函数名得到 LLVM fn ptr
extern "C" void* shadow_spawn_nullary(void* (*fn)()) {
    ShadowFuture* fut = new ShadowFuture();
    fut->ready = 0; fut->value = nullptr;
    ShadowTask* t = new ShadowTask();
    t->fn_nullary = fn; t->fn_thunk = nullptr; t->arg = nullptr;
    t->fut = fut; t->result = nullptr; t->thread = std::thread(task_run, t);
    {
        std::lock_guard<std::mutex> lk(g_tasks_mtx);
        g_tasks.push_back(t);
    }
    return fut;
}

// spawn 一个 thunk(closure_ptr) —— codegen 生成 thunk 做间接调用
extern "C" void* shadow_spawn_thunk(void* (*fn)(void*), void* arg) {
    ShadowFuture* fut = new ShadowFuture();
    fut->ready = 0; fut->value = nullptr;
    ShadowTask* t = new ShadowTask();
    t->fn_nullary = nullptr; t->fn_thunk = fn; t->arg = arg;
    t->fut = fut; t->result = nullptr; t->thread = std::thread(task_run, t);
    {
        std::lock_guard<std::mutex> lk(g_tasks_mtx);
        g_tasks.push_back(t);
    }
    return fut;
}

// await 一个 Future —— 阻塞等待直至就绪，返回其值（void* = any）
extern "C" void* shadow_future_await(void* fut_h) {
    if (!fut_h) return nullptr;
    ShadowFuture* fut = (ShadowFuture*)fut_h;
    ThreadGCState* s = gc_get_thread_state();
    std::unique_lock<std::mutex> lk(fut->mtx);
    // 阻塞等待期间标记 blocked（stw_state=2）：不跑 mutator，根集冻结。
    // 否则 collect 的协作式 STW 会永远等一个阻塞在 cv.wait 的线程
    // （它到不了 poll 安全点）→ 死锁。对齐 Windows「阻塞线程不被等待」。
    s->stw_state.store(2, std::memory_order_release);
    fut->cv.wait(lk, [fut]() { return fut->ready == 1; });
    s->stw_state.store(0, std::memory_order_release);
    return fut->value;
}

// 排空所有 spawned 线程（fire-and-forget spawn 收尾；atexit 兜底亦调用）。
// join 保证线程在进程退出前完成，避免 std::thread 析构时仍 joinable 而 terminate。
extern "C" void shadow_sched_run() {
    std::vector<ShadowTask*> local;
    {
        std::lock_guard<std::mutex> lk(g_tasks_mtx);
        local.swap(g_tasks);
    }
    ThreadGCState* s = gc_get_thread_state();
    // join 阻塞期间标记 blocked（不跑 mutator），防协作式 STW 死锁。
    s->stw_state.store(2, std::memory_order_release);
    for (ShadowTask* t : local) {
        if (t->thread.joinable()) t->thread.join();
        gc_perm_root_remove(t->fut ? t->fut->value : nullptr);
        if (t->fut) delete t->fut;
        delete t;
    }
    s->stw_state.store(0, std::memory_order_release);
}

// 手动构造/完成 Future（用于 async 函数返回 Future）
extern "C" void* shadow_future_new() {
    return new ShadowFuture();
}
extern "C" void shadow_future_complete(void* fut_h, void* value) {
    if (!fut_h) return;
    ShadowFuture* fut = (ShadowFuture*)fut_h;
    {
        std::lock_guard<std::mutex> lk(fut->mtx);
        fut->value = value;
        fut->ready = 1;
    }
    gc_perm_root_add(value);
    fut->cv.notify_all();
}

// 进程退出兜底：join 所有仍在运行的 spawned 线程，避免 std::terminate。
// 注：本机 glibc(2.39) 未导出 atexit 动态符号，仅导出 __cxa_atexit，故改用它注册。
extern "C" int __cxa_atexit(void (*func)(void*), void* arg, void* dso_handle);
extern "C" void* __dso_handle;
static int _sched_atexit_reg = (__cxa_atexit((void(*)(void*))shadow_sched_run, nullptr, __dso_handle), 0);

// ============================================================
// P11-10: 原生文件系统监听 (Windows ReadDirectoryChangesW)
// ============================================================
// 在 C++ 运行时启动后台线程监控目录变更，Shadow 层通过 poll 接口
// 获取累积的变更事件。避免 shadow-lang 层的 mtime 轮询延迟。
//
// API:
//   shadow_fs_watch_start(path) -> int  (1=成功, 0=失败)
//   shadow_fs_watch_poll() -> string   (JSON: [{"path":"...","event":1},...])
//   shadow_fs_watch_stop() -> void     (停止所有监控)

#ifdef _WIN32
struct FsWatchEntry {
    HANDLE hDir;
    HANDLE hEvent;
    OVERLAPPED overlapped;
    char buffer[4096];
    std::string dirPath;
    std::thread thread;
    std::atomic<bool> running;
};

struct FsWatchManager {
    std::vector<FsWatchEntry*> entries;
    std::mutex queueMtx;
    std::vector<std::pair<std::string, int>> changeQueue; // (path, event_type)
    std::atomic<bool> stopAll{false};

    static const int EVENT_CREATED = 1;
    static const int EVENT_CHANGED = 2;
    static const int EVENT_DELETED = 3;
    static const int EVENT_RENAMED = 4;
};

static FsWatchManager* g_fsWatchMgr = nullptr;
static std::mutex g_fsWatchInitMtx;

static FsWatchManager* getFsWatchMgr() {
    std::lock_guard<std::mutex> lk(g_fsWatchInitMtx);
    if (!g_fsWatchMgr) {
        g_fsWatchMgr = new FsWatchManager();
    }
    return g_fsWatchMgr;
}

static void fsWatchThreadFunc(FsWatchEntry* entry) {
    while (entry->running.load() && !g_fsWatchMgr->stopAll.load()) {
        DWORD bytesReturned = 0;
        BOOL ok = ReadDirectoryChangesW(
            entry->hDir,
            entry->buffer,
            sizeof(entry->buffer),
            TRUE, // recursive
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
            &bytesReturned,
            &entry->overlapped,
            NULL
        );
        if (!ok) break;

        // Wait for completion with timeout
        DWORD waitResult = WaitForSingleObject(entry->hEvent, 500);
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult != WAIT_OBJECT_0) break;

        if (!GetOverlappedResult(entry->hDir, &entry->overlapped, &bytesReturned, FALSE)) {
            break;
        }
        if (bytesReturned == 0) continue;

        // Parse FILE_NOTIFY_INFORMATION records
        BYTE* ptr = (BYTE*)entry->buffer;
        DWORD offset = 0;
        while (offset < bytesReturned) {
            FILE_NOTIFY_INFORMATION* fni = (FILE_NOTIFY_INFORMATION*)(ptr + offset);
            // Convert wchar filename to UTF-8
            int wlen = fni->FileNameLength / sizeof(WCHAR);
            std::wstring wpath(fni->FileName, wlen);
            std::string utf8Path(entry->dirPath);
            if (!utf8Path.empty() && utf8Path.back() != '/' && utf8Path.back() != '\\') {
                utf8Path += "\\";
            }
            // WideCharToMultiByte
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), wlen, NULL, 0, NULL, NULL);
            if (utf8Len > 0) {
                std::string fileName(utf8Len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), wlen, &fileName[0], utf8Len, NULL, NULL);
                utf8Path += fileName;
            }

            int eventType = FsWatchManager::EVENT_CHANGED;
            switch (fni->Action) {
                case FILE_ACTION_ADDED: eventType = FsWatchManager::EVENT_CREATED; break;
                case FILE_ACTION_MODIFIED: eventType = FsWatchManager::EVENT_CHANGED; break;
                case FILE_ACTION_REMOVED: eventType = FsWatchManager::EVENT_DELETED; break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                case FILE_ACTION_RENAMED_NEW_NAME: eventType = FsWatchManager::EVENT_RENAMED; break;
            }

            {
                std::lock_guard<std::mutex> lk(g_fsWatchMgr->queueMtx);
                g_fsWatchMgr->changeQueue.push_back({utf8Path, eventType});
            }

            if (fni->NextEntryOffset == 0) break;
            offset += fni->NextEntryOffset;
        }
    }
}

extern "C" int shadow_fs_watch_start(const char* path) {
    FsWatchManager* mgr = getFsWatchMgr();

    // Check if already watching this path
    for (auto* e : mgr->entries) {
        if (e->dirPath == path) return 1;
    }

    FsWatchEntry* entry = new FsWatchEntry();
    entry->dirPath = path;
    entry->running.store(true);

    entry->hDir = CreateFileA(
        path,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL
    );
    if (entry->hDir == INVALID_HANDLE_VALUE) {
        delete entry;
        return 0;
    }

    entry->hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!entry->hEvent) {
        CloseHandle(entry->hDir);
        delete entry;
        return 0;
    }

    memset(&entry->overlapped, 0, sizeof(OVERLAPPED));
    entry->overlapped.hEvent = entry->hEvent;

    entry->thread = std::thread(fsWatchThreadFunc, entry);
    mgr->entries.push_back(entry);
    return 1;
}

extern "C" const char* shadow_fs_watch_poll() {
    FsWatchManager* mgr = getFsWatchMgr();

    std::lock_guard<std::mutex> lk(mgr->queueMtx);
    // 线程安全：返回 malloc 拷贝（旧实现返回 static std::string 的 c_str()，
    // 多线程下一次 poll 覆盖上一次的缓冲 → 内容错乱）。
    std::string result;
    if (mgr->changeQueue.empty()) {
        result = "[]";
        return dup_str(result);
    }

    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (auto& [path, event] : mgr->changeQueue) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"path\":\"";
        // Escape backslashes and quotes for JSON
        for (char c : path) {
            if (c == '\\') ss << "\\\\";
            else if (c == '"') ss << "\\\"";
            else ss << c;
        }
        ss << "\",\"event\":" << event << "}";
    }
    ss << "]";
    mgr->changeQueue.clear();
    result = ss.str();
    return dup_str(result);
}

extern "C" void shadow_fs_watch_stop() {
    FsWatchManager* mgr = getFsWatchMgr();
    mgr->stopAll.store(true);

    for (auto* entry : mgr->entries) {
        entry->running.store(false);
        if (entry->hEvent) SetEvent(entry->hEvent);
        if (entry->thread.joinable()) entry->thread.join();
        if (entry->hDir) CloseHandle(entry->hDir);
        if (entry->hEvent) CloseHandle(entry->hEvent);
        delete entry;
    }
    mgr->entries.clear();
    mgr->changeQueue.clear();
    mgr->stopAll.store(false);
}

#else
// Non-Windows stubs
extern "C" int shadow_fs_watch_start(const char* path) { return 0; }
extern "C" const char* shadow_fs_watch_poll() { return "[]"; }
extern "C" void shadow_fs_watch_stop() {}
#endif

// ============================================================
// P12-2: 互斥锁原语 (用于 LSP 多线程)
// ============================================================
// shadow-0.3 运行时已支持 std::thread (async/spawn)，此处补充 mutex
// 供 LSP 服务器的编译互斥和写入互斥使用。

extern "C" void* shadow_mutex_new() {
    return new std::mutex();
}

extern "C" void shadow_mutex_lock(void* mtx) {
    if (!mtx) return;
    ((std::mutex*)mtx)->lock();
}

extern "C" void shadow_mutex_unlock(void* mtx) {
    if (!mtx) return;
    ((std::mutex*)mtx)->unlock();
}

extern "C" void shadow_mutex_destroy(void* mtx) {
    if (!mtx) return;
    delete (std::mutex*)mtx;
}

// ============================================================
// P12-3: LSP 异步请求处理 (多线程)
// ============================================================
// shadow_spawn_thunk 在本文件前部定义 (~line 4322)，创建 std::thread 并
// 通过 thread_local GCTLSGuard 自动注册 GC 线程状态。此处利用它为 LSP 实现
// fire-and-forget 请求处理：主线程读取消息后，将请求 body 传递给独立线程
// 分发和写入响应。
//
// 为什么不直接用 Shadow 的 spawn 关键字？
// 0.3 源码由 0.2 编译器 (shadow-lang.exe) 编译，0.2 词法器不识别 'spawn'
// 关键字 (TOK_SPAWN 是 0.3 新增)。因此只能通过 C++ 运行时直接调用
// shadow_spawn_thunk 来实现多线程。

// P12-3: LSP 异步请求处理 — 函数指针注册模式
// 问题：直接 extern 引用 __mod_lsp_lsp_lsp_dispatch 等符号会导致用户程序
// （用 --exe 编译）链接失败，因为用户程序不包含 LSP 代码。
// 解决：使用函数指针注册模式。s03.exe 启动时调用 shadow_lsp_register_hooks
// 注册 dispatch/write 回调；用户程序不注册，shadow_lsp_spawn_request 为 no-op。
static void* (*g_lsp_dispatch_hook)(void*) = nullptr;
static int  (*g_lsp_write_hook)(const char*) = nullptr;

extern "C" void shadow_lsp_register_hooks(void* (*dispatch_fn)(void*),
                                           int (*write_fn)(const char*)) {
    g_lsp_dispatch_hook = dispatch_fn;
    g_lsp_write_hook = write_fn;
}

// 异步请求处理包装器 — shadow_spawn_thunk 回调签名: void* (*)(void*)
static void* lsp_async_wrapper(void* arg) {
    char* body = (char*)arg;
    if (g_lsp_dispatch_hook) {
        void* resp = g_lsp_dispatch_hook(body);
        free(body);
        if (resp && g_lsp_write_hook) {
            g_lsp_write_hook((const char*)resp);
        }
    } else {
        free(body);
    }
    return nullptr;
}

// 在独立线程中处理 LSP 请求 (fire-and-forget)
// 未注册 hooks 时为 no-op（用户程序编译时不会触发此路径）
extern "C" void shadow_lsp_spawn_request(const char* body) {
    if (!body || !g_lsp_dispatch_hook) return;
    char* body_copy = strdup(body);
    if (!body_copy) return;
    shadow_spawn_thunk(lsp_async_wrapper, body_copy);
}


