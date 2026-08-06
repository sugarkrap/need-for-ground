/*
 * module.c - LoadLibrary/GetProcAddress mapped onto the dynamic linker.
 *
 * The interesting case is d3d9.dll: the game calls
 * LoadLibraryA("d3d9.dll") + GetProcAddress("Direct3DCreate9") in some code
 * paths and imports it directly in others. Both end up at the same place -
 * DXVK Native's libdxvk_d3d9.so, which is already linked into the executable,
 * so the "handle" is just RTLD_DEFAULT.
 *
 * GetProcAddress for kernel32/user32 names resolves against our own shim
 * symbols via dlsym(RTLD_DEFAULT, ...). That requires the executable to be
 * linked with --export-dynamic (see native/meson.build).
 *
 * Note the ABI hazard: a function found this way is used by the caller with
 * the convention its prototype declares, which for Win32 imports is WINAPI
 * (stdcall). Our shims are defined WINAPI, so that matches. Anything resolved
 * out of a plain Linux .so (i.e. the DXVK exports) is cdecl, which is why
 * d3d9_native.h declares those cdecl too.
 */
#include "shim_internal.h"

#include <stdint.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Pseudo-module handles. Values are arbitrary but must be distinguishable
 * from NULL and from each other. */
#define MODULE_SELF     ((HMODULE)0x10000)
#define MODULE_KERNEL32 ((HMODULE)0x10001)
#define MODULE_USER32   ((HMODULE)0x10002)
#define MODULE_D3D9     ((HMODULE)0x10003)
#define MODULE_WINMM    ((HMODULE)0x10004)

struct known_module {
    const char *win_name;
    HMODULE handle;
};

/* DLLs the game asks for by name that we satisfy in-process. */
static const struct known_module g_known[] = {
    { "kernel32.dll", MODULE_KERNEL32 },
    { "kernel32",     MODULE_KERNEL32 },
    { "user32.dll",   MODULE_USER32 },
    { "user32",       MODULE_USER32 },
    { "d3d9.dll",     MODULE_D3D9 },
    { "d3d9",         MODULE_D3D9 },
    { "winmm.dll",    MODULE_WINMM },
    { "winmm",        MODULE_WINMM },
};

static HMODULE lookup_known(LPCSTR name)
{
    size_t i;
    const char *base;

    if (!name)
        return MODULE_SELF;

    /* Accept a full path; only the basename identifies the DLL. */
    base = strrchr(name, '\\');
    base = base ? base + 1 : name;

    for (i = 0; i < sizeof(g_known) / sizeof(g_known[0]); i++) {
        if (strcasecmp(base, g_known[i].win_name) == 0)
            return g_known[i].handle;
    }
    return NULL;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR name)
{
    HMODULE h = lookup_known(name);

    if (!h) {
        nfsu2_shim_trace("GetModuleHandleA(%s): not present", name);
        SetLastError(ERROR_MOD_NOT_FOUND);
    }
    return h;
}

HMODULE WINAPI LoadLibraryA(LPCSTR name)
{
    HMODULE h = lookup_known(name);

    if (h)
        return h;

    /*
     * Anything else (dinput8.dll, the SafeDisc helper, mss32.dll for audio)
     * is not implemented. Returning NULL is the truthful answer and lets the
     * caller's own error path run instead of crashing later.
     */
    nfsu2_shim_trace("LoadLibraryA(%s): unimplemented, returning NULL", name);
    SetLastError(ERROR_MOD_NOT_FOUND);
    return NULL;
}

HMODULE WINAPI LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    (void)file; (void)flags;
    return LoadLibraryA(name);
}

BOOL WINAPI FreeLibrary(HMODULE module)
{
    (void)module;
    return TRUE;
}

FARPROC WINAPI GetProcAddress(HMODULE module, LPCSTR name)
{
    void *sym;

    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    /* Ordinal lookups (high word zero) cannot be mapped to ELF symbols. */
    if ((uintptr_t)name < 0x10000) {
        nfsu2_shim_trace("GetProcAddress(ordinal %u): unsupported", (unsigned)(uintptr_t)name);
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    (void)module;

    sym = dlsym(RTLD_DEFAULT, name);
    if (!sym) {
        nfsu2_shim_trace("GetProcAddress(%s): not implemented", name);
        SetLastError(ERROR_PROC_NOT_FOUND);
        return NULL;
    }
    return (FARPROC)sym;
}

DWORD WINAPI GetModuleFileNameA(HMODULE module, LPSTR buf, DWORD size)
{
    char exe[PATH_MAX_FALLBACK];
    ssize_t n;
    DWORD len;

    (void)module;
    if (!buf || size == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) {
        nfsu2_set_last_error_from_errno(errno);
        return 0;
    }
    exe[n] = '\0';

    /*
     * Returned as a host path on purpose: the game splits this to derive its
     * own install directory, and nfsu2_path_to_host() passes absolute POSIX
     * paths straight through, so the round trip stays consistent.
     */
    len = (DWORD)snprintf(buf, size, "%s", exe);
    if (len >= size) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    return len;
}
