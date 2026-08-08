/* ============================================================
 * shadow-0.4 rt/ — 异常标志 / panic / exit（rt_err.c）
 * 与 0.3 语义一致：全局标志 + 消息缓冲，Shadow 层 try/catch
 * 通过 shadow_exception_occurred 检测。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static volatile int32_t g_exception_flag = 0;
static char g_exception_msg[512];

extern void shadow_throw_str(const char* msg) {
    g_exception_flag = 1;
    if (msg) {
        strncpy(g_exception_msg, msg, sizeof(g_exception_msg) - 1);
        g_exception_msg[sizeof(g_exception_msg) - 1] = '\0';
    } else {
        g_exception_msg[0] = '\0';
    }
}

extern int32_t shadow_exception_occurred(void) {
    return g_exception_flag;
}

extern const char* shadow_exception_value(void) {
    return g_exception_msg;
}

extern void shadow_exception_clear(void) {
    g_exception_flag = 0;
}

extern void shadow_panic_msg(const char* msg) {
    fprintf(stderr, "panic: %s\n", msg ? msg : "");
    fflush(stderr);
    ExitProcess(1);
}
