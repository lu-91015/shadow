/* ============================================================
 * shadow-0.5 rt/ — 轻量符号索引器（shadow_index.c）
 *
 * 目的：为 LSP 提供「免全量类型检查」的符号表快路径。
 *
 * 背景（实测）：LSP 每次编辑都会跑 api_load_combined + api_check（tok+parse+tc）。
 * 在编译器自身源码（src/lsp/lsp.shadow，combined 185,477 tokens）上单次耗时
 * **355.7 秒**。而符号表（fmeta/smeta/emeta）所需的信息——函数名、签名、返回类型、
 * 行列区间、is_pub、所属模块——**全部写在源码字面上**，无需类型推导即可抽取。
 *
 * 本文件用纯 C 词法扫描复刻 main_load_combined 的合并坐标布局，直接产出与
 * api_fmeta/api_smeta/api_emeta 同格式的字符串，耗时降到毫秒级。
 *
 * 复刻的契约（逐条对齐 src/main.shadow）：
 *   - main_load_combined：主文件（keep_dsb=1）→ DFS import（keep_dsb=0）
 *     → runtime_lib.shadow（keep_dsb=0，命名空间 shadow.runtime）最后追加；
 *     main_parts 每元素 = 一行，合并坐标 = 该数组下标 + 1（1-based）。
 *   - main_collect_module：模块内容先追加、再递归 import（DFS 后序）；
 *     path_stack 命中直接返回成功（循环 import 合法）；compiled 命中跳过（菱形依赖）。
 *   - main_resolve_import：① project_root 文件/目录 ②[deps]（本实现不支持→兜底）
 *     ②.5 std 根回退 ③失败。main_collect_dir 目录项按 strcmp 升序（fixed-point 确定性）。
 *   - fmeta = name|L|C|EL|EC|rettype|sig|is_pub|module
 *       L/C   = `kimo` 关键字的 1-based 行/列（parse_function_inner 的 mk_loc 位置）
 *       EL/EC = 函数体右花括号之后的位置（token 区间左闭右开，end_col 为末字符列+1）
 *       module= tc_mod_of(mangle 名)：仅非豁免符号带模块名，豁免（extern / main /
 *               __impl__* / shadow_* / rt_*）一律为 ""
 *   - smeta / emeta = name|L|C|EL|EC
 *
 * 输出以 "\n" 连接（不是 ";"）：sig 里可能出现 ';' 的场景虽少，但换行绝不会出现。
 *
 * 无法可靠解析的情形（[deps]/SPK 依赖、目录读失败、文件读失败）一律置 g_ok=0，
 * 由 shadow 侧退回全量 api_check —— 索引器只做「能确定的快路径」，绝不猜。
 *
 * 暴露给 shadow（@extern）：
 *   shadow_index_build(entry_path, entry_src, arg0) -> int   // 1=可用, 0=需兜底
 *   shadow_index_fmeta() / _smeta() / _emeta() -> const char*
 *   shadow_index_ok() -> int
 *   shadow_index_reset() -> void
 *
 * 独立自测（不需自举，秒级迭代）：
 *   clang -DSHADOW_INDEX_STANDALONE -O1 rt/shadow_index.c -o build/cindex.exe
 *   ./build/cindex.exe src/lsp/lsp.shadow build/shadow.exe
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* 返回给 shadow 的字符串必须由宿主分配器分配（GC 可见）。
   独立自测模式下没有 shadow 运行时，退化为 malloc。 */
#ifdef SHADOW_INDEX_STANDALONE
static void* sx_host_alloc(int32_t n) { return malloc((size_t)n); }
#else
extern void* __rt_shadow_malloc(int32_t n);
static void* sx_host_alloc(int32_t n) { return __rt_shadow_malloc(n); }
#endif

static const char* sx_host_dup(const char* s) {
    size_t n;
    char* p;
    if (!s) s = "";
    n = strlen(s);
    p = (char*)sx_host_alloc((int32_t)(n + 1));
    if (!p) return "";
    memcpy(p, s, n + 1);
    return p;
}

/* ---------------- 动态字符串（内部用，malloc） ---------------- */
typedef struct { char* p; int len; int cap; } Buf;

static void buf_init(Buf* b) { b->p = NULL; b->len = 0; b->cap = 0; }
static void buf_free(Buf* b) { if (b->p) free(b->p); b->p = NULL; b->len = 0; b->cap = 0; }
static void buf_clear(Buf* b) { b->len = 0; if (b->p) b->p[0] = 0; }

static int buf_reserve(Buf* b, int extra) {
    int ncap;
    char* np;
    if (b->cap > 0 && b->len + extra + 1 <= b->cap) return 1;
    ncap = b->cap > 0 ? b->cap : 256;
    while (ncap < b->len + extra + 1) {
        if (ncap > (1 << 30)) return 0;
        ncap *= 2;
    }
    np = (char*)realloc(b->p, (size_t)ncap);
    if (!np) return 0;
    b->p = np; b->cap = ncap;
    if (b->len == 0) b->p[0] = 0;
    return 1;
}
static void buf_putn(Buf* b, const char* s, int n) {
    if (!s || n <= 0) return;
    if (!buf_reserve(b, n)) return;
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n; b->p[b->len] = 0;
}
static void buf_puts(Buf* b, const char* s) { if (s) buf_putn(b, s, (int)strlen(s)); }
static void buf_putc(Buf* b, char c) { buf_putn(b, &c, 1); }
static void buf_puti(Buf* b, int v) { char t[32]; sprintf(t, "%d", v); buf_puts(b, t); }
static const char* buf_cstr(Buf* b) { return b->p ? b->p : ""; }

/* ---------------- 字符串列表 ---------------- */
typedef struct { char** v; int n; int cap; } SList;

static void sl_init(SList* l) { l->v = NULL; l->n = 0; l->cap = 0; }
static void sl_free(SList* l) {
    int i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL; l->n = 0; l->cap = 0;
}
static int sl_push(SList* l, const char* s) {
    size_t n;
    char* c;
    if (l->n >= l->cap) {
        int ncap = l->cap > 0 ? l->cap * 2 : 16;
        char** nv = (char**)realloc(l->v, sizeof(char*) * (size_t)ncap);
        if (!nv) return 0;
        l->v = nv; l->cap = ncap;
    }
    n = strlen(s ? s : "");
    c = (char*)malloc(n + 1);
    if (!c) return 0;
    memcpy(c, s ? s : "", n + 1);
    l->v[l->n++] = c;
    return 1;
}
static int sl_has(SList* l, const char* s) {
    int i;
    for (i = 0; i < l->n; i++) if (strcmp(l->v[i], s) == 0) return 1;
    return 0;
}
static void sl_pop(SList* l) { if (l->n > 0) { free(l->v[l->n - 1]); l->n--; } }

/* ---------------- 全局状态 ---------------- */
static int  g_ok = 0;
static int  g_combined = 0;        /* 已追加的 combined 行数（== len(main_parts)） */
/* 主模块（combined 中第一个追加的模块）在 combined 内的结束行。
   对齐 tc_main_module_end() == tc_g_mod_starts[1] - 1：模块图为前序追加
   （先自身、后依赖），故第一个 append 结束时的 g_combined 即该值。
   LSP 用它把符号计数过滤到主文件（api_main_end）。 */
static int  g_main_end = 0;
static Buf  g_fmeta, g_smeta, g_emeta, g_gmeta;
static char g_std_root[1024];
static SList g_compiled;           /* main_compiled */
static SList g_stack;              /* main_path_stack */
static SList g_dep_names;          /* shadow.sbg [deps] 的键名（main_sbg_deps_names） */
static int  g_depth_guard = 0;     /* 递归深度上限，防病态输入 */
static int  g_inited = 0;

/* ---------------- 基础工具 ---------------- */
static int sx_is_file(const char* path) {
    DWORD a;
    if (!path || !path[0]) return 0;
    a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
}
static char* sx_read_file(const char* path) {
    FILE* f;
    long sz;
    char* buf;
    size_t rd;
    if (!path || !path[0]) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}
/* main_path_join：任一侧为空时不产生分隔符 */
static void sx_join(const char* root, const char* rel, char* out, int n) {
    if (!root || !root[0]) { snprintf(out, (size_t)n, "%s", rel ? rel : ""); return; }
    if (!rel || !rel[0]) { snprintf(out, (size_t)n, "%s", root); return; }
    snprintf(out, (size_t)n, "%s\\%s", root, rel);
}
/* main_exe_dir：无分隔符返回 "" */
static void sx_dir_of(const char* path, char* out, int n) {
    int last = -1, i;
    out[0] = 0;
    if (!path) return;
    for (i = 0; path[i]; i++) if (path[i] == '\\' || path[i] == '/') last = i;
    if (last < 0) return;
    if (last >= n) last = n - 1;
    memcpy(out, path, (size_t)last);
    out[last] = 0;
}
/* main_bslash：/ → \ （原地） */
static void sx_bslash(char* s) { for (; *s; s++) if (*s == '/') *s = '\\'; }

/* main_key_to_ns：core\ast.shadow → core.ast */
static void sx_key_to_ns(const char* key, char* out, int n) {
    int ln, i;
    snprintf(out, (size_t)n, "%s", key ? key : "");
    ln = (int)strlen(out);
    if (ln > 7 && strcmp(out + ln - 7, ".shadow") == 0) out[ln - 7] = 0;
    for (i = 0; out[i]; i++) if (out[i] == '\\') out[i] = '.';
}
/* main_basename_of：a\b\c.shadow → c */
static void sx_basename_of(const char* key, char* out, int n) {
    int ln, i, last = -1;
    snprintf(out, (size_t)n, "%s", key ? key : "");
    ln = (int)strlen(out);
    if (ln > 7 && strcmp(out + ln - 7, ".shadow") == 0) { out[ln - 7] = 0; ln -= 7; }
    for (i = 0; i < ln; i++) if (out[i] == '\\' || out[i] == '/') last = i;
    if (last >= 0) memmove(out, out + last + 1, (size_t)(ln - last));
}
static char* sx_trim_inplace(char* s) {
    char* e;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = 0; e--; }
    return s;
}
/* main_is_dsb_line：trim 后以 "dsb " 开头 */
static int sx_is_dsb_line(const char* line, int len) {
    int i = 0;
    while (i < len && isspace((unsigned char)line[i])) i++;
    if (len - i < 4) return 0;
    return strncmp(line + i, "dsb ", 4) == 0 ? 1 : 0;
}

/* ---------------- 行切分 ---------------- */
typedef struct {
    const char* src;
    int* off;      /* 每行起始偏移 */
    int* len;      /* 每行长度（不含 \n） */
    int  n;
} Lines;

static int lines_split(const char* src, Lines* L) {
    int n = 1, i = 0, k = 0, start = 0;
    const char* p;
    L->src = src; L->off = NULL; L->len = NULL; L->n = 0;
    for (p = src; *p; p++) if (*p == '\n') n++;
    L->off = (int*)malloc(sizeof(int) * (size_t)n);
    L->len = (int*)malloc(sizeof(int) * (size_t)n);
    if (!L->off || !L->len) { free(L->off); free(L->len); L->off = NULL; L->len = NULL; return 0; }
    for (i = 0; ; i++) {
        if (src[i] == '\n' || src[i] == 0) {
            L->off[k] = start;
            L->len[k] = i - start;
            k++;
            start = i + 1;
            if (src[i] == 0) break;
            if (k >= n) break;
        }
    }
    L->n = k;
    return 1;
}
static void lines_free(Lines* L) { free(L->off); free(L->len); L->off = NULL; L->len = NULL; L->n = 0; }

/* ---------------- 扫描游标 ---------------- */
typedef struct {
    const char* s;
    int i;      /* 字节偏移 */
    int li;     /* 0-based 行号 */
    int col;    /* 1-based 字节列 */
} Cur;

static void cur_adv(Cur* c) {
    if (!c->s[c->i]) return;
    if (c->s[c->i] == '\n') { c->li++; c->col = 1; }
    else { c->col++; }
    c->i++;
}

/* 跳过空白与注释（含跨行块注释） */
static void cur_skip_ws(Cur* c) {
    for (;;) {
        char ch = c->s[c->i];
        if (ch == 0) return;
        if (isspace((unsigned char)ch)) { cur_adv(c); continue; }
        if (ch == '/' && c->s[c->i + 1] == '/') {
            while (c->s[c->i] && c->s[c->i] != '\n') cur_adv(c);
            continue;
        }
        if (ch == '/' && c->s[c->i + 1] == '*') {
            cur_adv(c); cur_adv(c);
            while (c->s[c->i]) {
                if (c->s[c->i] == '*' && c->s[c->i + 1] == '/') { cur_adv(c); cur_adv(c); break; }
                cur_adv(c);
            }
            continue;
        }
        return;
    }
}
static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* 读标识符（游标须已停在标识符首字符）；返回长度 */
static int cur_read_ident(Cur* c, char* out, int n) {
    int k = 0;
    while (c->s[c->i] && is_ident_char(c->s[c->i])) {
        if (k < n - 1) out[k] = c->s[c->i];
        k++;
        cur_adv(c);
    }
    out[k < n ? k : n - 1] = 0;
    return k;
}
/* 跳过字符串/字符字面量（游标停在引号上） */
static void cur_skip_quoted(Cur* c) {
    char q = c->s[c->i];
    cur_adv(c);
    while (c->s[c->i]) {
        if (c->s[c->i] == '\\' && c->s[c->i + 1]) { cur_adv(c); cur_adv(c); continue; }
        if (c->s[c->i] == q) { cur_adv(c); return; }
        cur_adv(c);
    }
}
/* 消费一段成对括号（游标须停在 open 上），可选捕获内部文本（不含最外层括号）。
   内部跳过注释与字面量。返回 1 表示正常闭合。 */
