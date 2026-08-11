/* ============================================================
 * shadow-0.5 rt/ — 非阻塞子进程 spawn（rt_proc_spawn.c）
 * 仅给编译器本体（LSP 异步编译 worker）使用：shadow_proc_launch/poll/reap。
 *
 * 与 rt_proc.c 故意分离：rt_proc.c 里的 shadow_sys_exec / shadow_exit /
 * shadow_env_get 已由 bootstrap/runtime_for_selfhost.o 提供，若把整个
 * rt_proc.o 链接进编译器会 duplicate symbol；本文件只含 LSP 需要、
 * runtime_for_selfhost 没有的 3 个符号，且不依赖 __rt_shadow_malloc，
 * 因此可安全链接进编译器本体（stage1/stage2 宿主）。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_ASYNC_JOBS 16
typedef struct { HANDLE hProc; HANDLE hThread; int used; } AsyncJob;
static AsyncJob g_jobs[MAX_ASYNC_JOBS];

/* 启动异步子进程。cmdline 形如 "shadow.exe --diag-json <tmp> --refs"。
 * 成功返回 job id (>=0)，失败返回 -1。stdout→out_path，stderr/stdin→NUL。 */
extern int shadow_proc_launch(const char* cmdline, const char* out_path) {
    HANDLE hOut = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) return -1;
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hNul == INVALID_HANDLE_VALUE) { CloseHandle(hOut); return -1; }
    /* 仅这两个句柄可被子进程继承（bInheritHandle=TRUE 时，STARTF_USESTDHANDLES
     * 会让子进程把 stdout/stderr/stdin 接到下面三个句柄，而非继承父进程 LSP 的 stdio） */
    SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hNul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hNul;
    si.hStdInput = hNul;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    char cl[32768];
    strncpy(cl, cmdline, sizeof(cl) - 1);
    cl[sizeof(cl) - 1] = '\0';
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hOut);
        CloseHandle(hNul);
        return -1;
    }
    /* 父进程关闭自己的副本（子进程已继承） */
    CloseHandle(hOut);
    CloseHandle(hNul);
    int slot = -1;
    int i;
    for (i = 0; i < MAX_ASYNC_JOBS; i++) {
        if (g_jobs[i].used == 0) { slot = i; break; }
    }
    if (slot < 0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return -1;
    }
    g_jobs[slot].hProc = pi.hProcess;
    g_jobs[slot].hThread = pi.hThread;
    g_jobs[slot].used = 1;
    return slot;
}

/* 非阻塞探测：1=已完成，0=仍在运行，-1=无效 job */
extern int shadow_proc_poll(int job) {
    if (job < 0 || job >= MAX_ASYNC_JOBS || g_jobs[job].used == 0) return -1;
    DWORD r = WaitForSingleObject(g_jobs[job].hProc, 0);
    if (r == WAIT_OBJECT_0) return 1;
    if (r == WAIT_TIMEOUT) return 0;
    return -1;
}

/* 回收 job 句柄 */
extern void shadow_proc_reap(int job) {
    if (job < 0 || job >= MAX_ASYNC_JOBS || g_jobs[job].used == 0) return;
    CloseHandle(g_jobs[job].hProc);
    CloseHandle(g_jobs[job].hThread);
    g_jobs[job].used = 0;
}
