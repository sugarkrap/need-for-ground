/*
 * core.c - shim lifecycle, handle table, last-error, tracing.
 */
#include "shim_internal.h"

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

void *nfsu2_obj_alloc(enum nfsu2_obj_kind kind, size_t size)
{
    struct nfsu2_object *obj = calloc(1, size);

    if (!obj) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    obj->kind = kind;
    obj->refs = 1;
    return obj;
}

void *nfsu2_obj_get(HANDLE h, enum nfsu2_obj_kind kind)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)h;

    if (!obj || h == INVALID_HANDLE_VALUE || obj->kind != kind) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return obj;
}

void nfsu2_obj_release(struct nfsu2_object *obj)
{
    if (!obj || --obj->refs > 0)
        return;

    switch (obj->kind) {
    case NFSU2_OBJ_FILE:   nfsu2_file_destroy(obj);   break;
    case NFSU2_OBJ_FIND:   nfsu2_find_destroy(obj);   break;
    case NFSU2_OBJ_EVENT:
    case NFSU2_OBJ_MUTEX:  nfsu2_sync_destroy(obj);   break;
    case NFSU2_OBJ_THREAD: nfsu2_thread_destroy(obj); break;
    case NFSU2_OBJ_MAPPING: nfsu2_mapping_destroy(obj); break;
    default:               free(obj);                 break;
    }
}

BOOL WINAPI CloseHandle(HANDLE h)
{
    struct nfsu2_object *obj = (struct nfsu2_object *)h;

    if (!obj || h == INVALID_HANDLE_VALUE) {
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

    if (!obj || src == INVALID_HANDLE_VALUE || !dst) {
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