static int cur_skip_balanced(Cur* c, char open, char close, Buf* cap) {
    int depth = 0;
    if (c->s[c->i] != open) return 0;
    for (;;) {
        char ch = c->s[c->i];
        if (ch == 0) return 0;
        if (ch == '/' && (c->s[c->i + 1] == '/' || c->s[c->i + 1] == '*')) {
            cur_skip_ws(c);
            continue;
        }
        if (ch == '"' || ch == '\'') {
            int from = c->i;
            cur_skip_quoted(c);
            if (cap && depth >= 1) buf_putn(cap, c->s + from, c->i - from);
            continue;
        }
        if (ch == open) {
            depth++;
            if (cap && depth > 1) buf_putc(cap, ch);
            cur_adv(c);
            continue;
        }
        if (ch == close) {
            depth--;
            if (depth == 0) { cur_adv(c); return 1; }
            if (cap) buf_putc(cap, ch);
            cur_adv(c);
            continue;
        }
        if (cap && depth >= 1) buf_putc(cap, ch);
        cur_adv(c);
    }
}

/* ---------------- 类型标注规范化 ---------------- */
/* 复刻 `api_type_name(parse_type(<源码文本>))` 的渲染结果。编译器侧不是回显源码原文，
   而是先 parse 成 Type 再按固定规则重排（compile_api.shadow:139 + ty_display_name
   type_checker.shadow:1403）：
     · array<T> / T[]     → "array<" + canon(T) + ">"（尖括号内无空格；裸 array → "array<>"）
     · dict|map|hashmap   → "dict<" + canon(K) + ", " + canon(V) + ">"（逗号后恒一个空格）
     · set<T>             → "set"    （ty_display_name 丢弃实参）
     · (A, B)             → "tuple"  （同上）
     · Name<T1, T2>       → "Name"   （TYPE_STRUCT 只回显名字，实参不展示）
     · 基础类型           → 原样关键字
   故源码写 `map<string,int>` 时编译器输出 `dict<string, int>`，直接抄原文必然失配。
   fmeta 的 rettype/参数类型与 gmeta 的 type 三处共用本函数。 */
typedef struct { const char* s; int i; } TCur;

static void tcur_ws(TCur* t) {
    while (t->s[t->i] && isspace((unsigned char)t->s[t->i])) t->i++;
}

static void tcur_ident(TCur* t, char* out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    if (!is_ident_start(t->s[t->i])) return;
    /* 允许 FQN 点号链 a.b.T（parser.shadow:435 的兼容态命名空间限定类型名） */
    while (is_ident_char(t->s[t->i]) ||
           (t->s[t->i] == '.' && is_ident_char(t->s[t->i + 1]))) {
        if (n + 1 < cap) out[n++] = t->s[t->i];
        t->i++;
    }
    out[n] = 0;
}

static void tcur_skip_pair(TCur* t, char open, char close) {
    int d = 0;
    if (t->s[t->i] != open) return;
    for (; t->s[t->i]; t->i++) {
        if (t->s[t->i] == open) d++;
        else if (t->s[t->i] == close) { d--; if (d == 0) { t->i++; return; } }
    }
}

static int tcur_is_prim(const char* id) {
    static const char* P[] = { "int", "long", "float", "double", "bool", "string",
                               "void", "any", "char", "short", "date", "timestamp",
                               "nanotimestamp", "opaque", NULL };
    int i;
    for (i = 0; P[i]; i++) if (strcmp(id, P[i]) == 0) return 1;
    return 0;
}

static void sx_canon_type_cur(TCur* t, Buf* out) {
    Buf base;
    char id[256];
    buf_init(&base);
    tcur_ws(t);
    if (t->s[t->i] == '(') {                       /* 元组 (A, B) → tuple */
        tcur_skip_pair(t, '(', ')');
        buf_puts(&base, "tuple");
    } else if (is_ident_start(t->s[t->i])) {
        tcur_ident(t, id, sizeof(id));
        tcur_ws(t);
        if (strcmp(id, "array") == 0) {
            buf_puts(&base, "array<");
            if (t->s[t->i] == '<') {
                t->i++;
                sx_canon_type_cur(t, &base);
                tcur_ws(t);
                if (t->s[t->i] == '>') t->i++;
            } else {
                buf_puts(&base, "unresolved");     /* 裸 array：elem=type_none_inner()=TYPE_INFER */
            }
            buf_putc(&base, '>');
        } else if (strcmp(id, "dict") == 0 || strcmp(id, "map") == 0 ||
                   strcmp(id, "hashmap") == 0) {
            buf_puts(&base, "dict<");
            if (t->s[t->i] == '<') {
                t->i++;
                sx_canon_type_cur(t, &base);
                tcur_ws(t);
                buf_puts(&base, ", ");
                if (t->s[t->i] == ',') t->i++;
                sx_canon_type_cur(t, &base);
                tcur_ws(t);
                if (t->s[t->i] == '>') t->i++;
            } else {
                /* 裸 dict：key/val 均 type_none_inner()=TYPE_INFER */
                buf_puts(&base, "unresolved, unresolved");
            }
            buf_putc(&base, '>');
        } else if (strcmp(id, "set") == 0) {
            tcur_skip_pair(t, '<', '>');
            /* 裸 set：api_type_name 无 TYPE_SET 分支 → ty_display_name 返回 "set"
               （elem 的 TYPE_INFER 不进展示名，与 array/dict 的递归渲染不同） */
            buf_puts(&base, "set");
        } else if (tcur_is_prim(id)) {
            buf_puts(&base, id);
        } else {
            tcur_skip_pair(t, '<', '>');           /* 泛型实参不进展示名 */
            buf_puts(&base, id);
        }
    } else {
        buf_puts(&base, "any");                    /* parse_type 报错分支回落 TYPE_ANY */
    }
    /* 后缀数组 T[]（可叠加 T[][]）→ array<array<T>> */
    for (;;) {
        int save = t->i, j;
        tcur_ws(t);
        if (t->s[t->i] != '[') { t->i = save; break; }
        j = t->i + 1;
        while (t->s[j] && isspace((unsigned char)t->s[j])) j++;
        if (t->s[j] != ']') { t->i = save; break; }
        {
            Buf w;
            buf_init(&w);
            buf_puts(&w, "array<");
            buf_puts(&w, buf_cstr(&base));
            buf_putc(&w, '>');
            buf_free(&base);
            base = w;
        }
        t->i = j + 1;
    }
    buf_puts(out, buf_cstr(&base));
    buf_free(&base);
}

/* 空标注 → 空串（api_type_name 对 type_none 返回 ""） */
static void sx_canon_type(const char* src, Buf* out) {
    TCur t;
    if (!src) return;
    t.s = src; t.i = 0;
    tcur_ws(&t);
    if (!t.s[t.i]) return;
    sx_canon_type_cur(&t, out);
}

/* 便捷封装：规范化到定长缓冲区 */
static void sx_canon_type_buf(const char* src, char* out, size_t cap) {
    Buf b;
    buf_init(&b);
    sx_canon_type(src, &b);
    snprintf(out, cap, "%s", buf_cstr(&b));
    buf_free(&b);
}

/* ---------------- 参数段格式化 ---------------- */
/* 把 `a: int, b: array<string>` 规范成 `a: int, b: array<string>`（与 api_check 的
   sp 拼接一致：参数名 + ": " + 类型名，逗号后一个空格）。
   顶层逗号切分需忽略 <>/()/[] 内的逗号（dict<string, int>）。 */
static void sx_format_params(const char* raw, Buf* out) {
    int n = (int)strlen(raw);
    int i = 0, seg_start = 0;
    int ang = 0, par = 0, brk = 0;
    int first = 1;
    for (i = 0; i <= n; i++) {
        char ch = (i < n) ? raw[i] : ',';
        if (i < n) {
            if (ch == '<') ang++;
            else if (ch == '>') { if (ang > 0) ang--; }
            else if (ch == '(') par++;
            else if (ch == ')') { if (par > 0) par--; }
            else if (ch == '[') brk++;
            else if (ch == ']') { if (brk > 0) brk--; }
        }
        if (ch == ',' && ang == 0 && par == 0 && brk == 0) {
            int seg_len = i - seg_start;
            char* seg = (char*)malloc((size_t)seg_len + 1);
            if (seg) {
                char* t;
                memcpy(seg, raw + seg_start, (size_t)seg_len);
                seg[seg_len] = 0;
                t = sx_trim_inplace(seg);
                if (t[0]) {
                    char* colon = NULL;
                    int a2 = 0, p2 = 0, b2 = 0, j;
                    for (j = 0; t[j]; j++) {
                        if (t[j] == '<') a2++;
                        else if (t[j] == '>') { if (a2 > 0) a2--; }
                        else if (t[j] == '(') p2++;
                        else if (t[j] == ')') { if (p2 > 0) p2--; }
                        else if (t[j] == '[') b2++;
                        else if (t[j] == ']') { if (b2 > 0) b2--; }
                        else if (t[j] == ':' && a2 == 0 && p2 == 0 && b2 == 0) { colon = t + j; break; }
                    }
                    if (!first) buf_puts(out, ", ");
                    first = 0;
                    if (colon) {
                        char* tn;
                        int va = 0;
                        *colon = 0;
                        /* 变参 `...name: T` → parser 包成 Param{name, type=array<T>, is_variadic}
                           （parse_params，parser.shadow:2281-2292），展示名去掉 `...` 且
                           类型变 array<T>：f_variadic(prefix: string, rest: array<int>) */
                        if (strncmp(sx_trim_inplace(t), "...", 3) == 0) { va = 1; }
                        buf_puts(out, sx_trim_inplace(t) + (va ? 3 : 0));
                        buf_puts(out, ": ");
                        tn = colon + 1;
                        if (va) {
                            Buf inner;
                            buf_init(&inner);
                            sx_canon_type(tn, &inner);
                            buf_puts(out, "array<");
                            buf_puts(out, buf_cstr(&inner));
                            buf_putc(out, '>');
                            buf_free(&inner);
                        } else {
                            sx_canon_type(tn, out);   /* 类型按 api_type_name 规则重排 */
                        }
                    } else {
                        buf_puts(out, t);
                    }
                }
                free(seg);
            }
            seg_start = i + 1;
        }
    }
}

/* ---------------- 符号发射 ---------------- */
/* tc_is_exempt_name + extern：豁免符号不 mangle → fmeta 的 module 段为 "" */
static int sx_is_exempt(const char* name, int has_extern) {
    if (has_extern) return 1;
    if (strcmp(name, "main") == 0) return 1;
    if (strncmp(name, "__impl__", 8) == 0) return 1;
    if (strncmp(name, "shadow_", 7) == 0) return 1;
    if (strncmp(name, "rt_", 3) == 0) return 1;
    return 0;
}

/* 模块扫描上下文 */
typedef struct {
    const int* comb;   /* 行索引 → combined 行号（0 表示该行被丢弃） */
    int  nlines;
    const char* ns;    /* 模块命名空间 */
} ModCtx;

static int mc_line(const ModCtx* m, int li) {
    if (li < 0 || li >= m->nlines) return 0;
    return m->comb[li];
}

static void emit_fmeta(const ModCtx* m, const char* name, int li, int col,
                       int eli, int ecol, const char* rettype, const char* sig,
                       int is_pub, int has_extern) {
    int L = mc_line(m, li), EL = mc_line(m, eli);
    if (L <= 0) return;
    if (EL <= 0) EL = L;
    if (g_fmeta.len > 0) buf_putc(&g_fmeta, '\n');
    buf_puts(&g_fmeta, name);   buf_putc(&g_fmeta, '|');
    buf_puti(&g_fmeta, L);      buf_putc(&g_fmeta, '|');
    buf_puti(&g_fmeta, col);    buf_putc(&g_fmeta, '|');
    buf_puti(&g_fmeta, EL);     buf_putc(&g_fmeta, '|');
    buf_puti(&g_fmeta, ecol);   buf_putc(&g_fmeta, '|');
    buf_puts(&g_fmeta, rettype);buf_putc(&g_fmeta, '|');
    buf_puts(&g_fmeta, sig);    buf_putc(&g_fmeta, '|');
    buf_puti(&g_fmeta, is_pub); buf_putc(&g_fmeta, '|');
    buf_puts(&g_fmeta, sx_is_exempt(name, has_extern) ? "" : m->ns);
}
/* smeta/emeta 与 fmeta 有两点关键差异（已对照 api_check 实测数据确认）：
   ① 名字不 demangle：compile_api 用 sd2.name/ed2.name 原样输出，而 TC 的
      tc_reg_name_for(..., exempt=0) 对 struct/enum **无条件** mangle
      （不看 tc_is_exempt_name、不看 is_pub）→ 恒为 __mod_<ns>__<Name>。
   ② parser 的 parse_struct/parse_enum 不像 parse_function 那样回填 loc.end
      （只有 parse_function 调 ps_prev_end_line/col）→ end_line/end_col == line/col。 */
