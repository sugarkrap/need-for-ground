/*
 * mapping.c - file mapping (CreateFileMapping / MapViewOfFile) on mmap.
 *
 * Implemented for real rather than stubbed: memory-mapping is how the game
 * streams its big .BUN/.VIV archives, so a stub would fail at load time rather
 * than degrade.
 */
#include "shim_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct nfsu2_mapping {
    struct nfsu2_object obj;
    int fd;         /* dup of the file's descriptor, or -1 for anonymous */
    size_t size;
    int prot;
};

/* Views have to be unmappable from just the base pointer that MapViewOfFile
 * returned, since that is all UnmapViewOfFile gets - hence this table rather
 * than a length stored alongside the caller's pointer. */
#define MAX_VIEWS 32
static struct {
    void *base;
    size_t size;
} g_views[MAX_VIEWS];

void nfsu2_mapping_destroy(struct nfsu2_object *obj)
{
    struct nfsu2_mapping *mapping = (struct nfsu2_mapping *)obj;

    if (mapping->fd >= 0)
        close(mapping->fd);
    free(mapping);
}

HANDLE WINAPI CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                                 DWORD size_high, DWORD size_low, LPCSTR name)
{
    struct nfsu2_mapping *mapping;
    int fd = -1;
    /* Composed in 64 bits then narrowed: size_t is 32 bits at the real target, so
     * shifting into it would be undefined. A mapping bigger than the address
     * space cannot be created anyway - the mmap below fails cleanly. */
    unsigned long long requested = ((unsigned long long)size_high << 32) | size_low;
    size_t size = (size_t)requested;

    (void)sa;
    if (requested > (unsigned long long)(size_t)-1) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (name)
        NFSU2_STUB("CreateFileMappingA with a name (mappings are process-local here)");

    if (file != INVALID_HANDLE_VALUE && file != NULL) {
        int source = nfsu2_file_descriptor(file);

        if (source < 0) {
            SetLastError(ERROR_INVALID_HANDLE);
            return NULL;
        }
        /* Duplicated so the mapping outlives a CloseHandle on the file, as on
         * Windows. */
        fd = dup(source);
        if (fd < 0) {
            nfsu2_set_last_error_from_errno(errno);
            return NULL;
        }
        if (size == 0) {
            off_t end = lseek(fd, 0, SEEK_END);
            if (end < 0) {
                nfsu2_set_last_error_from_errno(errno);
                close(fd);
                return NULL;
            }
            size = (size_t)end;
        }
    } else if (size == 0) {
        /* An anonymous mapping with no size has nothing to map. */
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    mapping = nfsu2_obj_alloc(NFSU2_OBJ_MAPPING, sizeof(*mapping));
    if (!mapping) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    mapping->fd = fd;
    mapping->size = size;
    mapping->prot = (protect == PAGE_READONLY) ? PROT_READ : (PROT_READ | PROT_WRITE);
    return (HANDLE)mapping;
}

LPVOID WINAPI MapViewOfFile(HANDLE handle, DWORD access, DWORD offset_high, DWORD offset_low,
                            SIZE_T size)
{
    struct nfsu2_mapping *mapping = nfsu2_obj_get(handle, NFSU2_OBJ_MAPPING);
    off_t offset = (off_t)(((unsigned long long)offset_high << 32) | offset_low);
    int prot;
    void *base;
    int i;

    if (!mapping)
        return NULL;

    prot = (access & (FILE_MAP_WRITE | FILE_MAP_ALL_ACCESS)) ? (PROT_READ | PROT_WRITE)
                                                            : PROT_READ;
    if (size == 0)
        size = mapping->size - (size_t)offset;

    base = mmap(NULL, size, prot,
                (access & FILE_MAP_COPY) ? MAP_PRIVATE : MAP_SHARED,
                mapping->fd, offset);
    if (base == MAP_FAILED) {
        nfsu2_set_last_error_from_errno(errno);
        return NULL;
    }

    for (i = 0; i < MAX_VIEWS; i++) {
        if (!g_views[i].base) {
            g_views[i].base = base;
            g_views[i].size = size;
            return base;
        }
    }
    /* No slot to remember the length, so UnmapViewOfFile could not unmap it
     * later. Better to fail here than to leak silently on every view. */
    nfsu2_shim_trace("MapViewOfFile: view table full (%d)", MAX_VIEWS);
    munmap(base, size);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return NULL;
}

BOOL WINAPI UnmapViewOfFile(LPCVOID base)
{
    int i;

    for (i = 0; i < MAX_VIEWS; i++) {
        if (g_views[i].base == base) {
            int rc = munmap(g_views[i].base, g_views[i].size);

            g_views[i].base = NULL;
            g_views[i].size = 0;
            if (rc != 0) {
                nfsu2_set_last_error_from_errno(errno);
                return FALSE;
            }
            return TRUE;
        }
    }
    SetLastError(ERROR_INVALID_ADDRESS);
    return FALSE;
}

BOOL WINAPI FlushViewOfFile(LPCVOID base, SIZE_T size)
{
    int i;

    for (i = 0; i < MAX_VIEWS; i++) {
        if (g_views[i].base == base) {
            size_t length = size ? size : g_views[i].size;

            if (msync(g_views[i].base, length, MS_SYNC) != 0) {
                nfsu2_set_last_error_from_errno(errno);
                return FALSE;
            }
            return TRUE;
        }
    }
    SetLastError(ERROR_INVALID_ADDRESS);
    return FALSE;
}
