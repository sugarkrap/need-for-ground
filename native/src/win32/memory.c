/*
 * memory.c - virtual memory APIs on mmap/mprotect.
 *
 * Reserve/commit is collapsed: a reservation is mapped PROT_NONE and commit
 * turns on the requested protection. That keeps VirtualAlloc's two-step usage
 * pattern working without tracking a region list, at the cost of not
 * supporting a commit that spans several separate reservations (the game does
 * not do that; its own allocator reserves once and commits within).
 */
#include "shim_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int prot_from_win32(DWORD protect)
{
    switch (protect & 0xff) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    case PAGE_EXECUTE_WRITECOPY: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_READ | PROT_WRITE;
    }
}

static size_t page_round_up(size_t n)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return (n + page - 1) & ~(page - 1);
}

static void *page_align_down(void *p)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return (void *)((uintptr_t)p & ~(uintptr_t)(page - 1));
}

LPVOID WINAPI VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect)
{
    size_t len = page_round_up(size);
    void *p;

    if (size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }

    if ((type & MEM_COMMIT) && addr) {
        /* Commit inside an existing reservation: just relax protection. */
        void *base = page_align_down(addr);
        size_t adjusted = page_round_up(size + ((char *)addr - (char *)base));

        if (mprotect(base, adjusted, prot_from_win32(protect)) != 0) {
            nfsu2_set_last_error_from_errno(errno);
            return NULL;
        }
        return addr;
    }

    p = mmap(addr, len,
             (type & MEM_COMMIT) ? prot_from_win32(protect) : PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p == MAP_FAILED) {
        nfsu2_set_last_error_from_errno(errno);
        return NULL;
    }
    return p;
}

BOOL WINAPI VirtualFree(LPVOID addr, SIZE_T size, DWORD type)
{
    if (!addr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (type & MEM_DECOMMIT) {
        if (mprotect(addr, page_round_up(size), PROT_NONE) != 0) {
            nfsu2_set_last_error_from_errno(errno);
            return FALSE;
        }
        return TRUE;
    }

    /*
     * MEM_RELEASE passes size 0 on Windows, which munmap cannot express. We
     * do not track reservation sizes, so this leaks the mapping rather than
     * unmapping the wrong range. Fine for a process that exits, wrong for
     * anything that cycles allocations - revisit with a region table if that
     * ever shows up.
     */
    if (size == 0) {
        NFSU2_STUB("VirtualFree(MEM_RELEASE) without size: mapping leaked");
        return TRUE;
    }
    if (munmap(addr, page_round_up(size)) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI VirtualProtect(LPVOID addr, SIZE_T size, DWORD protect, PDWORD old_protect)
{
    void *base = page_align_down(addr);
    size_t len = page_round_up(size + ((char *)addr - (char *)base));

    if (mprotect(base, len, prot_from_win32(protect)) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    /* We do not read back the previous protection; report what was asked for
     * so the common save/restore idiom is at least self-consistent. */
    if (old_protect)
        *old_protect = protect;
    return TRUE;
}

SIZE_T WINAPI VirtualQuery(LPCVOID addr, PMEMORY_BASIC_INFORMATION info, SIZE_T len)
{
    if (!info || len < sizeof(*info))
        return 0;
    NFSU2_STUB("VirtualQuery");
    memset(info, 0, sizeof(*info));
    info->BaseAddress = page_align_down((void *)addr);
    info->AllocationBase = info->BaseAddress;
    info->RegionSize = (SIZE_T)sysconf(_SC_PAGESIZE);
    info->State = MEM_COMMIT;
    info->Protect = PAGE_READWRITE;
    info->Type = MEM_PRIVATE;
    return sizeof(*info);
}

/*
 * IsBad*Ptr: these exist in the import list because MSVC's CRT and the game's
 * own defensive code call them. A faithful implementation needs a probe with
 * a SIGSEGV handler; treating any non-NULL pointer as valid matches what
 * these return in practice for live data and avoids installing a handler that
 * would fight with a debugger. Null is the case that actually gets hit.
 */
BOOL WINAPI IsBadReadPtr(LPCVOID ptr, UINT_PTR size)
{
    (void)size;
    return ptr == NULL;
}

BOOL WINAPI IsBadWritePtr(LPVOID ptr, UINT_PTR size)
{
    (void)size;
    return ptr == NULL;
}

BOOL WINAPI IsBadCodePtr(FARPROC ptr)
{
    return ptr == NULL;
}
