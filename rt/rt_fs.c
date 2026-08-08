/* ============================================================
 * shadow-0.4 rt/ — 文件 / 目录 / 路径（rt_fs.c）
 * 仅 Windows API + MSVCRT 最小部分。符号对应 runtime_lib.shadow
 * @extern 与 codegen 引用的 shadow_* 缺失符号。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern void* __rt_shadow_malloc(int32_t n);
static char* rt_dup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)__rt_shadow_malloc((int32_t)(n + 1));
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* ---------------- 文件读写 ---------------- */
extern const char* shadow_read_file(const char* path) {
    FILE* f;
    long len;
    char* buf;
    size_t read_len;
    if (!path) return rt_dup("");
    f = fopen(path, "rb");
    if (!f) return rt_dup("");
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return rt_dup(""); }
    read_len = fread(buf, 1, (size_t)len, f);
    buf[read_len] = '\0';
    fclose(f);
    return buf;
}

extern int shadow_write_file(const char* path, const char* content) {
    FILE* f;
    size_t w;
    if (!path || !content) return 0;
    f = fopen(path, "w");
    if (!f) return 0;
    w = fwrite(content, 1, strlen(content), f);
    fclose(f);
    return w == strlen(content) ? 1 : 0;
}

extern int shadow_file_exists(const char* path) {
    FILE* f;
    if (!path) return 0;
    f = fopen(path, "r");
    if (f) { fclose(f); return 1; }
    return 0;
}

extern int shadow_delete_file(const char* path) {
    if (!path) return 0;
    return DeleteFileA(path) ? 1 : 0;
}

extern int shadow_rename_file(const char* old_path, const char* new_path) {
    if (!old_path || !new_path) return 0;
    return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}

/* 分号分隔的目录条目列表（不含 . 与 ..） */
extern const char* shadow_list_dir(const char* path) {
    WIN32_FIND_DATAA fd;
    HANDLE hFind;
    char search[1024];
    char* result;
    size_t cap = 256, used = 0;
    if (!path) return rt_dup("");
    snprintf(search, sizeof(search), "%s\\*", path);
    hFind = FindFirstFileA(search, &fd);
    result = (char*)malloc(cap);
    if (!result) return rt_dup("");
    result[0] = '\0';
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0) {
                size_t n = strlen(fd.cFileName);
                if (used + n + 2 > cap) {
                    cap = cap * 2 + 64;
                    result = (char*)realloc(result, cap);
                    if (!result) { FindClose(hFind); return rt_dup(""); }
                }
                if (used > 0) { result[used++] = ';'; }
                memcpy(result + used, fd.cFileName, n);
                used += n;
                result[used] = '\0';
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    return result;
}

extern int shadow_mkdir(const char* path) {
    if (!path) return 0;
    return CreateDirectoryA(path, NULL) ? 1 : 0;
}

extern int32_t shadow_rmdir(const char* path) {
    if (!path) return 0;
    return RemoveDirectoryA(path) ? 1 : 0;
}

extern int shadow_path_exists(const char* path) {
    if (!path) return 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
}

/* ---------------- 路径工具（统一 / 分隔符） ---------------- */
static void rt_to_slashes(char* p) {
    while (*p) {
        if (*p == '\\') *p = '/';
        p++;
    }
}
static int rt_path_abs(const char* p) {
    return (p[0] == '/') || (p[1] == ':' && p[2] == '/');
}

extern const char* shadow_path_normalize(const char* path) {
    char* p;
    if (!path) return rt_dup("");
    p = rt_dup(path);
    if (p) rt_to_slashes(p);
    return p;
}

extern const char* shadow_path_join(const char* base, const char* child) {
    char buf[2048];
    char* b;
    char* c;
    size_t bl;
    if (!child || child[0] == '\0') return rt_dup(base ? base : "");
    if (rt_path_abs(child)) return rt_dup(child);
    if (!base || base[0] == '\0') return rt_dup(child);
    b = rt_dup(base);
    c = rt_dup(child);
    if (b) rt_to_slashes(b);
    if (c) rt_to_slashes(c);
    bl = strlen(b);
    while (bl > 0 && b[bl - 1] == '/') { b[bl - 1] = '\0'; bl--; }
    while (c && c[0] == '/') c++;
    snprintf(buf, sizeof(buf), "%s/%s", b, c);
    return rt_dup(buf);
}

extern const char* shadow_path_dirname(const char* path) {
    char* p;
    size_t n;
    if (!path || path[0] == '\0') return rt_dup(".");
    p = rt_dup(path);
    if (p) rt_to_slashes(p);
    n = strlen(p);
    while (n > 1 && p[n - 1] == '/') { p[n - 1] = '\0'; n--; }
    {
        long pos = -1;
        long i;
        for (i = (long)n - 1; i >= 0; i--) {
            if (p[i] == '/') { pos = i; break; }
        }
        if (pos < 0) return rt_dup(".");
        if (pos == 0) return rt_dup("/");
        if (pos == 2 && p[1] == ':') {
            char d[4] = { p[0], p[1], p[2], '\0' };
            return rt_dup(d);
        }
        p[pos] = '\0';
        return rt_dup(p);
    }
}

extern const char* shadow_path_basename(const char* path) {
    char* p;
    size_t n;
    long pos = -1;
    long i;
    if (!path || path[0] == '\0') return rt_dup("");
    p = rt_dup(path);
    if (p) rt_to_slashes(p);
    n = strlen(p);
    while (n > 1 && p[n - 1] == '/') { p[n - 1] = '\0'; n--; }
    for (i = (long)n - 1; i >= 0; i--) {
        if (p[i] == '/') { pos = i; break; }
    }
    if (pos < 0) return rt_dup(p);
    return rt_dup(p + pos + 1);
}

/* ---------------- .lu 序列化原语（rt_*_bytes，二进制） ---------------- */
extern int32_t rt_write_bytes(const char* path, void* p, int32_t n) {
    FILE* f;
    size_t written;
    if (!path || !p || n < 0) return 0;
    f = fopen(path, "wb");
    if (!f) return 0;
    written = fwrite(p, 1, (size_t)n, f);
    fclose(f);
    return written == (size_t)n ? 1 : 0;
}

/* 读整个文件为字节缓冲（rt_buf_alloc 布局，len 可查） */
extern void* rt_read_bytes(const char* path) {
    extern void* rt_buf_alloc(int32_t n);
    FILE* f;
    long len;
    void* buf;
    size_t read_len;
    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    buf = rt_buf_alloc((int32_t)len);
    if (!buf) { fclose(f); return NULL; }
    read_len = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_len != (size_t)len) { return NULL; }
    return buf;
}

/* 附加：copy_file / file_mtime（0.3 兼容） */
extern int shadow_copy_file(const char* src, const char* dst) {
    if (!src || !dst) return 0;
    return CopyFileA(src, dst, FALSE) ? 1 : 0;
}

extern int32_t shadow_file_mtime(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!path) return -1;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fd)) return -1;
    {
        FILETIME ft = fd.ftLastWriteTime;
        ULARGE_INTEGER ul;
        ul.LowPart = ft.dwLowDateTime;
        ul.HighPart = ft.dwHighDateTime;
        /* 1601→1970 偏移（100ns 单位） */
        return (int32_t)((ul.QuadPart / 10000000ULL) - 11644473600ULL);
    }
}
