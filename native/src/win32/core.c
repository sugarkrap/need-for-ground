/*
 * core.c - shim lifecycle, handle table, last-error, tracing.
 */
#include "shim_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread DWORD g_last_error;
static int g_trace = -1;

int nfsu2_shim_trace_enabled(void)
{
    if (g_trace < 0) {
        const char *v = getenv("NFSU2_SHIM_TRACE");
        g_trace = (v && *v && *v != '0') ? 1 : 0;
    }
    return g_trace;
}

void nfsu2_shim_trace(const char *fmt, ...)
{
    va_list ap;

    if (!nfsu2_shim_trace_enabled())
        return;

    fputs("[nfsu2/shim] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int nfsu2_win32_init(const char *game_root)
{
    char resolved[PATH_MAX_FALLBACK];
    int rc;

    rc = nfsu2_path_set_root(game_root, resolved, sizeof(resolved));
    if (rc != 0)
        return rc;

    nfsu2_shim_trace("init: game root = %s", resolved);
    return 0;
}

void nfsu2_win32_shutdown(void)
{
    /* Settings the game wrote through RegSetValueExA are flushed here so they
     * survive a clean exit; see advapi32/registry.c. */
    nfsu2_registry_flush();
    nfsu2_path_reset();
}

/* --- handles ----------------------------------------------------------- */

/*
 * A table of the handles we have handed out, and why guessing is not good enough.
 *
 * A HANDLE here is a pointer to one of our objects, so the obvious check is to
 * dereference it and look at the `kind` field. That is what this used to do, and
 * the game's startup broke it: it calls CloseHandle on a pointer that is not one of
 * ours, whose first two words happen to read as `{kind = NFSU2_OBJ_FIND, refs = 1}`
 * - two small integers, which arbitrary game data is full of. The payload was then
 * read as a find handle and closedir() was called on 0x1, inside libc, five frames
 * away from anything that would explain it.
 *
 * Windows answers a handle it does not recognise with ERROR_INVALID_HANDLE, and so
 * must this: passing a stale or foreign handle is ordinary behaviour, and turning it
 * into a crash somewhere else is the worst possible response. Only pointers this
 * table issued are dereferenced.
 *
 * Open-addressed set, sized generously - the game holds a few hundred handles at
 * most - and locked, because handles cross threads.
 */
#define HANDLE_TABLE_SLOTS 4096

static void *g_handles[HANDLE_TABLE_SLOTS];
static size_t g_handle_count;
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t handle_slot(const void *p)
{
    /* Pointers are 8- or 16-byte aligned, so the low bits carry nothing; shift
     * them out before mixing. */
    uintptr_t v = (uintptr_t)p >> 3;

    v *= 2654435761u; /* Knuth's multiplicative hash */
    return (size_t)(v & (HANDLE_TABLE_SLOTS - 1));
}

static void handle_register(void *p)
{
    size_t slot, probe;

    pthread_mutex_lock(&g_handle_lock);
    if (g_handle_count + 1 >= HANDLE_TABLE_SLOTS / 2) {
        /* Refusing to grow silently: a table this full means a handle leak, and
         * losing track of handles would reintroduce exactly the bug above. */
        nfsu2_shim_trace("handle table is %zu/%d full - handles are leaking",
                         g_handle_count, HANDLE_TABLE_SLOTS);
    }
    slot = handle_slot(p);
    for (probe = 0; probe < HANDLE_TABLE_SLOTS; probe++) {
        size_t index = (slot + probe) & (HANDLE_TABLE_SLOTS - 1);

        if (!g_handles[index]) {
            g_handles[index] = p;
            g_handle_count++;
            break;
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
}

/* Remove and report whether it was there - so a double close is answerable. */
static int handle_unregister(void *p)
{
    size_t slot, probe;
    int found = 0;

    pthread_mutex_lock(&g_handle_lock);
    slot = handle_slot(p);
    for (probe = 0; probe < HANDLE_TABLE_SLOTS; probe++) {
        size_t index = (slot + probe) & (HANDLE_TABLE_SLOTS - 1);

        if (g_handles[index] == p) {
            /*
             * Tombstone-free deletion is wrong with linear probing, so keep it
             * simple and re-insert the rest of this cluster.
             */
            g_handles[index] = NULL;
            g_handle_count--;
            found = 1;
            for (probe = probe + 1; probe < HANDLE_TABLE_SLOTS; probe++) {
                size_t next = (slot + probe) & (HANDLE_TABLE_SLOTS - 1);
                void *moved = g_handles[next];

                if (!moved)
                    break;
                g_handles[next] = NULL;
                g_handle_count--;
                pthread_mutex_unlock(&g_handle_lock);
                handle_register(moved);
                pthread_mutex_lock(&g_handle_lock);
            }
            break;
        }
        if (!g_handles[index])
            break;
    }
    pthread_mutex_unlock(&g_handle_lock);
    return found;
}

int nfsu2_obj_is_live(const void *p)
{
    size_t slot, probe;
    int found = 0;

    if (!p)
        return 0;

    pthread_mutex_lock(&g_handle_lock);
    slot = handle_slot(p);
    for (probe = 0; probe < HANDLE_TABLE_SLOTS; probe++) {
        size_t index = (slot + probe) & (HANDLE_TABLE_SLOTS - 1);

        if (g_handles[index] == p) {
            found = 1;
            break;
        }
        if (!g_handles[index])
            break;
    }
    pthread_mutex_unlock(&g_handle_lock);
    return found;
}

void *nfsu2_obj_alloc(enum nfsu2_obj_kind kind, size_t size)
{
    struct nfsu2_object *obj = calloc(1, size);

    if (!obj) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    obj->kind = kind;
    obj->refs = 1;
    handle_register(obj);
    nfsu2_shim_trace("HANDLE alloc %p kind=%d size=%zu", (void *)obj, (int)kind, size);
    return obj;
}

void *nfsu2_obj_get(HANDLE h, enum nfsu2_obj_kind kind)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)h;

    if (!nfsu2_obj_is_live(obj) || obj->kind != kind) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return obj;
}

void nfsu2_obj_release(struct nfsu2_object *obj)
{
    if (!obj || --obj->refs > 0)
        return;

    /* Out of the table before the destructor runs, so a second close finds
     * nothing rather than a freed object. */
    nfsu2_shim_trace("HANDLE free %p kind=%d", (void *)obj, (int)obj->kind);
    handle_unregister(obj);

    switch (obj->kind) {
    case NFSU2_OBJ_FILE:   nfsu2_file_destroy(obj);   break;
    case NFSU2_OBJ_FIND:   nfsu2_find_destroy(obj);   break;
    case NFSU2_OBJ_EVENT:
    case NFSU2_OBJ_MUTEX:  nfsu2_sync_destroy(obj);   break;
    case NFSU2_OBJ_THREAD: nfsu2_thread_destroy(obj); break;
    case NFSU2_OBJ_MAPPING: nfsu2_mapping_destroy(obj); break;
    case NFSU2_OBJ_SNAPSHOT: nfsu2_snapshot_destroy(obj); break;
    default:               free(obj);                 break;
    }
}

/*
 * The pseudo-handles: -1 for the current process, -2 for the current thread, the
 * same values Windows uses. They are not pointers and must never be dereferenced.
 */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)-2)

