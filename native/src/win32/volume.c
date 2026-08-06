/*
 * volume.c - drives and free space.
 *
 * The game asks two questions here: "is there room for a save game" and "what
 * drive letter is the CD in". The first is answered honestly from statvfs.
 *
 * The second used to be answered "there is no CD, and the disc check is gone
 * anyway - that is what tools/unwrap.py is for". Half of that is wrong, and
 * launching the game is what showed it: unwrap.py removes *SafeDisc*, not EA's own
 * media check, which is still in the executable. It reads the drive it was
 * installed from out of its own registry ("CD Drive"="J:\\"), asks
 * GetDriveTypeA about it, and puts up "Please insert Disc 2" when the answer is
 * DRIVE_NO_ROOT_DIR - which is what a letter with nothing mapped reports.
 *
 * So a mapped drive is now reported for what the user says it is
 * (NFSU2_DRIVE_J=cdrom:/mnt/disc), and an unmapped one still reads as absent.
 * Nothing is claimed to exist that does not.
 */
#include "shim_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

DWORD WINAPI GetLogicalDrives(void)
{
    DWORD mask = 1u << 2; /* C:, the install drive */
    char letter;

    /* Plus whatever the user mapped, or a caller scanning the drives would never
     * consider the letter its own registry told it to look at. */
    for (letter = 'A'; letter <= 'Z'; letter++) {
        if (nfsu2_path_drive(letter))
            mask |= 1u << (letter - 'A');
    }
    return mask;
}

UINT WINAPI GetDriveTypeA(LPCSTR root)
{
    if (!root || !*root)
        return DRIVE_FIXED; /* the current directory, i.e. the game root */

    if ((root[0] == 'C' || root[0] == 'c') && root[1] == ':')
        return DRIVE_FIXED;

    if (nfsu2_path_drive(root[0])) {
        UINT type = nfsu2_path_drive_is_cdrom(root[0]) ? DRIVE_CDROM : DRIVE_FIXED;

        nfsu2_shim_trace("GetDriveTypeA(%c:) = %s", root[0],
                         type == DRIVE_CDROM ? "DRIVE_CDROM" : "DRIVE_FIXED");
        return type;
    }

    /*
     * Nothing mapped there. DRIVE_NO_ROOT_DIR is what Windows reports for a letter
     * with nothing mounted, and it is what makes the game's "no disc" path run -
     * which is the honest outcome when no disc has been provided.
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
