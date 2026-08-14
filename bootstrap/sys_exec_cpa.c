/* ============================================================
 * shadow-0.5 bootstrap — shadow_sys_exec 的 CreateProcessA 实现
 *
 * 背景：bootstrap/runtime_for_selfhost.o 里的 shadow_sys_exec 用
 * _popen(cmd, "r")（即 cmd.exe /c）启动子进程。cmd /c 对"以引号开头且
 * 程序路径含空格"的命令会剥错引号，导致仓库路径含空格（如 "TRAE SOLO CN"）
 * 时所有子进程启动失败（'C:/.../TRAE' is not recognized）。
 *
 * 此文件提供一份 CreateProcessA 版的 shadow_sys_exec（逻辑移植自
 * rt/rt_proc.c，那一份本就是 CreateProcessA），编译为独立 .o 后，通过
 * llvm-objcopy 把 blob 内的 shadow_sys_exec 降为弱符号，让本文件的强符号
 * 覆盖它。CreateProcessA 直接解析带引号的含空格路径，彻底解决该问题。
 *
 * 仅覆盖 shadow_sys_exec 一个符号；shadow_exit / shadow_env_get 等仍由
 * runtime_for_selfhost.o 提供。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Shadow 运行时分配器（由 runtime_for_selfhost.o / miniz.o 提供） */
extern void* __rt_shadow_malloc(int32_t n);

static char* rt_dup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)__rt_shadow_malloc((int32_t)(n + 1));
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* 执行命令并捕获 stdout+stderr（返回 malloc 字符串；失败返回空串）。
 * 用 CreateProcessA + 匿名管道，规避 cmd /c 的引号剥除陷阱，正确处理
 * 含空格的程序/参数路径。 */
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
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRead);
    return out ? out : rt_dup("");
}
