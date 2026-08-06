/*
 * time.c - timing and clock APIs.
 *
 * The game's frame pacing and physics stepping read GetTickCount /
 * timeGetTime / QueryPerformanceCounter, so all three must agree about
 * "now" and must be monotonic. All of them derive from CLOCK_MONOTONIC here.
 */
#include "shim_internal.h"

#include <mmsystem.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define NS_PER_SEC 1000000000ULL

static unsigned long long g_base_ns;

static unsigned long long mono_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * NS_PER_SEC + (unsigned long long)ts.tv_nsec;
}

/*
 * Anchored at load time rather than at the first call, so the first
 * GetTickCount() of the process is not forced to 0 - code that samples a
 * "start time" during init and diffs against it later would see a
 * discontinuity on the very first frame otherwise.
 */
static void __attribute__((constructor)) init_clock_base(void)
{
    g_base_ns = mono_ns();
}

static unsigned long long since_start_ns(void)
{
    if (g_base_ns == 0)
        init_clock_base();
    return mono_ns() - g_base_ns;
}

DWORD WINAPI GetTickCount(void)
{
    return (DWORD)(since_start_ns() / 1000000ULL);
}

ULONGLONG WINAPI GetTickCount64(void)
{
    return (ULONGLONG)(since_start_ns() / 1000000ULL);
}

DWORD WINAPI timeGetTime(void)
{
    return GetTickCount();
}

MMRESULT WINAPI timeBeginPeriod(UINT period)
{
    /* Linux has no global timer-resolution knob to raise; nanosleep already
     * gives us better granularity than the 1ms this asks for. */
    (void)period;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeEndPeriod(UINT period)
{
    (void)period;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS caps, UINT size)
{
    if (!caps || size < sizeof(*caps))
        return TIMERR_NOCANDO;
    caps->wPeriodMin = 1;
    caps->wPeriodMax = 1000000;
    return TIMERR_NOERROR;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *freq)
{
    if (!freq)
        return FALSE;
    freq->QuadPart = (LONGLONG)NS_PER_SEC;
    return TRUE;
}

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *counter)
{
    if (!counter)
        return FALSE;
    counter->QuadPart = (LONGLONG)since_start_ns();
    return TRUE;
}

VOID WINAPI Sleep(DWORD ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

DWORD WINAPI SleepEx(DWORD ms, BOOL alertable)
{
    /* No APC queue in this shim, so alertable waits simply are not alerted. */
    (void)alertable;
    Sleep(ms);
    return 0;
}

#define UNIX_TO_FILETIME_OFFSET 116444736000000000ULL

VOID WINAPI GetSystemTimeAsFileTime(FILETIME *out)
{
    struct timespec ts;
    unsigned long long ticks;

    if (!out)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    ticks = (unsigned long long)ts.tv_sec * 10000000ULL +
            (unsigned long long)ts.tv_nsec / 100ULL + UNIX_TO_FILETIME_OFFSET;
    out->dwLowDateTime = (DWORD)(ticks & 0xffffffffU);
    out->dwHighDateTime = (DWORD)(ticks >> 32);
}

static void tm_to_systemtime(const struct tm *tm, SYSTEMTIME *out)
{
    out->wYear = (WORD)(tm->tm_year + 1900);
    out->wMonth = (WORD)(tm->tm_mon + 1);
    out->wDayOfWeek = (WORD)tm->tm_wday;
    out->wDay = (WORD)tm->tm_mday;
    out->wHour = (WORD)tm->tm_hour;
    out->wMinute = (WORD)tm->tm_min;
    out->wSecond = (WORD)tm->tm_sec;
    out->wMilliseconds = 0;
}

VOID WINAPI GetSystemTime(SYSTEMTIME *out)
{
    time_t now = time(NULL);
    struct tm tm;

    if (!out)
        return;
    gmtime_r(&now, &tm);
    tm_to_systemtime(&tm, out);
}

VOID WINAPI GetLocalTime(SYSTEMTIME *out)
{
    time_t now = time(NULL);
    struct tm tm;

    if (!out)
        return;
    localtime_r(&now, &tm);
    tm_to_systemtime(&tm, out);
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *out)
{
    unsigned long long ticks;
    time_t unix_time;
    struct tm tm;

    if (!ft || !out)
        return FALSE;
    ticks = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < UNIX_TO_FILETIME_OFFSET)
        return FALSE;
    unix_time = (time_t)((ticks - UNIX_TO_FILETIME_OFFSET) / 10000000ULL);
    gmtime_r(&unix_time, &tm);
    tm_to_systemtime(&tm, out);
    out->wMilliseconds = (WORD)((ticks / 10000ULL) % 1000ULL);
    return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *ft, FILETIME *out)
{
    unsigned long long ticks;
    time_t now = time(NULL);
    struct tm tm;
    long long bias_ticks;

    if (!ft || !out)
        return FALSE;
    localtime_r(&now, &tm);
    bias_ticks = (long long)tm.tm_gmtoff * 10000000LL;
    ticks = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    ticks = (unsigned long long)((long long)ticks + bias_ticks);
    out->dwLowDateTime = (DWORD)(ticks & 0xffffffffU);
    out->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION tz)
{
    time_t now = time(NULL);
    struct tm tm;

    if (!tz)
        return TIME_ZONE_ID_INVALID;
    localtime_r(&now, &tm);
    memset(tz, 0, sizeof(*tz));
    tz->Bias = (LONG)(-tm.tm_gmtoff / 60);
    return tm.tm_isdst > 0 ? TIME_ZONE_ID_DAYLIGHT : TIME_ZONE_ID_STANDARD;
}
