/*
 * heap.c - Win32 heap APIs on malloc.
 *
 * Every heap the game creates maps onto the same glibc heap; HeapCreate hands
 * back a pseudo-handle that only exists so HeapAlloc has something to
 * validate. That is safe here because the game never relies on
 * HeapDestroy-frees-everything semantics (its own allocator sits on top and
 * pairs alloc/free itself), but it is a real difference worth knowing about
 * if a leak ever gets chased down to this layer.
 */
#include "shim_internal.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* Static pseudo-handles - deliberately not heap objects, so CloseHandle and
 * HeapDestroy on them are harmless no-ops. */
static struct nfsu2_object g_process_heap = { NFSU2_OBJ_HEAP, 1 };

HANDLE WINAPI GetProcessHeap(void)
{
    return (HANDLE)&g_process_heap;
}

HANDLE WINAPI HeapCreate(DWORD flags, SIZE_T initial, SIZE_T max_size)
{
    (void)flags; (void)initial; (void)max_size;
    return (HANDLE)&g_process_heap;
}

BOOL WINAPI HeapDestroy(HANDLE heap)
{
    (void)heap;
    return TRUE;
}

LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T size)
{
    void *p;

    (void)heap;
    /* Win32 allows zero-size allocations that must return a unique pointer. */
    p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
    if (!p)
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID ptr)
{
    (void)heap; (void)flags;
    free(ptr);
    return TRUE;
}

LPVOID WINAPI HeapReAlloc(HANDLE heap, DWORD flags, LPVOID ptr, SIZE_T size)
{
    size_t old_size;
    void *p;

    (void)heap;
    if (!ptr)
        return HeapAlloc(heap, flags, size);

    old_size = malloc_usable_size(ptr);
    p = realloc(ptr, size ? size : 1);
    if (!p) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if ((flags & HEAP_ZERO_MEMORY) && size > old_size)
        memset((char *)p + old_size, 0, size - old_size);
    return p;
}

SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID ptr)
{
    (void)heap; (void)flags;
    if (!ptr)
        return (SIZE_T)-1;
    /*
     * malloc_usable_size is >= the requested size, where Win32 returns
     * exactly what was requested. Callers that only use it to bound a copy
     * are fine; anything that round-trips it as an element count is not.
     */
    return (SIZE_T)malloc_usable_size((void *)ptr);
}

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
    /* GMEM_MOVEABLE handles are not supported: the game only uses GMEM_FIXED
     * (clipboard/DDE paths, where a plain pointer is what it dereferences). */
    if (flags & GMEM_MOVEABLE)
        NFSU2_STUB("GlobalAlloc(GMEM_MOVEABLE)");
    return (flags & GMEM_ZEROINIT) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL mem)
{
    free(mem);
    return NULL;
}

SIZE_T WINAPI GlobalSize(HGLOBAL mem)
{
    return mem ? (SIZE_T)malloc_usable_size(mem) : 0;
}
