/*
 * sync.c - critical sections, events, mutexes, TLS, interlocked ops, threads.
 */
#include "shim_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* --- critical sections -------------------------------------------------- */

/*
 * CRITICAL_SECTION is an opaque-by-contract struct, so we stash a
 * pthread_mutex_t pointer in LockSemaphore and leave the rest zeroed. Nothing
 * outside this shim inspects the other fields (the game never does; MSVC's
 * CRT does not either).
 */
VOID WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    pthread_mutexattr_t attr;

    if (!m || !cs)
        return;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE); /* Win32 CS is recursive */
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);

    memset(cs, 0, sizeof(*cs));
    cs->LockSemaphore = (HANDLE)m;
    cs->SpinCount = 0;
}

BOOL WINAPI InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin)
{
    (void)spin;
    InitializeCriticalSection(cs);
    return TRUE;
}

VOID WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs)
{
    pthread_mutex_t *m;

    if (!cs || !cs->LockSemaphore)
        return;
    m = (pthread_mutex_t *)cs->LockSemaphore;
    pthread_mutex_destroy(m);
    free(m);
    cs->LockSemaphore = NULL;
}

VOID WINAPI EnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return;
    /* Some code paths lock a section that was memset to zero rather than
     * initialised; initialise lazily instead of dereferencing NULL. */
    if (!cs->LockSemaphore)
        InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
}

VOID WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs || !cs->LockSemaphore)
        return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL WINAPI TryEnterCriticalSection(CRITICAL_SECTION *cs)
{
    if (!cs)
        return FALSE;
    if (!cs->LockSemaphore)
        InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) != 0)
        return FALSE;
    cs->RecursionCount++;
    return TRUE;
}

/* --- events and mutexes ------------------------------------------------- */

struct nfsu2_sync {
    struct nfsu2_object obj;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int signalled;
    int manual_reset;
    char *name;
};

void nfsu2_sync_destroy(struct nfsu2_object *obj)
{
    struct nfsu2_sync *s = (struct nfsu2_sync *)obj;

    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s->name);
    free(s);
}

static struct nfsu2_sync *sync_create(enum nfsu2_obj_kind kind, BOOL manual, BOOL signalled,
                                      LPCSTR name)
{
    struct nfsu2_sync *s = nfsu2_obj_alloc(kind, sizeof(*s));

    if (!s)
        return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->manual_reset = manual ? 1 : 0;
    s->signalled = signalled ? 1 : 0;
    s->name = name ? strdup(name) : NULL;
    return s;
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCSTR name)
{
    (void)sa;
    /*
     * Named objects are process-local here. The game only uses a name to
     * detect a second instance of itself, which we do not support anyway.
     */
    return (HANDLE)sync_create(NFSU2_OBJ_EVENT, manual, initial, name);
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL owned, LPCSTR name)
{
    (void)sa;
    return (HANDLE)sync_create(NFSU2_OBJ_MUTEX, FALSE, !owned, name);
}

BOOL WINAPI SetEvent(HANDLE h)
{
    struct nfsu2_sync *s = nfsu2_obj_get(h, NFSU2_OBJ_EVENT);

    if (!s)
        return FALSE;
    pthread_mutex_lock(&s->lock);
    s->signalled = 1;
    if (s->manual_reset)
        pthread_cond_broadcast(&s->cond);
    else
        pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE h)
{
    struct nfsu2_sync *s = nfsu2_obj_get(h, NFSU2_OBJ_EVENT);

    if (!s)
        return FALSE;
    pthread_mutex_lock(&s->lock);
    s->signalled = 0;
    pthread_mutex_unlock(&s->lock);
    return TRUE;
}

BOOL WINAPI ReleaseMutex(HANDLE h)
{
    struct nfsu2_sync *s = nfsu2_obj_get(h, NFSU2_OBJ_MUTEX);

    if (!s)
        return FALSE;
    pthread_mutex_lock(&s->lock);
    s->signalled = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return TRUE;
}