static void emit_meta5(Buf* dst, const ModCtx* m, const char* name,
                       int li, int col, int eli, int ecol) {
    int L = mc_line(m, li);
    (void)eli; (void)ecol;
    if (L <= 0) return;
    if (dst->len > 0) buf_putc(dst, '\n');
    buf_puts(dst, "__mod_");
    buf_puts(dst, m->ns);
    buf_puts(dst, "__");
    buf_puts(dst, name); buf_putc(dst, '|');
    buf_puti(dst, L);    buf_putc(dst, '|');
    buf_puti(dst, col);  buf_putc(dst, '|');
    buf_puti(dst, L);    buf_putc(dst, '|');
    buf_puti(dst, col);
}

/* gmeta：顶层 let / mut → `name|L|C|L|C|type`（compile_api.shadow:484-491）。
   · name：TC 只豁免 SHADOW_ / rt_ / shadow_ 三种前缀，其余一律 mangle 并回写
           gv.name（type_checker.shadow:2098-2107）——注意与 fmeta 的豁免集不同。
   · loc ：parser 取的是**变量名**位置而非 let 关键字（parser.shadow:2761）。
   · EL/EC：parser 不回填 globals 的 loc.end → 恒等于 L/C。
   · type：api_type_name(type_ref)；无 `: T` 标注时为空串（api_type_name 对
           type_none 返回 ""，且 TC 的 TYPE_INFER 兜底未回写 gv.type_ref）。   */
static void emit_gmeta(const ModCtx* m, const char* name, int li, int col,
                       const char* ty) {
    int L = mc_line(m, li);
    if (L <= 0) return;
    if (g_gmeta.len > 0) buf_putc(&g_gmeta, '\n');
    if (strncmp(name, "SHADOW_", 7) != 0 && strncmp(name, "rt_", 3) != 0 &&
        strncmp(name, "shadow_", 7) != 0) {
        buf_puts(&g_gmeta, "__mod_");
        buf_puts(&g_gmeta, m->ns);
        buf_puts(&g_gmeta, "__");
    }
    buf_puts(&g_gmeta, name); buf_putc(&g_gmeta, '|');
    buf_puti(&g_gmeta, L);    buf_putc(&g_gmeta, '|');
    buf_puti(&g_gmeta, col);  buf_putc(&g_gmeta, '|');
    buf_puti(&g_gmeta, L);    buf_putc(&g_gmeta, '|');
    buf_puti(&g_gmeta, col);  buf_putc(&g_gmeta, '|');
    buf_puts(&g_gmeta, ty);
}

/* 扫描单个 `kimo` 声明（游标须停在 kimo 关键字之后），产出一条 fmeta。
   顶层函数与 impl 方法共用此逻辑——parser 会把 impl 方法一并压进 prog.funcs：
     · inherent impl `impl T {}`      → 原名入 funcs（parser.shadow:2861）
     · trait impl   `impl Tr for T {}`→ 压入 __impl__<T>__<Tr>__<m> 的 mangle 副本
                                        （parser.shadow:2822/2842）
   name_prefix 即上述前缀（inherent 传 ""）。fmeta 的 name 与 sig 都用带前缀的全名：
   api_demangle 只剥 "__mod_" 前缀，对 "__impl__" 原样返回。 */
static void sx_scan_kimo(Cur* c, const ModCtx* m, int kw_li, int kw_col,
                         int is_pub, int is_extern, const char* name_prefix) {
    char name[256], full[512], rtbuf[256];
    Buf praw, sig, rett;
    int eli, ecol;
    buf_init(&praw); buf_init(&sig); buf_init(&rett);

    cur_skip_ws(c);
    name[0] = 0;
    if (is_ident_start(c->s[c->i])) cur_read_ident(c, name, sizeof(name));
    cur_skip_ws(c);
    /* 泛型参数 <T, U: int|long> */
    if (c->s[c->i] == '<') cur_skip_balanced(c, '<', '>', NULL);
    cur_skip_ws(c);
    if (c->s[c->i] == '(') cur_skip_balanced(c, '(', ')', &praw);
    cur_skip_ws(c);
    /* 返回类型 */
    if (c->s[c->i] == '-' && c->s[c->i + 1] == '>') {
        cur_adv(c); cur_adv(c);
        cur_skip_ws(c);
        while (c->s[c->i] && c->s[c->i] != '{' && c->s[c->i] != ';' && c->s[c->i] != '\n') {
            buf_putc(&rett, c->s[c->i]);
            cur_adv(c);
        }
    }
    cur_skip_ws(c);
    /* 函数体：loc.end 由 parse_function 回填为右花括号之后的位置 */
    eli = c->li; ecol = c->col;
    if (c->s[c->i] == '{') {
        cur_skip_balanced(c, '{', '}', NULL);
        eli = c->li; ecol = c->col;
    } else if (c->s[c->i] == ';') {
        cur_adv(c);
        eli = c->li; ecol = c->col;
    }
    if (name[0]) {
        const char* rt;
        snprintf(full, sizeof(full), "%s%s", name_prefix ? name_prefix : "", name);
        /* 返回类型走 api_type_name 渲染。`->` 是强制语法（ps_expect(TOK_ARROW)），
           缺失或类型位置非法时 parse_type 走报错分支回落 TYPE_ANY → "any"，
           所以这里空串一律补 "any"（合法源码永远命中不到）。 */
        sx_canon_type_buf(buf_cstr(&rett), rtbuf, sizeof(rtbuf));
        if (!rtbuf[0]) snprintf(rtbuf, sizeof(rtbuf), "any");
        rt = rtbuf;
        buf_puts(&sig, "fn ");
        buf_puts(&sig, full);
        buf_putc(&sig, '(');
        sx_format_params(buf_cstr(&praw), &sig);
        buf_puts(&sig, ") -> ");
        buf_puts(&sig, rt);
        emit_fmeta(m, full, kw_li, kw_col, eli, ecol, rt, buf_cstr(&sig), is_pub, is_extern);
    }
    buf_free(&praw); buf_free(&sig); buf_free(&rett);
}

/* ---------------- 顶层定义扫描 ---------------- */
static void sx_scan_module(const char* src, const ModCtx* m) {
    Cur c;
    int pend_pub = 0, pend_extern = 0;
    c.s = src; c.i = 0; c.li = 0; c.col = 1;

    for (;;) {
        char ch;
        cur_skip_ws(&c);
        ch = c.s[c.i];
        if (ch == 0) return;

        /* 属性：@extern("...") / 其它 @attr(...) */
        if (ch == '@') {
            char attr[64];
            cur_adv(&c);
            if (is_ident_start(c.s[c.i])) {
                cur_read_ident(&c, attr, sizeof(attr));
                if (strcmp(attr, "extern") == 0) pend_extern = 1;
            }
            cur_skip_ws(&c);
            if (c.s[c.i] == '(') cur_skip_balanced(&c, '(', ')', NULL);
            continue;
        }
        if (ch == '"' || ch == '\'') { cur_skip_quoted(&c); continue; }

        if (!is_ident_start(ch)) {
            /* 顶层杂散花括号（不应出现于合法源码）：整体跳过以免错位 */
            if (ch == '{') { cur_skip_balanced(&c, '{', '}', NULL); pend_pub = 0; pend_extern = 0; continue; }
            cur_adv(&c);
            continue;
        }

        {
            char kw[64];
            int kw_li = c.li, kw_col = c.col;
            cur_read_ident(&c, kw, sizeof(kw));

            if (strcmp(kw, "pub") == 0) { pend_pub = 1; continue; }

            if (strcmp(kw, "kimo") == 0) {
                sx_scan_kimo(&c, m, kw_li, kw_col, pend_pub, pend_extern, "");
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            /* 旧语法宏 `macro name { param => body; }`：desugar 为函数
               （parser.shadow:2911-2948）→ 参数单 Param{name:param, type=int}、
               返回 TYPE_INT、loc=宏名位置（kw 之后）且 end=start。新语法
               `macro name(params) { body }` 只进 ps_macros，不进 prog.funcs → 跳过。 */
            if (strcmp(kw, "macro") == 0) {
                char mname[256], mparam[256];
                int eli, ecol;
                cur_skip_ws(&c);
                if (!is_ident_start(c.s[c.i])) { pend_pub = 0; pend_extern = 0; continue; }
                cur_read_ident(&c, mname, sizeof(mname));
                cur_skip_ws(&c);
                if (c.s[c.i] == '(') {         /* 新语法：整块跳过 */
                    cur_skip_balanced(&c, '(', ')', NULL);
                    cur_skip_ws(&c);
                    if (c.s[c.i] == '{') cur_skip_balanced(&c, '{', '}', NULL);
                    pend_pub = 0; pend_extern = 0;
                    continue;
                }
                if (c.s[c.i] != '{') { pend_pub = 0; pend_extern = 0; continue; }
                cur_adv(&c);                   /* '{' */
                cur_skip_ws(&c);
                mparam[0] = 0;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, mparam, sizeof(mparam));
                /* 跳过 `=> body;` 到 '}'（字符串/括号已由 skip_quoted/balanced 处理） */
                while (c.s[c.i] && c.s[c.i] != '}') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    if (c.s[c.i] == '{') { cur_skip_balanced(&c, '{', '}', NULL); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] == '}') cur_adv(&c);
                if (c.s[c.i] == ';') cur_adv(&c);
                /* mfunc.loc 取右花括号之后的**下一个 token** 位置（parser.shadow:2931：
                   mk_loc(ps_cur_line/col) 在 ps_expect(TOK_RBRACE) 之后——ps_adv 已跳过
                   空白/换行，故多行宏落在后续行的首个 token 处），end=start=loc */
                cur_skip_ws(&c);
                eli = c.li; ecol = c.col;
                if (mname[0]) {
                    char msig[600];
                    if (mparam[0]) {
                        snprintf(msig, sizeof(msig), "fn %s(%s: int) -> int", mname, mparam);
                    } else {
                        snprintf(msig, sizeof(msig), "fn %s() -> int", mname);
                    }
                    emit_fmeta(m, mname, eli, ecol, eli, ecol, "int", msig, 0, 0);
                }
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            if (strcmp(kw, "struct") == 0 || strcmp(kw, "enum") == 0) {
                char name[256];
                int eli, ecol;
                int is_struct = (kw[0] == 's');
                cur_skip_ws(&c);
                name[0] = 0;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, name, sizeof(name));
                cur_skip_ws(&c);
                if (c.s[c.i] == '<') cur_skip_balanced(&c, '<', '>', NULL);
                cur_skip_ws(&c);
                eli = c.li; ecol = c.col;
                if (c.s[c.i] == '{') {
                    cur_skip_balanced(&c, '{', '}', NULL);
                    eli = c.li; ecol = c.col;
                }
                if (name[0]) {
                    emit_meta5(is_struct ? &g_smeta : &g_emeta, m, name,
                               kw_li, kw_col, eli, ecol);
                }
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            /* trait / interface 声明：方法只进 TraitDef.methods，不进 prog.funcs
               （parse_trait_def，parser.shadow:2552）→ 整块跳过。 */
            if (strcmp(kw, "trait") == 0 || strcmp(kw, "interface") == 0) {
                while (c.s[c.i] && c.s[c.i] != '{') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] == '{') cur_skip_balanced(&c, '{', '}', NULL);
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            /* impl 块：方法会被 parser 压进 prog.funcs → 必须进 fmeta。
               `impl Trait for Type {}` 的方法带 __impl__<Type>__<Trait>__ 前缀；
               `impl Type {}`（inherent）用原名。 */
            if (strcmp(kw, "impl") == 0) {
                char first[256], type_name[256], prefix[600];
                int mpub = 0, depth;
                first[0] = 0; type_name[0] = 0; prefix[0] = 0;

                cur_skip_ws(&c);
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, first, sizeof(first));
                cur_skip_ws(&c);
                if (c.s[c.i] == '<') { cur_skip_balanced(&c, '<', '>', NULL); cur_skip_ws(&c); }
                if (is_ident_start(c.s[c.i])) {
                    char kw2[64];
                    int si = c.i, sli = c.li, scol = c.col;
                    cur_read_ident(&c, kw2, sizeof(kw2));
                    if (strcmp(kw2, "for") == 0) {
                        cur_skip_ws(&c);
                        if (is_ident_start(c.s[c.i])) cur_read_ident(&c, type_name, sizeof(type_name));
                        cur_skip_ws(&c);
                        if (c.s[c.i] == '<') { cur_skip_balanced(&c, '<', '>', NULL); cur_skip_ws(&c); }
                        snprintf(prefix, sizeof(prefix), "__impl__%s__%s__", type_name, first);
                    } else {
                        c.i = si; c.li = sli; c.col = scol;   /* 非 for：退回，按 inherent 处理 */
                    }
                }
                /* 定位块首 '{' */
                while (c.s[c.i] && c.s[c.i] != '{') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] != '{') { pend_pub = 0; pend_extern = 0; continue; }
                cur_adv(&c);          /* 吃掉 '{'，手工维护深度以便逐个方法扫描 */
                depth = 1;
                while (depth > 0 && c.s[c.i]) {
                    char mkw[64];
                    int mli, mcol;
                    cur_skip_ws(&c);
                    if (!c.s[c.i]) break;
                    if (c.s[c.i] == '}') { cur_adv(&c); depth--; continue; }
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    /* 方法体已被 sx_scan_kimo 内的 skip_balanced 吃掉；这里的 '{'
                       只可能来自异常源码，整块跳过以免深度错乱。 */
                    if (c.s[c.i] == '{') { cur_skip_balanced(&c, '{', '}', NULL); continue; }
                    if (!is_ident_start(c.s[c.i])) { cur_adv(&c); continue; }
                    mli = c.li; mcol = c.col;
                    cur_read_ident(&c, mkw, sizeof(mkw));
                    if (strcmp(mkw, "pub") == 0) { mpub = 1; continue; }
                    if (strcmp(mkw, "kimo") == 0) {
                        sx_scan_kimo(&c, m, mli, mcol, mpub, 0, prefix);
                        mpub = 0;
                        continue;
                    }
                    mpub = 0;
                }
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            /* 顶层 let / mut → gmeta */
            if (strcmp(kw, "let") == 0 || strcmp(kw, "mut") == 0) {
                char gname[256], tybuf[512];
                int nli, ncol, ang = 0;
                Buf gty;

                cur_skip_ws(&c);
                nli = c.li; ncol = c.col;      /* loc = 变量名位置 */
                gname[0] = 0;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, gname, sizeof(gname));
                cur_skip_ws(&c);
                buf_init(&gty);
                if (c.s[c.i] == ':') {
                    cur_adv(&c);
                    cur_skip_ws(&c);
                    /* 类型标注读到顶层 '=' 或 ';'（泛型实参内的符号需按尖括号平衡） */
                    while (c.s[c.i]) {
                        char t = c.s[c.i];
                        if (t == '<') ang++;
                        else if (t == '>') { if (ang > 0) ang--; }
                        else if (ang == 0 && (t == '=' || t == ';')) break;
                        else if (t == '\n') break;
                        buf_putc(&gty, t);
                        cur_adv(&c);
                    }
                }
                if (gname[0]) {
                    /* 无 `: T` 标注时 parser 填 type_none_inner()——它是 kind=TYPE_INFER
                       的**非空** Type（core/utils.shadow:225），而 api_type_none 只判空
                       指针，故渲染成 "unresolved" 而不是空串。 */
                    sx_canon_type_buf(buf_cstr(&gty), tybuf, sizeof(tybuf));
                    if (!tybuf[0]) snprintf(tybuf, sizeof(tybuf), "unresolved");
                    emit_gmeta(m, gname, nli, ncol, tybuf);
                }
                buf_free(&gty);
                /* 跳过初始化表达式到顶层 ';'（跳过字符串与各类括号） */
                {
                    int par = 0, brk = 0, brc = 0;
                    while (c.s[c.i]) {
                        char t = c.s[c.i];
                        if (t == '"' || t == '\'') { cur_skip_quoted(&c); continue; }
                        if (t == '(') { cur_skip_balanced(&c, '(', ')', NULL); continue; }
                        if (t == '[') { cur_skip_balanced(&c, '[', ']', NULL); continue; }
                        if (t == '{') { cur_skip_balanced(&c, '{', '}', NULL); continue; }
                        if (t == ';' && par == 0 && brk == 0 && brc == 0) { cur_adv(&c); break; }
                        cur_adv(&c);
                    }
                }
                pend_pub = 0; pend_extern = 0;
                continue;
            }

            /* 其它顶层语句（import / dsb / type ...）：由外层循环逐 token 掠过 */
            pend_pub = 0; pend_extern = 0;
            continue;
        }
    }
}

