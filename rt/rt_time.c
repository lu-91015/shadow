/* ============================================================
 * shadow-0.4 rt/ — 时间原语（rt_time.c）
 * date=YYYYMMDD、timestamp=YYYYMMDDHHMMSSmmm、nanotimestamp=纳秒。
 * 格式对齐 0.3 C++ runtime。
 * ============================================================ */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

extern void* __rt_shadow_malloc(int32_t n);
static char* rt_dup(const char* s) {
    size_t n = strlen(s);
    char* p = (char*)__rt_shadow_malloc((int32_t)(n + 1));
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

extern int32_t rt_date_now(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_s(&lt, &t);
    return (int32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
}

extern int64_t rt_timestamp_now(void) {
    time_t t = time(NULL);
    struct tm lt;
    SYSTEMTIME st;
    localtime_s(&lt, &t);
    GetLocalTime(&st);
    /* YYYYMMDDHHMMSSmmm（17 位） */
    return (int64_t)(lt.tm_year + 1900) * 10000000000000LL
         + (int64_t)(lt.tm_mon + 1) * 100000000000LL
         + (int64_t)lt.tm_mday * 1000000000LL
         + (int64_t)lt.tm_hour * 10000000LL
         + (int64_t)lt.tm_min * 100000LL
         + (int64_t)lt.tm_sec * 1000LL
         + (int64_t)st.wMilliseconds;
}

extern int64_t rt_nanotimestamp_now(void) {
    LARGE_INTEGER freq, count;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count)) {
        return (int64_t)(count.QuadPart * 1000000000LL / freq.QuadPart);
    }
    return (int64_t)time(NULL) * 1000000000LL;
}

extern const char* rt_date_to_str(int32_t d) {
    char buf[16];
    int32_t year = d / 10000;
    int32_t md = d % 10000;
    int32_t month = md / 100;
    int32_t day = md % 100;
    sprintf(buf, "%04d.%02d.%02d", year, month, day);
    return rt_dup(buf);
}

extern const char* rt_timestamp_to_str(int64_t t) {
    char buf[32];
    int64_t ms_part = t % 1000;
    int64_t sec_part = (t / 1000) % 100;
    int64_t min_part = (t / 100000) % 100;
    int64_t hour_part = (t / 10000000) % 100;
    int64_t day_part = (t / 1000000000LL) % 100;
    int64_t month_part = (t / 100000000000LL) % 100;
    int64_t year_part = t / 10000000000000LL;
    sprintf(buf, "%04lld.%02lld.%02lld %02lld:%02lld:%02lld.%03lld",
            (long long)year_part, (long long)month_part, (long long)day_part,
            (long long)hour_part, (long long)min_part, (long long)sec_part,
            (long long)ms_part);
    return rt_dup(buf);
}

extern const char* rt_nanotimestamp_to_str(int64_t n) {
    char buf[32];
    sprintf(buf, "%lld", (long long)n);
    return rt_dup(buf);
}

/* 附加：sleep / now / today（0.3 兼容） */
extern void shadow_sleep(int32_t ms) {
    Sleep((DWORD)ms);
}

extern int32_t shadow_now(void) {
    return (int32_t)time(NULL);
}

extern int32_t shadow_today(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_s(&lt, &t);
    return (int32_t)((lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday);
}