static DWORD sync_wait(struct nfsu2_sync *s, DWORD timeout_ms)
{
    DWORD result = WAIT_OBJECT_0;

    pthread_mutex_lock(&s->lock);
    while (!s->signalled) {
        if (timeout_ms == 0) {
            result = WAIT_TIMEOUT;
            break;
        }
        if (timeout_ms == INFINITE) {
            pthread_cond_wait(&s->cond, &s->lock);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000);
            ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) != 0 && !s->signalled) {
                result = WAIT_TIMEOUT;
                break;
            }
        }
    }
    /* Auto-reset events and mutexes consume the signal. */
    if (result == WAIT_OBJECT_0 && !s->manual_reset)
        s->signalled = 0;
    pthread_mutex_unlock(&s->lock);
    return result;
}

DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD timeout_ms)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)h;

    if (!obj || h == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return WAIT_FAILED;
    }
    if (obj->kind == NFSU2_OBJ_EVENT || obj->kind == NFSU2_OBJ_MUTEX)
        return sync_wait((struct nfsu2_sync *)obj, timeout_ms);
    if (obj->kind == NFSU2_OBJ_THREAD)
        return nfsu2_thread_wait(obj, timeout_ms);

    SetLastError(ERROR_INVALID_HANDLE);
    return WAIT_FAILED;
}

DWORD WINAPI WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL wait_all,
                                    DWORD timeout_ms)
{
    DWORD i;

    if (!handles || count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }

    if (wait_all) {
        for (i = 0; i < count; i++) {
            DWORD r = WaitForSingleObject(handles[i], timeout_ms);
            if (r != WAIT_OBJECT_0)
                return r;
        }
        return WAIT_OBJECT_0;
    }

    /*
     * Wait-any is polled rather than properly multiplexed. Correct but not
     * efficient; the game uses it on a two-handle set at thread shutdown, not
     * per frame.
     */
    for (;;) {
        for (i = 0; i < count; i++) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                return WAIT_OBJECT_0 + i;
        }
        if (timeout_ms == 0)
            return WAIT_TIMEOUT;
        Sleep(1);
        if (timeout_ms != INFINITE) {
            if (timeout_ms <= 1)
                return WAIT_TIMEOUT;
            timeout_ms -= 1;
        }
    }
}

/*
 * Interlocked* are deliberately absent: Wine's winnt.h already defines them as
 * FORCEINLINE static functions over the compiler's atomic builtins (see
 * winnt.h:7695 onwards), so ported code that calls them gets a correct inline
 * implementation and defining our own is a redefinition error.
 *
 * The one thing this loses is GetProcAddress("InterlockedCompareExchange"),
 * which resolves to nothing because there is no out-of-line symbol. Nothing in
 * the game's import list needs that - it imports them for direct calls.
 */

/* --- TLS --------------------------------------------------------------- */

#define TLS_SLOTS 128

static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_tls_used[TLS_SLOTS];
static __thread void *g_tls_values[TLS_SLOTS];

DWORD WINAPI TlsAlloc(void)
{
    DWORD i;

    pthread_mutex_lock(&g_tls_lock);
    for (i = 0; i < TLS_SLOTS; i++) {
        if (!g_tls_used[i]) {
            g_tls_used[i] = 1;
            pthread_mutex_unlock(&g_tls_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&g_tls_lock);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return TLS_OUT_OF_INDEXES;
}

BOOL WINAPI TlsFree(DWORD index)
{
    if (index >= TLS_SLOTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pthread_mutex_lock(&g_tls_lock);
    g_tls_used[index] = 0;
    pthread_mutex_unlock(&g_tls_lock);
    return TRUE;
}

LPVOID WINAPI TlsGetValue(DWORD index)
{
    if (index >= TLS_SLOTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    SetLastError(ERROR_SUCCESS);
    return g_tls_values[index];
}

BOOL WINAPI TlsSetValue(DWORD index, LPVOID value)
{
    if (index >= TLS_SLOTS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    g_tls_values[index] = value;
    return TRUE;
}
