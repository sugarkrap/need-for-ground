/* Internal plumbing shared by the shim translation units. */
#ifndef NFSU2_SHIM_INTERNAL_H
#define NFSU2_SHIM_INTERNAL_H

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <errno.h>
#include <stddef.h>

/*
 * Kinds of kernel object our HANDLEs can point at.
 *
 * One rule, learned the hard way: **two object types must never share a kind.**
 * The kind selects the destructor in nfsu2_obj_release, so sharing one means the
 * wrong destructor runs over the wrong struct. toolhelp.c's process snapshot used
 * to be tagged NFSU2_OBJ_FIND on the reasoning that it had "the same lifetime
 * shape and no separate destructor needed" - and CloseHandle on a snapshot then
 * ran the *find* destructor, which read the snapshot's process count as a `char *`
 * and its first pid as a `DIR *` and called closedir(1). It surfaced as a fault
 * inside libc during the game's startup, five frames from anything that explained
 * it. A kind per type, always, even when the type needs no cleanup.
 */
enum nfsu2_obj_kind {
    NFSU2_OBJ_FILE = 1,
    NFSU2_OBJ_FIND,
    NFSU2_OBJ_EVENT,
    NFSU2_OBJ_MUTEX,
    NFSU2_OBJ_THREAD,
    NFSU2_OBJ_HEAP,
    NFSU2_OBJ_MAPPING,
    NFSU2_OBJ_SNAPSHOT
};

struct nfsu2_object {
    enum nfsu2_obj_kind kind;
    int refs;
};

/* Allocate a zeroed object of `size` bytes with its header filled in. */
void *nfsu2_obj_alloc(enum nfsu2_obj_kind kind, size_t size);
/* Was this pointer issued as a handle and not yet closed? The guard in front of
 * every dereference of a HANDLE - see the table in core.c for why. */
int nfsu2_obj_is_live(const void *p);

/* Type-checked cast from a HANDLE; returns NULL and sets last error if wrong. */
void *nfsu2_obj_get(HANDLE h, enum nfsu2_obj_kind kind);
/* Drop a reference; calls the per-kind destructor at zero. */
void nfsu2_obj_release(struct nfsu2_object *obj);

/* Per-kind teardown, implemented in the owning translation unit. */
void nfsu2_file_destroy(struct nfsu2_object *obj);
void nfsu2_find_destroy(struct nfsu2_object *obj);
void nfsu2_sync_destroy(struct nfsu2_object *obj);
void nfsu2_thread_destroy(struct nfsu2_object *obj);
void nfsu2_snapshot_destroy(struct nfsu2_object *obj);

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
/* Host directory for a drive letter (NFSU2_DRIVE_J=...), or NULL for the install
 * drive. The game keeps the drive it was installed from in its own registry. */
const char *nfsu2_path_drive(char letter);
/* Is that mapped drive a CD-ROM (NFSU2_DRIVE_J=cdrom:/mnt/disc)? volume.c answers
 * GetDriveTypeA from this, and the game's media check reads that answer. */
int nfsu2_path_drive_is_cdrom(char letter);

/* The filter set by SetUnhandledExceptionFilter (process.c), for exception.c. */
LPTOP_LEVEL_EXCEPTION_FILTER nfsu2_top_level_filter(void);

/* Write the registry store back to disk if it changed (advapi32/registry.c). */
void nfsu2_registry_flush(void);

/* Underlying descriptor of a file HANDLE, for mmap (file.c -> mapping.c). */
int nfsu2_file_descriptor(HANDLE handle);
void nfsu2_mapping_destroy(struct nfsu2_object *obj);

#endif /* NFSU2_SHIM_INTERNAL_H */
