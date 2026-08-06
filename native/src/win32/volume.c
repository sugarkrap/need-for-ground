/*
 * volume.c - drives and free space.
 *
 * The game asks two questions here: "is there room for a save game" and "what
 * drive letter is the CD in". The first is answered honestly from statvfs. The
 * second has no answer on Linux and does not need one - the disc check is gone
 * (that is what tools/unwrap.py is for), and the data files are resolved through
 * the game root, so a drive-letter probe should find exactly one drive: C:,
 * fixed, which is the game root.
 */
#include "shim_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

DWORD WINAPI GetLogicalDrives(void)
{
    /* Bit 2 is C:. Reporting only C: means a caller scanning for a CD-ROM
     * finds none, which is the truth here and the answer that makes its
     * "no disc" path run instead of a wrong-drive path. */
    return 1u << 2;
}

UINT WINAPI GetDriveTypeA(LPCSTR root)
{
    if (!root || !*root)
        return DRIVE_FIXED; /* the current directory, i.e. the game root */

    if ((root[0] == 'C' || root[0] == 'c') && root[1] == ':')
        return DRIVE_FIXED;

    /*
     * Deliberately not DRIVE_CDROM for any letter: claiming a CD-ROM exists
     * would send the game looking for a disc it cannot find. DRIVE_NO_ROOT_DIR
     * is what Windows reports for a letter with nothing mounted.
     */
    return DRIVE_NO_ROOT_DIR;
}

BOOL WINAPI GetDiskFreeSpaceA(LPCSTR root, LPDWORD sectors_per_cluster,
                              LPDWORD bytes_per_sector, LPDWORD free_clusters,
                              LPDWORD total_clusters)
{
    struct statvfs stat;
    const char *target = nfsu2_path_root();
    unsigned long long block_size;

    (void)root; /* every path here lives under the game root */

    if (statvfs(target, &stat) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }

    block_size = stat.f_frsize ? stat.f_frsize : stat.f_bsize;

    /*
     * Win32 reports free space as a product of four values that must not
     * overflow a DWORD. Presenting one filesystem block as one 512-byte-sector
     * cluster keeps the cluster count honest for volumes up to 2TB, which is
     * where a DWORD of clusters runs out anyway.
     */
    if (bytes_per_sector)
        *bytes_per_sector = 512;
    if (sectors_per_cluster)
        *sectors_per_cluster = (DWORD)(block_size / 512 ? block_size / 512 : 1);
    if (free_clusters)
        *free_clusters = (DWORD)(stat.f_bavail > 0xffffffffu ? 0xffffffffu : stat.f_bavail);
    if (total_clusters)
        *total_clusters = (DWORD)(stat.f_blocks > 0xffffffffu ? 0xffffffffu : stat.f_blocks);
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR directory, PULARGE_INTEGER free_to_caller,
                                PULARGE_INTEGER total, PULARGE_INTEGER free_total)
{
    struct statvfs stat;
    const char *target = nfsu2_path_root();
    unsigned long long block_size;

    (void)directory;

    if (statvfs(target, &stat) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    block_size = stat.f_frsize ? stat.f_frsize : stat.f_bsize;

    /* 64-bit, so no clamping is needed - this is the call to prefer. */
    if (free_to_caller)
        free_to_caller->QuadPart = (ULONGLONG)stat.f_bavail * block_size;
    if (total)
        total->QuadPart = (ULONGLONG)stat.f_blocks * block_size;
    if (free_total)
        free_total->QuadPart = (ULONGLONG)stat.f_bfree * block_size;
    return TRUE;
}

DWORD WINAPI GetLongPathNameA(LPCSTR short_path, LPSTR long_path, DWORD size)
{
    char host[PATH_MAX_FALLBACK];
    DWORD needed;

    if (!short_path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    /*
     * There are no 8.3 short names to expand, but the case-insensitive path
     * resolver *is* the right thing to run here: it turns whatever case the
     * caller has into what is actually on disk, which is the same "give me the
     * canonical spelling" service this API provides on Windows.
     */
    if (nfsu2_path_to_host(short_path, host, sizeof(host)) != 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    needed = (DWORD)strlen(host) + 1;
    if (!long_path || size < needed)
        return needed;
    memcpy(long_path, host, needed);
    return needed - 1;
}
