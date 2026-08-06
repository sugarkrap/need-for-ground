/*
 * mmtimer.c - the multimedia timers (timeSetEvent / timeKillEvent).
 *
 * These are periodic callbacks with millisecond resolution - a 2004 game's
 * answer to "call me every N ms regardless of frame rate", typically for audio
 * streaming or a fixed-step update. Implemented with a thread per timer, which
 * is what they are: the callback runs off the caller's thread, exactly as on
 * Windows.
 *
 * The callback convention matters. LPTIMECALLBACK is WINAPI (stdcall on i386)
 * while pthreads are SysV cdecl, so the thread body has to be a trampoline -
 * the same boundary win32/thread.c crosses for CreateThread.
 */
#include "../win32/shim_internal.h"

#include <mmsystem.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_TIMERS 16

struct mm_timer {
    int in_use;
    UINT id;
    UINT period_ms;
    UINT flags;
    LPTIMECALLBACK callback;
    DWORD_PTR user;
    volatile int cancelled;
    pthread_t thread;
};

static struct mm_timer g_timers[MAX_TIMERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void sleep_ms(UINT ms)
{
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

static void *timer_thread(void *arg)
{
    struct mm_timer *timer = arg;
    UINT period = timer->period_ms ? timer->period_ms : 1;

    for (;;) {
        sleep_ms(period);
        if (timer->cancelled)
            break;

        /* Crossing back into stdcall: the game's callback is WINAPI. */
        timer->callback(timer->id, 0, timer->user, 0, 0);

        if ((timer->flags & TIME_PERIODIC) != TIME_PERIODIC)
            break; /* TIME_ONESHOT */
    }

    pthread_mutex_lock(&g_lock);
    timer->in_use = 0;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback,
                             DWORD_PTR user, UINT flags)
{
    struct mm_timer *timer = NULL;
    int i;

    (void)resolution; /* nanosleep already beats what this asks for */

    if (!callback)
        return 0; /* 0 is the failure return for this API, not an MMRESULT */

    if ((flags & TIME_CALLBACK_FUNCTION) != TIME_CALLBACK_FUNCTION &&
        (flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))) {
        /* Event-based notification would need the callback pointer treated as
         * an event HANDLE; nothing here asks for it, and guessing would signal
         * the wrong object. */
        NFSU2_STUB("timeSetEvent with event callback");
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].in_use) {
            timer = &g_timers[i];
            break;
        }
    }
    if (!timer) {
        pthread_mutex_unlock(&g_lock);
        nfsu2_shim_trace("timeSetEvent: timer table full (%d)", MAX_TIMERS);
        return 0;
    }

    memset(timer, 0, sizeof(*timer));
    timer->in_use = 1;
    timer->id = (UINT)(i + 1); /* non-zero: zero means failure */
    timer->period_ms = delay;
    timer->flags = flags;
    timer->callback = callback;
    timer->user = user;

    if (pthread_create(&timer->thread, NULL, timer_thread, timer) != 0) {
        timer->in_use = 0;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    pthread_detach(timer->thread);
    pthread_mutex_unlock(&g_lock);
    return timer->id;
}

MMRESULT WINAPI timeKillEvent(UINT id)
{
    int index = (int)id - 1;

    if (index < 0 || index >= MAX_TIMERS)
        return MMSYSERR_INVALPARAM;

    pthread_mutex_lock(&g_lock);
    if (!g_timers[index].in_use) {
        pthread_mutex_unlock(&g_lock);
        return MMSYSERR_INVALPARAM;
    }
    /*
     * Flagged rather than cancelled: pthread_cancel could stop the thread
     * inside the game's own callback, half-way through whatever it was doing.
     * The thread notices before the next callback, so the worst case is one
     * extra period of latency at shutdown.
     */
    g_timers[index].cancelled = 1;
    pthread_mutex_unlock(&g_lock);
    return TIMERR_NOERROR;
}
