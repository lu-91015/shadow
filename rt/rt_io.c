/* ============================================================
 * shadow-0.4 rt/ — LSP stdio/JSON FFI（rt_io.c）
 * ------------------------------------------------------------
 * Phase 14（LSP 服务器）新增的 C 层原语：
 *   shadow_stdin_read_line   —— 读一行（去掉 \r\n），EOF 返回 ""
 *   shadow_stdin_read_n      —— 精确读 n 字节（Content-Length body）
 *   shadow_stdout_write_raw  —— 原始字节写 stdout（不转换换行）
 *   shadow_dbg_mark          —— stderr 调试日志（未缓冲）
 *   shadow_json_get_raw      —— JSON 对象按 key 取值原始片段
 *   shadow_json_array_get    —— JSON 数组按下标取元素原始片段
 *   shadow_json_array_len    —— JSON 数组元素个数
 *   shadow_file_mtime        —— 文件修改时间（已存在于 rt_fs.c，这里兜底声明）
 *
 * 字符串约定：返回 char*（i8*），用 __rt_shadow_malloc 分配（GC 可追踪），
 * 与 0.4 Shadow 侧 string 表示一致。
 *
 * JSON 提取语义（与 0.3 shadow_json_get_raw 对齐）：
 *   - 对象/数组值  返回原始 {...}/[...] 片段
 *   - 字符串值     剥掉首尾引号，内部转义（\n 等）原样保留
 *   - 数字/bool/null 返回原样文本
 * 只依赖 MSVCRT + Windows API，禁止 STL/第三方库（原则 3）。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <io.h>      /* _setmode / _fileno */
#include <fcntl.h>   /* _O_BINARY */

/* ---- 二进制模式切换 + 分配 ---- */

static int rt_io_bin_ready = 0;

static void rt_io_ensure_bin(void) {
    if (rt_io_bin_ready) return;
    /* 关键：stdin/stdout 切二进制模式，保证 Content-Length 分帧精确字节 */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    rt_io_bin_ready = 1;
}

extern void* __rt_shadow_malloc(int32_t n);

static char* rt_io_strndup(const char* s, int32_t n) {
    char* p = (char*)__rt_shadow_malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

static char* rt_io_strdup(const char* s) {
    if (!s) s = "";
    return rt_io_strndup(s, (int32_t)strlen(s));
}

/* ---- stdin ---- */

/* 读一行（到 \n 或 EOF），返回去掉尾部 \r\n 的内容；EOF 且无内容返回 "" */
extern const char* shadow_stdin_read_line(void) {
    rt_io_ensure_bin();
    static char linebuf[65536];
    int32_t n = 0;
    int c;
    while (n < (int32_t)sizeof(linebuf) - 1) {
        c = fgetc(stdin);
        if (c == EOF) break;
        if (c == '\n') break;
        linebuf[n++] = (char)c;
    }
    /* 去掉尾部 \r */
    if (n > 0 && linebuf[n - 1] == '\r') n--;
    if (n == 0 && c == EOF) return "";
    return rt_io_strndup(linebuf, n);
}

/* 精确读 n 字节（EOF 提前结束则只返回已读部分） */
extern const char* shadow_stdin_read_n(int32_t n) {
    rt_io_ensure_bin();
    if (n <= 0) return "";
    char* buf = (char*)malloc((size_t)n);
    if (!buf) return "";
    int32_t got = 0;
    while (got < n) {
        size_t r = fread(buf + got, 1, (size_t)(n - got), stdin);
        if (r == 0) break;
        got += (int32_t)r;
    }
    if (got == 0) { free(buf); return ""; }
    char* out = rt_io_strndup(buf, got);
    free(buf);
    return out;
}

/* ---- stdout / stderr ---- */

/* 原始字节写 stdout（不追加、不转换换行） */
extern void shadow_stdout_write_raw(const char* s) {
    if (!s) return;
    rt_io_ensure_bin();
    size_t n = strlen(s);
    if (n == 0) return;
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}

/* stderr 调试日志（未缓冲，崩溃时仍存活） */
extern void shadow_dbg_mark(const char* s) {
    if (!s) return;
    fprintf(stderr, "%s\n", s);
    fflush(stderr);
}

/* ---- JSON 提取（最小实现，仅遍历不校验） ---- */

static const char* rt_json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* 在 json 对象文本中定位 "key" 的值起始位置；未找到返回 NULL */
static const char* rt_json_find_key(const char* json, const char* key) {
    int32_t klen = (int32_t)strlen(key);
    if (klen == 0) return NULL;
    const char* p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* 边界检查：p 前必须是 '"'，p+klen 后必须是 '"' ':' */
        if (p == json || p[-1] == '"') {
            const char* after = p + klen;
            if (*after == '"') {
                after++;
                after = rt_json_skip_ws(after);
                if (*after == ':') {
                    return rt_json_skip_ws(after + 1);
                }
            }
        }
        p = p + klen;
    }
    return NULL;
}