/* ---------------- import 解析 ---------------- */
/* 目录=包：收集 dir 下全部 .shadow，按 strcmp 升序（复刻 main_collect_dir 插入排序） */
static int sx_collect_dir(const char* dir_key, const char* project_root,
                          SList* keys, SList* roots) {
    char full[1024], search[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    SList tmp;
    int i, j, found = 0;

    sx_join(project_root, dir_key, full, sizeof(full));
    if (!full[0]) return 0;
    snprintf(search, sizeof(search), "%s\\*", full);
    h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    sl_init(&tmp);
    do {
        int nl = (int)strlen(fd.cFileName);
        if (nl > 7 && strcmp(fd.cFileName + nl - 7, ".shadow") == 0) {
            char mkey[1024];
            if (dir_key && dir_key[0]) snprintf(mkey, sizeof(mkey), "%s\\%s", dir_key, fd.cFileName);
            else snprintf(mkey, sizeof(mkey), "%s", fd.cFileName);
            if (!sl_push(&tmp, mkey)) { FindClose(h); sl_free(&tmp); return 0; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (tmp.n == 0) { sl_free(&tmp); return 0; }
    /* 插入排序（升序），与 main_collect_dir 的 rt_str_cmp 顺序一致 */
    for (j = 1; j < tmp.n; j++) {
        char* cur = tmp.v[j];
        i = j - 1;
        while (i >= 0 && strcmp(tmp.v[i], cur) > 0) { tmp.v[i + 1] = tmp.v[i]; i--; }
        tmp.v[i + 1] = cur;
    }
    for (i = 0; i < tmp.n; i++) {
        if (!sl_push(keys, tmp.v[i]) || !sl_push(roots, project_root)) { sl_free(&tmp); return 0; }
        found = 1;
    }
    sl_free(&tmp);
    return found;
}

/* main_rest_to_rel：a.b → a\b */
static void sx_rest_to_rel(const char* rest, char* out, int n) {
    int i;
    snprintf(out, (size_t)n, "%s", rest ? rest : "");
    for (i = 0; out[i]; i++) if (out[i] == '.') out[i] = '\\';
}

static int sx_resolve_in_base(const char* base, const char* key_prefix, const char* rest,
                              const char* project_root, SList* keys, SList* roots) {
    char rel[1024], fkey[1024], full[1200], dkey[1024];
    if (rest && rest[0]) {
        sx_rest_to_rel(rest, rel, sizeof(rel));
        if (key_prefix && key_prefix[0]) snprintf(fkey, sizeof(fkey), "%s\\%s.shadow", key_prefix, rel);
        else snprintf(fkey, sizeof(fkey), "%s.shadow", rel);
        {
            char relsh[1100];
            snprintf(relsh, sizeof(relsh), "%s.shadow", rel);
            sx_join(base, relsh, full, sizeof(full));
        }
        if (sx_is_file(full)) {
            if (!sl_push(keys, fkey) || !sl_push(roots, project_root)) return 0;
            return 1;
        }
        if (key_prefix && key_prefix[0]) snprintf(dkey, sizeof(dkey), "%s\\%s", key_prefix, rel);
        else snprintf(dkey, sizeof(dkey), "%s", rel);
        return sx_collect_dir(dkey, project_root, keys, roots);
    }
    return sx_collect_dir(key_prefix ? key_prefix : "", project_root, keys, roots);
}

/* 复刻 main_resolve_import 的 ① 与 ②.5；命中 [deps] 场景（②）本实现不支持 → 返回 0 兜底 */
static int sx_resolve_import(const char* import_path, const char* project_root,
                             SList* keys, SList* roots) {
    char module[512], rest[512], base[1024], mfile[1200];
    const char* dot;
    int hit = 0;

    dot = strchr(import_path, '.');
    if (dot) {
        int k = (int)(dot - import_path);
        if (k >= (int)sizeof(module)) k = (int)sizeof(module) - 1;
        memcpy(module, import_path, (size_t)k); module[k] = 0;
        snprintf(rest, sizeof(rest), "%s", dot + 1);
    } else {
        snprintf(module, sizeof(module), "%s", import_path);
        rest[0] = 0;
    }

    /* ① 主模块自身 */
    sx_join(project_root, module, base, sizeof(base));
    if (!rest[0]) {
        char modsh[600];
        snprintf(modsh, sizeof(modsh), "%s.shadow", module);
        sx_join(project_root, modsh, mfile, sizeof(mfile));
        if (sx_is_file(mfile)) {
            if (!sl_push(keys, modsh) || !sl_push(roots, project_root)) return 0;
            hit = 1;
        } else {
            hit = sx_collect_dir(module, project_root, keys, roots);
        }
    } else {
        hit = sx_resolve_in_base(base, module, rest, project_root, keys, roots);
    }

    /* ② deps / SPK：编译器在此优先于 std 回退解析。本实现不覆盖依赖解包 →
       首段命中 [deps] 即整体兜底，绝不越过它去撞 std 同名模块（否则两侧都"成功"
       却指向不同文件，形成静默错位）。 */
    if (!hit && sl_has(&g_dep_names, module)) return 0;

    /* ②.5 内置标准库回退 */
    if (!hit && g_std_root[0]) {
        char sbase[1024];
        sx_join(g_std_root, module, sbase, sizeof(sbase));
        if (!rest[0]) hit = sx_collect_dir(module, g_std_root, keys, roots);
        else hit = sx_resolve_in_base(sbase, module, rest, g_std_root, keys, roots);
    }
    return hit;
}

/* main_import_target：解析 import 行（行首已 trim）→ 模块路径；否则 "" */
static int sx_import_target(const char* line, int len, char* out, int n) {
    int i = 0, endp, semi = -1, ci, tl;
    char tmp[1024];
    out[0] = 0;
    while (i < len && isspace((unsigned char)line[i])) i++;
    if (len - i < 7 || strncmp(line + i, "import ", 7) != 0) return 0;
    i += 7;
    endp = len;
    { int k; for (k = i; k < len; k++) if (line[k] == ';') { semi = k; break; } }
    if (semi >= 0) endp = semi;
    if (endp - i >= (int)sizeof(tmp)) endp = i + (int)sizeof(tmp) - 1;
    memcpy(tmp, line + i, (size_t)(endp - i));
    tmp[endp - i] = 0;
    {
        char* t = sx_trim_inplace(tmp);
        char* cm = strstr(t, "//");
        if (cm) { *cm = 0; t = sx_trim_inplace(t); }
        tl = (int)strlen(t);
        if (tl >= 2 && t[tl - 1] == '*' && t[tl - 2] == '.') { t[tl - 2] = 0; tl -= 2; }
        if (tl <= 0) return 0;
        snprintf(out, (size_t)n, "%s", t);
    }
    (void)ci;
    return 1;
}

/* ---------------- 模块收集（复刻 main_collect_module） ---------------- */
static int sx_collect_module(const char* project_root, const char* key,
                             int keep_dsb, const char* ns_override,
                             const char* mem_src, const char* mem_key);

static int sx_append_and_scan(const char* src, int keep_dsb, const char* ns) {
    Lines L;
    int* comb;
    int i, dsb_line = -1;
    ModCtx m;

    if (!lines_split(src, &L)) return 0;
    comb = (int*)malloc(sizeof(int) * (size_t)(L.n > 0 ? L.n : 1));
    if (!comb) { lines_free(&L); return 0; }

    if (!keep_dsb) {
        for (i = 0; i < L.n; i++) {
            if (sx_is_dsb_line(src + L.off[i], L.len[i])) { dsb_line = i; break; }
        }
    }
    for (i = 0; i < L.n; i++) {
        if (i == dsb_line) { comb[i] = 0; continue; }   /* 丢弃 dsb 行，不推进坐标 */
        g_combined++;
        comb[i] = g_combined;
    }
    m.comb = comb; m.nlines = L.n; m.ns = ns;
    sx_scan_module(src, &m);

    /* 首个模块（== 主模块，前序追加）结束行 → api_main_end 的真值 */
    if (g_main_end == 0) g_main_end = g_combined;

    free(comb);
    lines_free(&L);
    return 1;
}

static int sx_collect_module(const char* project_root, const char* key,
                             int keep_dsb, const char* ns_override,
                             const char* mem_src, const char* mem_key) {
    char full[1200], my_ns[512], bnm[256];
    char* src = NULL;
    int owned = 0;
    Lines L;
    int i, rc = 1;

    if (sl_has(&g_stack, key)) return 1;      /* 循环 import 合法 */
    if (sl_has(&g_compiled, key)) return 1;   /* 菱形依赖去重 */
    if (++g_depth_guard > 256) { g_depth_guard--; return 0; }
    if (!sl_push(&g_stack, key)) { g_depth_guard--; return 0; }

    sx_join(project_root, key, full, sizeof(full));
    /* LSP：主模块用编辑器内存缓冲区覆盖磁盘内容 */
    if (mem_key && mem_key[0] && strcmp(key, mem_key) == 0 && mem_src && mem_src[0]) {
        src = (char*)mem_src;
        owned = 0;
    } else {
        src = sx_read_file(full);
        owned = 1;
    }
    if (!src || !src[0]) { if (owned) free(src); sl_pop(&g_stack); g_depth_guard--; return 0; }

    if (ns_override && ns_override[0]) snprintf(my_ns, sizeof(my_ns), "%s", ns_override);
    else sx_key_to_ns(key, my_ns, sizeof(my_ns));

    /* dsb 必须 == basename（严格态）：不符说明源码有错，交给全量编译报诊断 */
    {
        Lines DL;
        char dname[256];
        dname[0] = 0;
        if (lines_split(src, &DL)) {
            for (i = 0; i < DL.n; i++) {
                if (sx_is_dsb_line(src + DL.off[i], DL.len[i])) {
                    char tmp[512];
                    int ln = DL.len[i];
                    char* t;
                    if (ln >= (int)sizeof(tmp)) ln = (int)sizeof(tmp) - 1;
                    memcpy(tmp, src + DL.off[i], (size_t)ln); tmp[ln] = 0;
                    t = sx_trim_inplace(tmp);
                    t += 4;
                    t = sx_trim_inplace(t);
                    { int tl = (int)strlen(t); if (tl > 0 && t[tl - 1] == ';') t[tl - 1] = 0; }
                    snprintf(dname, sizeof(dname), "%s", sx_trim_inplace(t));
                    break;
                }
            }
            lines_free(&DL);
        }
        if (dname[0]) {
            sx_basename_of(key, bnm, sizeof(bnm));
            if (strcmp(dname, bnm) != 0) {
                if (owned) free(src);
                sl_pop(&g_stack); g_depth_guard--;
                return 0;
            }
        }
    }

    if (!sx_append_and_scan(src, keep_dsb, my_ns)) {
        if (owned) free(src);
        sl_pop(&g_stack); g_depth_guard--;
        return 0;
    }

    /* DFS 递归 import */
    if (lines_split(src, &L)) {
        for (i = 0; i < L.n && rc; i++) {
            char tgt[512];
            if (!sx_import_target(src + L.off[i], L.len[i], tgt, sizeof(tgt))) continue;
            {
                SList keys, roots;
                int hit, ri;
                sl_init(&keys); sl_init(&roots);
                hit = sx_resolve_import(tgt, project_root, &keys, &roots);
                if (!hit) { rc = 0; }
                for (ri = 0; ri < keys.n && rc; ri++) {
                    if (!sx_collect_module(roots.v[ri], keys.v[ri], 0, "", mem_src, mem_key)) rc = 0;
                }
                sl_free(&keys); sl_free(&roots);
            }
        }
        lines_free(&L);
    } else {
        rc = 0;
    }

    if (owned) free(src);
    sl_pop(&g_stack);
    sl_push(&g_compiled, key);
    g_depth_guard--;
    return rc;
}

/* ---------------- std 根探测（复刻 main_load_combined 的四级候选） ---------------- */
static void sx_find_std_root(const char* arg0) {
    char sxd[1024], c1[1200], chk[1300], c2[1200];
    g_std_root[0] = 0;
    sx_dir_of(arg0 ? arg0 : "", sxd, sizeof(sxd));
    if (sxd[0]) {
        snprintf(c1, sizeof(c1), "%s\\..\\src", sxd);
        snprintf(chk, sizeof(chk), "%s\\std\\iter.shadow", c1);
        if (sx_is_file(chk)) { snprintf(g_std_root, sizeof(g_std_root), "%s", c1); return; }
        sx_join(sxd, "src", c2, sizeof(c2));
        snprintf(chk, sizeof(chk), "%s\\std\\iter.shadow", c2);
        if (sx_is_file(chk)) { snprintf(g_std_root, sizeof(g_std_root), "%s", c2); return; }
    }
    if (sx_is_file("src\\std\\iter.shadow")) { snprintf(g_std_root, sizeof(g_std_root), "src"); return; }
    {
        char sd[1024], parent[1024], c3[1200];
        int stry;
        snprintf(sd, sizeof(sd), "%s", sxd);
        for (stry = 0; stry < 4; stry++) {
            sx_dir_of(sd, parent, sizeof(parent));
            if (!parent[0] || strcmp(parent, sd) == 0) break;
            snprintf(sd, sizeof(sd), "%s", parent);
            sx_join(sd, "src", c3, sizeof(c3));
            snprintf(chk, sizeof(chk), "%s\\std\\iter.shadow", c3);
            if (sx_is_file(chk)) { snprintf(g_std_root, sizeof(g_std_root), "%s", c3); return; }
        }
    }
}

/* ---------------- shadow.sbg [deps] 键名收集（复刻 main_parse_sbg） ----------------
   只取键名：import 首段命中 deps 时，编译器走 ② deps/SPK 解析（本实现不覆盖）→ 必须兜底。
   未命中 deps 的 import 仍按 ①项目内 → ②.5 std 解析，与编译器完全一致。
   支持两种写法：`[deps]` 段下 `name = "path"`，以及 `[deps.<name>]` 子表。            */
static void sx_load_sbg_deps(const char* sbg_path) {
    char* src;
    char section[256];
    int i, n;

    src = sx_read_file(sbg_path);
    if (!src) return;
    section[0] = 0;
    n = (int)strlen(src);
    i = 0;
    while (i < n) {
        int b = i, e, t0, t1;
        while (i < n && src[i] != '\n') i++;
        e = i;
        if (i < n) i++;
        if (e > b && src[e - 1] == '\r') e--;
        t0 = b; t1 = e;
        while (t0 < t1 && isspace((unsigned char)src[t0])) t0++;
        while (t1 > t0 && isspace((unsigned char)src[t1 - 1])) t1--;
        if (t1 <= t0) continue;
        if (src[t0] == '#') continue;
        if (src[t0] == '[') {
            int c = t0 + 1;
            while (c < t1 && src[c] != ']') c++;
            if (c < t1) {
                int s0 = t0 + 1, s1 = c, ln;
                while (s0 < s1 && isspace((unsigned char)src[s0])) s0++;
                while (s1 > s0 && isspace((unsigned char)src[s1 - 1])) s1--;
                ln = s1 - s0;
                if (ln >= (int)sizeof(section)) ln = (int)sizeof(section) - 1;
                memcpy(section, src + s0, (size_t)ln);
                section[ln] = 0;
                /* [deps.<name>] 子表：段名本身即依赖名 */
                if (strncmp(section, "deps.", 5) == 0 && section[5]) {
                    if (!sl_has(&g_dep_names, section + 5)) sl_push(&g_dep_names, section + 5);
                }
            }
            continue;
        }
        if (strcmp(section, "deps") != 0) continue;
        {
            int eq = t0, k1, ln;
            char key[256];
            while (eq < t1 && src[eq] != '=') eq++;
            if (eq >= t1) continue;
            k1 = eq;
            while (k1 > t0 && isspace((unsigned char)src[k1 - 1])) k1--;
            ln = k1 - t0;
            if (ln <= 0) continue;
            if (ln >= (int)sizeof(key)) ln = (int)sizeof(key) - 1;
            memcpy(key, src + t0, (size_t)ln);
            key[ln] = 0;
            if (!sl_has(&g_dep_names, key)) sl_push(&g_dep_names, key);
        }
    }
    free(src);
}

/* ---------------- 对外 API ---------------- */
static void sx_state_reset(void) {
    if (!g_inited) {
        buf_init(&g_fmeta); buf_init(&g_smeta); buf_init(&g_emeta); buf_init(&g_gmeta);
        g_inited = 1;
    }
    buf_clear(&g_fmeta); buf_clear(&g_smeta); buf_clear(&g_emeta); buf_clear(&g_gmeta);
    sl_free(&g_compiled); sl_free(&g_stack); sl_free(&g_dep_names);
    sl_init(&g_compiled); sl_init(&g_stack); sl_init(&g_dep_names);
    g_combined = 0;
    g_main_end = 0;
    g_depth_guard = 0;
    g_ok = 0;
}

int shadow_index_build(const char* entry_path, const char* entry_src, const char* arg0) {
    char path[1200], dir[1024], key[512], sbg[1300];
    int rc;

    sx_state_reset();
    if (!entry_path || !entry_path[0]) return 0;

    snprintf(path, sizeof(path), "%s", entry_path);
    sx_bslash(path);
    sx_dir_of(path, dir, sizeof(dir));
    {
        int n = (int)strlen(path), last = -1, i;
        for (i = 0; i < n; i++) if (path[i] == '\\') last = i;
        snprintf(key, sizeof(key), "%s", last >= 0 ? path + last + 1 : path);
    }

    /* shadow.sbg 是每个项目的常规清单文件，仅凭其存在就整体兜底会让快路径在真实项目里
       几乎永不生效。这里只收集 [deps] 键名：真正 import 到某个 dep 时才兜底（见
       sx_resolve_import），未用到 dep 的项目照常走快路径。
       注意 sbg 的 [package] name 不参与命名空间派生（ns 恒由 main_key_to_ns 路径派生），
       故不解析该段亦无偏差。 */
    sx_join(dir, "shadow.sbg", sbg, sizeof(sbg));
    sx_load_sbg_deps(sbg);

    sx_find_std_root(arg0 ? arg0 : "");

    rc = sx_collect_module(dir, key, 1, "", entry_src, key);
    if (!rc) { g_ok = 0; return 0; }

    /* runtime_lib.shadow 最后追加（命名空间 shadow.runtime，去 dsb 行） */
    {
        char rt_path[1200], cand[1300], cand2[1300], exe_dir[1024];
        char* rt_src;
        snprintf(rt_path, sizeof(rt_path), "src\\runtime\\runtime_lib.shadow");
        sx_dir_of(arg0 ? arg0 : "", exe_dir, sizeof(exe_dir));
        if (exe_dir[0]) {
            snprintf(cand, sizeof(cand), "%s\\..\\src\\runtime\\runtime_lib.shadow", exe_dir);
            if (sx_is_file(cand)) snprintf(rt_path, sizeof(rt_path), "%s", cand);
        }
        if (!sx_is_file(rt_path)) {
            sx_join(dir, "runtime\\runtime_lib.shadow", cand2, sizeof(cand2));
            if (sx_is_file(cand2)) snprintf(rt_path, sizeof(rt_path), "%s", cand2);
        }
        rt_src = sx_read_file(rt_path);
        if (rt_src && rt_src[0]) {
            if (!sx_append_and_scan(rt_src, 0, "shadow.runtime")) { free(rt_src); g_ok = 0; return 0; }
            free(rt_src);
        }
    }

    g_ok = 1;
    return 1;
}

const char* shadow_index_fmeta(void) { return sx_host_dup(g_inited ? buf_cstr(&g_fmeta) : ""); }
const char* shadow_index_smeta(void) { return sx_host_dup(g_inited ? buf_cstr(&g_smeta) : ""); }
const char* shadow_index_emeta(void) { return sx_host_dup(g_inited ? buf_cstr(&g_emeta) : ""); }
const char* shadow_index_gmeta(void) { return sx_host_dup(g_inited ? buf_cstr(&g_gmeta) : ""); }
int  shadow_index_ok(void) { return g_ok; }
int  shadow_index_lines(void) { return g_combined; }
int  shadow_index_main_end(void) { return g_main_end; }

/* ---------------- semanticTokens：轻量词法 + delta 编码 ----------------
 * 对齐 src/lsp/lsp.shadow 的 lsp_semantic_tokens_json 语义：
 *   tokenType: variable=0 function=1 keyword=4 type=5 number=7 string=8
 *   tokenModifiers: bold=1；delta 每 token 5 int：[deltaLine, deltaChar, len, type, mod]
 *   输出 JSON {"data":[...]}（宿主分配，GC 可见）。
 * 仅扫 mem_src（LSP 内存文档，didChange 后镜像），不读文件。
 * ident 分类：prev token == "kimo" → 函数定义(1,bold)；函数名(fmeta+内建) → 1
 *   （后跟 '(' → bold）；类型名(smeta+emeta) → 5；否则 0（variable）。
 */
static const char* const SX_KWS[] = {
    "kimo","let","if","else","while","for","return","break","continue","true",
    "false","as","struct","enum","import","pub","in","type","null","int",
    "long","float","double","bool","string","void","any","char","short","match",
    "new","mut","impl","try","catch","throw","panic","macro","async","await",
    "spawn","yield","trait","interface","do","date","timestamp","nanotimestamp",
    "opaque","with","dsb","defer"
};
#define SX_KW_N ((int)(sizeof(SX_KWS) / sizeof(SX_KWS[0])))
static const char* const SX_BUILTINS[] = {
    "len","str_eq","str_contains","str_index_of","str_replace","str_trim",
    "str_to_upper","str_to_lower","str_char_at","str_split","str_join",
    "str_format","substr2","int_to_str","str_to_int","array_push","array_pop",
    "print","println","str_len","array_len"
};
#define SX_BUILTIN_N ((int)(sizeof(SX_BUILTINS) / sizeof(SX_BUILTINS[0])))

typedef struct { const char** v; int n; int cap; } SxSet;
static void sx_set_push(SxSet* s, const char* str) {
    if (s->n >= s->cap) { int nc = s->cap ? s->cap * 2 : 64; const char** nv = (const char**)realloc(s->v, sizeof(char*) * nc); if (!nv) return; s->v = nv; s->cap = nc; }
    s->v[s->n++] = str;
}
static int sx_set_cmp(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }
static void sx_set_sort(SxSet* s) { if (s->n > 1) qsort(s->v, s->n, sizeof(char*), sx_set_cmp); }
static int sx_set_has(const SxSet* s, const char* name) {
    const char** r = (const char**)bsearch(&name, s->v, s->n, sizeof(char*), sx_set_cmp);
    return r != NULL;
}
/* 从 meta 文本提取每行第一字段（name|...）到集合 */
static void sx_set_from_meta(SxSet* s, const char* meta) {
    const char* p = meta;
    while (p && *p) {
        const char* nl = strchr(p, '\n');
        const char* bar = strchr(p, '|');
        int n = (nl ? (int)(nl - p) : (int)strlen(p));
        int m = bar ? (int)(bar - p) : n;
        if (m > n) m = n;
        if (m > 0) {
            char* dup = (char*)malloc((size_t)m + 1);
            if (dup) { memcpy(dup, p, (size_t)m); dup[m] = 0; sx_set_push(s, dup); }
        }
        if (!nl) break;
        p = nl + 1;
    }
}
static void sx_set_free(SxSet* s) { int i; for (i = 0; i < s->n; i++) free((void*)s->v[i]); free(s->v); s->v = NULL; s->n = s->cap = 0; }

static int sx_is_keyword(const char* s, int n) {
    int i;
    for (i = 0; i < SX_KW_N; i++) {
        if ((int)strlen(SX_KWS[i]) == n && memcmp(SX_KWS[i], s, (size_t)n) == 0) return 1;
    }
    return 0;
}
static void sx_tok_emit(Buf* out, int* first, int* prev_line, int* prev_char,
                        int line0, int col0, int len, int type, int mod) {
    int dl = line0 - *prev_line;
    int dc = (dl == 0) ? (col0 - *prev_char) : col0;
    if (!*first) buf_putc(out, ',');
    *first = 0;
    buf_puti(out, dl); buf_putc(out, ',');
    buf_puti(out, dc); buf_putc(out, ',');
    buf_puti(out, len); buf_putc(out, ',');
    buf_puti(out, type); buf_putc(out, ',');
    buf_puti(out, mod);
    *prev_line = line0;
    *prev_char = col0 + len;
}

const char* shadow_index_tokens(const char* src) {
    SxSet funcs = {0}, types = {0};
    Buf out;
    int i, first = 1, prev_line = 0, prev_char = 0, line0 = 0, col0 = 0;
    char prev_tok[64] = "";
    const char* s = src ? src : "";
    if (!g_inited) return sx_host_dup("");
    /* 符号集合：fmeta → 函数名；smeta+emeta → 类型名；+ 内建函数 */
    sx_set_from_meta(&funcs, buf_cstr(&g_fmeta));
    sx_set_from_meta(&types, buf_cstr(&g_smeta));
    sx_set_from_meta(&types, buf_cstr(&g_emeta));
    for (i = 0; i < SX_BUILTIN_N; i++) {
        char* dup = (char*)malloc(strlen(SX_BUILTINS[i]) + 1);
        if (dup) { strcpy(dup, SX_BUILTINS[i]); sx_set_push(&funcs, dup); }
    }
    sx_set_sort(&funcs); sx_set_sort(&types);

    buf_init(&out);
    buf_puts(&out, "{\"data\":[");
    while (*s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '\n') { line0++; col0 = 0; s++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { col0++; s++; continue; }
        if (ch == '/' && s[1] == '/') {           /* 行注释：跳过（不产 token） */
            while (*s && *s != '\n') { col0++; s++; }
            continue;
        }
        if (ch == '/' && s[1] == '*') {           /* 块注释：跳过，跨行维护行列 */
            s += 2; col0 += 2;
            while (*s && !(*s == '*' && s[1] == '/')) {
                if (*s == '\n') { line0++; col0 = 0; } else col0++;
                s++;
            }
            if (*s) { s += 2; col0 += 2; }
            continue;
        }
        if (ch == '"' || ch == '\'') {            /* 字符串/字符：含引号，处理转义 */
            char q = (char)ch;
            int sl = line0, sc = col0;
            s++; col0++;
            while (*s && *s != q) {
                if (*s == '\\' && s[1]) { s += 2; col0 += 2; }
                else { if (*s == '\n') { line0++; col0 = 0; } else col0++; s++; }
            }
            if (*s == q) { s++; col0++; }
            sx_tok_emit(&out, &first, &prev_line, &prev_char, sl, sc, col0 - sc, 8, 0);
            continue;
        }
        if (ch >= '0' && ch <= '9') {             /* 数字字面量 */
            int sl = line0, sc = col0;
            while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '.')) { col0++; s++; }
            sx_tok_emit(&out, &first, &prev_line, &prev_char, sl, sc, col0 - sc, 7, 0);
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') {  /* 标识符/关键字 */
            int sl = line0, sc = col0;
            int n = 0, type = -1, mod = 0;
            char tok[128];
            while (*s && (isalnum((unsigned char)*s) || *s == '_')) { if (n < 126) tok[n++] = (char)*s; col0++; s++; }
            tok[n] = 0;
            if (sx_is_keyword(tok, n)) { type = 4; }
            else {
                if (strcmp(prev_tok, "kimo") == 0) { type = 1; mod = 1; }
                else if (sx_set_has(&funcs, tok)) {
                    type = 1;
                    if (*s == '(') mod = 1;
                }
                else if (sx_set_has(&types, tok)) { type = 5; }
                else { type = 0; }
            }
            if (type >= 0) sx_tok_emit(&out, &first, &prev_line, &prev_char, sl, sc, n, type, mod);
            memcpy(prev_tok, tok, (size_t)n + 1);
            continue;
        }
        {   /* 标点/运算符：单字符 token，仅记录 prev */
            prev_tok[0] = (char)ch; prev_tok[1] = 0;
            col0++; s++;
        }
    }
    buf_puts(&out, "]}");
    sx_set_free(&funcs); sx_set_free(&types);
    return sx_host_dup(buf_cstr(&out));
}

/* ---------------- hover：纯索引快路径（无需完整 TC） ----------------
 * 给定文档源码 src 与 1-based (line, col)，定位光标下的标识符（不含 '.'），
 * 从 fmeta/gmeta/smeta/emeta + 内建表 + 源码局部声明 推断其类型/签名，
 * 返回与 api_hover_info 同格式的 hover 内容串；定位不到或索引不足以判定时
 * 返回 ""（宿主回退全量 TC）。优先级对齐 api_hover_info：
 *   函数(fmeta) → 内建 → 局部变量/参数 → 全局(gmeta) → 结构体(smeta) → 枚举(emeta)
 * 说明：局部变量类型仅做"源码向前扫描最后一个同名 let/mut/参数声明"的启发式，
 *   可能误命中同名外层变量；这是快路径的已知近似，索引无法判定时仍走全量 TC。 */
static int sx_ident_char_c(char c) {
    return isalnum((unsigned char)c) || c == '_';
}
/* 在 src 的 1-based (line,col) 处定位标识符，成功写入 out 返回 1（不含 '.'） */
static int sx_ident_at(const char* src, int line, int col, char* out, int cap) {
    out[0] = 0;
    if (!src || line < 1 || col < 1) return 0;
    int i = 0, cur = 1;
    while (src[i] && cur < line) { if (src[i] == '\n') cur++; i++; }
    if (cur != line) return 0;
    int line_start = i;
    int line_end = i;
    while (src[line_end] && src[line_end] != '\n') line_end++;
    int c0 = line_start + (col - 1);
    if (c0 < line_start || c0 > line_end) return 0;
    int s = c0, e = c0;
    while (s > line_start && sx_ident_char_c(src[s - 1])) s--;
    while (e < line_end && sx_ident_char_c(src[e])) e++;
    if (e <= s) return 0;
    int n = e - s;
    if (n >= cap) n = cap - 1;
    memcpy(out, src + s, (size_t)n); out[n] = 0;
    if (!(isalpha((unsigned char)out[0]) || out[0] == '_')) return 0;
    return 1;
}
/* 取 line[0..linelen) 第 idx 个 '|' 字段，写入 ob 返回长度（不含结尾 '\0'） */
static int sx_field_copy(const char* line, int linelen, int idx, char* ob, int obcap) {
    int f = 0; const char* p = line; const char* end = line + linelen;
    while (p < end && f < idx) { if (*p == '|') f++; p++; }
    if (f != idx) { if (obcap > 0) ob[0] = 0; return 0; }
    const char* q = p;
    while (q < end && *q != '|') q++;
    int n = (int)(q - p);
    if (n >= obcap) n = obcap - 1;
    if (n > 0) memcpy(ob, p, (size_t)n);
    if (obcap > 0) ob[n] = 0;
    return n;
}
/* meta 的 name 段可能带 __mod_<ns>__ 前缀（smeta/emeta/gmeta 的 compiler mangle），
 * fmeta 顶层函数名则不带。返回去掉该前缀后的裸名（NUL 结尾，写入内部静态缓冲），
 * 用于与源码标识符比较。注意 name 是 meta 行内指针，strlen 会越界，必须按 namelen 截断。 */
static const char* sx_bare_name(const char* name, int namelen) {
    static char s_bn[512];
    const char* b = name;
    int n = namelen;
    if (namelen >= 6 && memcmp(name, "__mod_", 6) == 0) {
        int i;
        for (i = 6; i + 1 < namelen; i++) {
            if (name[i] == '_' && name[i + 1] == '_') {
                b = name + i + 2;
                n = namelen - (i + 2);
                break;
            }
        }
    }
    if (n >= (int)sizeof(s_bn)) n = (int)sizeof(s_bn) - 1;
    if (n < 0) n = 0;
    memcpy(s_bn, b, (size_t)n);
    s_bn[n] = 0;
    return s_bn;
}
/* meta 首行 name 段匹配 ident 即按 kind 产出 hover 串到 out；命中返回 1。
 * kind: 0=fmeta(sig+内部标记) 1=gmeta(let type) 2=smeta(struct) 3=emeta(enum) */
static int sx_meta_first(const char* meta, const char* ident, Buf* out, int kind) {
    buf_clear(out);
    if (!meta || !*meta) return 0;
    int ilen = (int)strlen(ident);
    const char* p = meta;
    while (*p) {
        const char* nl = strchr(p, '\n');
        int linelen = nl ? (int)(nl - p) : (int)strlen(p);
        const char* bar = (const char*)memchr(p, '|', (size_t)linelen);
        int namelen = bar ? (int)(bar - p) : linelen;
        const char* bn = sx_bare_name(p, namelen);
        int bnlen = (int)strlen(bn);
        if (bnlen == ilen && memcmp(bn, ident, (size_t)ilen) == 0) {
            char fld[2048], fld2[64];
            if (kind == 0) {
                sx_field_copy(p, linelen, 6, fld, sizeof(fld));
                sx_field_copy(p, linelen, 7, fld2, sizeof(fld2));
                buf_puts(out, fld);
                if (!(fld2[0] == '1' && fld2[1] == 0)) buf_puts(out, "\n\n（内部函数）");
            } else if (kind == 1) {
                sx_field_copy(p, linelen, 5, fld, sizeof(fld));
                buf_puts(out, "let "); buf_puts(out, ident); buf_puts(out, ": "); buf_puts(out, fld);
            } else if (kind == 2) {
                buf_puts(out, "struct "); buf_puts(out, ident);
            } else {
                buf_puts(out, "enum "); buf_puts(out, ident);
            }
            return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}
/* 内建函数 hover 表（与 api_builtin_entries 同数据源） */
static const char* const SX_BUILTIN_HOVER[][3] = {
    {"len","len(s) -> int","返回字符串或数组长度"},
    {"str_eq","str_eq(a, b) -> int","字符串相等比较，相等返回 1 否则 0"},
    {"str_contains","str_contains(s, needle) -> int","检查 s 是否包含子串 needle"},
    {"str_index_of","str_index_of(s, needle) -> int","返回子串起始下标，未找到返回 -1"},
    {"str_replace","str_replace(s, old, new) -> string","替换所有匹配子串"},
    {"str_trim","str_trim(s) -> string","去除首尾空白"},
    {"str_to_upper","str_to_upper(s) -> string","转大写"},
    {"str_to_lower","str_to_lower(s) -> string","转小写"},
    {"str_char_at","str_char_at(s, idx) -> string","取第 idx 个字符"},
    {"str_split","str_split(s, delim) -> array","按分隔符拆分为字符串数组"},
    {"str_join","str_join(arr, sep) -> string","用分隔符连接数组元素"},
    {"str_format","str_format(fmt, args) -> string","按 {0} 占位符格式化"},
    {"substr2","substr2(s, start, end) -> string","取子串 [start, end)"},
    {"int_to_str","int_to_str(v) -> string","整数转字符串"},
    {"long_to_str","long_to_str(v) -> string","长整数转字符串"},
    {"float_to_str","float_to_str(v) -> string","浮点转字符串"},
    {"bool_to_str","bool_to_str(v) -> string","布尔转字符串"},
    {"parse_int","parse_int(s) -> int","字符串解析为整数"},
    {"parse_long","parse_long(s) -> long","字符串解析为长整数"},
    {"parse_float","parse_float(s) -> float","字符串解析为浮点"},
    {"parse_bool","parse_bool(s) -> bool","字符串解析为布尔"},
    {"println","println(s)","打印并换行"},
    {"print","print(s)","打印不换行"},
    {"to_string","to_string(v) -> string","任意值转字符串"},
    {"array_push","array_push(arr, v) -> array","数组尾部追加元素，返回新数组"},
    {"file_read","file_read(path) -> string","读取文件内容"},
    {"file_write","file_write(path, content) -> int","写入文件，返回写入字节数"},
    {"file_exists","file_exists(path) -> int","检查文件是否存在"},
    {"file_delete","file_delete(path) -> int","删除文件"},
    {"file_list_dir","file_list_dir(path) -> string","列出目录文件（分号分隔）"},
    {"dir_create","dir_create(path) -> int","创建目录"},
    {"path_exists","path_exists(path) -> int","检查路径是否存在"},
    {"path_join","path_join(base, child) -> string","拼接路径"},
    {"path_dirname","path_dirname(path) -> string","取路径目录名"},
    {"path_basename","path_basename(path) -> string","取路径文件名"},
    {"hashmap_new","hashmap_new() -> map","创建 HashMap"},
    {"hashmap_insert","hashmap_insert(m, k, v)","插入键值对"},
    {"hashmap_get","hashmap_get(m, k) -> any","取键对应值"},
    {"hashmap_contains","hashmap_contains(m, k) -> int","检查键是否存在"},
    {"hashmap_remove","hashmap_remove(m, k)","删除键"},
    {"hashmap_size","hashmap_size(m) -> int","返回元素数量"},
    {"set_new","set_new() -> set","创建 Set"},
    {"set_add","set_add(s, v)","添加元素"},
    {"set_contains","set_contains(s, v) -> int","检查元素是否存在"},
    {"set_remove","set_remove(s, v)","删除元素"},
    {"set_size","set_size(s) -> int","返回元素数量"},
    {"rt_spawn_nullary","rt_spawn_nullary(f)","spawn 无参协程"},
    {"rt_future_await","rt_future_await(f)","等待 Future 完成"},
    {NULL, NULL, NULL}
};
static int sx_builtin_hover(const char* ident, Buf* out) {
    buf_clear(out);
    int i = 0;
    while (SX_BUILTIN_HOVER[i][0]) {
        if (strcmp(SX_BUILTIN_HOVER[i][0], ident) == 0) {
            buf_puts(out, SX_BUILTIN_HOVER[i][1]);
            buf_puts(out, "\n\n（内置函数）");
            if (SX_BUILTIN_HOVER[i][2] && SX_BUILTIN_HOVER[i][2][0]) {
                buf_puts(out, "\n\n"); buf_puts(out, SX_BUILTIN_HOVER[i][2]);
            }
            return 1;
        }
        i++;
    }
    return 0;
}
/* 源码向前扫描最后一个同名 let/mut/var 或函数参数声明，产出 "let ident: TYPE" 启发式结果 */
static int sx_decl_has(const char* line, int linelen, const char* ident, Buf* out) {
    char buf[8192];
    int n = linelen < (int)sizeof(buf) - 1 ? linelen : (int)sizeof(buf) - 1;
    memcpy(buf, line, (size_t)n); buf[n] = 0;
    int ilen = (int)strlen(ident);
    if (ilen == 0) return 0;
    const char* kws[3] = { "let ", "mut ", "var " };
    int ki;
    for (ki = 0; ki < 3; ki++) {
        const char* kp = strstr(buf, kws[ki]);
        while (kp) {
            const char* t = kp + strlen(kws[ki]);
            while (*t == ' ' || *t == '\t') t++;
            if (strncmp(t, ident, (size_t)ilen) == 0 && !sx_ident_char_c(t[ilen])) {
                const char* colon = strchr(t + ilen, ':');
                if (colon) {
                    const char* ty = colon + 1;
                    while (*ty == ' ' || *ty == '\t') ty++;
                    const char* te = ty;
                    while (*te && *te != '=' && *te != ',' && *te != ')' && *te != ';' && *te != '\n') te++;
                    int tlen = (int)(te - ty);
                    buf_clear(out);
                    buf_puts(out, "let "); buf_puts(out, ident); buf_puts(out, ": ");
                    if (tlen > 0) buf_putn(out, ty, tlen);
                    return 1;
                }
            }
            kp = strstr(kp + 1, kws[ki]);
        }
    }
    /* 函数参数：fn ...( ... ident : TYPE ... ) */
    const char* fn = strstr(buf, "fn ");
    if (fn) {
        const char* ob = strchr(fn, '(');
        if (ob) {
            const char* cb = strchr(ob, ')');
            const char* pend = cb ? cb : (buf + n);
            const char* q = ob + 1;
            while (q < pend) {
                while (q < pend && (*q == ' ' || *q == '\t')) q++;
                if (strncmp(q, ident, (size_t)ilen) == 0 && !sx_ident_char_c(q[ilen])) {
                    const char* colon = strchr(q + ilen, ':');
                    if (colon && colon < pend) {
                        const char* ty = colon + 1;
                        while (*ty == ' ' || *ty == '\t') ty++;
                        const char* te = ty;
                        while (te < pend && *te != ',' && *te != ')' && *te != '\n') te++;
                        int tlen = (int)(te - ty);
                        buf_clear(out);
                        buf_puts(out, "let "); buf_puts(out, ident); buf_puts(out, ": ");
                        if (tlen > 0) buf_putn(out, ty, tlen);
                        return 1;
                    }
                }
                const char* comma = strchr(q + 1, ',');
                if (!comma || comma >= pend) break;
                q = comma + 1;
            }
        }
    }
    return 0;
}
static int sx_local_type(const char* src, int line, const char* ident, Buf* out) {
    buf_clear(out);
    int cur = 1;
    const char* p = src;
    while (*p && cur <= line) {
        const char* nl = strchr(p, '\n');
        int linelen = nl ? (int)(nl - p) : (int)strlen(p);
        sx_decl_has(p, linelen, ident, out); /* 命中则填充 out；循环继续取更靠近 hover 的声明 */
        cur++;
        if (!nl) break;
        p = nl + 1;
    }
    return buf_cstr(out)[0] ? 1 : 0;
}
const char* shadow_index_hover(const char* src, int line, int col) {
    char ident[256];
    Buf out; buf_init(&out);
    if (!g_inited) return sx_host_dup("");
    if (!sx_ident_at(src, line, col, ident, sizeof(ident))) return sx_host_dup("");
    if (sx_meta_first(buf_cstr(&g_fmeta), ident, &out, 0)) return sx_host_dup(buf_cstr(&out));
    if (sx_builtin_hover(ident, &out)) return sx_host_dup(buf_cstr(&out));
    if (sx_local_type(src, line, ident, &out)) return sx_host_dup(buf_cstr(&out));
    if (sx_meta_first(buf_cstr(&g_gmeta), ident, &out, 1)) return sx_host_dup(buf_cstr(&out));
    if (sx_meta_first(buf_cstr(&g_smeta), ident, &out, 2)) return sx_host_dup(buf_cstr(&out));
    if (sx_meta_first(buf_cstr(&g_emeta), ident, &out, 3)) return sx_host_dup(buf_cstr(&out));
    return sx_host_dup("");
}

void shadow_index_reset(void) { sx_state_reset(); }

/* 单调毫秒时钟：进程启动以来的毫秒数（QueryPerformanceCounter）。
   供 LSP DBG 日志打时间戳、诊断防抖窗口判定。返回 int（进程存活期内不会溢出 32 位）。 */
static LARGE_INTEGER g_clock_freq;
static LARGE_INTEGER g_clock_base;
static int g_clock_inited = 0;
int shadow_now_ms(void) {
    if (!g_clock_inited) {
        QueryPerformanceFrequency(&g_clock_freq);
        QueryPerformanceCounter(&g_clock_base);
        g_clock_inited = 1;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG ms = (now.QuadPart - g_clock_base.QuadPart) * 1000 / g_clock_freq.QuadPart;
    return (int)ms;
}

/* ============ 工作区级精确符号表（真实坐标 JSON，供扩展侧 goto-def/hover） ============
 * 与 sx_scan_module 同源逻辑，但：① 对单文件扫描（Cur.li/col 即真实文件行/列）；
 *   ② 直接产出 JSON（{symbols:[...]}），不绕 combined 坐标、不依赖 import 图；
 *   ③ 仅编译进独立 cindex.exe（本段整体在 SHADOW_INDEX_STANDALONE 内），
 *      不影响 link 进 shadow.exe 的快路径，更不触及自举固定点 IR。 */
#ifdef SHADOW_INDEX_STANDALONE

/* JSON 字符串转义（" \ 及控制字符） */
static void sj_str(Buf* b, const char* s) {
    static const char* hex = "0123456789abcdef";
    unsigned char ch;
    if (!s) return;
    for (; *s; s++) {
        ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { buf_putc(b, '\\'); buf_putc(b, (char)ch); }
        else if (ch == '\n') buf_puts(b, "\\n");
        else if (ch == '\r') buf_puts(b, "\\r");
        else if (ch == '\t') buf_puts(b, "\\t");
        else if (ch < 0x20) { buf_puts(b, "\\u00"); buf_putc(b, hex[(ch >> 4) & 0xf]); buf_putc(b, hex[ch & 0xf]); }
        else buf_putc(b, (char)ch);
    }
}

static void sj_emit(Buf* out, int* first, const char* file, int line, int col,
                    const char* kind, const char* name, int is_pub,
                    const char* ns, const char* sig) {
    if (!*first) buf_putc(out, ',');
    *first = 0;
    buf_puts(out, "{\"file\":\"");
    sj_str(out, file);
    buf_puts(out, "\",\"line\":");
    buf_puti(out, line);
    buf_puts(out, ",\"col\":");
    buf_puti(out, col);
    buf_puts(out, ",\"kind\":\"");
    buf_puts(out, kind);
    buf_puts(out, "\",\"name\":\"");
    sj_str(out, name);
    buf_puts(out, "\",\"is_pub\":");
    buf_puti(out, is_pub);
    buf_puts(out, ",\"ns\":\"");
    sj_str(out, ns ? ns : "");
    buf_puts(out, "\",\"sig\":\"");
    sj_str(out, sig ? sig : "");
    buf_puts(out, "\"}");
}

/* 镜像 sx_scan_kimo，但产出 JSON（真实坐标） */
static void sx_scan_kimo_real(Cur* c, int kw_li, int kw_col, int is_pub, int is_extern,
                              const char* name_prefix, const char* file, const char* ns,
                              Buf* out, int* first) {
    char name[256], full[512], rtbuf[256];
    Buf praw, sig, rett;
    int eli, ecol, nli = 0, ncol = 1;
    (void)is_extern;
    buf_init(&praw); buf_init(&sig); buf_init(&rett);
    cur_skip_ws(c);
    name[0] = 0;
    nli = c->li; ncol = c->col;
    if (is_ident_start(c->s[c->i])) cur_read_ident(c, name, sizeof(name));
    cur_skip_ws(c);
    if (c->s[c->i] == '<') cur_skip_balanced(c, '<', '>', NULL);
    cur_skip_ws(c);
    if (c->s[c->i] == '(') cur_skip_balanced(c, '(', ')', &praw);
    cur_skip_ws(c);
    if (c->s[c->i] == '-' && c->s[c->i + 1] == '>') {
        cur_adv(c); cur_adv(c);
        cur_skip_ws(c);
        while (c->s[c->i] && c->s[c->i] != '{' && c->s[c->i] != ';' && c->s[c->i] != '\n') {
            buf_putc(&rett, c->s[c->i]); cur_adv(c);
        }
    }
    cur_skip_ws(c);
    eli = c->li; ecol = c->col;
    if (c->s[c->i] == '{') { cur_skip_balanced(c, '{', '}', NULL); eli = c->li; ecol = c->col; }
    else if (c->s[c->i] == ';') { cur_adv(c); eli = c->li; ecol = c->col; }
    if (name[0]) {
        const char* rt;
        snprintf(full, sizeof(full), "%s%s", name_prefix ? name_prefix : "", name);
        sx_canon_type_buf(buf_cstr(&rett), rtbuf, sizeof(rtbuf));
        if (!rtbuf[0]) snprintf(rtbuf, sizeof(rtbuf), "any");
        rt = rtbuf;
        buf_puts(&sig, "fn "); buf_puts(&sig, full); buf_putc(&sig, '(');
        sx_format_params(buf_cstr(&praw), &sig);
        buf_puts(&sig, ") -> "); buf_puts(&sig, rt);
        sj_emit(out, first, file, nli, ncol, "fn", full, is_pub, ns, buf_cstr(&sig));
    }
    buf_free(&praw); buf_free(&sig); buf_free(&rett);
}

/* 单文件实时坐标扫描（顶层循环镜像 sx_scan_module） */
static void sx_scan_file_real(const char* path, const char* rel_key, const char* src,
                              Buf* out, int* first) {
    Cur c;
    int pend_pub = 0, pend_extern = 0;
    char ns[512];
    if (rel_key && rel_key[0]) {
        int i, ln = (int)strlen(rel_key);
        snprintf(ns, sizeof(ns), "%s", rel_key);
        if (ln > 7 && strcmp(ns + ln - 7, ".shadow") == 0) ns[ln - 7] = 0;
        for (i = 0; ns[i]; i++) if (ns[i] == '\\') ns[i] = '.';
    } else ns[0] = 0;

    c.s = src; c.i = 0; c.li = 0; c.col = 1;
    for (;;) {
        char ch;
        cur_skip_ws(&c);
        ch = c.s[c.i];
        if (ch == 0) return;
        if (ch == '@') {
            char attr[64];
            cur_adv(&c);
            if (is_ident_start(c.s[c.i])) cur_read_ident(&c, attr, sizeof(attr));
            cur_skip_ws(&c);
            if (c.s[c.i] == '(') cur_skip_balanced(&c, '(', ')', NULL);
            continue;
        }
        if (ch == '"' || ch == '\'') { cur_skip_quoted(&c); continue; }
        if (!is_ident_start(ch)) {
            if (ch == '{') { cur_skip_balanced(&c, '{', '}', NULL); pend_pub = 0; pend_extern = 0; continue; }
            cur_adv(&c); continue;
        }
        {
            char kw[64];
            int kw_li = c.li, kw_col = c.col;
            cur_read_ident(&c, kw, sizeof(kw));
            if (strcmp(kw, "pub") == 0) { pend_pub = 1; continue; }
            if (strcmp(kw, "kimo") == 0) {
                sx_scan_kimo_real(&c, kw_li, kw_col, pend_pub, pend_extern, "", path, ns, out, first);
                pend_pub = 0; pend_extern = 0; continue;
            }
            if (strcmp(kw, "struct") == 0 || strcmp(kw, "enum") == 0) {
                char name[256]; int eli, ecol, nli = 0, ncol = 1; int is_struct = (kw[0] == 's');
                cur_skip_ws(&c); name[0] = 0;
                nli = c.li; ncol = c.col;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, name, sizeof(name));
                cur_skip_ws(&c);
                if (c.s[c.i] == '<') cur_skip_balanced(&c, '<', '>', NULL);
                cur_skip_ws(&c);
                eli = c.li; ecol = c.col;
                if (c.s[c.i] == '{') { cur_skip_balanced(&c, '{', '}', NULL); eli = c.li; ecol = c.col; }
                if (name[0]) sj_emit(out, first, path, nli, ncol, is_struct ? "struct" : "enum", name, pend_pub, ns, "");
                pend_pub = 0; pend_extern = 0; continue;
            }
            if (strcmp(kw, "trait") == 0 || strcmp(kw, "interface") == 0) {
                char name[256]; int nli = 0, ncol = 1;
                cur_skip_ws(&c); name[0] = 0;
                nli = c.li; ncol = c.col;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, name, sizeof(name));
                while (c.s[c.i] && c.s[c.i] != '{') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] == '{') cur_skip_balanced(&c, '{', '}', NULL);
                if (name[0]) sj_emit(out, first, path, nli, ncol, "trait", name, pend_pub, ns, "");
                pend_pub = 0; pend_extern = 0; continue;
            }
            if (strcmp(kw, "impl") == 0) {
                char first_[256], type_name[256], prefix[600];
                int mpub = 0, depth;
                first_[0] = 0; type_name[0] = 0; prefix[0] = 0;
                cur_skip_ws(&c);
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, first_, sizeof(first_));
                cur_skip_ws(&c);
                if (c.s[c.i] == '<') { cur_skip_balanced(&c, '<', '>', NULL); cur_skip_ws(&c); }
                if (is_ident_start(c.s[c.i])) {
                    char kw2[64]; int si = c.i, sli = c.li, scol = c.col;
                    cur_read_ident(&c, kw2, sizeof(kw2));
                    if (strcmp(kw2, "for") == 0) {
                        cur_skip_ws(&c);
                        if (is_ident_start(c.s[c.i])) cur_read_ident(&c, type_name, sizeof(type_name));
                        cur_skip_ws(&c);
                        if (c.s[c.i] == '<') { cur_skip_balanced(&c, '<', '>', NULL); cur_skip_ws(&c); }
                        snprintf(prefix, sizeof(prefix), "__impl__%s__%s__", type_name, first_);
                    } else { c.i = si; c.li = sli; c.col = scol; }
                }
                while (c.s[c.i] && c.s[c.i] != '{') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] != '{') { pend_pub = 0; pend_extern = 0; continue; }
                cur_adv(&c); depth = 1;
                while (depth > 0 && c.s[c.i]) {
                    char mkw[64]; int mli, mcol;
                    cur_skip_ws(&c);
                    if (!c.s[c.i]) break;
                    if (c.s[c.i] == '}') { cur_adv(&c); depth--; continue; }
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    if (c.s[c.i] == '{') { cur_skip_balanced(&c, '{', '}', NULL); continue; }
                    if (!is_ident_start(c.s[c.i])) { cur_adv(&c); continue; }
                    mli = c.li; mcol = c.col;
                    cur_read_ident(&c, mkw, sizeof(mkw));
                    if (strcmp(mkw, "pub") == 0) { mpub = 1; continue; }
                    if (strcmp(mkw, "kimo") == 0) {
                        sx_scan_kimo_real(&c, mli, mcol, mpub, 0, prefix, path, ns, out, first);
                        mpub = 0; continue;
                    }
                    mpub = 0;
                }
                pend_pub = 0; pend_extern = 0; continue;
            }
            if (strcmp(kw, "let") == 0 || strcmp(kw, "mut") == 0) {
                char gname[256], tybuf[512]; int nli, ncol, ang = 0; Buf gty;
                cur_skip_ws(&c); nli = c.li; ncol = c.col; gname[0] = 0;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, gname, sizeof(gname));
                cur_skip_ws(&c); buf_init(&gty);
                if (c.s[c.i] == ':') {
                    cur_adv(&c); cur_skip_ws(&c);
                    while (c.s[c.i]) {
                        char t = c.s[c.i];
                        if (t == '<') ang++;
                        else if (t == '>') { if (ang > 0) ang--; }
                        else if (ang == 0 && (t == '=' || t == ';')) break;
                        else if (t == '\n') break;
                        buf_putc(&gty, t); cur_adv(&c);
                    }
                }
                if (gname[0]) {
                    sx_canon_type_buf(buf_cstr(&gty), tybuf, sizeof(tybuf));
                    if (!tybuf[0]) snprintf(tybuf, sizeof(tybuf), "unresolved");
                    sj_emit(out, first, path, nli, ncol, "global", gname, pend_pub, ns, tybuf);
                }
                buf_free(&gty);
                { int par = 0, brk = 0, brc = 0;
                  while (c.s[c.i]) {
                      char t = c.s[c.i];
                      if (t == '"' || t == '\'') { cur_skip_quoted(&c); continue; }
                      if (t == '(') { cur_skip_balanced(&c, '(', ')', NULL); continue; }
                      if (t == '[') { cur_skip_balanced(&c, '[', ']', NULL); continue; }
                      if (t == '{') { cur_skip_balanced(&c, '{', '}', NULL); continue; }
                      if (t == ';' && par == 0 && brk == 0 && brc == 0) { cur_adv(&c); break; }
                      cur_adv(&c);
                  } }
                pend_pub = 0; pend_extern = 0; continue;
            }
            if (strcmp(kw, "type") == 0) {
                char tname[256]; int tnli = 0, tncol = 1;
                cur_skip_ws(&c); tname[0] = 0;
                tnli = c.li; tncol = c.col;
                if (is_ident_start(c.s[c.i])) cur_read_ident(&c, tname, sizeof(tname));
                if (tname[0]) sj_emit(out, first, path, tnli, tncol, "type", tname, pend_pub, ns, "");
                while (c.s[c.i] && c.s[c.i] != ';' && c.s[c.i] != '\n') {
                    if (c.s[c.i] == '"' || c.s[c.i] == '\'') { cur_skip_quoted(&c); continue; }
                    cur_adv(&c);
                }
                if (c.s[c.i] == ';') cur_adv(&c);
                pend_pub = 0; pend_extern = 0; continue;
            }
            pend_pub = 0; pend_extern = 0; continue;
        }
    }
}

