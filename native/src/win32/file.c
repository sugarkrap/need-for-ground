/*
 * file.c - kernel32 file I/O on top of POSIX.
 *
 * Scope is what the game imports (analysis/win32_imports.txt): synchronous
 * reads/writes, seeks, attributes, directory enumeration. No overlapped I/O -
 * the import list has GetOverlappedResult but only the serial-port code path
 * uses it, which we do not implement.
 */
#include "shim_internal.h"

#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct nfsu2_file {
    struct nfsu2_object obj;
    int fd;
    char *host_path;
};

struct nfsu2_find {
    struct nfsu2_object obj;
    char *dir;      /* host directory being enumerated */
    char *pattern;  /* Windows wildcard, matched case-insensitively */
    DIR *dir_handle;
};

#define UNIX_TO_FILETIME_OFFSET 116444736000000000ULL

static void time_t_to_filetime(time_t t, FILETIME *out)
{
    unsigned long long ticks = (unsigned long long)t * 10000000ULL + UNIX_TO_FILETIME_OFFSET;
    out->dwLowDateTime = (DWORD)(ticks & 0xffffffffU);
    out->dwHighDateTime = (DWORD)(ticks >> 32);
}

static DWORD attrs_from_stat(const struct stat *st)
{
    DWORD attrs = 0;

    if (S_ISDIR(st->st_mode))
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & S_IWUSR))
        attrs |= FILE_ATTRIBUTE_READONLY;
    if (attrs == 0)
        attrs = FILE_ATTRIBUTE_NORMAL;
    return attrs;
}

void nfsu2_file_destroy(struct nfsu2_object *obj)
{
    struct nfsu2_file *f = (struct nfsu2_file *)obj;

    if (f->fd >= 0)
        close(f->fd);
    free(f->host_path);
    free(f);
}

void nfsu2_find_destroy(struct nfsu2_object *obj)
{
    struct nfsu2_find *find = (struct nfsu2_find *)obj;

    if (find->dir_handle)
        closedir(find->dir_handle);
    free(find->dir);
    free(find->pattern);
    free(find);
}

/* mapping.c needs the underlying descriptor to mmap; nothing else should. */
int nfsu2_file_descriptor(HANDLE handle)
{
    struct nfsu2_file *f = nfsu2_obj_get(handle, NFSU2_OBJ_FILE);

    return f ? f->fd : -1;
}

