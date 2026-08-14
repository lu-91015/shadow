/* ============================================================
 * shadow-0.5 rt/ — 测试运行器并行子进程启动器（rt_test_par.c）
 * 仅供 build/run_tests.exe 使用：shadow_test_launch / shadow_test_wait_any /
 * shadow_test_job_tag / shadow_test_reap。
 *
 * 设计：把每个测试用例作为「独立的 shadow.exe 子进程」启动（CreateProcessA），
 * 把子进程的 stdout 与 stderr 合并重定向到调用方指定的 out_path 文件；父进程用
 * WaitForMultipleObjects 阻塞等待「任一」子进程结束，再按 tag（用例下标）回收。
 *
 * 与 rt_proc_spawn.c 同理：本文件只含 run_tests.exe 需要、runtime_for_selfhost
 * 没有的 4 个符号，且不依赖 __rt_shadow_malloc / GC（编译器/测试运行器都没链接
 * rt_gc.o），因此可安全链接进 build/run_tests.exe，不会引入线程/GC 符号冲突。
 *
 * 为什么不用 Shadow 原生 rt_spawn + rt_future：那套依赖 rt_gc_thread_attach /
 * rt_gc_thread_detach（仅定义在 rt/rt_gc.c -> rt_gc.o，未链入本运行器），用了会
 * 触发 undefined-symbol 链接失败。故改为自包含的 Win32 子进程方案。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <process.h>

#define TEST_PAR_MAX 64
typedef struct { HANDLE hProc; HANDLE hThread; int used; int tag; } TestJob;
static TestJob g_jobs[TEST_PAR_MAX];

/* 启动一个测试子进程。cmdline 形如 "shadow.exe <test> --run ..."。
 * stdout 与 stderr 合并写入 out_path（方便复用 run_tests 的启发式判定）。
 * tag 由调用方自由定义（本运行器用来作用例下标，回收时据此取回元数据）。
 * 成功返回 job slot (>=0)，失败（无法建槽/启动失败）返回 -1。 */
extern int shadow_test_launch(const char* cmdline, const char* out_path, int tag) {
    HANDLE hOut = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) return -1;
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hNul == INVALID_HANDLE_VALUE) { CloseHandle(hOut); return -1; }
    /* stdout 与 stderr 指向同一个可继承的 out_path 句柄，实现合并捕获。 */
    SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hNul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    /* 先确定空闲槽位，以便分配「每槽位独立缓存目录」。 */
    int slot = -1, i;
    for (i = 0; i < TEST_PAR_MAX; i++) {
        if (g_jobs[i].used == 0) { slot = i; break; }
    }
    if (slot < 0) { CloseHandle(hOut); CloseHandle(hNul); return -1; }

    /* 每个槽位独立缓存目录：build/lu_cache_s<slot>。
     * 多个 shadow.exe 子进程各自写自己的目录，不再并发写同一份 build/lu_cache，
     * 彻底消除 -j 全量回归时出现的 .lu 竞态损坏（编译错误 / "missing value" 的根因）。 */
    char dname[48];
    _snprintf(dname, sizeof(dname), "build\\lu_cache_s%d", slot);
    CreateDirectoryA(dname, NULL); /* 已存在则静默失败，无妨 */

    /* 构造子进程环境块：继承父进程环境 + 追加 SHADOW_LU_CACHE_DIR。
     * lpEnvironment 非空时 CreateProcessA 以该块作为【完整】环境（不自动合并父环境），
     * 故需先把父环境整体拷入，再追加我们的变量。
     * 用 CRT _environ 数组（每元素是 "NAME=VALUE"，以 NULL 结尾）可靠构造，
     * 避免手工拼接 GetEnvironmentStringsA 块导致的 off-by-one / 变量合并 bug
     * （曾导致子进程收不到 SHADOW_LU_CACHE_DIR，回退默认 build/lu_cache 竞态，
     *  造成 T3 全量 -j 8 残留 133 失败）。 */
    char myvar[128];
    _snprintf(myvar, sizeof(myvar), "SHADOW_LU_CACHE_DIR=build\\lu_cache_s%d", slot);
    size_t needed = 1; /* 结尾双 NUL（myvar 自带一个，再加一个收尾） */
    int ei;
    for (ei = 0; environ[ei] != NULL; ei++) needed += strlen(environ[ei]) + 1;
    needed += strlen(myvar) + 1; /* myvar 本身 + 其结尾 NUL */
    char* newenv = (char*)malloc(needed);
    if (newenv == NULL) { CloseHandle(hOut); CloseHandle(hNul); return -1; }
    char* ep = newenv;
    for (ei = 0; environ[ei] != NULL; ei++) {
        strcpy(ep, environ[ei]);
        ep += strlen(environ[ei]) + 1;
    }
    strcpy(ep, myvar);
    ep += strlen(myvar) + 1;
    *ep = '\0'; /* 双 NUL 收尾 */

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = hNul;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    char cl[32768];
    strncpy(cl, cmdline, sizeof(cl) - 1);
    cl[sizeof(cl) - 1] = '\0';
    if (!CreateProcessA(NULL, cl, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, newenv, NULL, &si, &pi)) {
        free(newenv);
        CloseHandle(hOut);
        CloseHandle(hNul);
        return -1;
    }
    free(newenv);
    /* 父进程关闭自己的句柄副本（子进程已继承）。 */
    CloseHandle(hOut);
    CloseHandle(hNul);
    g_jobs[slot].hProc = pi.hProcess;
    g_jobs[slot].hThread = pi.hThread;
    g_jobs[slot].used = 1;
    g_jobs[slot].tag = tag;
    return slot;
}

/* 阻塞直到至少一个已启动的子进程结束；返回其 slot (>=0)，无活跃进程返回 -1。 */
extern int shadow_test_wait_any(void) {
    HANDLE hs[TEST_PAR_MAX];
    int n = 0, i;
    for (i = 0; i < TEST_PAR_MAX; i++) {
        if (g_jobs[i].used == 1) { hs[n++] = g_jobs[i].hProc; }
    }
    if (n == 0) return -1;
    DWORD r = WaitForMultipleObjects((DWORD)n, hs, FALSE, INFINITE);
    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + (DWORD)n) {
        HANDLE h = hs[r - WAIT_OBJECT_0];
        int j;
        for (j = 0; j < TEST_PAR_MAX; j++) {
            if (g_jobs[j].used == 1 && g_jobs[j].hProc == h) return j;
        }
    }
    return -1;
}

/* 取某 slot 的 tag（用例下标）。无效 slot 返回 -1。 */
extern int shadow_test_job_tag(int job) {
    if (job < 0 || job >= TEST_PAR_MAX || g_jobs[job].used == 0) return -1;
    return g_jobs[job].tag;
}

/* 回收 slot 句柄并释放槽位。 */
extern int shadow_test_reap(int job) {
    if (job < 0 || job >= TEST_PAR_MAX || g_jobs[job].used == 0) return -1;
    CloseHandle(g_jobs[job].hProc);
    CloseHandle(g_jobs[job].hThread);
    g_jobs[job].used = 0;
    g_jobs[job].tag = -1;
    return 0;
}
