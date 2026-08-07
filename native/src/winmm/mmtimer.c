/*
 * mmtimer.c - the multimedia timers (timeSetEvent / timeKillEvent).
 *
 * These are periodic callbacks with millisecond resolution - a 2004 game's answer to
 * "call me every N ms regardless of frame rate". This game uses them for its audio
 * engine, which is why the details below matter more than they look.
 *
 * The callback convention matters: LPTIMECALLBACK is WINAPI (stdcall on i386) while
 * pthreads are SysV cdecl, so the thread body is a trampoline - the same boundary
 * win32/thread.c crosses for CreateThread.
 *
 * Two things this got wrong, both of which stopped the game's audio dead while
 * looking like a working implementation:
 *
 *   - **The callback ran on a thread with no TEB.** Game code reached through a
 *     callback is game code: it touches fs:[0] for the SEH chain the moment it uses
 *     __try, and on a thread whose %fs is whatever glibc left there that is not
 *     survivable. win32/thread.c installs a TEB before anything else runs on a
 *     thread it creates, and this has to do the same. Nothing about the failure
 *     pointed at timers - the audio buffer simply never got written to.
 *
 *   - **A thread per timer.** The game arms TIME_ONESHOT timers with a 1 ms delay
 *     and re-arms from inside the callback, so a thread per timer is a thread per
 *     millisecond, against a table of 16. Once the table filled, timeSetEvent
 *     returned 0 and the game's pump stopped being called at all.
 *
 * So there is one service thread for every timer, with a TEB, which is also what
 * Windows does.
 */
#include "../win32/shim_internal.h"

#include <nfsu2/teb.h>

#include <mmsystem.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Generous, because the game arms one-shots faster than it retires them and a full
 * table is not a degraded mode - it is silence. */
#define MAX_TIMERS 64

struct mm_timer {
    int in_use;
    UINT id;
    UINT period_ms;
    UINT flags;
    LPTIMECALLBACK callback;
    DWORD_PTR user;
    /* When this timer is next due, on the monotonic clock, in milliseconds. */
    unsigned long long due_ms;
};

/* Armed and fired, for answering "is the game's pump running at all" without a line
 * per millisecond. The first of each is traced; after that only the counts change. */
static unsigned long g_armed;
static unsigned long g_fired;

static struct mm_timer g_timers[MAX_TIMERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_t g_service;
static int g_service_running;

static unsigned long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

/*
 * The service thread. One pass: collect everything due, release the lock, then call
 * the callbacks.
 *
 * The lock must not be held across a callback, and not only for latency: this game
 * re-arms its timer from *inside* the callback, so a callback that called
 * timeSetEvent while the lock was held would deadlock. For the same reason a
 * one-shot's slot is freed before its callback runs, so that the re-arm has somewhere
 * to go.
 */
static void *timer_service(void *arg)
{
    char teb_error[128] = "";

    (void)arg;

    /*
     * A TEB before any game code runs here. See the header comment: a WINAPI
     * callback is game code, and the SEH chain it expects lives at fs:[0].
     */
    if (nfsu2_teb_install(teb_error, sizeof(teb_error)) != 0)
        nfsu2_shim_trace("timeSetEvent: no TEB for the timer thread: %s - a callback "
                         "that uses __try will not survive", teb_error);

    pthread_mutex_lock(&g_lock);
    for (;;) {
        struct {
            LPTIMECALLBACK callback;
            UINT id;
            DWORD_PTR user;
        } due[MAX_TIMERS];
        int due_count = 0;
        unsigned long long now = now_ms();
        unsigned long long next = 0;
        int i;

        for (i = 0; i < MAX_TIMERS; i++) {
            struct mm_timer *timer = &g_timers[i];

            if (!timer->in_use)
                continue;
            if (timer->due_ms > now) {
                if (!next || timer->due_ms < next)
                    next = timer->due_ms;
                continue;
            }

            due[due_count].callback = timer->callback;
            due[due_count].id = timer->id;
            due[due_count].user = timer->user;
            due_count++;

            if ((timer->flags & TIME_PERIODIC) == TIME_PERIODIC) {
                timer->due_ms = now + (timer->period_ms ? timer->period_ms : 1);
                if (!next || timer->due_ms < next)
                    next = timer->due_ms;
            } else {
                /* Freed before the callback runs, so a re-arm from inside it has a
                 * slot. This is the case the game actually uses. */
                timer->in_use = 0;
            }
        }

        pthread_mutex_unlock(&g_lock);
        for (i = 0; i < due_count; i++) {
            if (!g_fired)
                nfsu2_shim_trace("timeSetEvent: the first timer callback is running "
                                 "(id %u) - the game's periodic work is happening",
                                 due[i].id);
            g_fired++;
            due[i].callback(due[i].id, 0, due[i].user, 0, 0);
        }
        pthread_mutex_lock(&g_lock);

        if (due_count > 0)
            continue; /* re-check: a callback may have armed something already due */

        if (next) {
            struct timespec deadline;
            unsigned long long target = next;

            deadline.tv_sec = (time_t)(target / 1000ull);
            deadline.tv_nsec = (long)(target % 1000ull) * 1000000L;
            pthread_cond_timedwait(&g_wake, &g_lock, &deadline);
        } else {
            /* Nothing armed. Wait to be told, rather than spinning at 1 kHz. */
            pthread_cond_wait(&g_wake, &g_lock);
        }
    }
    /* Not reached: the service thread runs for the life of the process. Present
     * because the compiler cannot see that, and a warning here is not worth having. */
    return NULL;
}

/* Caller holds g_lock. */
static int service_start_locked(void)
{
    pthread_condattr_t attr;

    if (g_service_running)
        return 0;

    /* The condition variable has to use the same clock the deadlines come from, or
     * pthread_cond_timedwait waits against the wrong epoch and every timer is late
     * by the difference between the two clocks. */
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_wake, &attr);
    pthread_condattr_destroy(&attr);

    if (pthread_create(&g_service, NULL, timer_service, NULL) != 0)
        return -1;
    pthread_detach(g_service);
    g_service_running = 1;
    return 0;
}

