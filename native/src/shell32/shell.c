/*
 * shell.c - the two shell32 entry points the game uses.
 *
 * SHGetFolderPathA is the one that matters: it is how the game finds "My
 * Documents" to put save games in. Mapping it onto the XDG user directories
 * rather than inventing a path under the game root means saves land where a
 * Linux user expects to find them - and where a backup tool already looks.
 */
#include "../win32/shim_internal.h"

#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *home_dir(void)
{
    const char *home = getenv("HOME");

    return (home && *home) ? home : "/tmp";
}

/* $XDG_DOCUMENTS_DIR, or the conventional fallback. */
static void documents_dir(char *out, size_t size)
{
    const char *xdg = getenv("XDG_DOCUMENTS_DIR");

    if (xdg && *xdg)
        snprintf(out, size, "%s", xdg);
    else
        snprintf(out, size, "%s/Documents", home_dir());
}

HRESULT WINAPI SHGetFolderPathA(HWND owner, int folder, HANDLE token, DWORD flags, LPSTR path)
{
    char resolved[PATH_MAX_FALLBACK];
    int create = (folder & CSIDL_FLAG_CREATE) != 0;

    (void)owner; (void)token; (void)flags;

    if (!path)
        return E_INVALIDARG;
    folder &= 0xff; /* strip CSIDL_FLAG_* */

    switch (folder) {
    case CSIDL_PERSONAL:          /* My Documents - where saves go */
    case CSIDL_MYVIDEO:
    case CSIDL_MYPICTURES:
    case CSIDL_MYMUSIC:
        documents_dir(resolved, sizeof(resolved));
        break;
    case CSIDL_APPDATA:
    case CSIDL_LOCAL_APPDATA:
    case CSIDL_COMMON_APPDATA: {
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg)
            snprintf(resolved, sizeof(resolved), "%s", xdg);
        else
            snprintf(resolved, sizeof(resolved), "%s/.local/share", home_dir());
        break;
    }
    case CSIDL_PROFILE:
        snprintf(resolved, sizeof(resolved), "%s", home_dir());
        break;
    case CSIDL_PROGRAM_FILES:
    case CSIDL_PROGRAM_FILESX86:
        /* Nothing installs anything; the game's own directory is the only
         * meaningful answer. */
        snprintf(resolved, sizeof(resolved), "%s", nfsu2_path_root());
        break;
    default:
        nfsu2_shim_trace("SHGetFolderPathA(CSIDL %d): unhandled", folder);
        return E_INVALIDARG;
    }

    /*
     * CSIDL_FLAG_CREATE asks for the directory to be created. My Documents is also
     * created without being asked, because on Windows it always exists - and the
     * game's save directory goes inside it, so a missing parent turns into "unable
     * to save" with nothing pointing at the cause.
     */
    if (create || folder == CSIDL_PERSONAL)
        mkdir(resolved, 0755); /* may already exist; that is fine */

    /* MAX_PATH is the documented buffer size for this API. */
    if (strlen(resolved) + 1 > MAX_PATH) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return E_FAIL;
    }
    memcpy(path, resolved, strlen(resolved) + 1);
    return S_OK;
}

HINSTANCE WINAPI ShellExecuteA(HWND owner, LPCSTR operation, LPCSTR file, LPCSTR parameters,
                               LPCSTR directory, INT show)
{
    (void)owner; (void)operation; (void)parameters; (void)directory; (void)show;

    /*
     * The game uses this to open its manual or an EA web page. Launching a
     * browser from a game process is the kind of thing that should be a
     * deliberate decision rather than a side effect of a port, so it is not
     * done - and the failure is reported the way Windows reports "no
     * association", which is a case the caller already has to handle.
     */
    nfsu2_shim_trace("ShellExecuteA(%s): not launching external applications",
                     file ? file : "(null)");
    SetLastError(ERROR_NO_ASSOCIATION);
    return (HINSTANCE)(UINT_PTR)SE_ERR_NOASSOC;
}