/* 递归遍历 root 下所有 .shadow，跳过无关目录；产出工作区级符号表 JSON */
static void sx_walk(const char* root, const char* dir, Buf* out, int* first) {
    char search[1400];
    WIN32_FIND_DATAA fd; HANDLE h;
    snprintf(search, sizeof(search), "%s\\*", dir);
    h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.cFileName[0] == '.') continue;   /* 隐藏/临时文件（.shadow_lsp_* 等） */
        char full[1400];
        snprintf(full, sizeof(full), "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* 外部/构建/工具链目录一律跳过 */
            if (strcmp(fd.cFileName, "node_modules") == 0 ||
                strcmp(fd.cFileName, "build") == 0 || strcmp(fd.cFileName, "target") == 0 ||
                strcmp(fd.cFileName, "dist") == 0 || strcmp(fd.cFileName, "out") == 0 ||
                strcmp(fd.cFileName, "vendor") == 0 ||
                strcmp(fd.cFileName, ".workbuddy") == 0 || strcmp(fd.cFileName, ".vscode") == 0) continue;
            /* 项目边界：含 shadow.sbg 的子目录 = 独立项目（shadow.exe -init 生成），
               递归扫描时跳过，避免把同目录树下的其他项目文件扫进本项目的符号表 */
            {
                char sbgp[1400];
                snprintf(sbgp, sizeof(sbgp), "%s\\shadow.sbg", full);
                if (sx_is_file(sbgp)) continue;
            }
            sx_walk(root, full, out, first);
        } else {
            int nl = (int)strlen(fd.cFileName);
            if (nl > 7 && strcmp(fd.cFileName + nl - 7, ".shadow") == 0) {
                char* src = sx_read_file(full);
                if (src && src[0]) {
                    const char* rel = full;
                    int rl = (int)strlen(root);
                    if (rl > 0 && strncmp(full, root, rl) == 0 && full[rl] == '\\') rel = full + rl + 1;
                    sx_scan_file_real(full, rel, src, out, first);
                    free(src);
                }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static const char* sx_workspace_json(const char* root) {
    Buf out; int first = 1;
    buf_init(&out);
    buf_puts(&out, "{\"symbols\":[");
    sx_walk(root, root, &out, &first);
    buf_puts(&out, "]}");
    return sx_host_dup(buf_cstr(&out));
}

#endif  /* SHADOW_INDEX_STANDALONE（工作区符号表） */

/* ---------------- 独立自测入口 ---------------- */
#ifdef SHADOW_INDEX_STANDALONE
int main(int argc, char** argv) {
    const char* arg0;
    int ok;
    if (argc < 2) {
        fprintf(stderr, "usage: cindex <entry.shadow> [arg0-exe] [--smeta|--emeta|--gmeta|--stat|--all]\n");
        return 2;
    }
    arg0 = (argc > 2 && strncmp(argv[2], "--", 2) != 0) ? argv[2] : argv[0];
    /* 工作区模式：-w <root> 扫描 root 下所有 .shadow，输出精确符号表 JSON */
    if (strcmp(argv[1], "-w") == 0) {
        const char* root = (argc > 2) ? argv[2] : ".";
        const char* j = sx_workspace_json(root);
        printf("%s\n", j ? j : "{\"symbols\":[]}");
        return 0;
    }
    ok = shadow_index_build(argv[1], "", arg0);
    {
        int i, want_s = 0, want_e = 0, want_g = 0, want_stat = 0, want_t = 0, want_all = 0;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--smeta") == 0) want_s = 1;
            else if (strcmp(argv[i], "--emeta") == 0) want_e = 1;
            else if (strcmp(argv[i], "--gmeta") == 0) want_g = 1;
            else if (strcmp(argv[i], "--stat") == 0) want_stat = 1;
            else if (strcmp(argv[i], "--tokens") == 0) want_t = 1;
            else if (strcmp(argv[i], "--hover") == 0) {
                int hl = (i + 1 < argc) ? atoi(argv[i + 1]) : 0;
                int hc = (i + 2 < argc) ? atoi(argv[i + 2]) : 0;
                char* hsrc = sx_read_file(argv[1]);
                if (!hsrc) { fprintf(stderr, "read fail\n"); return 1; }
                const char* h = shadow_index_hover(hsrc, hl, hc);
                printf("hover(%d,%d)=[%s]\n", hl, hc, h ? h : "");
                free(hsrc);
                return 0;
            }
            else if (strcmp(argv[i], "--all") == 0) want_all = 1;
        }
        if (want_t) {
            /* 语义 token 快照（delta 编码 JSON），自测 shadow_index_tokens */
            char* src = sx_read_file(argv[1]);
            if (!src) { fprintf(stderr, "read fail\n"); return 1; }
            printf("%s\n", shadow_index_tokens(src));
            free(src);
            return 0;
        }
        if (want_stat) {
            int nf = 0, ns = 0, ne = 0, ng = 0;
            const char* p;
            if (buf_cstr(&g_fmeta)[0]) { nf = 1; for (p = buf_cstr(&g_fmeta); *p; p++) if (*p == '\n') nf++; }
            if (buf_cstr(&g_smeta)[0]) { ns = 1; for (p = buf_cstr(&g_smeta); *p; p++) if (*p == '\n') ns++; }
            if (buf_cstr(&g_emeta)[0]) { ne = 1; for (p = buf_cstr(&g_emeta); *p; p++) if (*p == '\n') ne++; }
            if (buf_cstr(&g_gmeta)[0]) { ng = 1; for (p = buf_cstr(&g_gmeta); *p; p++) if (*p == '\n') ng++; }
            printf("ok=%d lines=%d main_end=%d funcs=%d structs=%d enums=%d globals=%d\n",
                   ok, g_combined, g_main_end, nf, ns, ne, ng);
            return ok ? 0 : 1;
        }
        if (want_all) {
            /* 与 `shadow <entry> --fmeta` 输出同构，供逐字节 diff 对照 */
            printf("%s\n---SMETA---\n%s\n---EMETA---\n%s\n---GMETA---\n%s\n",
                   buf_cstr(&g_fmeta), buf_cstr(&g_smeta),
                   buf_cstr(&g_emeta), buf_cstr(&g_gmeta));
        }
        else if (want_s) printf("%s\n", buf_cstr(&g_smeta));
        else if (want_e) printf("%s\n", buf_cstr(&g_emeta));
        else if (want_g) printf("%s\n", buf_cstr(&g_gmeta));
        else printf("%s\n", buf_cstr(&g_fmeta));
    }
    return ok ? 0 : 1;
}
#endif