HANDLE WINAPI CreateFileA(LPCSTR name, DWORD access, DWORD share,
                          LPSECURITY_ATTRIBUTES sa, DWORD creation,
                          DWORD flags, HANDLE template_file)
{
    char host[PATH_MAX_FALLBACK];
    struct nfsu2_file *f;
    int oflags = 0;
    int fd;
    int rc;

    (void)share; (void)sa; (void)flags; (void)template_file;

    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    rc = nfsu2_path_to_host(name, host, sizeof(host));
    if (rc != 0) {
        nfsu2_set_last_error_from_errno(-rc);
        return INVALID_HANDLE_VALUE;
    }

    if ((access & GENERIC_WRITE) && (access & GENERIC_READ))
        oflags = O_RDWR;
    else if (access & GENERIC_WRITE)
        oflags = O_WRONLY;
    else
        oflags = O_RDONLY;

    switch (creation) {
    case CREATE_NEW:        oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS:     oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     break;
    case OPEN_ALWAYS:       oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    fd = open(host, oflags, 0644);
    if (fd < 0) {
        nfsu2_shim_trace("CreateFileA(%s -> %s) failed: %s", name, host, strerror(errno));
        nfsu2_set_last_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }

    f = nfsu2_obj_alloc(NFSU2_OBJ_FILE, sizeof(*f));
    if (!f) {
        close(fd);
        return INVALID_HANDLE_VALUE;
    }
    f->fd = fd;
    f->host_path = strdup(host);
    /* Successes are traced, not just failures. A run where the game opened
     * nothing at all used to be indistinguishable from a run where the tracing
     * simply did not cover opens - see the note on GetCurrentDirectoryA. */
    nfsu2_shim_trace("CreateFileA(%s -> %s) = %p", name, host, (void *)f);
    SetLastError(ERROR_SUCCESS);
    return (HANDLE)f;
}

HANDLE WINAPI CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                          LPSECURITY_ATTRIBUTES sa, DWORD creation,
                          DWORD flags, HANDLE template_file)
{
    char narrow[PATH_MAX_FALLBACK];

    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    /* Narrowed and handed to the A version: paths reach the filesystem as bytes
     * either way, and the whole path-resolution layer works in bytes. */
    if (!WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), NULL, NULL)) {
        SetLastError(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileA(narrow, access, share, sa, creation, flags, template_file);
}

BOOL WINAPI ReadFile(HANDLE h, LPVOID buf, DWORD count, LPDWORD read_out, LPOVERLAPPED ov)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    ssize_t total = 0;

    if (!f)
        return FALSE;
    if (ov) {
        NFSU2_STUB("ReadFile with OVERLAPPED");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    while ((DWORD)total < count) {
        ssize_t n = read(f->fd, (char *)buf + total, count - (DWORD)total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            nfsu2_set_last_error_from_errno(errno);
            return FALSE;
        }
        if (n == 0)
            break; /* EOF: Win32 reports success with a short count */
        total += n;
    }

    if (read_out)
        *read_out = (DWORD)total;
    /*
     * A short read is reported loudly. Win32 calls it success with a smaller
     * count, and this game does not always check: it loads structured data into
     * a buffer and then walks counts out of that buffer's header, so a partial
     * read becomes a wild pointer walk somewhere else entirely.
     */
    if ((DWORD)total < count)
        nfsu2_shim_trace("ReadFile(%s): SHORT READ - asked for %lu, got %ld",
                         f->host_path ? f->host_path : "?",
                         (unsigned long)count, (long)total);
    return TRUE;
}

BOOL WINAPI WriteFile(HANDLE h, LPCVOID buf, DWORD count, LPDWORD written_out, LPOVERLAPPED ov)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    ssize_t total = 0;

    if (!f)
        return FALSE;
    if (ov) {
        NFSU2_STUB("WriteFile with OVERLAPPED");
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    while ((DWORD)total < count) {
        ssize_t n = write(f->fd, (const char *)buf + total, count - (DWORD)total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            nfsu2_set_last_error_from_errno(errno);
            return FALSE;
        }
        total += n;
    }

    if (written_out)
        *written_out = (DWORD)total;
    return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE h, LONG distance, PLONG distance_high, DWORD method)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    long long offset = distance;
    int whence;
    off_t pos;

    if (!f)
        return INVALID_SET_FILE_POINTER;

    if (distance_high)
        offset = ((long long)*distance_high << 32) | (unsigned int)distance;

    switch (method) {
    case FILE_BEGIN:   whence = SEEK_SET; break;
    case FILE_CURRENT: whence = SEEK_CUR; break;
    case FILE_END:     whence = SEEK_END; break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_SET_FILE_POINTER;
    }

    pos = lseek(f->fd, (off_t)offset, whence);
    if (pos == (off_t)-1) {
        nfsu2_set_last_error_from_errno(errno);
        return INVALID_SET_FILE_POINTER;
    }

    if (distance_high)
        *distance_high = (LONG)((unsigned long long)pos >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)((unsigned long long)pos & 0xffffffffU);
}

DWORD WINAPI GetFileSize(HANDLE h, LPDWORD size_high)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    struct stat st;

    if (!f)
        return INVALID_FILE_SIZE;
    if (fstat(f->fd, &st) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return INVALID_FILE_SIZE;
    }
    if (size_high)
        *size_high = (DWORD)((unsigned long long)st.st_size >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)((unsigned long long)st.st_size & 0xffffffffU);
}

DWORD WINAPI GetFileType(HANDLE h)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    struct stat st;

    if (!f)
        return FILE_TYPE_UNKNOWN;
    if (fstat(f->fd, &st) != 0)
        return FILE_TYPE_UNKNOWN;
    if (S_ISCHR(st.st_mode))
        return FILE_TYPE_CHAR;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return FILE_TYPE_PIPE;
    return FILE_TYPE_DISK;
}

