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
#include <cctype>
#include <sstream>
#include <atomic>
#include <cstdarg>
#include <variant>
#include <sys/stat.h>

// Forward declarations: ShadowDict/ShadowArray 完整定义在下方（line ~617），
// 此处提前声明以供 shadow_hashmap_*（line ~412）使用 —— hashmap_* 已与 dict 统一表示。
// 由于 hashmap_* 需要访问成员（new ShadowDict、m->data 等），
// 必须把完整 struct 定义移到此处（不能仅 forward declare）。
using DictValue = std::variant<int64_t, double, std::string, bool, void*>;
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

struct ShadowDict {
    uint32_t type_tag; // 0=array, 1=dict
    std::unordered_map<std::string, DictValue> data;
    ShadowDict() : type_tag(1) {}
};

struct ShadowArray {
    uint32_t type_tag; // 0=array, 1=dict, 2=set
    std::vector<DictValue> data;
    ShadowArray() : type_tag(0) {}
};

struct ShadowSet {
    uint32_t type_tag; // 0=array, 1=dict, 2=set
    std::unordered_set<std::string> data;
    ShadowSet() : type_tag(2) {}
};

struct ShadowArena {
    uint32_t type_tag; // 3=arena
    size_t block_size;
    std::vector<void*> blocks;
    std::vector<size_t> caps;
    std::vector<size_t> used;
    size_t total_used;
    size_t high_water;
    ShadowArena(size_t bs) : type_tag(3), block_size(bs), total_used(0), high_water(0) {}
};

// miniz: 用于 .spk 包的 DEFLATE 压缩（miniz.h 是 self-contained，会自动 include 它需要的一切）
// 必须在 src 中只 #include 一次 miniz.h；miniz.c 在 build_s03.bat 中单独编译并链接
#include "miniz-3.1.2/miniz.h"

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
    m->data[key] = DictValue(std::string(value ? value : ""));
}

// dict<K, int/long/bool/date/timestamp> 的 hashmap_insert 变体。
// codegen 按第 3 个实参的类型分派到此函数；否则 int 会被当作 const char*
// 解引用（崩溃）。值以 int64_t 变体存放，与 shadow_dict_set/get_int 共享布局。
extern "C" void shadow_hashmap_insert_int(void* map, const char* key, int64_t value) {
    if (!map || !key) return;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    m->data[key] = DictValue(value);
}

extern "C" const char* shadow_hashmap_get(void* map, const char* key) {
    if (!map || !key) return dup_str("");
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    auto it = m->data.find(key);
    if (it == m->data.end()) return dup_str("");
    // 仅当值为 string 变体时返回；非 string 变体返回其字符串表示（与原行为一致）
    if (std::holds_alternative<std::string>(it->second)) {
        return dup_str(std::get<std::string>(it->second).c_str());
    }
    return dup_str(value_to_string(it->second).c_str());
}

extern "C" int shadow_hashmap_contains(void* map, const char* key) {
    if (!map || !key) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return m->data.find(key) != m->data.end() ? 1 : 0;
}

extern "C" int shadow_hashmap_remove(void* map, const char* key) {
    if (!map || !key) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return m->data.erase(key) > 0 ? 1 : 0;
}

extern "C" int shadow_hashmap_size(void* map) {
    if (!map) return 0;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    return (int)m->data.size();
}

extern "C" void shadow_hashmap_free(void* map) {
    if (!map) return;
    ShadowDict* m = reinterpret_cast<ShadowDict*>(map);
    delete m;
}

// ── Set operations (ShadowSet: std::unordered_set<string>) ──
// set<T> 容器的运行时实现。
// 统一存储为 string：string 元素直接存，int/long 元素序列化为 string。
// 这样 set<int> 和 set<string> 可以共存于同一容器类型。

extern "C" void* shadow_set_new() {
    return new ShadowSet();
}

extern "C" int shadow_set_add(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    s->data.insert(std::string(value));
    return 0;
}

extern "C" int shadow_set_contains(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return s->data.find(value) != s->data.end() ? 1 : 0;
}

extern "C" int shadow_set_remove(void* set, const char* value) {
    if (!set || !value) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return s->data.erase(value) > 0 ? 1 : 0;
}

extern "C" int shadow_set_size(void* set) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return (int)s->data.size();
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
    s->data.insert(std::to_string(value));
    return 0;
}

extern "C" int shadow_set_contains_int(void* set, int64_t value) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return s->data.find(std::to_string(value)) != s->data.end() ? 1 : 0;
}

extern "C" int shadow_set_remove_int(void* set, int64_t value) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    return s->data.erase(std::to_string(value)) > 0 ? 1 : 0;
}

// shadow_set_at_string: return string element at given index (strdup'd, caller owns).
// Used by for (x in set) iteration. Order is unspecified (unordered_set).
extern "C" const char* shadow_set_at_string(void* set, int32_t idx) {
    if (!set) return nullptr;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    if (idx < 0 || (size_t)idx >= s->data.size()) {
        fprintf(stderr, "error: set index %d out of bounds (size=%zu)\n",
                idx, s->data.size());
        return nullptr;
    }
    auto it = s->data.begin();
    std::advance(it, idx);
    return strdup(it->c_str());
}

// shadow_set_at_int: return int64 element at given index (parsed from stored string).
// Used by for (x in set<int>) / for (x in set<long>) iteration.
extern "C" int64_t shadow_set_at_int(void* set, int32_t idx) {
    if (!set) return 0;
    ShadowSet* s = reinterpret_cast<ShadowSet*>(set);
    if (idx < 0 || (size_t)idx >= s->data.size()) {
        fprintf(stderr, "error: set index %d out of bounds (size=%zu)\n",
                idx, s->data.size());
        return 0;
    }
    auto it = s->data.begin();
    std::advance(it, idx);
    try {
        return std::stoll(*it);
    } catch (...) {
        return 0;
    }
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
    if (std::holds_alternative<int64_t>(v)) {
        return std::to_string(std::get<int64_t>(v));
    }
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) {
        return std::get<bool>(v) ? "true" : "false";
    }
    if (std::holds_alternative<std::string>(v)) {
        return std::get<std::string>(v);
    }
    if (std::holds_alternative<void*>(v)) {
        ShadowDict* d = reinterpret_cast<ShadowDict*>(std::get<void*>(v));
        ShadowArray* a = reinterpret_cast<ShadowArray*>(std::get<void*>(v));
        if (d) return value_to_string_dict(d);
        if (a) return value_to_string_array(a);
    }
    return "null";
}

static std::string value_to_string_dict(ShadowDict* d) {
    if (!d) return "{}";
    std::string result = "{";
    bool first = true;
    for (const auto& [k, v] : d->data) {
        if (!first) result += ", ";
        result += "\"" + k + "\": ";
        result += value_to_string(v);
        first = false;
    }
    result += "}";
    return result;
}

