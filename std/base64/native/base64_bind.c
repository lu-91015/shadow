/* ============================================================
 * Shadow 0.5 标准库 std/base64 — [native] C 实现
 * 编译：被 shadow 链接器以 clang -std=c++20 编译，
 *       故导出符号必须用 extern "C" 防止 name mangling。
 * ABI：Shadow string <-> const char*（data 指针，不以 \0 结尾）；
 *       因此每个函数显式接收 inlen（由 Shadow 侧 shadow_string_len 提供），
 *       严禁用 strlen 读取输入。返回 string 用 malloc + \0 结尾，
 *       对齐 rt/rt_zip.c 的 shadow_content_hash 约定。
 * 失败/空输入一律返回空字符串（malloc(1) 存 '\0'），不返回 NULL，
 * 以免 Shadow 侧 string 句柄为 null 引发崩溃；调用方用 len()==0 判断失败。
 * ============================================================ */
#include <stdlib.h>
#include <string.h>

static const char B64_ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* 返回空字符串（1 字节 '\0'）。调用方据 len() 是否为 0 判定失败。 */
static char* shadow_empty(void) {
    char* o = (char*)malloc(1);
    if (o) o[0] = '\0';
    return o;
}

extern "C" char* shadow_base64_encode(const char* in, int inlen) {
    if (!in || inlen <= 0) return shadow_empty();
    int outlen = 4 * ((inlen + 2) / 3);
    char* out = (char*)malloc((size_t)outlen + 1);
    if (!out) return shadow_empty();
    int i = 0, o = 0;
    while (i + 3 <= inlen) {
        unsigned int v = ((unsigned char)in[i] << 16) |
                         ((unsigned char)in[i + 1] << 8) |
                         (unsigned char)in[i + 2];
        out[o++] = B64_ENC[(v >> 18) & 63];
        out[o++] = B64_ENC[(v >> 12) & 63];
        out[o++] = B64_ENC[(v >> 6) & 63];
        out[o++] = B64_ENC[v & 63];
        i += 3;
    }
    int rem = inlen - i;
    if (rem == 1) {
        unsigned int v = (unsigned char)in[i] << 16;
        out[o++] = B64_ENC[(v >> 18) & 63];
        out[o++] = B64_ENC[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned int v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out[o++] = B64_ENC[(v >> 18) & 63];
        out[o++] = B64_ENC[(v >> 12) & 63];
        out[o++] = B64_ENC[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[outlen] = '\0';
    return out;
}

extern "C" char* shadow_base64_decode(const char* in, int inlen) {
    if (!in || inlen <= 0) return shadow_empty();
    static int tbl[256];
    static int inited = 0;
    if (!inited) {
        int i;
        for (i = 0; i < 256; i++) tbl[i] = -1;
        for (i = 0; i < 26; i++) { tbl['A' + i] = i; tbl['a' + i] = 26 + i; }
        for (i = 0; i < 10; i++) tbl['0' + i] = 52 + i;
        tbl['+'] = 62; tbl['/'] = 63;
        inited = 1;
    }
    int pad = 0;
    while (inlen > 0 && in[inlen - 1] == '=') { pad++; inlen--; }
    if (inlen == 0) return shadow_empty();
    /* 解码缓冲区尺寸：显著字符数 inlen 可非 4 倍数（末尾余数组贡献额外字节），
       故用 (inlen*3)/4 而非 (inlen/4)*3，否则会少算余数组的 1~2 字节导致堆越界。 */
    int outcap = (inlen * 3) / 4;
    char* out = (char*)malloc((size_t)outcap + 1);
    if (!out) return shadow_empty();
    int o = 0, i = 0;
    while (i + 4 <= inlen) {
        int c0 = tbl[(unsigned char)in[i]];
        int c1 = tbl[(unsigned char)in[i + 1]];
        int c2 = tbl[(unsigned char)in[i + 2]];
        int c3 = tbl[(unsigned char)in[i + 3]];
        if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) { free(out); return shadow_empty(); }
        unsigned int v = ((unsigned int)c0 << 18) | ((unsigned int)c1 << 12) |
                         ((unsigned int)c2 << 6) | (unsigned int)c3;
        out[o++] = (char)((v >> 16) & 0xff);
        out[o++] = (char)((v >> 8) & 0xff);
        out[o++] = (char)(v & 0xff);
        i += 4;
    }
    if (inlen % 4 == 2 && i + 2 <= inlen) {
        int c0 = tbl[(unsigned char)in[i]];
        int c1 = tbl[(unsigned char)in[i + 1]];
        if (c0 < 0 || c1 < 0) { free(out); return shadow_empty(); }
        unsigned int v = ((unsigned int)c0 << 18) | ((unsigned int)c1 << 12);
        out[o++] = (char)((v >> 16) & 0xff);
    } else if (inlen % 4 == 3 && i + 3 <= inlen) {
        int c0 = tbl[(unsigned char)in[i]];
        int c1 = tbl[(unsigned char)in[i + 1]];
        int c2 = tbl[(unsigned char)in[i + 2]];
        if (c0 < 0 || c1 < 0 || c2 < 0) { free(out); return shadow_empty(); }
        unsigned int v = ((unsigned int)c0 << 18) | ((unsigned int)c1 << 12) | ((unsigned int)c2 << 6);
        out[o++] = (char)((v >> 16) & 0xff);
        out[o++] = (char)((v >> 8) & 0xff);
    }
    out[o] = '\0';
    return out;
}

extern "C" char* shadow_hex_encode(const char* in, int inlen) {
    if (!in || inlen <= 0) return shadow_empty();
    char* out = (char*)malloc((size_t)(2 * inlen) + 1);
    if (!out) return shadow_empty();
    static const char HX[] = "0123456789abcdef";
    int i, o = 0;
    for (i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        out[o++] = HX[c >> 4];
        out[o++] = HX[c & 0xf];
    }
    out[o] = '\0';
    return out;
}

extern "C" char* shadow_hex_decode(const char* in, int inlen) {
    if (!in || inlen <= 0 || inlen % 2 != 0) return shadow_empty();
    char* out = (char*)malloc((size_t)(inlen / 2) + 1);
    if (!out) return shadow_empty();
    int i, o = 0;
    for (i = 0; i + 1 < inlen; i += 2) {
        int hi = 0, lo = 0;
        char c1 = in[i], c2 = in[i + 1];
        if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
        else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
        else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
        else { free(out); return shadow_empty(); }
        if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
        else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
        else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
        else { free(out); return shadow_empty(); }
        out[o++] = (char)((hi << 4) | lo);
    }
    out[o] = '\0';
    return out;
}
