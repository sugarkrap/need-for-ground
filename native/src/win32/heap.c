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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* --- what each block was asked for, and a canary in the slack -------------- */

/*
 * Two jobs, one table, and the design constraint is that **the pointer must not
 * move**. An earlier attempt put a header in front of every block: that broke
 * alignment the game had been getting by accident, and it turned a GlobalFree of a
 * HeapAlloc block into free() on an interior pointer. So the size lives beside the
 * block instead of inside it.
 *
 * Job one is correctness. HeapSize used to answer malloc_usable_size, which is *at
 * least* what was asked for; Win32 answers what was asked for. This game round-trips
 * that number - padding allocations by 32 bytes made it fail earlier and
 * consistently, which can only happen if the inflated value is being used.
 *
 * Job two is finding the writer. glibc rounds allocations up, so there are usually a
 * few unused bytes past the request that are nonetheless *usable* - a program that
 * overruns by a little lands there and nothing notices until a much later malloc
 * reports "invalid next size" from somewhere unrelated. Filling that gap with a
 * pattern and checking it on free turns that into a report naming the block, its
 * size, and how far past the end the write went.
 */
#define HEAP_CANARY 0xb7u

struct heap_record {
    void *ptr;              /* NULL: empty. HEAP_TOMBSTONE: erased, keep probing. */
    unsigned int requested;
};

#define HEAP_TOMBSTONE ((void *)(uintptr_t)1)

static struct heap_record *g_records;
static size_t g_capacity;
static size_t g_live;
static size_t g_used_slots;   /* live + tombstones, for the load factor */
static unsigned int g_overruns;
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t record_slot(const void *ptr, size_t capacity)
{
    uintptr_t value = (uintptr_t)ptr >> 4;

    value *= 2654435761u;
    return (size_t)value & (capacity - 1);
}

/* Caller holds the lock. */
static struct heap_record *record_find(const void *ptr)
{
    size_t probe;
    size_t slot;

    if (!g_records || !ptr)
        return NULL;
    slot = record_slot(ptr, g_capacity);
    for (probe = 0; probe < g_capacity; probe++) {
        struct heap_record *record = &g_records[(slot + probe) & (g_capacity - 1)];

        if (!record->ptr)
            return NULL;
        if (record->ptr == ptr)
            return record;
    }
    return NULL;
}

static void record_put(void *ptr, unsigned int requested);

/* Caller holds the lock. Doubling, because a game allocates in the hundreds of
 * thousands and a fixed table would either waste memory or stop recording. */
static void record_grow(void)
{
    struct heap_record *old = g_records;
    size_t old_capacity = g_capacity;
    size_t i;

    g_capacity = old_capacity ? old_capacity * 2 : 4096;
    g_records = calloc(g_capacity, sizeof(*g_records));
    if (!g_records) {
        /* Out of memory for bookkeeping: keep the old table rather than lose it. */
        g_records = old;
        g_capacity = old_capacity;
        return;
    }
    g_live = 0;
    g_used_slots = 0;
    for (i = 0; i < old_capacity; i++) {
        if (old[i].ptr && old[i].ptr != HEAP_TOMBSTONE)
            record_put(old[i].ptr, old[i].requested);
    }
    free(old);
}

/* Caller holds the lock. */
static void record_put(void *ptr, unsigned int requested)
{
    size_t probe;
    size_t slot;

    if (!g_records || (g_used_slots + 1) * 4 > g_capacity * 3)
        record_grow();
    if (!g_records)
        return;

    slot = record_slot(ptr, g_capacity);
    for (probe = 0; probe < g_capacity; probe++) {
        struct heap_record *record = &g_records[(slot + probe) & (g_capacity - 1)];

        if (record->ptr == ptr) {           /* reused address */
            record->requested = requested;
            return;
        }
        if (!record->ptr || record->ptr == HEAP_TOMBSTONE) {
            if (!record->ptr)
                g_used_slots++;
            record->ptr = ptr;
            record->requested = requested;
            g_live++;
            return;
        }
    }
}

static void canary_fill(void *ptr, size_t requested)
{
    size_t usable = malloc_usable_size(ptr);

    if (usable > requested)
        memset((char *)ptr + requested, HEAP_CANARY, usable - requested);
}

/*
 * Check the pattern and say exactly what was found. `api` is where the block was
 * being handed back, which is the closest thing to a culprit this can offer - the
 * write itself happened at some earlier moment in the game's own code.
 */