BOOL WINAPI SetEndOfFile(HANDLE h)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);
    off_t pos;

    if (!f)
        return FALSE;
    pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos == (off_t)-1 || ftruncate(f->fd, pos) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI FlushFileBuffers(HANDLE h)
{
    struct nfsu2_file *f = nfsu2_obj_get(h, NFSU2_OBJ_FILE);

    if (!f)
        return FALSE;
    if (fsync(f->fd) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI GetFileAttributesA(LPCSTR name)
{
    char host[PATH_MAX_FALLBACK];
    struct stat st;
    int rc;

    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    rc = nfsu2_path_to_host(name, host, sizeof(host));
    if (rc != 0 || stat(host, &st) != 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attrs_from_stat(&st);
}

BOOL WINAPI DeleteFileA(LPCSTR name)
{
    char host[PATH_MAX_FALLBACK];
    int rc = name ? nfsu2_path_to_host(name, host, sizeof(host)) : -EINVAL;

    if (rc != 0) {
        nfsu2_set_last_error_from_errno(-rc);
        return FALSE;
    }
    if (unlink(host) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI MoveFileA(LPCSTR from, LPCSTR to)
{
    char host_from[PATH_MAX_FALLBACK];
    char host_to[PATH_MAX_FALLBACK];

    if (!from || !to) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (nfsu2_path_to_host(from, host_from, sizeof(host_from)) != 0 ||
        nfsu2_path_to_host(to, host_to, sizeof(host_to)) != 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    if (rename(host_from, host_to) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
    char host[PATH_MAX_FALLBACK];

    (void)sa;
    if (!name || nfsu2_path_to_host(name, host, sizeof(host)) != 0) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (mkdir(host, 0755) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR name)
{
    char host[PATH_MAX_FALLBACK];

    if (!name || nfsu2_path_to_host(name, host, sizeof(host)) != 0) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (rmdir(host) != 0) {
        nfsu2_set_last_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

/* --- directory enumeration --------------------------------------------- */

static BOOL fill_find_data(struct nfsu2_find *find, const char *name, WIN32_FIND_DATAA *data)
{
    char probe[PATH_MAX_FALLBACK];
    struct stat st;

    snprintf(probe, sizeof(probe), "%s/%s", find->dir, name);
    if (stat(probe, &st) != 0)
        return FALSE;

    memset(data, 0, sizeof(*data));
    data->dwFileAttributes = attrs_from_stat(&st);
    time_t_to_filetime(st.st_ctime, &data->ftCreationTime);
    time_t_to_filetime(st.st_atime, &data->ftLastAccessTime);
    time_t_to_filetime(st.st_mtime, &data->ftLastWriteTime);
    data->nFileSizeLow = (DWORD)((unsigned long long)st.st_size & 0xffffffffU);
    data->nFileSizeHigh = (DWORD)((unsigned long long)st.st_size >> 32);
    snprintf(data->cFileName, sizeof(data->cFileName), "%s", name);
    return TRUE;
}

static BOOL find_advance(struct nfsu2_find *find, WIN32_FIND_DATAA *data)
{
    struct dirent *ent;

    while ((ent = readdir(find->dir_handle)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (fnmatch(find->pattern, ent->d_name, FNM_CASEFOLD) != 0)
            continue;
        if (fill_find_data(find, ent->d_name, data))
            return TRUE;
    }
    SetLastError(ERROR_NO_MORE_FILES);
    return FALSE;
}

HANDLE WINAPI FindFirstFileA(LPCSTR spec, WIN32_FIND_DATAA *data)
{
    char host[PATH_MAX_FALLBACK];
    char dir[PATH_MAX_FALLBACK];
    const char *pattern;
    struct nfsu2_find *find;
    char *slash;
    int rc;

    if (!spec || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }

    /*
     * The wildcard component must not go through case-insensitive resolution
     * (there is no such directory entry), so translate the directory part
     * only and keep the pattern as given.
     */
    rc = nfsu2_path_to_host(spec, host, sizeof(host));
    if (rc != 0) {
        nfsu2_set_last_error_from_errno(-rc);
        return INVALID_HANDLE_VALUE;
    }
    snprintf(dir, sizeof(dir), "%s", host);
    slash = strrchr(dir, '/');
    if (!slash) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    *slash = '\0';
    pattern = slash + 1;

    find = nfsu2_obj_alloc(NFSU2_OBJ_FIND, sizeof(*find));
    if (!find)
        return INVALID_HANDLE_VALUE;

    find->dir = strdup(dir);
    find->pattern = strdup(pattern);
    find->dir_handle = opendir(dir);
    if (!find->dir || !find->pattern || !find->dir_handle) {
        nfsu2_set_last_error_from_errno(errno);
        nfsu2_obj_release(&find->obj);
        return INVALID_HANDLE_VALUE;
    }

    if (!find_advance(find, data)) {
        nfsu2_obj_release(&find->obj);
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)find;
}

BOOL WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data)
{
    struct nfsu2_find *find = nfsu2_obj_get(h, NFSU2_OBJ_FIND);

    if (!find || !data)
        return FALSE;
    return find_advance(find, data);
}

BOOL WINAPI FindClose(HANDLE h)
{
    struct nfsu2_find *find = nfsu2_obj_get(h, NFSU2_OBJ_FIND);

    if (!find)
        return FALSE;
    nfsu2_obj_release(&find->obj);
    return TRUE;
}

/* --- current / full paths ---------------------------------------------- */

/*
 * The drive-qualified spelling of the game root. This has to carry a drive
 * letter, and the reason is worth spelling out, because it cost a long hunt.
 *
 * The game's file layer is a virtual filesystem with one *device* per drive
 * letter reported by GetLogicalDrives, and it resolves which device to use by
 * taking the prefix up to the ':' of the path it is given. Its search path is
 * whatever GetCurrentDirectoryA returns (FUN_006f791a). This function used to
 * return the host path - "/mnt/games/..." early on, and "." once the root went
 * unset - and neither names a device, so every lookup fell back to the abstract
 * base device, whose open method is literally `or eax,-1; ret 0xc`. No Win32
 * call was ever made, so a trace of the run showed no file I/O at all and it
 * looked as though the game had simply not asked for its data.
 *
 * It had. What followed was worse than a failed load: the game requests its
 * memory files asynchronously into a buffer its allocator fills with 0xee, and
 * the completion handler (FUN_005793c0) relocates the header's record array
 * *without checking the failure flag it is handed*. With the buffer still
 * poison, the record count read as 0x44443333, and the handler wrote a pointer
 * every 20 bytes across the heap until it ran off the end - corrupting glibc's
 * arena on the way and then faulting. The heap corruption chased through
 * canaries and guard pages was this, one indirection removed.
 *
 * "C:" is the honest answer under this shim's own drive mapping: path.c already
 * resolves an unmapped drive letter to the game root, and volume.c already
 * reports C: as the install drive, so C:\ *is* where the game is installed as
 * far as anything here is concerned.
 */
#define NFSU2_ROOT_WINDOWS_PATH "C:\\"

UINT WINAPI GetCurrentDirectoryA(UINT size, LPSTR buf)
{
    static const char root[] = NFSU2_ROOT_WINDOWS_PATH;
    UINT need = (UINT)sizeof(root); /* includes the terminator */

    /* Win32: too small returns the size *with* the terminator and writes
     * nothing; success returns the length without it. The game passes a 512-byte
     * stack buffer, but the contract is what callers rely on. */
    if (!buf || size < need)
        return need;
    memcpy(buf, root, need);
    nfsu2_shim_trace("GetCurrentDirectoryA = %s (the game root; its file system "
                     "resolves a device from the drive letter)", root);
    return need - 1;
}

DWORD WINAPI GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buf, LPSTR *file_part)
{
    char host[PATH_MAX_FALLBACK];
    DWORD need;

    if (!name || nfsu2_path_to_host(name, host, sizeof(host)) != 0) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    need = (DWORD)strlen(host) + 1;
    if (!buf || size < need)
        return need;
    memcpy(buf, host, need);
    if (file_part) {
        char *slash = strrchr(buf, '/');
        *file_part = slash ? slash + 1 : buf;
    }
    return need - 1;
}
