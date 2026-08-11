/* ============================================================
 * shadow-0.4 rt/ — 进程 / 退出 / 环境变量（rt_proc.c）
 * shadow_sys_exec 用 CreateProcessA + 匿名管道捕获 stdout，
 * 替代 C++ _popen（规避 cmd /c 引号剥除陷阱，修复 --run 输出为空）。
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

/* 执行命令并捕获 stdout（返回 malloc 字符串；失败返回空串） */
extern const char* shadow_sys_exec(const char* cmd) {
    HANDLE hRead = NULL, hWrite = NULL;
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char* out;
    size_t cap, used;
    DWORD nread;
    if (!cmd) return rt_dup("");
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return rt_dup("");
    /* 确保读端不被子进程继承 */
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    memset(&pi, 0, sizeof(pi));
    {
        char cmdline[32768];
        strncpy(cmdline, cmd, sizeof(cmdline) - 1);
        cmdline[sizeof(cmdline) - 1] = '\0';
        if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hRead);
            CloseHandle(hWrite);
            return rt_dup("");
        }
    }
    CloseHandle(hWrite);
    cap = 4096;
    used = 0;
    out = (char*)malloc(cap);
    if (!out) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hRead);
        return rt_dup("");
    }
    out[0] = '\0';
    for (;;) {
        char buf[4096];
        DWORD total = 0;
        if (!ReadFile(hRead, buf, sizeof(buf), &nread, NULL) || nread == 0) break;
        total = nread;
        if (used + total + 1 > cap) {
            cap = cap * 2 + 64;
            out = (char*)realloc(out, cap);
            if (!out) break;
        }
        memcpy(out + used, buf, total);
        used += total;
        out[used] = '\0';
        if (nread < sizeof(buf)) {
            /* 可能还有数据，继续读直到 EOF */
        }
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return out ? out : rt_dup("");
}

/* ============================================================
 * 注：非阻塞子进程 spawn（shadow_proc_launch/poll/reap）已迁到
 * rt/rt_proc_spawn.c，仅链接进编译器本体；本文件保留 shadow_sys_exec /
 * shadow_exit / shadow_env_get，与 bootstrap/runtime_for_selfhost.o
 * 协同（runtime_for_selfhost 提供其定义，本文件为 user 运行时提供）。
 * ============================================================ */

extern void shadow_exit(int code) {
    ExitProcess((UINT)code);
}

extern const char* shadow_env_get(const char* name) {
    DWORD n;
    char* buf;
    if (!name) return rt_dup("");
    n = GetEnvironmentVariableA(name, NULL, 0);
    if (n == 0) return rt_dup("");
    buf = (char*)__rt_shadow_malloc((int32_t)n);
    if (!buf) return rt_dup("");
    GetEnvironmentVariableA(name, buf, n);
    return buf;
}
