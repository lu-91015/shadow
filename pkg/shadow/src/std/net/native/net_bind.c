/* ============================================================
 * Shadow 0.5 标准库 std/net — [native] C 实现（跨平台）
 * 编译：被 shadow 链接器以 clang -std=c++20 编译，
 *       故导出符号必须用 extern "C" 防止 name mangling。
 * ABI：Shadow string <-> const char*（data 指针，不以 \0 结尾）；
 *       因此每个函数显式接收长度参数，严禁用 strlen 读取输入。
 *       返回 string 一律 malloc + \0 结尾；失败/错误返回空字符串
 *       （malloc(1) 存 '\0'），不返回 NULL，避免 Shadow 侧句柄为 null 崩溃。
 * 链接：Windows 端 ws2_32（见下方 #pragma comment + 编译器默认 -lws2_32）；
 *       Linux/POSIX 端使用 libc（<sys/socket.h>），无需额外库。
 * ============================================================ */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

static char* shadow_empty(void) {
    char* o = (char*)malloc(1);
    if (o) o[0] = '\0';
    return o;
}

#ifdef _WIN32
/* Winsock 一次性初始化（幂等）。返回 0 成功，-1 失败。 */
static int ws_ready(void) {
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        done = 1;
    }
    return 0;
}
#endif

/* shadow_net_socket() -> handle:long（-1 失败）
 * 创建 TCP 客户端套接字。 */
extern "C" int64_t shadow_net_socket(void) {
#ifdef _WIN32
    if (ws_ready() != 0) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    return (int64_t)(intptr_t)s;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    return (int64_t)fd;
#endif
}

/* shadow_net_connect(h, host, hostlen, port) -> int（0 成功 / -1 失败）
 * 通过 getaddrinfo 解析 host（显式 hostlen，禁用 strlen），连接 port。 */
extern "C" int32_t shadow_net_connect(int64_t h, const char* host, int hostlen, int32_t port) {
    if (h == -1 || !host || hostlen <= 0) return -1;
#ifdef _WIN32
    if (ws_ready() != 0) return -1;
    SOCKET s = (SOCKET)(intptr_t)h;
#else
    int s = (int)h;
#endif
    char* hp = (char*)malloc((size_t)hostlen + 1);
    if (!hp) return -1;
    memcpy(hp, host, (size_t)hostlen);
    hp[hostlen] = '\0';
    struct addrinfo hints, *res = NULL, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    sprintf(portstr, "%d", (int)port);
    int g = getaddrinfo(hp, portstr, &hints, &res);
    free(hp);
    if (g != 0 || !res) return -1;
    int r = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) { r = 0; break; }
    }
    freeaddrinfo(res);
    return r;
}

/* shadow_net_send(h, data, datalen) -> int（已发送字节数 / -1 失败）
 * 显式 datalen，绝不依赖 strlen。 */
extern "C" int32_t shadow_net_send(int64_t h, const char* data, int datalen) {
    if (h == -1 || !data || datalen <= 0) return -1;
#ifdef _WIN32
    SOCKET s = (SOCKET)(intptr_t)h;
    int sent = send(s, data, datalen, 0);
#else
    int s = (int)h;
    int sent = (int)send(s, data, (size_t)datalen, 0);
#endif
    return (int32_t)sent;
}

/* shadow_net_recv(h, maxlen) -> string
 * 单次 recv，最多 maxlen 字节，malloc+\0 返回；对端关闭/出错返回空串。 */
extern "C" char* shadow_net_recv(int64_t h, int32_t maxlen) {
    if (h == -1 || maxlen <= 0) return shadow_empty();
#ifdef _WIN32
    SOCKET s = (SOCKET)(intptr_t)h;
#else
    int s = (int)h;
#endif
    char* buf = (char*)malloc((size_t)maxlen + 1);
    if (!buf) return shadow_empty();
    int n = (int)recv(s, buf, (int)maxlen, 0);
    if (n <= 0) { free(buf); return shadow_empty(); }
    buf[n] = '\0';
    return buf;
}

/* shadow_net_recv_all(h) -> string
 * 持续 recv 直到对端关闭（适用于 HTTP Connection: close），
 * 内部动态扩容，最终 malloc+\0 返回；失败返回空串。 */
extern "C" char* shadow_net_recv_all(int64_t h) {
    if (h == -1) return shadow_empty();
#ifdef _WIN32
    SOCKET s = (SOCKET)(intptr_t)h;
#else
    int s = (int)h;
#endif
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return shadow_empty();
    while (1) {
        if (len + 4096 > cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return shadow_empty(); }
            buf = nb;
        }
        int n = (int)recv(s, buf + len, 4096, 0);
        if (n > 0) { len += (size_t)n; }
        else { break; }  // 0=关闭, <0=出错
    }
    buf[len] = '\0';
    return buf;
}

/* shadow_net_close(h) -> int（1 成功 / 0 失败） */
extern "C" int32_t shadow_net_close(int64_t h) {
    if (h == -1) return 0;
#ifdef _WIN32
    SOCKET s = (SOCKET)(intptr_t)h;
    return (closesocket(s) == 0) ? 1 : 0;
#else
    int s = (int)h;
    return (close(s) == 0) ? 1 : 0;
#endif
}

/* shadow_net_bind(port) -> handle:long（-1 失败）
 * 创建 TCP 服务端套接字，bind(0.0.0.0:port) + listen。 */
extern "C" int64_t shadow_net_bind(int32_t port) {
#ifdef _WIN32
    if (ws_ready() != 0) return -1;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    if (listen(s, 16) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    return (int64_t)(intptr_t)s;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(s);
        return -1;
    }
    if (listen(s, 16) != 0) {
        close(s);
        return -1;
    }
    return (int64_t)s;
#endif
}

/* shadow_net_accept(server_h) -> handle:long（客户端句柄 / -1 失败）
 * 从已完成连接队列取一个客户端，不阻塞于 connect 之前。 */
extern "C" int64_t shadow_net_accept(int64_t server_h) {
    if (server_h == -1) return -1;
#ifdef _WIN32
    SOCKET s = (SOCKET)(intptr_t)server_h;
    SOCKET c = accept(s, NULL, NULL);
    if (c == INVALID_SOCKET) return -1;
    return (int64_t)(intptr_t)c;
#else
    int s = (int)server_h;
    int c = accept(s, NULL, NULL);
    if (c < 0) return -1;
    return (int64_t)c;
#endif
}
