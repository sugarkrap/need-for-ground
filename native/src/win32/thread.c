/*
 * thread.c - CreateThread and friends on pthreads.
 *
 * The thread entry point the game passes is WINAPI (stdcall on i386) while
 * pthread_create wants a SysV cdecl function, so every thread goes through a
 * trampoline. This is the same convention boundary d3d9_native.h deals with,
 * just in the opposite direction.
 */
#include "shim_internal.h"

#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

struct nfsu2_thread {
    struct nfsu2_object obj;
    pthread_t tid;
    LPTHREAD_START_ROUTINE start;
    LPVOID param;
    DWORD exit_code;
    int finished;
    int detached;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static __thread struct nfsu2_thread *g_current_thread;

void nfsu2_thread_destroy(struct nfsu2_object *obj)
{
    struct nfsu2_thread *t = (struct nfsu2_thread *)obj;

    if (!t->detached && !t->finished)
        pthread_detach(t->tid);
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->lock);
    free(t);
}

static void *thread_trampoline(void *arg)
{
    struct nfsu2_thread *t = arg;
    DWORD rc;

    g_current_thread = t;
    rc = t->start(t->param);

    pthread_mutex_lock(&t->lock);
    t->exit_code = rc;
    t->finished = 1;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack_size,
                           LPTHREAD_START_ROUTINE start, LPVOID param,
                           DWORD flags, LPDWORD thread_id)
{
    struct nfsu2_thread *t;
    pthread_attr_t attr;
    int rc;

    (void)sa;

    if (!start) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (flags & CREATE_SUSPENDED) {
        /* Would need a start gate; nothing in this game creates suspended
         * threads, so fail loudly rather than silently running it early. */
        NFSU2_STUB("CreateThread(CREATE_SUSPENDED)");
        SetLastError(ERROR_NOT_SUPPORTED);
        return NULL;
    }

    t = nfsu2_obj_alloc(NFSU2_OBJ_THREAD, sizeof(*t));
    if (!t)
        return NULL;
    t->start = start;
    t->param = param;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);

    pthread_attr_init(&attr);
    if (stack_size)
        pthread_attr_setstacksize(&attr, stack_size < 65536 ? 65536 : stack_size);
    rc = pthread_create(&t->tid, &attr, thread_trampoline, t);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        nfsu2_set_last_error_from_errno(rc);
        nfsu2_obj_release(&t->obj);
        return NULL;
    }

    if (thread_id)
        *thread_id = (DWORD)(uintptr_t)t->tid;
    return (HANDLE)t;
}

DWORD nfsu2_thread_wait(struct nfsu2_object *obj, DWORD timeout_ms)
{
    struct nfsu2_thread *t = (struct nfsu2_thread *)obj;
    DWORD result = WAIT_OBJECT_0;

    pthread_mutex_lock(&t->lock);
    while (!t->finished) {
        if (timeout_ms == 0) {
            result = WAIT_TIMEOUT;
            break;
        }
        if (timeout_ms == INFINITE) {
            pthread_cond_wait(&t->cond, &t->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000);
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&t->cond, &t->lock, &ts) != 0 && !t->finished) {
                result = WAIT_TIMEOUT;
                break;
            }
        }
    }
    pthread_mutex_unlock(&t->lock);

    if (result == WAIT_OBJECT_0 && !t->detached) {
        pthread_join(t->tid, NULL);
        t->detached = 1;
    }
    return result;
}

BOOL WINAPI GetExitCodeThread(HANDLE h, LPDWORD exit_code)
{
    struct nfsu2_thread *t = nfsu2_obj_get(h, NFSU2_OBJ_THREAD);

    if (!t || !exit_code)
        return FALSE;
    *exit_code = t->finished ? t->exit_code : STILL_ACTIVE;
    return TRUE;
}

VOID WINAPI ExitThread(DWORD exit_code)
{
    struct nfsu2_thread *t = g_current_thread;

    if (t) {
        pthread_mutex_lock(&t->lock);
        t->exit_code = exit_code;
        t->finished = 1;
        pthread_cond_broadcast(&t->cond);
        pthread_mutex_unlock(&t->lock);
    }
    pthread_exit(NULL);
}

DWORD WINAPI GetCurrentThreadId(void)
{
    return (DWORD)(uintptr_t)pthread_self();
}

HANDLE WINAPI GetCurrentThread(void)
{
    /* Pseudo-handle: only ever passed straight back to Set/GetThreadPriority,
     * which are no-ops here. */
    return (HANDLE)-2;
}

/*
 * Priorities are deliberately no-ops. Raising thread priority on Linux needs
 * CAP_SYS_NICE or a configured rtkit; failing the call is the honest answer
 * and the game ignores the return value anyway.
 */
BOOL WINAPI SetThreadPriority(HANDLE h, int priority)
{
    (void)h; (void)priority;
    return TRUE;
}

int WINAPI GetThreadPriority(HANDLE h)
{
    (void)h;
    return THREAD_PRIORITY_NORMAL;
}

DWORD WINAPI SuspendThread(HANDLE h)
{
    (void)h;
    NFSU2_STUB("SuspendThread");
    return (DWORD)-1;
}

DWORD WINAPI ResumeThread(HANDLE h)
{
    (void)h;
    NFSU2_STUB("ResumeThread");
    return (DWORD)-1;
}

BOOL WINAPI TerminateThread(HANDLE h, DWORD exit_code)
{
    struct nfsu2_thread *t = nfsu2_obj_get(h, NFSU2_OBJ_THREAD);

    (void)exit_code;
    if (!t)
        return FALSE;
    /* pthread_cancel is not equivalent, but this only happens at shutdown. */
    NFSU2_STUB("TerminateThread");
    pthread_cancel(t->tid);
    return TRUE;
}
