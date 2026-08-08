/* ============================================================
 * Shadow 0.4 — rt_zip.c：SPK 打包/解包/列表（§3.8.4）
 * ------------------------------------------------------------
 * 基于 miniz（bootstrap/miniz.o，ZIP 容器）封装三个编译器 CLI 函数：
 *   shadow_zip_pack(src_dir, out_spk)  目录 → ZIP（STORE 模式）
 *   shadow_zip_unpack(spk, out_dir)    ZIP → 目录（幂等，覆盖）
 *   shadow_zip_list(spk)               列出条目（分号分隔，可解析）
 * 同时提供 content_hash 计算（FNV-1a 64，供 SPK 校验用）：
 *   shadow_content_hash(path)          文件内容哈希（hex 字符串）
 *
 * 设计约束（§6.2）：纯 C + 系统 API + miniz；不依赖 C++ runtime。
 * ZIP 内路径统一正斜杠 '/'；磁盘路径用 '\\'。
 * ============================================================ */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "miniz.h"

/* ---------------- 字符串构建（分号分隔列表） ---------------- */
typedef struct strbuf {
    char* p;
    size_t len;
    size_t cap;
} strbuf;

static void sb_init(strbuf* b) {
    b->cap = 256;
    b->len = 0;
    b->p = (char*)malloc(b->cap);
    if (b->p) b->p[0] = '\0';
}

static void sb_free(strbuf* b) {
    if (b->p) { free(b->p); b->p = NULL; }
}

static void sb_put(strbuf* b, const char* s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->p = (char*)realloc(b->p, b->cap);
        if (!b->p) return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_put_ch(strbuf* b, char c) {
    char s[2];
    s[0] = c; s[1] = '\0';
    sb_put(b, s);
}

/* ---------------- 目录递归收集（分号分隔相对路径） ---------------- */
/* 把 dir 下所有文件的相对路径（反斜杠）追加到 list；dir 以 \ 结尾 */
static void zip_collect_dir(const char* dir, const char* base, strbuf* list) {
    char search[2048];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(search, sizeof(search), "%s*", dir);
    h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        {
            char full[2048];
            snprintf(full, sizeof(full), "%s%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* 跳过 .spk_meta 打包元数据目录 */
                if (strcmp(fd.cFileName, ".spk_meta") == 0) continue;
                /* 跳过构建产物与版本库。build/ 下有 lu_cache 增量缓存（可达数 MB），
                   其键是「发布方本机源文件路径」的 hash，且消费方只读自己 CWD 下的
                   build\lu_cache —— 打进包里既无用又臃肿。 */
                if (strcmp(fd.cFileName, "build") == 0) continue;
                if (strcmp(fd.cFileName, ".git") == 0) continue;
                {
                    char sub[2048];
                    snprintf(sub, sizeof(sub), "%s%s\\", dir, fd.cFileName);
                    zip_collect_dir(sub, base, list);
                }
            } else {
                /* 相对路径：base 后的部分 */
                size_t bl = strlen(base);
                const char* rel = full + bl;
                if (list->len > 0) sb_put_ch(list, ';');
                sb_put(list, rel);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* 目录是否存在且为目录 */
static int zip_is_dir(const char* p) {
    DWORD a = GetFileAttributesA(p);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return (a & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* 确保目录存在（递归创建） */
static void zip_mkdirs(const char* path) {
    char tmp[2048];
    size_t i, n = strlen(path);
    if (n >= sizeof(tmp)) return;
    memcpy(tmp, path, n + 1);
    for (i = 0; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            CreateDirectoryA(tmp, NULL);
            tmp[i] = '\\';
        }
    }
    CreateDirectoryA(path, NULL);
}

/* ---------------- pack：目录 → ZIP（STORE） ---------------- */
/* 返回 0 成功，-1 失败 */
extern int32_t shadow_zip_pack(const char* src_dir, const char* out_spk) {
    mz_zip_archive zip;
    strbuf files;
    char base[2048];
    size_t bl;
    size_t i, n;
    int rc = -1;

    if (!src_dir || !out_spk) return -1;
    if (!zip_is_dir(src_dir)) return -1;
    bl = strlen(src_dir);
    if (bl == 0 || bl >= sizeof(base)) return -1;
    memcpy(base, src_dir, bl + 1);
    /* base 统一以 \ 结尾（相对路径基准） */
    if (base[bl - 1] != '\\' && base[bl - 1] != '/') {
        base[bl] = '\\';
        base[bl + 1] = '\0';
    }
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_spk, 0)) return -1;

    sb_init(&files);
    zip_collect_dir(base, base, &files);
    n = files.len ? 1 : 0;
    /* 拆分为单个相对路径逐个 add（STORE：level=0） */
    {
        char* cur = files.p;
        char* p = files.p;
        while (p && *p) {
            char* semi = strchr(p, ';');
            char save = 0;
            char full[2048];
            char zname[2048];
            size_t k;
            if (semi) { save = *semi; *semi = '\0'; }
            snprintf(full, sizeof(full), "%s%s", base, p);
            /* zip 内路径统一正斜杠 */
            {
                size_t zl = strlen(p);
                for (k = 0; k < zl && k + 1 < sizeof(zname); k++) {
                    zname[k] = (p[k] == '\\') ? '/' : p[k];
                }
                zname[zl] = '\0';
            }
            if (!mz_zip_writer_add_file(&zip, zname, full, NULL, 0, 0)) {
                if (semi) *semi = save;
                goto done;
            }
            if (semi) *semi = save;
            p = semi ? semi + 1 : NULL;
            (void)cur;
        }
    }
    if (!mz_zip_writer_finalize_archive(&zip)) goto done;
    rc = 0;
done:
    mz_zip_writer_end(&zip);
    sb_free(&files);
    return rc;
}

/* ---------------- unpack：ZIP → 目录 ---------------- */
/* 返回 0 成功，-1 失败 */
extern int32_t shadow_zip_unpack(const char* spk, const char* out_dir) {
    mz_zip_archive zip;
    mz_uint n, i;
    int rc = -1;
    if (!spk || !out_dir) return -1;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, spk, 0)) return -1;
    n = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < n; i++) {
        char name[2048];
        char outpath[4096];
        size_t nl;
        size_t k;
        if (!mz_zip_reader_get_filename(&zip, i, name, sizeof(name))) continue;
        nl = strlen(name);
        /* 防目录穿越：拒绝 .. 段 */
        {
            size_t j;
            for (j = 0; j + 1 < nl; j++) {
                if (name[j] == '.' && name[j + 1] == '.') goto skip;
            }
        }
        /* 目录条目（以 / 结尾）→ mkdir */
        if (nl > 0 && (name[nl - 1] == '/' || name[nl - 1] == '\\')) {
            snprintf(outpath, sizeof(outpath), "%s\\%s", out_dir, name);
            for (k = 0; outpath[k]; k++) if (outpath[k] == '/') outpath[k] = '\\';
            zip_mkdirs(outpath);
            continue;
        }
        snprintf(outpath, sizeof(outpath), "%s\\%s", out_dir, name);
        for (k = 0; outpath[k]; k++) if (outpath[k] == '/') outpath[k] = '\\';
        /* 只创建父目录（zip_mkdirs 会把末段当目录建，文件名不能传给它） */
        {
            char parent[4096];
            size_t pl = strlen(outpath);
            size_t cut = pl;
            while (cut > 0 && outpath[cut - 1] != '\\') cut--;
            if (cut > 0 && cut < pl) {
                memcpy(parent, outpath, cut - 1);
                parent[cut - 1] = '\0';
                zip_mkdirs(parent);
            } else {
                zip_mkdirs(out_dir);
            }
        }
        if (!mz_zip_reader_extract_to_file(&zip, i, outpath, 0)) goto done;
skip:
        (void)0;
    }
    rc = 0;
done:
    mz_zip_reader_end(&zip);
    return rc;
}