MMRESULT WINAPI timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback,
                             DWORD_PTR user, UINT flags)
{
    struct mm_timer *timer = NULL;
    UINT id;
    int i;

    (void)resolution; /* the service thread's resolution already beats what this asks */

    if (!callback)
        return 0; /* 0 is the failure return for this API, not an MMRESULT */

    if ((flags & TIME_CALLBACK_FUNCTION) != TIME_CALLBACK_FUNCTION &&
        (flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE))) {
        /* Event-based notification would need the callback pointer treated as an
         * event HANDLE; nothing here asks for it, and guessing would signal the
         * wrong object. */
        NFSU2_STUB("timeSetEvent with event callback");
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    if (service_start_locked() != 0) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    for (i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].in_use) {
            timer = &g_timers[i];
            break;
        }
    }
    if (!timer) {
        pthread_mutex_unlock(&g_lock);
        /* Loud: this used to be reachable in normal play, and the symptom was the
         * game's audio pump silently never being called again. */
        nfsu2_shim_trace("timeSetEvent: timer table full (%d) - THIS DROPS A TIMER "
                         "the game asked for", MAX_TIMERS);
        return 0;
    }

    memset(timer, 0, sizeof(*timer));
    timer->in_use = 1;
    timer->id = (UINT)(i + 1); /* non-zero: zero means failure */
    timer->period_ms = delay;
    timer->flags = flags;
    timer->callback = callback;
    timer->user = user;
    timer->due_ms = now_ms() + delay;
    id = timer->id;
    if (!g_armed)
        nfsu2_shim_trace("timeSetEvent: first timer armed - %u ms, flags 0x%x (%s)",
                         delay, flags,
                         (flags & TIME_PERIODIC) == TIME_PERIODIC ? "periodic" : "one-shot");
    g_armed++;

    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return id;
}

MMRESULT WINAPI timeKillEvent(UINT id)
{
    int index = (int)id - 1;

    if (index < 0 || index >= MAX_TIMERS)
        return MMSYSERR_INVALPARAM;

    pthread_mutex_lock(&g_lock);
    if (!g_timers[index].in_use) {
        /*
         * Not an error worth reporting as one: a one-shot that has already fired is
         * gone, and killing it is what a caller does on a timer it is not sure about.
         * Windows returns MMSYSERR_INVALPARAM for an unknown id, so that is kept.
         */
        pthread_mutex_unlock(&g_lock);
        return MMSYSERR_INVALPARAM;
    }
    /*
     * Cleared rather than cancelled mid-flight. The service thread may be inside this
     * timer's callback right now, with the lock released; it will not be called again
     * once the slot is free, and stopping it part-way through - which is what
     * pthread_cancel would do - could leave the game's own state half-updated.
     */
    g_timers[index].in_use = 0;
    pthread_cond_signal(&g_wake);
    pthread_mutex_unlock(&g_lock);
    return TIMERR_NOERROR;
}