static std::string value_to_string_array(ShadowArray* a) {
    if (!a) return "[]";
    std::string result = "[";
    for (size_t i = 0; i < a->data.size(); ++i) {
        if (i > 0) result += ", ";
        result += value_to_string(a->data[i]);
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
        d->data[key] = val;
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
        a->data.push_back(parse_json_value(json, pos));
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
        try { return std::stoll(num_str); } catch (...) {}
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
        if (key) d->data[key] = val;
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
    auto it = d->data.find(key);
    if (it == d->data.end()) return nullptr;
    DictValue& val = it->second;
    if (std::holds_alternative<int64_t>(val)) {
        g_any_int_buf = std::get<int64_t>(val);
        return &g_any_int_buf;
    }
    if (std::holds_alternative<double>(val)) {
        g_any_float_buf = std::get<double>(val);
        return &g_any_float_buf;
    }
    if (std::holds_alternative<bool>(val)) {
        g_any_bool_buf = std::get<bool>(val);
        return &g_any_bool_buf;
    }
    if (std::holds_alternative<std::string>(val)) {
        g_str_buf = std::get<std::string>(val);
        return const_cast<char*>(g_str_buf.c_str());
    }
    if (std::holds_alternative<void*>(val)) {
        void* p = std::get<void*>(val);
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
    for (const auto& [k, v] : d->data) {
        a->data.push_back(DictValue(k));
    }
    return a;
}

// shadow_dict_size: return number of entries in dict (C++ ShadowDict)
// Used by for (k in dict) iteration to avoid layout mismatch with
// Shadow runtime's shadow_array_len (which reads Shadow-layout arrays).
extern "C" int32_t shadow_dict_size(void* dict_ptr) {
    if (!dict_ptr) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    return (int32_t)d->data.size();
}

// shadow_dict_key_at: return key string at given index (C++ ShadowDict)
// Used by for (k in dict) iteration. Returns strdup'd string (caller owns).
extern "C" const char* shadow_dict_key_at(void* dict_ptr, int32_t idx) {
    if (!dict_ptr) return nullptr;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    if (idx < 0 || (size_t)idx >= d->data.size()) {
        fprintf(stderr, "error: dict index %d out of bounds (size=%zu)\n",
                idx, d->data.size());
        return nullptr;
    }
    auto it = d->data.begin();
    std::advance(it, idx);
    return strdup(it->first.c_str());
}

// Get all values as array (semicolon-separated, values as strings)
extern "C" const char* shadow_dict_values(void* dict_ptr) {
    if (!dict_ptr) return dup_str("");
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    std::string result;
    bool first = true;
    for (const auto& [k, v] : d->data) {
        if (!first) result += ";";
        result += value_to_string(v);
        first = false;
    }
    return dup_str(result);
}

// Check if dict has key
extern "C" int shadow_dict_has_key(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    return d->data.find(key) != d->data.end() ? 1 : 0;
}

// Create Array from values (variadic)
extern "C" void* shadow_array_create(int count, ...) {
    ShadowArray* a = new ShadowArray();
    a->data.resize(count);
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
        a->data[i] = val;
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
            arr->data.push_back(std::string(1, c));
        }
    } else {
        size_t pos = 0;
        while (true) {
            size_t next = s.find(d, pos);
            if (next == std::string::npos) {
                arr->data.push_back(s.substr(pos));
                break;
            }
            arr->data.push_back(s.substr(pos, next - pos));
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
    result->data = a->data;  // Copy all elements
    
    int64_t adjusted_idx = idx;
    if (adjusted_idx < 0) adjusted_idx = (int64_t)result->data.size() + adjusted_idx;
    if (adjusted_idx < 0 || (size_t)adjusted_idx >= result->data.size()) return result;
    
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
    
    result->data[adjusted_idx] = val;
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
    
    a->data.push_back(val);
    return a;
}

// Get Array element by index Ã¢ÂÂ return boxed value
extern "C" void* shadow_array_get(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) return nullptr;
    DictValue& val = a->data[idx];
    if (std::holds_alternative<int64_t>(val)) {
        // Transparent 'any' model: return the raw int64 value, not a box pointer.
        // The self-hosted codegen (and shadowc's compiled code) treat 'any' as the
        // raw value (e.g. an LLVM handle stored as i64), so returning &g_any_int_buf
        // would hand a box address where a raw handle is expected.
        return (void*)(intptr_t)std::get<int64_t>(val);
    }
    if (std::holds_alternative<double>(val)) {
        g_any_float_buf = std::get<double>(val);
        return &g_any_float_buf;
    }
    if (std::holds_alternative<std::string>(val)) {
        const char* c = std::get<std::string>(val).c_str();
        return (void*)c;
    }
    if (std::holds_alternative<bool>(val)) {
        g_any_bool_buf = std::get<bool>(val);
        return &g_any_bool_buf;
    }
    if (std::holds_alternative<void*>(val)) {
        void* vp = std::get<void*>(val);
        return vp;
    }
    return nullptr;
}

// Get Array length
extern "C" int32_t shadow_array_len(void* array_ptr) {
    if (!array_ptr) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    int32_t n = (int32_t)a->data.size();
    return n;
}

// Create array from int buffer (non-variadic, for shadow-lang codegen)
extern "C" void* shadow_array_create_ints(int32_t count, int32_t* values) {
    ShadowArray* a = new ShadowArray();
    a->data.resize(count);
    for (int32_t i = 0; i < count; i++) {
        a->data[i] = (int64_t)values[i];
    }
    // Register with GC so shadow_gc_collect can reclaim it when unreachable.
    shadow_gc_register(a, 1, (int64_t)sizeof(ShadowArray));
    return a;
}

// Create array from pointer array (string/any/nested-array element buffers)
extern "C" void* shadow_array_create_ptrs(int32_t count, void** values) {
    ShadowArray* a = new ShadowArray();
    a->data.resize(count);
    for (int32_t i = 0; i < count; i++) {
        a->data[i] = (DictValue)values[i];
    }
    shadow_gc_register(a, 1, (int64_t)sizeof(ShadowArray));
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
    for (size_t i = 0; i < a->data.size(); ++i) {
        if (i > 0) result += d;
        result += value_to_string(a->data[i]);
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
    for (size_t i = 0; i < a->data.size(); i++) {
        if (i > 0) result += d;
        const DictValue& v = a->data[i];
        if (std::holds_alternative<void*>(v)) {
            void* p = std::get<void*>(v);
            if (p) result += (const char*)p;
        } else if (std::holds_alternative<std::string>(v)) {
            result += std::get<std::string>(v);
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
        if (!a->data.empty()) {
            return dup_str(value_to_string_array(a).c_str());
        }
        return dup_str("[]");
    }
    ShadowDict* d = reinterpret_cast<ShadowDict*>(val_ptr);
    if (d && d->type_tag == 1) {
        if (!d->data.empty()) {
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
    auto it = d->data.find(key);
    if (it == d->data.end()) return dup_str("");
    const DictValue& v = it->second;
    if (std::holds_alternative<std::string>(v))
        return dup_str(std::get<std::string>(v).c_str());
    // Fall back to stringifying other variants (int/bool/etc.).
    return dup_str(value_to_string(v).c_str());
}

extern "C" int64_t shadow_json_get_int(const char* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(const_cast<char*>(dict_ptr));
    if (!d || d->type_tag != 1) return 0;
    auto it = d->data.find(key);
    if (it == d->data.end()) return 0;
    const DictValue& v = it->second;
    if (std::holds_alternative<int64_t>(v)) return std::get<int64_t>(v);
    if (std::holds_alternative<double>(v)) return (int64_t)std::get<double>(v);
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1 : 0;
    if (std::holds_alternative<std::string>(v)) {
        try { return std::stoll(std::get<std::string>(v)); } catch (...) { return 0; }
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
        return (int32_t)a->data.size();
    }
    ShadowDict* d = reinterpret_cast<ShadowDict*>(val_ptr);
    if (d && d->type_tag == 1) {
        return (int32_t)d->data.size();
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

extern "C" void shadow_free(void* ptr) {
    if (!ptr) return;
    // GC 登记的对象（shadow_gc_alloc / __rt_shadow_malloc 分配）：先从 meta 删除，
    // 防止 sweep 阶段 double-free / 残留元数据导致 UAF。随后直接 free。
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
        auto* ar = reinterpret_cast<ShadowArena*>(ptr);
        for (void* b : ar->blocks) {
            free(b);
        }
        delete ar;
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
    if (ar->blocks.empty() || ar->used.back() + n > ar->caps.back()) {
        size_t cap = n > ar->block_size ? n : ar->block_size;
        void* block = calloc(1, cap);
        if (!block) return nullptr;
        ar->blocks.push_back(block);
        ar->caps.push_back(cap);
        ar->used.push_back(0);
    }
    size_t idx = ar->blocks.size() - 1;
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
    for (size_t i = 0; i < ar->blocks.size(); ++i) {
        size_t u = ar->used[i];
        if (target <= consumed + u) {
            ar->used[i] = target - consumed;
            keep_blocks = i + 1;
            break;
        }
        consumed += u;
    }
    for (size_t i = keep_blocks; i < ar->blocks.size(); ++i) {
        free(ar->blocks[i]);
    }
    ar->blocks.resize(keep_blocks);
    ar->caps.resize(keep_blocks);
    ar->used.resize(keep_blocks);
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
        g_cli_args->data.push_back(std::string(argv[i] ? argv[i] : ""));
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
    return (int32_t)g_cli_args->data.size();
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
                for (const auto& e : arr_b->data) {
                    result->data.push_back(e);
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
            a->data.push_back(DictValue((int64_t)v));
    } else {
        for (int64_t v = s; v > (int64_t)e; v += step)
            a->data.push_back(DictValue((int64_t)v));
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
    d->data[(const char*)key] = dv;
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
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return 0;
    }
    if (std::holds_alternative<int64_t>(a->data[idx]))
        return std::get<int64_t>(a->data[idx]);
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_get_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" const char* shadow_array_get_string(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return nullptr;
    }
    if (std::holds_alternative<std::string>(a->data[idx]))
        return strdup(std::get<std::string>(a->data[idx]).c_str());
    return nullptr;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_get_bool Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" int32_t shadow_array_get_bool(void* array_ptr, int32_t idx) {
    if (!array_ptr) return 0;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return 0;
    }
    if (std::holds_alternative<bool>(a->data[idx]))
        return std::get<bool>(a->data[idx]) ? 1 : 0;
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_set_int Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ

// shadow_array_get_ptr: get pointer-sized element (string, any, nested array)
extern "C" void* shadow_array_get_ptr(void* array_ptr, int32_t idx) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return nullptr;
    }
    // Full variant coverage (mirrors shadow_array_get). The transparent-`any`
    // model expects raw handles for scalars: an int element is returned as
    // (void*)(intptr_t)value so shadow_any_as_int can peel it; previously
    // only string/void* were handled and scalars fell through to nullptr,
    // which made `a[i] as int` read 0 instead of the real element.
    DictValue& val = a->data[idx];
    if (std::holds_alternative<int64_t>(val))
        return (void*)(intptr_t)std::get<int64_t>(val);
    if (std::holds_alternative<double>(val)) {
        g_any_float_buf = std::get<double>(val);
        return &g_any_float_buf;
    }
    if (std::holds_alternative<std::string>(val))
        return (void*)strdup(std::get<std::string>(val).c_str());
    if (std::holds_alternative<bool>(val)) {
        g_any_bool_buf = std::get<bool>(val);
        return &g_any_bool_buf;
    }
    if (std::holds_alternative<void*>(val))
        return std::get<void*>(val);
    return nullptr;
}

// shadow_array_set_ptr: set pointer-sized element (string, any, nested array)
extern "C" void shadow_array_set_ptr(void* array_ptr, int32_t idx, void* val) {
    if (!array_ptr) return;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return;
    }
    a->data[idx] = (DictValue)val;
}
extern "C" void* shadow_array_set_int(void* array_ptr, int32_t idx, int64_t val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return array_ptr;
    }
    a->data[idx] = val;
    return array_ptr;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_array_set_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" void* shadow_array_set_string(void* array_ptr, int32_t idx, const char* val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (idx < 0) idx = (int32_t)a->data.size() + idx;
    if (idx < 0 || (size_t)idx >= a->data.size()) {
        fprintf(stderr, "error: array index %d out of bounds (size=%zu)\n",
                idx, a->data.size());
        return array_ptr;
    }
    a->data[idx] = std::string(val ? val : "");
    return array_ptr;
}

// ── array_push: append an element to the end, return the (stable) array ptr ──
extern "C" void* shadow_array_push_int(void* array_ptr, int32_t val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->data.push_back((DictValue)(int64_t)val);
    return array_ptr;
}

// 快速路径 array_push（shadow 层 C 布局数组，kind=2）：
//   [0]=len(int32) [4]=cap(int32) [8]=elem_size(int32) [12..]=data
// 单次 C 调用完成 len/cap 读取 + 扩容 + 元素写入 + len 更新，
// 消除 shadow 层每次 push 的多次 extern 调用（__rt_shadow_load_int×3 +
// __rt_shadow_store_int×2 → 1 次调用）。语义与 runtime_lib.shadow 的
// shadow_array_push_int 完全一致（含 null 首推、2x 倍增扩容、es 4/8 分派）。
extern "C" void* shadow_gc_alloc(int32_t size, int32_t kind);  // defined later in TU
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
    a->data.push_back((DictValue)val);
    return array_ptr;
}
extern "C" void* shadow_array_push_float(void* array_ptr, double val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->data.push_back((DictValue)val);
    return array_ptr;
}
extern "C" void* shadow_array_push_ptr(void* array_ptr, void* val) {
    if (!array_ptr) return array_ptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    a->data.push_back((DictValue)val);
    return array_ptr;
}

// ── array_pop: remove and return the last element (transparent-any value) ──
// 返回裸值（小整数直接为 intptr 值；字符串/指针元素返回指针；double 装箱为 AnyBox）。
extern "C" void* shadow_array_pop(void* array_ptr) {
    if (!array_ptr) return nullptr;
    ShadowArray* a = reinterpret_cast<ShadowArray*>(array_ptr);
    if (a->data.empty()) return nullptr;
    DictValue v = a->data.back();
    a->data.pop_back();
    if (std::holds_alternative<int64_t>(v)) return (void*)(intptr_t)std::get<int64_t>(v);
    if (std::holds_alternative<bool>(v)) return (void*)(intptr_t)(std::get<bool>(v) ? 1 : 0);
    if (std::holds_alternative<double>(v)) {
        AnyBox* b = new AnyBox;
        b->magic = ANYBOX_MAGIC;
        b->tag = 2;  // float
        b->value = 0;
        memcpy(&b->value, &std::get<double>(v), sizeof(double));
        shadow_gc_register(b, 3, (int64_t)sizeof(AnyBox));
        return b;
    }
    if (std::holds_alternative<std::string>(v)) return (void*)strdup(std::get<std::string>(v).c_str());
    return std::get<void*>(v);
}

// Ã¢ÂÂÃ¢ÂÂ shadow_dict_get_int Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" int64_t shadow_dict_get_int(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return 0;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    auto it = d->data.find(key);
    if (it != d->data.end() && std::holds_alternative<int64_t>(it->second))
        return std::get<int64_t>(it->second);
    return 0;
}

