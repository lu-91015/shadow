/* __cxa_atexit / __dso_handle shim for Windows MSVC link.
 * runtime_for_selfhost.cpp registers a scheduler-exit hook via __cxa_atexit
 * (a glibc-only symbol). MSVC CRT provides neither symbol, so provide a
 * minimal atexit-backed implementation here. */
#include <stdlib.h>

typedef void (*cxa_dtor_fn)(void*);

typedef struct { cxa_dtor_fn fn; void* arg; } cxa_entry;
static cxa_entry g_entries[512];
static int g_n = 0;

static void cxa_run_all(void) {
    int i;
    for (i = g_n - 1; i >= 0; i--) g_entries[i].fn(g_entries[i].arg);
}

int __cxa_atexit(cxa_dtor_fn fn, void* arg, void* dso) {
    (void)dso;
    if (g_n < 512) {
        g_entries[g_n].fn = fn;
        g_entries[g_n].arg = arg;
        g_n++;
        if (g_n == 1) atexit(cxa_run_all);
    }
    return 0;
}

void* __dso_handle = (void*)&__dso_handle;
