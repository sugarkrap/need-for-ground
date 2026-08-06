/* Internal plumbing shared by the shim translation units. */
#ifndef NFSU2_SHIM_INTERNAL_H
#define NFSU2_SHIM_INTERNAL_H

#include "../shim_dll_macros.h"

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <errno.h>
#include <stddef.h>

/* Kinds of kernel object our HANDLEs can point at. */
enum nfsu2_obj_kind {
    NFSU2_OBJ_FILE = 1,
    NFSU2_OBJ_FIND,
    NFSU2_OBJ_EVENT,
    NFSU2_OBJ_MUTEX,
    NFSU2_OBJ_THREAD,
    NFSU2_OBJ_HEAP,
    NFSU2_OBJ_MAPPING
};

struct nfsu2_object {
    enum nfsu2_obj_kind kind;
    int refs;
};

/* Allocate a zeroed object of `size` bytes with its header filled in. */
void *nfsu2_obj_alloc(enum nfsu2_obj_kind kind, size_t size);
/* Type-checked cast from a HANDLE; returns NULL and sets last error if wrong. */
void *nfsu2_obj_get(HANDLE h, enum nfsu2_obj_kind kind);
/* Drop a reference; calls the per-kind destructor at zero. */
void nfsu2_obj_release(struct nfsu2_object *obj);

/* Per-kind teardown, implemented in the owning translation unit. */
void nfsu2_file_destroy(struct nfsu2_object *obj);
void nfsu2_find_destroy(struct nfsu2_object *obj);
void nfsu2_sync_destroy(struct nfsu2_object *obj);
void nfsu2_thread_destroy(struct nfsu2_object *obj);

/* Thread-handle wait, dispatched from WaitForSingleObject (thread.c). */
DWORD nfsu2_thread_wait(struct nfsu2_object *obj, DWORD timeout_ms);

/* errno -> Win32 error code, and the common "fail with this errno" helper. */
DWORD nfsu2_errno_to_win32(int err);
void nfsu2_set_last_error_from_errno(int err);

/*
 * Path translation (path.c). We keep our own buffer size rather than PATH_MAX
 * because Win32 callers pass MAX_PATH (260) buffers and we need headroom for
 * the game root prefix.
 */
#define PATH_MAX_FALLBACK 4096

/* Resolve and remember the game root; writes the resolved root to `out`. */
int nfsu2_path_set_root(const char *root, char *out, size_t out_size);
void nfsu2_path_reset(void);
const char *nfsu2_path_root(void);

/* Write the registry store back to disk if it changed (advapi32/registry.c). */
void nfsu2_registry_flush(void);

#endif /* NFSU2_SHIM_INTERNAL_H */