// Ã¢ÂÂÃ¢ÂÂ shadow_dict_get_string Ã¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂÃ¢ÂÂ
extern "C" const char* shadow_dict_get_string(void* dict_ptr, const char* key) {
    if (!dict_ptr || !key) return nullptr;
    ShadowDict* d = reinterpret_cast<ShadowDict*>(dict_ptr);
    auto it = d->data.find(key);
    if (it != d->data.end() && std::holds_alternative<std::string>(it->second))
        return strdup(std::get<std::string>(it->second).c_str());
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
    AnyBox* b = new AnyBox;
    b->magic = ANYBOX_MAGIC;
    b->tag = tag; b->value = value;
    // Register with GC (kind=3) so the GC can trace AnyBox.value → GC objects.
    // Without this, objects referenced only through `any` variables are invisible
    // to the GC and get prematurely freed (use-after-free → 0xC0000005).
    shadow_gc_register(b, 3, (int64_t)sizeof(AnyBox));
    return b;
}
extern "C" void* shadow_any_box_ptr(int32_t tag, void* ptr) {
    AnyBox* b = new AnyBox;
    b->magic = ANYBOX_MAGIC;
    b->tag = tag;
    b->value = (int64_t)(intptr_t)ptr;
    shadow_gc_register(b, 3, (int64_t)sizeof(AnyBox));
    return b;
}
// Box a raw C string pointer as an AnyBox (tag 3 = string).
extern "C" void* shadow_any_box_string(void* s) {
    AnyBox* b = new AnyBox;
    b->magic = ANYBOX_MAGIC;
    b->tag = 3;
    b->value = (int64_t)(intptr_t)s;
    shadow_gc_register(b, 3, (int64_t)sizeof(AnyBox));
    return b;
}
// Box a double as an AnyBox (tag 2 = float). Bit-cast the double to int64 so
// the exact bit pattern survives (no precision loss through int conversion).
extern "C" void* shadow_any_box_double(double d) {
    int64_t bits = 0;
    memcpy(&bits, &d, sizeof(bits));
    AnyBox* b = new AnyBox;
    b->magic = ANYBOX_MAGIC;
    b->tag = 2;
    b->value = bits;
    shadow_gc_register(b, 3, (int64_t)sizeof(AnyBox));
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

// ── GC: generational lock-free mark-sweep with finalizer (Shadow 0.3 §11) ──
// v2: Generational STW mark-sweep with lock-free metadata and finalizer support.
//   - shadow_gc_alloc(size, kind): allocate raw GC heap block (tracked, young gen)
//   - shadow_gc_register(ptr, kind, size): track an existing C++ object
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
//   - Old gen (gen=1): objects surviving PROMOTION_THRESHOLD minor GCs.
//   - Remembered set: old→young references tracked via write barrier.
//
// Lock-free design:
//   - GCMeta.marked uses std::atomic<uint32_t> for lock-free mark phase.
//   - GC collect uses a spinlock (std::atomic_flag) to coordinate concurrent collect triggers.
//   - Root vectors use thread-local accumulation + atomic snapshot for mark phase.
//
// Finalizer:
//   - GCMeta has finalizer_fn (void(*)(void*)) and finalizer_data fields.
//   - During sweep, finalizers run in a separate pass BEFORE freeing.
//   - Finalizers may resurrect objects (add back to root set) — objects with
//     finalizers that were dead get one extra cycle before actual free.
//
// kind: 0 = raw struct (conservative byte scan for inner pointers),
//       1 = ShadowArray (precise: trace void* elements),
//       2 = ShadowDict  (precise: trace void* values),
//       3 = AnyBox      (precise: trace value field as pointer, delete on sweep).
//           AnyBox holds an `any` value; its .value field may be a GC-managed
//           pointer (struct/array). Without kind==3 tracing, the GC cannot
//           follow the global→AnyBox→ShadowArray reachability chain, causing
//           premature frees of objects still referenced through `any` globals.
struct GCMeta {
    std::atomic<uint32_t> marked;   // 跨轮存活标记（含"分配即黑"）——sweep 只认它
    std::atomic<uint32_t> visited;  // 本轮遍历去重位（三色标记：collect 开始清 visited，不动 marked）
    int32_t  kind;    // 0=raw, 1=array, 2=dict, 3=anybox
    int32_t  owned;   // 1 = gc_alloc'd (free()), 0 = registered (delete)
    int64_t  size;    // 对齐后容量（rt_alloc_cap 返回、保守扫描范围）
    int64_t  req;     // 实际请求大小（g_heap_bytes / GC pacing 记账，避免对齐放大触发）
    uint32_t gen;     // 0=young, 1=old (tenured)
    uint32_t survived;// minor GC survival count (for promotion)
    void(*finalizer_fn)(void*);  // finalizer callback (nullptr = none)
    void*   finalizer_data;      // passed to finalizer (usually the object ptr)
    bool    finalized;           // finalizer already ran (prevent double-finalize)

    GCMeta() : marked(0), visited(0), kind(0), owned(0), size(0), req(0), gen(0), survived(0),
               finalizer_fn(nullptr), finalizer_data(nullptr), finalized(false) {}
    GCMeta(uint32_t m, int32_t k, int32_t o, int64_t s)
        : marked(m), visited(0), kind(k), owned(o), size(s), req(s), gen(0), survived(0),
          finalizer_fn(nullptr), finalizer_data(nullptr), finalized(false) {}
    GCMeta(uint32_t m, int32_t k, int32_t o, int64_t s, int64_t r)
        : marked(m), visited(0), kind(k), owned(o), size(s), req(r), gen(0), survived(0),
          finalizer_fn(nullptr), finalizer_data(nullptr), finalized(false) {}
    // Explicit copy: std::atomic is non-copyable, so we load/store the value.
    GCMeta(const GCMeta& o)
        : marked(o.marked.load(std::memory_order_relaxed)),
          visited(o.visited.load(std::memory_order_relaxed)),
          kind(o.kind), owned(o.owned), size(o.size), req(o.req), gen(o.gen), survived(o.survived),
          finalizer_fn(o.finalizer_fn), finalizer_data(o.finalizer_data), finalized(o.finalized) {}
    GCMeta& operator=(const GCMeta& o) {
        marked.store(o.marked.load(std::memory_order_relaxed), std::memory_order_relaxed);
        visited.store(o.visited.load(std::memory_order_relaxed), std::memory_order_relaxed);
        kind = o.kind; owned = o.owned; size = o.size; req = o.req; gen = o.gen; survived = o.survived;
        finalizer_fn = o.finalizer_fn; finalizer_data = o.finalizer_data; finalized = o.finalized;
        return *this;
    }
};

#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <algorithm>

// ── 自定义开放寻址哈希表（替代 std::unordered_map<void*, GCMeta>）──
// 分配/清扫热点：unordered_map 每次插入分配节点、遍历缓存不友好（node-based
// 分散内存）。开放寻址 + 线性探测 + 连续存储 → 插入无节点分配、遍历缓存友好。
// 指针键本身分布良好；负载因子 0.7。
// 删除用墓碑（TOMBSTONE 哨兵）而非回移：O(1) 删除且不搬动条目，配合
// 活条目双向链表使 for_each 只遍历存活条目（表在峰值后不收缩，若遍历全部
// 槽位，str_reverse 每轮 GC 要扫 128MB 空槽 → 100x 退化）。墓碑+存活数
// 超 70% 时按需重建（清墓碑，必要时扩容）。
class GCMetaMap {
public:
    struct Entry {
        void* key;      // nullptr = 空槽, TOMBSTONE = 已删, 其它 = 活
        GCMeta meta;
        int32_t next;   // 活条目链表：下一索引，-1 = 尾
        int32_t prev;   // 活条目链表：上一索引，-1 = 无
    };

    static void* const TOMBSTONE;

    GCMetaMap() { slots.assign(64, Entry{}); mask = 63; }

    size_t size() const { return count; }
    size_t rebuilds() const { return rebuild_count; }
    size_t capacity() const { return slots.size(); }
    size_t tombstones_n() const { return tombstones; }
    size_t total_probes = 0;
    size_t insert_count = 0;
    size_t max_probe = 0;
    size_t max_occ = 0;

    GCMeta* find(void* key) {
        size_t i = hash(key) & mask;
        while (slots[i].key) {
            if (slots[i].key == key) return &slots[i].meta;
            i = (i + 1) & mask;
        }
        return nullptr;
    }

    GCMeta& operator[](void* key) {
        size_t i = hash(key) & mask;
        int32_t first_tomb = -1;
        while (slots[i].key) {
            if (slots[i].key == key) return slots[i].meta;
            if (slots[i].key == TOMBSTONE && first_tomb < 0) first_tomb = (int32_t)i;
            i = (i + 1) & mask;
        }
        if ((count + tombstones + 1) * 10 >= slots.size() * 7) {
            rebuild();
            return (*this)[key];
        }
        size_t ins = (first_tomb >= 0) ? (size_t)first_tomb : i;
        if (first_tomb >= 0) tombstones--;
        slots[ins].key = key;
        slots[ins].meta = GCMeta();
        slots[ins].prev = -1;
        slots[ins].next = head;
        if (head >= 0) slots[head].prev = (int32_t)ins;
        head = (int32_t)ins;
        count++;
        return slots[ins].meta;
    }

    // 批量插入（攒批 flush 用）：一次探测 + 字段初始化，免默认构造再覆盖。
    GCMeta* insert(void* key, int32_t kind, int32_t asize, int32_t req) {
        size_t i = hash(key) & mask;
        int32_t first_tomb = -1;
        size_t probes = 0;
        while (slots[i].key) {
            probes++;
            if (slots[i].key == key) return &slots[i].meta;
            if (slots[i].key == TOMBSTONE && first_tomb < 0) first_tomb = (int32_t)i;
            i = (i + 1) & mask;
        }
        total_probes += probes + 1;
        insert_count++;
        if (probes + 1 > max_probe) max_probe = probes + 1;
        size_t occ = count + tombstones;
        if (occ > max_occ) max_occ = occ;
        if ((count + tombstones + 1) * 10 >= slots.size() * 7) {
            rebuild();
            return insert(key, kind, asize, req);
        }
        size_t ins = (first_tomb >= 0) ? (size_t)first_tomb : i;
        if (first_tomb >= 0) tombstones--;
        slots[ins].key = key;
        GCMeta& m = slots[ins].meta;
        m.marked.store(1, std::memory_order_relaxed);
        m.visited.store(0, std::memory_order_relaxed);
        m.kind = kind;
        m.owned = 1;
        m.size = (int64_t)asize;
        m.req = (int64_t)req;
        m.gen = 0;
        m.survived = 0;
        m.finalizer_fn = nullptr;
        m.finalizer_data = nullptr;
        m.finalized = false;
        slots[ins].prev = -1;
        slots[ins].next = head;
        if (head >= 0) slots[head].prev = (int32_t)ins;
        head = (int32_t)ins;
        count++;
        return &slots[ins].meta;
    }

    bool erase(void* key) {
        size_t i = hash(key) & mask;
        while (slots[i].key) {
            if (slots[i].key == key) {
                int32_t idx = (int32_t)i;
                if (slots[idx].prev >= 0) slots[slots[idx].prev].next = slots[idx].next;
                else head = slots[idx].next;
                if (slots[idx].next >= 0) slots[slots[idx].next].prev = slots[idx].prev;
                slots[idx].key = TOMBSTONE;
                slots[idx].meta = GCMeta();
                slots[idx].prev = -1;
                slots[idx].next = -1;
                count--;
                tombstones++;
                return true;
            }
            i = (i + 1) & mask;
        }
        return false;
    }

    // 按元数据指针直接擦除（sweep 用，免二次哈希查找）。
    // 指针由 for_each 回调提供，指向 slots 内条目；sweep 期间无插入 → 无 rebuild，
    // 指针稳定；erase 只清当前槽，不影响其它条目。GCMeta 在 Entry 内偏移固定，
    // 由指针差算出槽位索引。
    bool erase_entry(GCMeta* m) {
        if (!m) return false;
        intptr_t off = (char*)m - (char*)&slots[0].meta;
        if (off < 0) return false;
        size_t idx = (size_t)off / sizeof(Entry);
        if (idx >= slots.size()) return false;
        if (slots[idx].key == nullptr || slots[idx].key == TOMBSTONE) return false;
        if (slots[idx].prev >= 0) slots[slots[idx].prev].next = slots[idx].next;
        else head = slots[idx].next;
        if (slots[idx].next >= 0) slots[slots[idx].next].prev = slots[idx].prev;
        slots[idx].key = TOMBSTONE;
        slots[idx].meta = GCMeta();
        slots[idx].prev = -1;
        slots[idx].next = -1;
        count--;
        tombstones++;
        return true;
    }

    // 遍历所有存活条目（回调签名：void(void* key, GCMeta& meta)）。
    // 顺序扫槽位 ~5ns/槽（缓存友好，读 80B 槽的 key 字段），链表随机跳转
    // ~100ns/条（缓存缺失）。交叉点约 5% 占用率：占用 ≥5% 顺序扫更快，
    // 更低才走链表避免扫大量空槽。
    template <typename F>
    void for_each(F&& f) {
        if ((int64_t)count * 20 >= (int64_t)slots.size()) {
            for (size_t i = 0; i < slots.size(); i++) {
                Entry& e = slots[i];
                if (e.key && e.key != TOMBSTONE) f(e.key, e.meta);
            }
        } else {
            int32_t idx = head;
            while (idx >= 0) {
                Entry& e = slots[idx];
                int32_t nxt = e.next;
                f(e.key, e.meta);
                idx = nxt;
            }
        }
    }

    // 清扫后活条目远小于容量时收缩表，保持缓存驻留（524288 槽 × 80B = 42MB
    // 远超 L3；收缩后 str_reverse 每轮 GC 后表回到 ~1K 槽，插入/擦除免缓存缺失）。
    void shrink_if_sparse() {
        if (count * 4 < slots.size() && slots.size() > 64) shrink();
    }

private:
    static size_t hash(void* p) {
        uintptr_t h = (uintptr_t)p;
        h ^= h >> 16;
        h *= 0x7feb352dU;
        h ^= h >> 15;
        h *= 0x846ca68bU;
        h ^= h >> 16;
        return (size_t)h;
    }

    void rebuild() {
        size_t live = count;
        size_t new_cap = slots.size();
        if ((live + 1) * 10 >= new_cap * 7) new_cap *= 2;
        rehash(new_cap);
    }

    void shrink() {
        size_t live = count;
        size_t new_cap = 64;
        while (new_cap < live * 2) new_cap *= 2;
        if (new_cap >= slots.size()) return;
        rehash(new_cap);
    }

private:
    void rehash(size_t new_cap) {
        std::vector<Entry> old;
        old.swap(slots);
        slots.assign(new_cap, Entry{});
        mask = new_cap - 1;
        count = 0;
        tombstones = 0;
        head = -1;
        for (auto& e : old) {
            if (e.key && e.key != TOMBSTONE) {
                size_t i = hash(e.key) & mask;
                while (slots[i].key) i = (i + 1) & mask;
                slots[i].key = e.key;
                slots[i].meta = std::move(e.meta);
                slots[i].prev = -1;
                slots[i].next = head;
                if (head >= 0) slots[head].prev = (int32_t)i;
                head = (int32_t)i;
                count++;
            }
        }
        rebuild_count++;
    }

    size_t rebuild_count = 0;
    size_t slots_size() const { return slots.size(); }

    std::vector<Entry> slots;
    size_t mask;
    size_t count = 0;
    size_t tombstones = 0;
    int32_t head = -1;
};

void* const GCMetaMap::TOMBSTONE = reinterpret_cast<void*>(uintptr_t(1));

// GC metadata table: void* → GCMeta (lock-free reads via atomic marked field)
static GCMetaMap g_gc_meta;

// ── 攒批登记快路径（对齐 Windows rt_gc.c 的 g_pend）──
// 单 mutator + GC 关闭时，分配只追加到 pending 缓冲（无锁无哈希），
// GC 启动 / 缓冲满 / 慢路径时统一 flush 进 g_gc_meta —— 分配密集热循环
// （str_reverse 等）省掉每分配的全局锁 + 哈希插入。
// 安全性：g_mutator_threads<=1 时 g_pend 只被本线程读写，无并发；
// 第二个 mutator attach 后快路径关闭，慢路径/GC 持锁 flush 全部 pending。
// 已知微小窗口（第二个 mutator attach 瞬间跨线程 free 本批对象）概率极低，
// 与 Windows 注释留档一致。
#define RT_PEND_CAP 64
struct PendingAlloc { void* p; int32_t kind; int32_t asize; int32_t req; };
static PendingAlloc g_pend[RT_PEND_CAP];
static int g_pend_n = 0;
static int64_t g_pend_bytes = 0;
static int64_t g_dbg_fast = 0;   // 攒批快路径命中次数（诊断）
static int64_t g_dbg_slow = 0;   // 慢路径次数（诊断）
static std::atomic<int32_t> g_mutator_threads{0};
// 前向声明：g_heap_bytes 定义在下方（GC 触发状态小节），flush 需要它记账。
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

// 锁内：把攒批的 pending 全部登记进 g_gc_meta（调用者必须已持 g_gc_mutex）。
static void gc_flush_pending_locked() {
    for (int i = 0; i < g_pend_n; i++) {
        PendingAlloc& e = g_pend[i];
        g_gc_meta.insert(e.p, e.kind, e.asize, e.req);
        g_heap_bytes.fetch_add((int64_t)e.req, std::memory_order_relaxed);
    }
    g_pend_n = 0;
    g_pend_bytes = 0;
}

// Lock-free collect coordination: spinlock prevents concurrent collect cycles.
// (gc_lock/gc_unlock defined after ThreadGCState — they mark stw_state while
// spinning so a concurrent collector is treated as blocked by the STW wait.)
static std::atomic_flag g_gc_collect_lock = ATOMIC_FLAG_INIT;

// Thread-local allocation counter for lock-free GC trigger.
static thread_local int64_t tl_gc_alloc_count = 0;
static std::atomic<int64_t> g_gc_total_alloc_count{0};

// ── 阶段计时（SHADOW_GC_PROFILE=1 时 atexit 打印分配/标记/清扫耗时）──
static int64_t g_prof_alloc_ns = 0;
static int64_t g_prof_mark_ns = 0;
static int64_t g_prof_sweep_ns = 0;
static int64_t g_prof_gc_ns = 0;
static int64_t g_prof_gc_count = 0;
static int64_t g_prof_meta_peak = 0;
static int64_t g_prof_meta_ops = 0;
static int64_t g_prof_mark_reset_ns = 0;   // mark：清 visited 的 for_each
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

static void prof_atexit(void) {
    if (!prof_on()) return;
    fprintf(stderr, "[PROF] alloc_ns=%lld mark_ns=%lld sweep_ns=%lld gc_ns=%lld gc_count=%lld meta_peak=%lld meta_ops=%lld total_alloc=%lld heap=%lld trigger=%lld\n",
            (long long)g_prof_alloc_ns, (long long)g_prof_mark_ns, (long long)g_prof_sweep_ns,
            (long long)g_prof_gc_ns, (long long)g_prof_gc_count, (long long)g_prof_meta_peak,
            (long long)g_prof_meta_ops, (long long)g_gc_total_alloc_count.load(std::memory_order_relaxed),
            (long long)g_heap_bytes.load(std::memory_order_relaxed),
            (long long)g_gc_trigger.load(std::memory_order_relaxed));
    fprintf(stderr, "[PROF] mark_reset_ns=%lld mark_roots_ns=%lld mark_wl_ns=%lld\n",
            (long long)g_prof_mark_reset_ns, (long long)g_prof_mark_roots_ns, (long long)g_prof_mark_wl_ns);
    fprintf(stderr, "[PROF] meta slots=%zu live=%zu tombs=%zu rebuilds=%zu avgprobe=%.1f maxprobe=%zu maxocc=%zu fast=%lld slow=%lld\n",
            g_gc_meta.capacity(), g_gc_meta.size(), g_gc_meta.tombstones_n(), g_gc_meta.rebuilds(),
            g_gc_meta.insert_count > 0 ? (double)g_gc_meta.total_probes / (double)g_gc_meta.insert_count : 0.0,
            g_gc_meta.max_probe, g_gc_meta.max_occ, (long long)g_dbg_fast, (long long)g_dbg_slow);
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
static std::atomic<int> g_gc_poll_flag{0};
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
    std::vector<std::vector<std::pair<void*, uint32_t>>> range_frames;  // LIFO per frame
    std::vector<std::pair<void*, uint32_t>> range_roots;                // flattened for scan
    // 协作式 STW 状态（对齐 Windows rt_gc.o 的 rt_gc_thread.gc_state）：
    //   0 = free-running（mutator，可能在任意点）
    //   1 = at safepoint（shadow_gc_poll 自旋等待放行）
    //   2 = blocked on g_gc_mutex（等锁，不跑 mutator，collect 无需等待）
    std::atomic<int> stw_state{0};
};
static thread_local ThreadGCState* tl_gc_state = nullptr;
static std::vector<ThreadGCState*> g_gc_thread_states;   // registry, guarded by g_gc_mutex
static std::mutex g_gc_mutex;                            // guards g_gc_meta, g_gc_remembered,
                                                        //   g_gc_perm_roots, g_gc_thread_states
static std::vector<void*> g_gc_perm_roots;     // permanent roots (popped by gc_perm_root_remove)

// ── 线程局部字符串长度/容量缓存 ──
// str_reverse 热循环里 shadow_string_concat_char_fast 每次 strlen(out)（out 0→43 增长）
// 是 O(n²) 瓶颈，且每次都要 g_pend 扫描 + g_gc_meta 哈希查找（带互斥锁）。缓存
// (ptr, len, cap) 三元组：命中即免 strlen 和哈希查找。
// ABA 安全：字符串只会在 GC 清扫（collect）时被释放，collect 后 g_gc_epoch 递增；
// 缓存条目记录 epoch，epoch 不匹配即视为失效（地址复用不会误用旧长度）。
// cap=0 表示"未知容量"（非堆对象/未查表），调用方按"无余量"处理（分配新缓冲，
// 输出仍正确，仅损失就地追加优化）。所有就地修改字符串的函数必须维护本缓存。
struct TLStrCache { void* p; int32_t len; int32_t cap; uint32_t epoch; };
static thread_local TLStrCache tl_str_cache[8];
static thread_local int32_t tl_str_cache_n = 0;

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
static inline void tl_str_get(void* p, int32_t* len, int32_t* cap) {
    if (tl_str_lookup(p, len, cap)) return;
    *len = (int32_t)strlen((const char*)p);
    *cap = 0;
    for (int i = 0; i < g_pend_n; i++) {
        if (g_pend[i].p == p) { *cap = g_pend[i].asize; break; }
    }
    if (*cap == 0) {
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        GCMeta* m = g_gc_meta.find(p);
        if (m) *cap = (int32_t)m->size;
    }
    tl_str_set(p, *len, *cap);
}

// 供 shadow 层就地拼接函数（shadow_string_concat_inplace / _char）在修改后同步缓存。
extern "C" void shadow_string_cache_set(void* p, int32_t len, int32_t cap) {
    tl_str_set(p, len, cap);
}

// 查询 GC 堆对象的数据区容量（分配时记录的 size）；非堆对象（字面量/栈上）返回 0。
// 供 shadow_string_concat_inplace 判断能否就地追加（与 Windows rt_gc.c 的 rt_alloc_cap 对齐）。
// 注意：攒批快路径期间新对象只进 g_pend、尚未入哈希表，哈希查找 miss 时须线性扫
// g_pend（≤RT_PEND_CAP=64；热循环里 s1 刚分配几乎必在批内）。
extern "C" int32_t rt_alloc_cap(void* p) {
    if (!p) return 0;
    for (int i = 0; i < g_pend_n; i++) {
        if (g_pend[i].p == p) return g_pend[i].asize;
    }
    std::lock_guard<std::mutex> lk(g_gc_mutex);
    GCMeta* m = g_gc_meta.find(p);
    if (!m) return 0;
    return (int32_t)m->size;
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

// 快速路径字符串查找：单次 C 调用完成朴素匹配，消除 shadow 层 shadow_index_of
// 逐字节 rt_get_byte 的 extern 调用开销（string_find 热循环 48M 次调用 → 300K 次）。
// 语义与 runtime_lib.shadow 的 shadow_index_of 完全一致（返回首次出现位置，无则 -1）。
extern "C" int32_t shadow_index_of_fast(void* s, void* needle) {
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
// g_gc_meta.find() before tracing — non-GC pointers are silently ignored.
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

static ThreadGCState* gc_get_thread_state() {
    (void)&gc_tls_guard;   // force construction of this thread's TLS guard
    if (tl_gc_state) return tl_gc_state;
    ThreadGCState* s = new ThreadGCState();
    {
        std::lock_guard<std::mutex> lk(g_gc_mutex);
        g_gc_thread_states.push_back(s);
    }
    g_mutator_threads.fetch_add(1, std::memory_order_relaxed);
    tl_gc_state = s;
    return s;
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
            if (s->stw_state.load(std::memory_order_relaxed) == 0) { all = 0; break; }
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
// ── thread's stack as GC roots. shadow-lang 0.2 doesn't emit explicit  ──
// ── stack root registration, so without this GC would reclaim objects   ──
// ── still referenced only from local variables (AST nodes, temporaries). ──
static void gc_scan_stack(std::vector<void*>& worklist) {
#ifdef _WIN32
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
    std::lock_guard<std::mutex> lk(g_gc_mutex);
    return (int64_t)g_gc_meta.size();
}

extern "C" int64_t shadow_gc_alloc_count() {
    // 对齐 Windows rt_gc.c：返回**堆字节**（未释放对象总字节），
    // 供长驻验收用例（ex_gc_longrun 等）读谷底判断泄漏。
    // 旧实现返回累计分配计数 → 单调递增 → 任何存活集恒定的程序都误报泄漏。
    return (int64_t)g_heap_bytes.load(std::memory_order_relaxed);
}

// Trace a single candidate child pointer; if it is a tracked GC object and
// not yet visited this cycle, mark it alive (marked=1) and push onto the
// worklist. Lock-free CAS on visited. 三色标记：
//   visited: 本轮遍历去重（存 epoch 世代号，collect 开始只 ++epoch 不清全表）
//   marked:  存活判定（分配即黑置 1；sweep 只认它；collect 末尾重置）
// 分离后「分配即黑」不再被 collect 开始的全量清 marked 破坏 → 刚分配未 root
// 的对象在并发窗口内不会被误回收。
static inline void gc_trace_child(void* child, std::vector<void*>& worklist) {
    if (!child) return;
    GCMeta* m = g_gc_meta.find(child);
    if (!m) return;
    uint32_t epoch = g_gc_epoch;
    uint32_t v = m->visited.load(std::memory_order_relaxed);
    if (v == epoch) return;
    if (!m->visited.compare_exchange_strong(v, epoch,
            std::memory_order_acq_rel)) return;
    m->marked.store(1, std::memory_order_relaxed);
    worklist.push_back(child);
}

// Write barrier: call when old-gen object `parent` gains a reference to `child`.
// If parent is old gen and child is young gen, add parent to remembered set.
extern "C" void shadow_gc_write_barrier(void* parent, void* child) {
    if (!parent || !child) return;
    gc_lock_blocked();
    GCMeta* pm = g_gc_meta.find(parent);
    if (pm && pm->gen == 1) {
        GCMeta* cm = g_gc_meta.find(child);
        if (cm && cm->gen == 0) {
            g_gc_remembered.insert(parent);
        }
    }
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
    g_gc_meta.for_each([&](void* obj, GCMeta& m) {
        bool refs = false;
        if (m.kind == 1) {
            ShadowArray* a = reinterpret_cast<ShadowArray*>(obj);
            for (const DictValue& v : a->data)
                if (std::holds_alternative<void*>(v) && std::get<void*>(v) == target) { refs = true; break; }
        } else if (m.kind == 2) {
            ShadowDict* d = reinterpret_cast<ShadowDict*>(obj);
            for (const auto& kv2 : d->data)
                if (std::holds_alternative<void*>(kv2.second) && std::get<void*>(kv2.second) == target) { refs = true; break; }
        } else if (m.kind == 0 && m.size > 0) {
            char* base = reinterpret_cast<char*>(obj);
            for (int64_t off = 0; off + (int64_t)sizeof(void*) <= m.size; off += sizeof(void*)) {
                void* cand; std::memcpy(&cand, base + off, sizeof(void*));
                if (cand == target) { refs = true; break; }
            }
        } else if (m.kind == 3) {
            AnyBox* b = reinterpret_cast<AnyBox*>(obj);
            if ((void*)(intptr_t)b->value == target) { refs = true; }
        }
        if (refs)
            fprintf(stderr, "[GCWATCH]   referrer %p kind=%d size=%lld marked=%u\n",
                    obj, m.kind, (long long)m.size, m.marked.load(std::memory_order_relaxed));
    });
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
static inline void gc_trace_object_children(void* obj, GCMeta& m, std::vector<void*>& worklist) {
    const int32_t tid = m.kind;
    if (tid == 0) {
        // 未分类对象：保守扫描（与 Windows 的"大对象保守"同精神；兜住
        // AnyBox.value / 字符串内嵌指针等无法精确判定的场景，只多保活不误收）。
        char* base = reinterpret_cast<char*>(obj);
        for (int64_t off = 0; off + (int64_t)sizeof(void*) <= m.size; off += sizeof(void*)) {
            void* cand;
            std::memcpy(&cand, base + off, sizeof(void*));
            gc_trace_child(cand, worklist);
        }
    } else if (tid == 2) {
        // RT_T_ARRAY（shadow 层 C 布局）：[len:4][cap:4][es:4][data@12]
        if (m.size < 12) return;
        int32_t es = 0, len = 0;
        std::memcpy(&es, (char*)obj + 8, 4);
        std::memcpy(&len, obj, 4);
        if (es == 8) {
            // 边界裁剪（对齐 Windows g_bad_array 保护：损坏头给出天文 len → 越界读）
            if (len < 0 || (int64_t)12 + (int64_t)len * 8 > m.size) {
                len = (int32_t)((m.size - 12) / 8);
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
        if (m.size < 16) return;
        int32_t tag = 0;
        std::memcpy(&tag, (char*)obj + 4, 4);
        if (tag == 3 || tag == 5) {
            void* v;
            std::memcpy(&v, (char*)obj + 8, 8);
            gc_trace_child(v, worklist);
        }
    } else if (tid == 4) {
        // RT_T_DICT：rt_dict { rt_kv** buckets@0; int32 n_buckets@8; int32 size@12; }
        if (m.size < 16) return;
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
        if (m.size < 16) return;
        void* env = nullptr;
        std::memcpy(&env, (char*)obj + 8, 8);
        gc_trace_child(env, worklist);
    } else if (tid >= 100) {
        // RT_T_USER：类型表 bitmap 精确追踪；未注册/大对象 → 保守扫描
        std::lock_guard<std::mutex> lk(g_gc_types_mtx);
        if ((size_t)tid < g_gc_types.size() && g_gc_types[(size_t)tid].size > 0) {
            int64_t sz = g_gc_types[(size_t)tid].size;
            int64_t bm = g_gc_types[(size_t)tid].bitmap;
            if (m.size > 512) {
                char* base = reinterpret_cast<char*>(obj);
                for (int64_t off = 0; off + 8 <= m.size; off += 8) {
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
            for (int64_t off = 0; off + 8 <= m.size; off += 8) {
                void* cand;
                std::memcpy(&cand, base + off, 8);
                gc_trace_child(cand, worklist);
            }
        }
    }
    // tid == 1（RT_T_STRING）及其它：不扫描
}

// Internal: run finalizers for dead objects, then free them.
// Objects with finalizers that haven't run yet get one extra cycle (resurrection).
// 单次 for_each 收集 to_free/resurrected（存 GCMeta* 免去 free 阶段二次哈希查找；
// erase_entry 按指针擦除）。sweep 期间无插入 → 无 rebuild，指针稳定。
static int64_t gc_sweep_dead(bool is_minor) {
    int64_t freed = 0;

    // ── Finalizer pass: run finalizers BEFORE freeing ──
    struct ToFree { void* obj; GCMeta* m; };
    std::vector<ToFree> to_free;
    std::vector<ToFree> resurrected;  // objects waiting for finalizer, get one more cycle

    g_gc_meta.for_each([&](void* key, GCMeta& m) {
        if (is_minor && m.gen != 0) return;  // minor GC: only sweep young-gen
        if (m.marked.load(std::memory_order_relaxed)) return;

        // Run finalizer if registered and not yet finalized.
        if (m.finalizer_fn && !m.finalized) {
            if (g_gc_log_on)
                fprintf(stderr, "[GCLOG] finalize #%lld %p kind=%d\n",
                        (long long)g_gc_collect_count, key, m.kind);
            m.finalized = true;
            // Call the finalizer. It may resurrect the object by re-rooting it.
            m.finalizer_fn(m.finalizer_data ? m.finalizer_data : key);
            // Give finalized objects one more cycle — they'll be freed next
            // collect if not resurrected by the finalizer.
            resurrected.push_back({key, &m});
        } else {
            // No finalizer (or already finalized) → free immediately.
            to_free.push_back({key, &m});
        }
    });

    // Forensics BEFORE resetting marks / freeing.
    if (g_gc_watch) {
        for (auto& tf : to_free) {
            if (tf.obj == g_gc_watch) gc_dump_referrers(tf.obj);
        }
    }

    // Free dead objects.
    for (auto& tf : to_free) {
        void* obj = tf.obj;
        GCMeta* m = tf.m;
        if (getenv("SHADOW_GC_SWEEP_DBG")) {
            // 诊断：被 free 对象是否仍被全局根直接引用（漏标证据）
            int refd = 0;
            for (auto& gkv : g_gc_global_roots) { if (gkv.second == obj) { refd = 1; break; } }
            if (refd)
                fprintf(stderr, "[sweep-dbg] FREE-ROOTED obj=%p kind=%d size=%lld\n", obj, m->kind, (long long)m->size);
        }
        if (g_gc_log_on)
            fprintf(stderr, "[GCLOG] sweep #%lld free %p kind=%d size=%lld gen=%u\n",
                    (long long)g_gc_collect_count, obj, m->kind, (long long)m->size, m->gen);
        if (m->kind == 0) g_diag_kind0++;
        // 先读 req/size/kind/owned 再 erase：erase 会把 meta 清零，之后
        // m->req 恒为 0 → g_heap_bytes 从不递减 → GC 风暴；m->size 恒为 0
        // → 空闲链表永不压入（fl_push=0）。
        int64_t mreq = m->req > 0 ? m->req : 0;
        int64_t msize = m->size;
        int32_t mkind = m->kind;
        int32_t mowned = m->owned;
        g_gc_meta.erase_entry(m);
        g_heap_bytes.fetch_sub(mreq, std::memory_order_relaxed);
        // Clear from remembered set if present。major GC 结束时整体 clear（全量重标
        // 不需要 remembered），这里跳过省 9M+ 次哈希擦除；minor GC 必须逐对象摘。
        if (is_minor) g_gc_remembered.erase(obj);
        // Clean up global roots that still point to this freed object.
        for (auto git = g_gc_global_roots.begin(); git != g_gc_global_roots.end(); ) {
            if (git->second == obj) git = g_gc_global_roots.erase(git);
            else ++git;
        }
        // 释放策略（对齐 Windows type_id 语义下的对象来源）：
        //   kind==1：RFS ShadowArray（new + register）→ delete
        //   kind==3 && !owned：RFS AnyBox（new + register）→ delete
        //   其它（用户产物全部 alloc：C 布局数组/结构体/closure/box/字符串）→ free
        //   单 mutator 时后者改走空闲链表复用（免 free/malloc 往返）。
        if (mkind == 1) {
            delete reinterpret_cast<ShadowArray*>(obj);
        } else if (mkind == 3 && !mowned) {
            delete reinterpret_cast<AnyBox*>(obj);
        } else if (msize >= 16 && msize <= 65536 && fl_enabled() && g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
            fl_push(obj, (int32_t)msize);
        } else {
            free(obj);
        }
        freed++;
    }

    // Resurrected objects: clear finalized flag won't help — they'll be
    // re-checked next cycle. If still unrooted, they'll be freed then.
    // Mark them so they survive THIS sweep (they were already in `dead`).
    for (auto& rf : resurrected) {
        rf.m->marked.store(1, std::memory_order_relaxed);
    }

    // 清扫后活条目远小于容量时收缩表（缓存驻留优化，见 GCMetaMap::shrink_if_sparse）。
    // 此时 to_free 的 GCMeta* 已不再使用，rehash 安全。
    g_gc_meta.shrink_if_sparse();

    return freed;
}

// ── Major GC: full mark-sweep over ALL objects (both generations) ──
extern "C" int64_t shadow_gc_collect() {
    if (g_gc_disabled) return 0;
    // P20-5: debug trace must honour g_gc_log_on — an unconditional fprintf here
    // pollutes the LSP server's stderr on every collect.
    if (g_gc_log_on) { fprintf(stderr, "[GC] major START meta=%zu\n", g_gc_meta.size()); fflush(stderr); }
    gc_lock();  // lock-free spinlock: prevent concurrent collect
    g_gc_running.store(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_gc_mutex);   // serialize GC metadata/root access
    gc_flush_pending_locked();   // 攒批对象先入表，标记/清扫才能覆盖它们
    gc_diag_init();
    // ── STW：请求所有其它线程到达安全点，冻结根集（多线程并发下防止
    //    扫根/清扫窗口内其它线程改 roots 或写刚分配对象 → 漏标 → 误回收）──
    gc_stw_begin();
    if (g_gc_log_on)
        fprintf(stderr, "[GCLOG] major collect #%lld meta=%zu perm=%zu global=%zu threads=%zu remembered=%zu conservative=%zu heap=%lld trigger=%lld\n",
                (long long)g_gc_major_count, g_gc_meta.size(), g_gc_perm_roots.size(),
                g_gc_global_roots.size(), g_gc_thread_states.size(), g_gc_remembered.size(),
                g_gc_conservative_regions.size(),
                (long long)g_heap_bytes.load(std::memory_order_relaxed),
                (long long)g_gc_trigger.load(std::memory_order_relaxed));

    // ── Mark phase: trace from all roots ──
    // 三色标记：visited 存 epoch 世代号（collect 开始只 ++epoch，免全量清 visited），
    // marked（存活位）不动。分配即黑（marked=1）的对象保留存活资格，保证「刚分配、
    // 尚未被 root」的对象在并发窗口内不被误回收；同时 trace_child 用 visited 去重，
    // 上一轮存活的对象本轮仍会被重新访问 → 子对象不漏标（修掉全量清 marked 的 UAF）。
    auto _gc_t0 = prof_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto _gc_t0_mark = _gc_t0;
    auto _gc_t1 = _gc_t0;
    uint32_t epoch = g_gc_epoch + 1;
    if (epoch == 0) {
        // epoch 回绕（2^32 次 GC 一次）：全量清 visited 后从 1 重新开始。
        g_gc_meta.for_each([](void*, GCMeta& m) {
            m.visited.store(0, std::memory_order_relaxed);
        });
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
    // Conservative scan: trace pointer-sized values in registered memory regions
    // (e.g., process data segment for binaries without explicit GC root registration).
    gc_trace_conservative_regions(worklist);
    gc_scan_stack(worklist);
    gc_scan_process_heap(worklist);
    if (prof_on()) {
        g_prof_mark_roots_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t1).count();
        _gc_t1 = std::chrono::steady_clock::now();
    }

    while (!worklist.empty()) {
        void* obj = worklist.back(); worklist.pop_back();
        GCMeta* m = g_gc_meta.find(obj);
        if (!m) continue;
        gc_trace_object_children(obj, *m, worklist);
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

    // Run finalizers + free dead objects（单次 for_each 收集 + 免哈希擦除）。
    int64_t freed = gc_sweep_dead(false);
    if (prof_on()) {
        g_prof_sweep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        g_prof_gc_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0_mark).count();
        g_prof_gc_count++;
    }

    // ── Promotion + GOGC pacing（合并两次 for_each 遍历）──
    // Promotion: surviving young-gen objects get promoted after threshold。
    // GOGC pacing：本轮 live = 真实可达堆（visited=1 的对象 size 和）。
    // 不能用清扫后的 g_heap_bytes：三色标记下它含"分配即黑"残留（本轮新分配、
    // 下轮才回收的不可达对象）。若用它做 pacing，trigger 每轮放大 → 周期内
    // 分配更多 → 残留更多 → 正反馈 → 谷底单调上涨（ex_gc_longrun min1/min2
    // 漂移的根因）。真实 live 只认本轮从 roots 可达（visited=1）的对象。
    int64_t live = 0;
    g_gc_meta.for_each([&](void*, GCMeta& m) {
        if (m.marked.load(std::memory_order_relaxed)) {
            if (m.gen == 0) {
                m.survived++;
                if (m.survived >= PROMOTION_THRESHOLD) {
                    m.gen = 1;  // promote to old gen
                }
            }
        }
        if (m.visited.load(std::memory_order_relaxed) == g_gc_epoch) live += m.req;
        // Reset marks for next cycle.
        m.marked.store(0, std::memory_order_relaxed);
    });
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
    if (g_gc_log_on) { fprintf(stderr, "[GC] minor START meta=%zu\n", g_gc_meta.size()); fflush(stderr); }
    gc_lock();
    std::lock_guard<std::mutex> lk(g_gc_mutex);   // serialize GC metadata/root access
    gc_flush_pending_locked();   // 攒批对象先入表，标记/清扫才能覆盖它们
    gc_diag_init();
    gc_stw_begin();
    if (g_gc_log_on)
        fprintf(stderr, "[GCLOG] minor collect #%lld young=%zu remembered=%zu\n",
                (long long)g_gc_minor_count, g_gc_meta.size(), g_gc_remembered.size());

    // ── Mark phase: trace from roots, but only mark young-gen objects ──
    // Old-gen objects are treated as alive (not swept in minor GC).
    // The remembered set provides old→young references as additional roots.
    // 三色标记：visited 存 epoch 世代号（只 ++epoch 不清全表），marked（存活位）
    // 不动 → 分配即黑的对象保留存活资格，防「刚分配未 root 即被 minor sweep」。
    auto _gc_t0 = prof_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto _gc_t0_mark = _gc_t0;
    uint32_t epoch = g_gc_epoch + 1;
    if (epoch == 0) {
        g_gc_meta.for_each([](void*, GCMeta& m) {
            m.visited.store(0, std::memory_order_relaxed);
        });
        epoch = 1;
    }
    g_gc_epoch = epoch;
    std::vector<void*> worklist;

    // Mark all old-gen objects as alive (they survive minor GC).
    g_gc_meta.for_each([](void*, GCMeta& m) {
        if (m.gen == 1) {
            m.marked.store(1, std::memory_order_relaxed);
        }
    });

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
        GCMeta* m = g_gc_meta.find(parent);
        if (!m) continue;
        gc_trace_object_children(parent, *m, worklist);
    }

    // Expand worklist: trace children of marked young-gen objects.
    while (!worklist.empty()) {
        void* obj = worklist.back(); worklist.pop_back();
        GCMeta* m = g_gc_meta.find(obj);
        if (!m) continue;
        gc_trace_object_children(obj, *m, worklist);
    }
    if (prof_on()) {
        g_prof_mark_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        _gc_t0_mark = _gc_t0;
        _gc_t0 = std::chrono::steady_clock::now();
    }

    // ── Sweep phase: only sweep young-gen objects that are unmarked ──
    g_gc_collect_count++;
    g_gc_minor_count++;

    // Run finalizers + free dead young-gen objects（单次 for_each 收集 + 免哈希擦除）。
    int64_t freed = gc_sweep_dead(true);
    if (prof_on()) {
        g_prof_sweep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0).count();
        g_prof_gc_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _gc_t0_mark).count();
        g_prof_gc_count++;
    }

    // ── Promotion: surviving young-gen objects get promoted after threshold ──
    g_gc_meta.for_each([](void* key, GCMeta& m) {
        if (m.gen == 0 && m.marked.load(std::memory_order_relaxed)) {
            m.survived++;
            if (m.survived >= PROMOTION_THRESHOLD) {
                m.gen = 1;  // promote to old gen
                if (g_gc_log_on)
                    fprintf(stderr, "[GCLOG] promote %p to old gen (survived %u)\n",
                            key, m.survived);
            }
        }
        // Reset marks for next cycle.
        m.marked.store(0, std::memory_order_relaxed);
    });

    gc_stw_end();
    gc_unlock();
    return freed;
}

// Register a finalizer callback for a GC-managed object.
// The finalizer fn(void* data) will be called BEFORE the object is freed.
// If the finalizer re-roots the object (adds it back to a root set),
// the object survives this cycle (resurrection).
extern "C" void shadow_gc_set_finalizer(void* ptr, void(*fn)(void*), void* data) {
    if (!ptr || !fn) return;
    GCMeta* m = g_gc_meta.find(ptr);
    if (!m) return;
    m->finalizer_fn = fn;
    m->finalizer_data = data;
}

// GC 登记对象删除辅助：shadow_free 在前部调用（GC 状态定义在后部）。
// 返回 1 = ptr 是 GC 登记对象（已从 meta/remembered 删除，调用方负责 free）；
// 返回 0 = 非 GC 对象（调用方走原 type_tag 逻辑）。
extern "C" int32_t shadow_gc_forget(void* ptr) {
    if (!ptr) return 0;
    // 攒批对象尚未入表：从 pending 摘除（调用方随后 free）。
    for (int i = 0; i < g_pend_n; i++) {
        if (g_pend[i].p == ptr) {
            g_pend_bytes -= g_pend[i].req;
            g_pend[i] = g_pend[g_pend_n - 1];
            g_pend_n--;
            return 1;
        }
    }
    gc_lock_blocked();
    GCMeta* m = g_gc_meta.find(ptr);
    if (!m) { g_gc_mutex.unlock(); return 0; }
    g_heap_bytes.fetch_sub(m->req > 0 ? m->req : 0, std::memory_order_relaxed);
    g_gc_meta.erase(ptr);
    g_gc_remembered.erase(ptr);
    g_gc_mutex.unlock();
    return 1;
}

// 诊断：指针是否在 GC meta 中（0=已回收/从未登记）
extern "C" int32_t shadow_gc_meta_contains(void* p) {
    if (!p) return 0;
    for (int i = 0; i < g_pend_n; i++) {
        if (g_pend[i].p == p) return 1;
    }
    gc_lock_blocked();
    int r = g_gc_meta.find(p) ? 1 : 0;
    g_gc_mutex.unlock();
    return r;
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
    // 触发检查：heap 记账含 pending 字节（攒批对象尚未入表）。
    if (g_gc_disabled == 0 && rt_gc_auto_on() && g_gc_running.load(std::memory_order_relaxed) == 0) {
        int32_t stress = rt_gc_stress_n();
        int64_t want = 0;
        if (stress > 0) {
            int64_t t = g_alloc_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((t % (int64_t)stress) == 0) want = 1;
        } else {
            int64_t hb = g_heap_bytes.load(std::memory_order_relaxed) + g_pend_bytes;
            if (hb >= g_gc_trigger.load(std::memory_order_relaxed) && hb > 0) want = 1;
        }
        if (want) {
            g_gc_poll_flag.store(1, std::memory_order_relaxed);  // 提示 poll 兜底复查
            gc_lock_blocked();
            gc_flush_pending_locked();
            g_gc_mutex.unlock();
            shadow_gc_collect();
        }
    }
    // 空闲链表快路径：单 mutator 时优先复用已死对象（免 malloc）。
    void* p = nullptr;
    if (fl_enabled() && asize >= 16 && asize <= 65536 &&
        g_mutator_threads.load(std::memory_order_relaxed) <= 1) {
        p = fl_pop(asize);
    }
    if (!p) {
        p = malloc((size_t)asize);
        if (!p) return nullptr;
    }
    // 攒批快路径：单 mutator + GC 关闭 + 缓冲未满 → 无锁无哈希，登记推迟到 flush。
    if (g_gc_disabled == 0 && rt_gc_auto_on() &&
        g_mutator_threads.load(std::memory_order_relaxed) <= 1 &&
        g_gc_running.load(std::memory_order_relaxed) == 0 &&
        g_pend_n < RT_PEND_CAP) {
        g_pend[g_pend_n].p = p;
        g_pend[g_pend_n].kind = kind;
        g_pend[g_pend_n].asize = asize;
        g_pend[g_pend_n].req = size;
        g_pend_n++;
        g_pend_bytes += size;
        tl_gc_alloc_count++;
        g_gc_total_alloc_count.fetch_add(1, std::memory_order_relaxed);
        g_dbg_fast++;
        return p;
    }
    g_dbg_slow++;
    {
        auto _t0 = prof_on() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        // 等锁期间标记 blocked（stw_state=2）：collect 扫根时跳过本线程
        // （等锁 = 不跑 mutator，根集冻结）。对齐 Windows「阻塞在 GC_LOCK
        // 上的线程不会被等待」的契约。
        ThreadGCState* s = gc_get_thread_state();
        while (!g_gc_mutex.try_lock()) {
            s->stw_state.store(2, std::memory_order_release);
            std::this_thread::yield();
        }
        s->stw_state.store(0, std::memory_order_release);
        gc_flush_pending_locked();
        g_gc_meta.insert(p, kind, asize, size);
        g_heap_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
        g_gc_mutex.unlock();
        if (prof_on()) {
            g_prof_alloc_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - _t0).count();
            g_prof_meta_ops++;
            int64_t sz = (int64_t)g_gc_meta.size();
            if (sz > g_prof_meta_peak) g_prof_meta_peak = sz;
        }
    }
    // Track allocation counts for diagnostics.
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
    if (!ptr) return;
    ThreadGCState* s = gc_get_thread_state();
    while (!g_gc_mutex.try_lock()) {
        s->stw_state.store(2, std::memory_order_release);
        std::this_thread::yield();
    }
    s->stw_state.store(0, std::memory_order_release);
    GCMeta& m = g_gc_meta[ptr];
    m.kind = kind;
    m.owned = 0;
    m.size = size;
    m.req = size;
    if (size > 0) g_heap_bytes.fetch_add(size, std::memory_order_relaxed);
    g_gc_mutex.unlock();
}

// ── Root management (LIFO frame discipline) ──
// These are called from generated IR; signatures use int32_t to match the
// codegen's i32 convention. The frame marker is a root-count snapshot.
extern "C" int32_t shadow_gc_frame_enter() {
    ThreadGCState* s = gc_get_thread_state();
    std::lock_guard<std::mutex> lk(s->mtx);
    s->named_frames.push_back({});
    s->range_frames.push_back({});
    return (int32_t)s->roots.size();
}
extern "C" int32_t shadow_gc_root_add(void* ptr) {
    ThreadGCState* s = gc_get_thread_state();
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
    if (!s->range_frames.empty()) {
        auto& fr = s->range_frames.back();
        // Remove the frame's ranges from the flattened scan list.
        for (auto& rg : fr) {
            for (size_t i = 0; i < s->range_roots.size(); i++) {
                if (s->range_roots[i] == rg) {
                    s->range_roots[i] = s->range_roots.back();
                    s->range_roots.pop_back();
                    break;
                }
            }
        }
        s->range_frames.pop_back();
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
    std::lock_guard<std::mutex> lk(s->mtx);
    s->range_frames.back().push_back({base, n});
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