/* 从值起始位置扫描，返回值结束位置（不含）；遇字符串处理转义与嵌套 */
static const char* rt_json_scan_value(const char* start) {
    char c = *start;
    if (c == '"') {
        const char* p = start + 1;
        while (*p) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') return p + 1;
            p++;
        }
        return p; /* 未闭合 */
    }
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        const char* p = start;
        while (*p) {
            if (*p == '"') {
                /* 跳过字符串（含转义） */
                p++;
                while (*p) {
                    if (*p == '\\') { p += 2; continue; }
                    if (*p == '"') break;
                    p++;
                }
                p++;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    /* 标量：到逗号 / } / ] 或空白为止 */
    {
        const char* p = start;
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
               *p != '\t' && *p != '\n' && *p != '\r') p++;
        return p;
    }
}

/* 取 JSON 字符串叶子值：剥首尾引号（内部转义保留） */
static char* rt_json_extract_string(const char* start, const char* end) {
    /* start 指向 '"'，end 指向结束引号后 */
    int32_t n = (int32_t)(end - start);
    if (n >= 2 && start[0] == '"' && end[-1] == '"') {
        return rt_io_strndup(start + 1, n - 2);
    }
    return rt_io_strndup(start, (int32_t)(end - start));
}

/* 对象按 key 取值原始片段 */
extern const char* shadow_json_get_raw(const char* json, const char* key) {
    if (!json || !key) return "";
    const char* start = rt_json_find_key(json, key);
    if (!start) return "";
    const char* end = rt_json_scan_value(start);
    if (end <= start) return "";
    if (*start == '"') {
        return rt_json_extract_string(start, end);
    }
    return rt_io_strndup(start, (int32_t)(end - start));
}

/* 数组按下标取元素原始片段；json 需是 [...] */
extern const char* shadow_json_array_get(const char* json, int32_t idx) {
    if (!json) return "";
    const char* p = rt_json_skip_ws(json);
    if (*p != '[') return "";
    p++;
    int32_t cur = 0;
    while (1) {
        p = rt_json_skip_ws(p);
        if (*p == ']') return "";   /* 越界 */
        if (*p == ',') { p++; continue; }
        /* 元素起始 */
        const char* start = p;
        const char* end = rt_json_scan_value(start);
        if (cur == idx) {
            if (*start == '"') return rt_json_extract_string(start, end);
            return rt_io_strndup(start, (int32_t)(end - start));
        }
        cur++;
        p = end;
    }
}

/* 数组元素个数；json 需是 [...] */
extern int32_t shadow_json_array_len(const char* json) {
    if (!json) return 0;
    const char* p = rt_json_skip_ws(json);
    if (*p != '[') return 0;
    p++;
    int32_t n = 0;
    while (1) {
        p = rt_json_skip_ws(p);
        if (*p == ']') return n;
        if (*p == ',') { p++; continue; }
        p = rt_json_scan_value(p);
        n++;
    }
}

/* ---- 文件 mtime ---- */
/* 说明：shadow_file_mtime 已由 rt_fs.c 定义，本文件不重复定义。
 * runtime_lib.shadow / compile_api 直接调用（codegen 映射 shadow_file_mtime）。 */
