/* ============================================================
 * Shadow 0.5 标准库 std/https — [native] C 实现（WinHttp，动态加载）
 * 提供 HTTPS（TLS）客户端能力。TLS 由 Windows WinHttp 栈透明提供，
 * 无需自带 openssl/wolfssl，也无需链接 winhttp.lib 导入库——
 * 这里用 LoadLibrary/GetProcAddress 动态加载 winhttp.dll，规避
 * LLVM 工具链缺导入库的问题。
 * ABI：Shadow string 为裸 data 指针（不以 \0 结尾），故 URL/body 显式带长度；
 *       返回 malloc+\0 字符串，失败/错误返回空串（不返回 NULL）。
 * 说明：这是「TLS 客户端」能力（OS 托管握手/加解密），并非「TLS 密码学原语库」。
 * ============================================================ */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <windows.h>
#include <winhttp.h>

/* WinHttp 函数指针类型（自命名，避免依赖 winhttp.h 是否导出同名 typedef；
   必须在使用前声明，故放在 static 变量之前） */
typedef HINTERNET (WINAPI *SHADOW_WH_OPEN_FN)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *SHADOW_WH_CONNECT_FN)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *SHADOW_WH_OPENREQ_FN)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
typedef BOOL (WINAPI *SHADOW_WH_SENDREQ_FN)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *SHADOW_WH_WRITEDATA_FN)(HINTERNET, LPCVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *SHADOW_WH_RECVERESP_FN)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *SHADOW_WH_QUERYAVAIL_FN)(HINTERNET, LPDWORD);
typedef BOOL (WINAPI *SHADOW_WH_READDATA_FN)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *SHADOW_WH_CLOSEH_FN)(HINTERNET);

/* 动态加载 winhttp.dll，缓存函数指针；返回 0 成功，-1 失败。 */
static HMODULE g_wh = NULL;
static SHADOW_WH_OPEN_FN        pOpen = NULL;
static SHADOW_WH_CONNECT_FN     pConnect = NULL;
static SHADOW_WH_OPENREQ_FN     pOpenReq = NULL;
static SHADOW_WH_SENDREQ_FN     pSendReq = NULL;
static SHADOW_WH_WRITEDATA_FN   pWrite = NULL;
static SHADOW_WH_RECVERESP_FN   pRecvResp = NULL;
static SHADOW_WH_QUERYAVAIL_FN  pQueryAvail = NULL;
static SHADOW_WH_READDATA_FN    pRead = NULL;
static SHADOW_WH_CLOSEH_FN      pClose = NULL;
typedef HINTERNET (WINAPI *WINHTTP_CONNECT_FN)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *WINHTTP_OPENREQ_FN)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD, DWORD);
typedef BOOL (WINAPI *WINHTTP_SENDREQ_FN)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *WINHTTP_WRITEDATA_FN)(HINTERNET, LPCVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *WINHTTP_RECVERESP_FN)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *WINHTTP_QUERYAVAIL_FN)(HINTERNET, LPDWORD);
typedef BOOL (WINAPI *WINHTTP_READDATA_FN)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *WINHTTP_CLOSEH_FN)(HINTERNET);

static int wh_ensure(void) {
    if (g_wh) return 0;
    g_wh = LoadLibraryA("winhttp.dll");
    if (!g_wh) return -1;
    pOpen      = (SHADOW_WH_OPEN_FN)GetProcAddress(g_wh, "WinHttpOpen");
    pConnect   = (SHADOW_WH_CONNECT_FN)GetProcAddress(g_wh, "WinHttpConnect");
    pOpenReq   = (SHADOW_WH_OPENREQ_FN)GetProcAddress(g_wh, "WinHttpOpenRequest");
    pSendReq   = (SHADOW_WH_SENDREQ_FN)GetProcAddress(g_wh, "WinHttpSendRequest");
    pWrite     = (SHADOW_WH_WRITEDATA_FN)GetProcAddress(g_wh, "WinHttpWriteData");
    pRecvResp  = (SHADOW_WH_RECVERESP_FN)GetProcAddress(g_wh, "WinHttpReceiveResponse");
    pQueryAvail= (SHADOW_WH_QUERYAVAIL_FN)GetProcAddress(g_wh, "WinHttpQueryDataAvailable");
    pRead      = (SHADOW_WH_READDATA_FN)GetProcAddress(g_wh, "WinHttpReadData");
    pClose     = (SHADOW_WH_CLOSEH_FN)GetProcAddress(g_wh, "WinHttpCloseHandle");
    if (!pOpen || !pConnect || !pOpenReq || !pSendReq || !pRecvResp ||
        !pQueryAvail || !pRead || !pClose) { return -1; }
    return 0;
}

