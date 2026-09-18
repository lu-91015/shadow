/* ============================================================
 * shadow-0.5 rt/ — 测试运行器并行子进程启动器（rt_test_par.c）
 * 仅供 build/run_tests.exe（Windows）与 build/linux/rtests（Linux）使用：
 * shadow_test_launch / shadow_test_wait_any / shadow_test_job_tag /
 * shadow_test_job_rc / shadow_test_reap。
 *
 * 设计：把每个测试用例作为「独立的 shadow 编译器子进程」启动，把子进程的
 * stdout 与 stderr 合并重定向到调用方指定的 out_path 文件；父进程阻塞等待
 * 「任一」子进程结束，再按 tag（用例下标）回收。
 *   - Windows：CreateProcessA + WaitForMultipleObjects；
 *   - Linux：fork/exec + waitpid（崩溃编码 0x80|signal，与 Windows 注释对齐）。
 *
 * 与 rt_proc_spawn.c 同理：本文件只含 run_tests 需要、runtime_for_selfhost
 * 没有的 4 个符号，且不依赖 __rt_shadow_malloc / GC（编译器/测试运行器都没链接
 * rt_gc.o），因此可安全链接进 build/run_tests.exe / build/linux/rtests，不会
 * 引入线程/GC 符号冲突。
 *
 * 为什么不用 Shadow 原生 rt_spawn + rt_future：那套依赖 rt_gc_thread_attach /
 * rt_gc_thread_detach（仅定义在 rt/rt_gc.c -> rt_gc.o，未链入本运行器），用了会
 * 触发 undefined-symbol 链接失败。故改为自包含的子进程方案。
 * ============================================================ */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <process.h>
#else
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#define TEST_PAR_MAX 64

#ifdef _WIN32
typedef struct { HANDLE hProc; HANDLE hThread; int used; int tag; DWORD rc; } TestJob;
#else
typedef struct { pid_t pid; int used; int tag; int rc; } TestJob;
#endif
static TestJob g_jobs[TEST_PAR_MAX];
static int g_init = 0;

static void job_array_init(void) {
    if (g_init) return;
    memset(g_jobs, 0, sizeof(g_jobs));
    for (int i = 0; i < TEST_PAR_MAX; i++) {
#ifdef _WIN32
        g_jobs[i].hProc = INVALID_HANDLE_VALUE;
        g_jobs[i].hThread = INVALID_HANDLE_VALUE;
#endif
        g_jobs[i].tag = -1;
        g_jobs[i].rc = (int)-1;
    }
    g_init = 1;
}

/* 启动一个测试子进程。cmdline 形如 "shadow.exe <test> --run ..."。
 * stdout 与 stderr 合并写入 out_path（方便复用 run_tests 的启发式判定）。
 * tag 由调用方自由定义（本运行器用来作用例下标，回收时据此取回元数据）。
 * 成功返回 job slot (>=0)，失败（无法建槽/启动失败）返回 -1。 */