/* ---------------- list：列出条目（分号分隔） ---------------- */
/* 返回 malloc 的字符串（调用方 rt_free），失败返回 NULL */
extern const char* shadow_zip_list(const char* spk) {
    mz_zip_archive zip;
    mz_uint n, i;
    strbuf out;
    if (!spk) return NULL;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, spk, 0)) return NULL;
    n = mz_zip_reader_get_num_files(&zip);
    sb_init(&out);
    for (i = 0; i < n; i++) {
        char name[2048];
        if (!mz_zip_reader_get_filename(&zip, i, name, sizeof(name))) continue;
        if (out.len > 0) sb_put_ch(&out, ';');
        sb_put(&out, name);
    }
    mz_zip_reader_end(&zip);
    return out.p;
}

/* ---------------- content_hash：文件内容 FNV-1a 64 ---------------- */
/* 返回 malloc 的 hex 字符串（16 字符），失败返回 NULL */
extern const char* shadow_content_hash(const char* path) {
    HANDLE h;
    unsigned char buf[65536];
    DWORD rd;
    uint64_t hsh = 0xcbf29ce484222325ULL;
    char* hex;
    int i;
    if (!path) return NULL;
    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &rd, NULL) || rd == 0) break;
        {
            DWORD j;
            for (j = 0; j < rd; j++) {
                hsh ^= buf[j];
                hsh *= 0x100000001b3ULL;
            }
        }
    }
    CloseHandle(h);
    hex = (char*)malloc(17);
    if (!hex) return NULL;
    for (i = 0; i < 8; i++) {
        sprintf(hex + i * 2, "%02llx", (unsigned long long)(hsh >> (56 - i * 8)) & 0xff);
    }
    hex[16] = '\0';
    return hex;
}