static char* shadow_empty(void) {
    char* o = (char*)malloc(1);
    if (o) o[0] = '\0';
    return o;
}

/* 把 UTF-8 的 URL 解析为 host（宽串）、port、path（宽串）。
 * 入参 url 不以 \0 结尾，需显式 urllen。 */
static int wh_parse(const char* url, int urllen,
                    wchar_t* hostW, int hostWcap, int* port,
                    wchar_t* pathW, int pathWcap) {
    if (!url || urllen <= 0) return -1;
    char* u = (char*)malloc((size_t)urllen + 1);
    if (!u) return -1;
    memcpy(u, url, (size_t)urllen);
    u[urllen] = '\0';
    // 去掉 scheme
    char* p = strstr(u, "://");
    char* body = (p != NULL) ? p + 3 : u;
    // host 与 path 切分
    char* slash = strchr(body, '/');
    char hostbuf[512];
    char pathbuf[1024];
    if (slash) {
        int hl = (int)(slash - body);
        if (hl >= (int)sizeof(hostbuf)) hl = (int)sizeof(hostbuf) - 1;
        memcpy(hostbuf, body, (size_t)hl);
        hostbuf[hl] = '\0';
        strncpy(pathbuf, slash, sizeof(pathbuf) - 1);
        pathbuf[sizeof(pathbuf) - 1] = '\0';
    } else {
        strncpy(hostbuf, body, sizeof(hostbuf) - 1);
        hostbuf[sizeof(hostbuf) - 1] = '\0';
        strncpy(pathbuf, "/", sizeof(pathbuf) - 1);
        pathbuf[sizeof(pathbuf) - 1] = '\0';
    }
    // port（:port 在 host 中）
    *port = 443;
    char* colon = strchr(hostbuf, ':');
    if (colon) {
        *colon = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) *port = 443;
    }
    MultiByteToWideChar(CP_UTF8, 0, hostbuf, -1, hostW, hostWcap);
    MultiByteToWideChar(CP_UTF8, 0, pathbuf, -1, pathW, pathWcap);
    free(u);
    return 0;
}

/* 通用 HTTPS 请求：verb="GET"/"POST"，body 可为 NULL（GET）。
 * 返回完整响应文本（malloc+\0），失败返回空串。 */
static char* wh_request(const char* url, int urllen, const char* verb,
                        const char* body, int bodylen) {
    if (wh_ensure() != 0) return shadow_empty();
    wchar_t hostW[512];
    wchar_t pathW[1024];
    int port = 443;
    if (wh_parse(url, urllen, hostW, 512, &port, pathW, 1024) != 0) return shadow_empty();

    HINTERNET hS = pOpen(L"Shadow/0.5", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!hS) return shadow_empty();
    HINTERNET hC = pConnect(hS, hostW, (INTERNET_PORT)port, 0);
    if (!hC) { pClose(hS); return shadow_empty(); }
    HINTERNET hR = pOpenReq(hC, (LPCWSTR)verb, pathW, NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hR) { pClose(hC); pClose(hS); return shadow_empty(); }

    BOOL ok;
    if (body && bodylen > 0) {
        char hdr[64];
        sprintf(hdr, "Content-Length: %d\r\n", bodylen);
        ok = pSendReq(hR, (LPCWSTR)hdr, (DWORD)strlen(hdr), (LPVOID)body, (DWORD)bodylen, (DWORD)bodylen, 0);
    } else {
        ok = pSendReq(hR, NULL, 0, NULL, 0, 0, 0);
    }
    if (!ok) { pClose(hR); pClose(hC); pClose(hS); return shadow_empty(); }

    if (!pRecvResp(hR, NULL)) { pClose(hR); pClose(hC); pClose(hS); return shadow_empty(); }

    // 读响应体
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { pClose(hR); pClose(hC); pClose(hS); return shadow_empty(); }
    DWORD avail = 0;
    while (pQueryAvail(hR, &avail) && avail > 0) {
        if (len + (size_t)avail + 1 > cap) {
            cap = len + (size_t)avail + 4096;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); pClose(hR); pClose(hC); pClose(hS); return shadow_empty(); }
            buf = nb;
        }
        DWORD got = 0;
        if (!pRead(hR, buf + len, avail, &got) || got == 0) break;
        len += (size_t)got;
    }
    buf[len] = '\0';
    pClose(hR); pClose(hC); pClose(hS);
    return buf;
}

extern "C" char* shadow_https_get(const char* url, int urllen) {
    return wh_request(url, urllen, "GET", NULL, 0);
}

extern "C" char* shadow_https_post(const char* url, int urllen, const char* body, int bodylen) {
    return wh_request(url, urllen, "POST", body, bodylen);
}