static int is_pseudo_handle(HANDLE h)
{
    return h == PSEUDO_CURRENT_PROCESS || h == PSEUDO_CURRENT_THREAD;
}

BOOL WINAPI CloseHandle(HANDLE h)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)h;

    /*
     * Not one of ours - stale, already closed, or never a handle at all. Windows
     * answers ERROR_INVALID_HANDLE; anything else here means dereferencing a
     * pointer chosen by someone else. The game's startup does exactly this.
     */
    /* Closing a pseudo-handle is a no-op that succeeds, as on Windows - the game
     * closes what DuplicateHandle gave it, and that is one of these. */
    if (is_pseudo_handle(h))
        return TRUE;

    if (!nfsu2_obj_is_live(obj)) {
        nfsu2_shim_trace("CloseHandle(%p): not a handle we issued", (void *)h);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    /* The process heap pseudo-handle is not refcounted. */
    if (obj->kind == NFSU2_OBJ_HEAP)
        return TRUE;

    nfsu2_obj_release(obj);
    return TRUE;
}

BOOL WINAPI DuplicateHandle(HANDLE src_proc, HANDLE src, HANDLE dst_proc, HANDLE *dst,
                            DWORD access, BOOL inherit, DWORD options)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)src;

    (void)src_proc; (void)dst_proc; (void)access; (void)inherit;

    if (!dst) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /*
     * `DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), ...)` is the
     * standard way to turn a pseudo-handle into one that outlives the call, and
     * the game's startup does exactly that. The pseudo-handle is its own duplicate
     * here: everything this shim implements for a thread handle accepts it, and it
     * cannot be dereferenced by mistake.
     *
     * Where this is not Windows: a real duplicate stays valid when handed to
     * another thread, while -2 always means "the thread asking". Nothing has needed
     * that yet, and the trace says so if something starts to.
     */
    if (is_pseudo_handle(src)) {
        nfsu2_shim_trace("DuplicateHandle(%s): returning the pseudo-handle itself",
                         src == PSEUDO_CURRENT_THREAD ? "current thread" : "current process");
        *dst = src;
        return TRUE;
    }

    if (!nfsu2_obj_is_live(obj)) {
        nfsu2_shim_trace("DuplicateHandle(%p): not a handle we issued", (void *)src);
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    obj->refs++;
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        nfsu2_obj_release(obj);
    return TRUE;
}

/* --- last error -------------------------------------------------------- */

DWORD WINAPI GetLastError(void)
{
    return g_last_error;
}

void WINAPI SetLastError(DWORD err)
{
    g_last_error = err;
}

DWORD nfsu2_errno_to_win32(int err)
{
    switch (err) {
    case 0:       return ERROR_SUCCESS;
    case ENOENT:  return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:   return ERROR_ACCESS_DENIED;
    case EEXIST:  return ERROR_FILE_EXISTS;
    case EMFILE:
    case ENFILE:  return ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM:  return ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC:  return ERROR_DISK_FULL;
    case EINVAL:  return ERROR_INVALID_PARAMETER;
    case EBADF:   return ERROR_INVALID_HANDLE;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EISDIR:  return ERROR_ACCESS_DENIED;
    case EXDEV:   return ERROR_NOT_SAME_DEVICE;
    default:      return ERROR_GEN_FAILURE;
    }
}

void nfsu2_set_last_error_from_errno(int err)
{
    SetLastError(nfsu2_errno_to_win32(err));
}