static void canary_check(const char *api, void *ptr, size_t requested)
{
    size_t usable = malloc_usable_size(ptr);
    const unsigned char *slack = (const unsigned char *)ptr + requested;
    size_t i;

    for (i = 0; requested + i < usable; i++) {
        if (slack[i] == HEAP_CANARY)
            continue;
        g_overruns++;
        fprintf(stderr,
                "\n[nfsu2] HEAP OVERRUN: %zu-byte block at %p was written %zu byte(s) "
                "past its end\n"
                "        (byte %zu of %zu unused: 0x%02x, expected 0x%02x) - seen at %s\n",
                requested, ptr, i + 1, i, usable - requested, slack[i], HEAP_CANARY, api);
        return;
    }
}

/* Record a fresh allocation. NULL is passed through so callers stay simple. */
static void *heap_track(void *ptr, size_t requested)
{
    if (!ptr)
        return NULL;
    pthread_mutex_lock(&g_heap_lock);
    record_put(ptr, (unsigned int)requested);
    canary_fill(ptr, requested);
    pthread_mutex_unlock(&g_heap_lock);
    return ptr;
}

/*
 * Stop tracking, checking the canary on the way out. Returns the requested size, or
 * (size_t)-1 if the block is not one of ours - which is not an error: VirtualAlloc
 * hands out mmap'd pages, and nothing prevents the game from crossing families.
 */
static size_t heap_untrack(const char *api, void *ptr)
{
    struct heap_record *record;
    size_t requested = (size_t)-1;

    if (!ptr)
        return requested;
    pthread_mutex_lock(&g_heap_lock);
    record = record_find(ptr);
    if (record) {
        requested = record->requested;
        canary_check(api, ptr, requested);
        record->ptr = HEAP_TOMBSTONE;
        g_live--;
    }
    pthread_mutex_unlock(&g_heap_lock);
    return requested;
}

unsigned int nfsu2_heap_overruns(void)
{
    return g_overruns;
}

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
    size_t wanted = size ? (size_t)size : 1; /* Win32 allows 0, with a unique pointer */

    (void)heap;
    p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, wanted) : malloc(wanted);
    if (!p) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return heap_track(p, wanted);
}

BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID ptr)
{
    (void)heap; (void)flags;
    heap_untrack("HeapFree", ptr);
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

    {
        size_t wanted = size ? (size_t)size : 1;
        size_t tracked = heap_untrack("HeapReAlloc", ptr);

        /* The tracked request is the honest old size; malloc_usable_size is the
         * fallback for a block that came from somewhere else. */
        old_size = (tracked != (size_t)-1) ? tracked : malloc_usable_size(ptr);
        p = realloc(ptr, wanted);
        if (!p) {
            /* The block survives; put it back so it is still tracked. */
            heap_track(ptr, old_size);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
        if ((flags & HEAP_ZERO_MEMORY) && wanted > old_size)
            memset((char *)p + old_size, 0, wanted - old_size);
        return heap_track(p, wanted);
    }
}

SIZE_T WINAPI HeapSize(HANDLE heap, DWORD flags, LPCVOID ptr)
{
    struct heap_record *record;
    SIZE_T result;

    (void)heap; (void)flags;
    if (!ptr)
        return (SIZE_T)-1;

    /* What was asked for, as Win32 answers - not malloc_usable_size, which is
     * larger by a variable amount and which this game round-trips. */
    pthread_mutex_lock(&g_heap_lock);
    record = record_find(ptr);
    result = record ? (SIZE_T)record->requested
                    : (SIZE_T)malloc_usable_size((void *)ptr);
    pthread_mutex_unlock(&g_heap_lock);
    return result;
}

HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T size)
{
    /* GMEM_MOVEABLE handles are not supported: the game only uses GMEM_FIXED
     * (clipboard/DDE paths, where a plain pointer is what it dereferences). */
    if (flags & GMEM_MOVEABLE)
        NFSU2_STUB("GlobalAlloc(GMEM_MOVEABLE)");
    /* Through HeapAlloc, so the two families share one view of every block: on
     * Windows both come from the process heap, and code from 2004 frees across them.
     * Nothing moves, so this is only bookkeeping. */
    return HeapAlloc(NULL, (flags & GMEM_ZEROINIT) ? HEAP_ZERO_MEMORY : 0, size);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL mem)
{
    HeapFree(NULL, 0, mem);
    return NULL;
}

SIZE_T WINAPI GlobalSize(HGLOBAL mem)
{
    return mem ? HeapSize(NULL, 0, mem) : 0;
}
