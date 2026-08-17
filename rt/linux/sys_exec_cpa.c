/* ============================================================
 * shadow-0.5 bootstrap — shadow_sys_exec 的 POSIX 实现 (Linux)
 *
 * 背景：bootstrap/runtime_for_selfhost.o 里的 shadow_sys_exec 用
 * _popen(cmd, "r")（即 cmd.exe /c）启动子进程。cmd /c 对"以引号开头且
 * 程序路径含空格"的命令会剥错引号，导致仓库路径含空格（如 "TRAE SOLO CN"）
 * 时所有子进程启动失败。
 *
 * 此文件提供一份 POSIX (fork + execvp + pipe) 版的 shadow_sys_exec。
 * 直接按空格/引号拆分命令行为 argv 后 execvp，正确处理含空格的程序/参数
 * 路径，且不经过 /bin/sh，彻底规避 shell 引号解释问题。
 *
 * 仅覆盖 shadow_sys_exec 一个符号；shadow_exit / shadow_env_get 等仍由
 * runtime_for_selfhost.o 提供。
 * ============================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

/* Shadow 运行时分配器（由 runtime_for_selfhost.o / miniz.o 提供） */
extern void* __rt_shadow_malloc(int32_t n);

static char* rt_dup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)__rt_shadow_malloc((int32_t)(n + 1));
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* 拆分命令行为 argv（NULL 结尾）。支持双引号/单引号包裹含空格参数。
 * 返回参数个数；*argv_out 由调用者 free。失败时返回 <=0。 */
static int split_args(const char* cmd, char*** argv_out) {
    char** argv = NULL;
    int count = 0, cap = 0;
    const char* p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int quote = 0;
        char buf[32768];
        int blen = 0;
        if (*p == '"' || *p == '\'') { quote = *p; p++; }
        while (*p) {
            if (quote) {
                if (*p == quote) { p++; break; }
                if (blen < (int)sizeof(buf) - 1) buf[blen++] = *p;
                p++;
            } else {
                if (*p == ' ' || *p == '\t') break;
                if (*p == '"' || *p == '\'') { quote = *p; p++; continue; }
                if (blen < (int)sizeof(buf) - 1) buf[blen++] = *p;
                p++;
            }
        }
        buf[blen] = '\0';
        if (count >= cap) {
            cap = cap * 2 + 4;
            argv = (char**)realloc(argv, (size_t)cap * sizeof(char*));
            if (!argv) return -1;
        }
        argv[count++] = strdup(buf);
    }
    argv = (char**)realloc(argv, (size_t)(count + 1) * sizeof(char*));
    if (argv) argv[count] = NULL;
    *argv_out = argv;
    return count;
}

static void free_argv(char** argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

/* 执行命令并捕获 stdout+stderr（返回 malloc 字符串；失败返回空串）。
 * 用 fork + execvp + 匿名管道，规避 shell 引号解释，正确处理
 * 含空格的程序/参数路径。stdout 与 stderr 合流到同一返回缓冲。 */
extern const char* shadow_sys_exec(const char* cmd) {
    if (!cmd) return rt_dup("");
    char** argv;
    int argc = split_args(cmd, &argv);
    if (argc <= 0) return rt_dup("");

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free_argv(argv);
        return rt_dup("");
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free_argv(argv);
        return rt_dup("");
    }
    if (pid == 0) {
        /* child */
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        /* 阻断 LD_LIBRARY_PATH / LD_PRELOAD 泄漏给派生的主机工具（llc / clang++ /
         * curl 等）。shadow 自身靠 launcher 的 --library-path 加载私有 glibc 2.39，
         * 但被它派生的主机工具若继承这套路径，会误加载包内那份 GLIBC_2.38 的
         * libLLVM / libstdc++，在 glibc 2.34 主机上直接崩溃（正是 --run 只产出
         * .ll 的根因）。主机工具通过自身 rpath / 系统默认路径查找依赖，无需这些变量。 */
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("LD_PRELOAD");
        execvp(argv[0], argv);
        /* 若到达这里说明 exec 失败 */
        _exit(127);
    }
    /* parent */
    close(pipefd[1]);
    free_argv(argv);

    size_t cap = 4096, used = 0;
    char* out = (char*)malloc(cap);
    if (!out) {
        close(pipefd[0]);
        return rt_dup("");
    }
    out[0] = '\0';
    for (;;) {
        char buf[4096];
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (used + (size_t)n + 1 > cap) {
            cap = cap * 2 + 64;
            char* nw = (char*)realloc(out, cap);
            if (!nw) break;
            out = nw;
        }
        memcpy(out + used, buf, (size_t)n);
        used += (size_t)n;
        out[used] = '\0';
    }
    close(pipefd[0]);
    int status;
    waitpid(pid, &status, 0);
    return out ? out : rt_dup("");
}