extern int shadow_test_launch(const char* cmdline, const char* out_path, int tag) {
    job_array_init();
    /* 先确定空闲槽位，以便分配「每槽位独立缓存目录」。 */
    int slot = -1, i;
    for (i = 0; i < TEST_PAR_MAX; i++) {
        if (g_jobs[i].used == 0) { slot = i; break; }
    }
    if (slot < 0) return -1;

#ifdef _WIN32
    HANDLE hOut = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE) return -1;
    HANDLE hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hNul == INVALID_HANDLE_VALUE) { CloseHandle(hOut); return -1; }
    /* stdout 与 stderr 指向同一个可继承的 out_path 句柄，实现合并捕获。 */
    SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hNul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    /* 每个槽位独立缓存目录：build/lu_cache_s<slot>。
     * 多个子进程各自写自己的目录，不再并发写同一份 build/lu_cache，
     * 彻底消除 -j 全量回归时出现的 .lu 竞态损坏（编译错误 / "missing value" 的根因）。 */
    char dname[96];
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
#else
    /* 每个槽位独立缓存目录：build/lu_cache_s<slot>，避免多子进程并发写同一份 .lu。 */
    char dname[96];
    snprintf(dname, sizeof(dname), "build/lu_cache_s%d", slot);
    mkdir(dname, 0777); /* 已存在则静默失败，无妨 */

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* ---- 子进程：重定向并注入缓存目录 ---- */
        int fd_out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int fd_nul = open("/dev/null", O_RDONLY);
        if (fd_out >= 0) { dup2(fd_out, STDOUT_FILENO); dup2(fd_out, STDERR_FILENO); }
        if (fd_nul >= 0) { dup2(fd_nul, STDIN_FILENO); }
        char var[128];
        snprintf(var, sizeof(var), "SHADOW_LU_CACHE_DIR=%s", dname);
        putenv(var); /* fork 后只改子进程环境，不影响父进程 */
        execl("/bin/sh", "sh", "-c", cmdline, (char*)NULL);
        _exit(127); /* exec 失败 */
    }
    g_jobs[slot].pid = pid;
#endif
    g_jobs[slot].used = 1;
    g_jobs[slot].tag = tag;
    g_jobs[slot].rc = (int)-1; /* 未取到退出码前的哨兵值 */
    return slot;
}

/* 阻塞直到至少一个已启动的子进程结束；返回其 slot (>=0)，无活跃进程返回 -1。 */
extern int shadow_test_wait_any(void) {
    job_array_init();
#ifdef _WIN32
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
            if (g_jobs[j].used == 1 && g_jobs[j].hProc == h) {
                GetExitCodeProcess(g_jobs[j].hProc, &g_jobs[j].rc);
                return j;
            }
        }
    }
    return -1;
#else
    for (;;) {
        int status = 0;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1; /* ECHILD 等 */
        }
        int j;
        for (j = 0; j < TEST_PAR_MAX; j++) {
            if (g_jobs[j].used == 1 && g_jobs[j].pid == w) {
                if (WIFEXITED(status)) {
                    g_jobs[j].rc = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    g_jobs[j].rc = 0x80 | WTERMSIG(status); /* 崩溃：0x80|signal */
                } else {
                    g_jobs[j].rc = -1;
                }
                return j;
            }
        }
        /* 未登记的后代（shadow 内部 fork 的 llc/clang 等孙进程）：继续等待下一个 */
    }
#endif
}

/* 取某 slot 的 tag（用例下标）。无效 slot 返回 -1。 */
extern int shadow_test_job_tag(int job) {
    if (job < 0 || job >= TEST_PAR_MAX || g_jobs[job].used == 0) return -1;
    return g_jobs[job].tag;
}

/* 取某 slot 的子进程退出码。须在 shadow_test_wait_any 返回该 slot 之后调用
 * （wait_any 内部已填充 rc）。无效 slot / 尚未回收返回 -1。
 * 正常退出为 0；崩溃程序在 Windows 返回异常码（如 0xC0000005），在 Linux 返回
 * 0x80|signal（如 SIGSEGV=139）；调用方据此把崩溃程序判为 FAIL。 */
extern int shadow_test_job_rc(int job) {
    if (job < 0 || job >= TEST_PAR_MAX || g_jobs[job].used == 0) return -1;
    return g_jobs[job].rc;
}

/* 回收 slot 句柄并释放槽位。 */
extern int shadow_test_reap(int job) {
    if (job < 0 || job >= TEST_PAR_MAX || g_jobs[job].used == 0) return -1;
#ifdef _WIN32
    CloseHandle(g_jobs[job].hProc);
    CloseHandle(g_jobs[job].hThread);
    g_jobs[job].hProc = INVALID_HANDLE_VALUE;
    g_jobs[job].hThread = INVALID_HANDLE_VALUE;
#else
    g_jobs[job].pid = -1;
#endif
    g_jobs[job].used = 0;
    g_jobs[job].tag = -1;
    g_jobs[job].rc = (int)-1;
    return 0;
}