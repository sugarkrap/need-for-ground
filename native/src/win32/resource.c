/*
 * resource.c - PE resource lookup.
 *
 * A native ELF has no .rsrc section, so there is nothing to find. The game uses
 * these for its window icon and its version block - neither of which is needed
 * for it to run, which is why honest failure is the right answer today rather
 * than a half-invented resource.
 *
 * If a resource ever *is* needed, the path is already clear and does not involve
 * changing these signatures: the repo has pefile available (see requirements.txt
 * and analysis/), so a tool can extract .rsrc from the unwrapped exe into a blob
 * plus an index, and this file grows a loader for it keyed on (type, name). That
 * is deliberately not built on speculation - no resource has been shown to
 * matter yet.
 */
#include "shim_internal.h"

#include <stdint.h>
#include <stdio.h>

static void trace_lookup(const char *api, LPCSTR type, LPCSTR name)
{
    /* Resource identifiers are either strings or small integers packed into the
     * pointer; both spellings are worth logging so a real dependency can be
     * identified from a trace. */
    char type_text[32];
    char name_text[32];

    if ((uintptr_t)type < 0x10000)
        snprintf(type_text, sizeof(type_text), "#%u", (unsigned)(uintptr_t)type);
    else
        snprintf(type_text, sizeof(type_text), "%s", type ? type : "(null)");

    if ((uintptr_t)name < 0x10000)
        snprintf(name_text, sizeof(name_text), "#%u", (unsigned)(uintptr_t)name);
    else
        snprintf(name_text, sizeof(name_text), "%s", name ? name : "(null)");

    nfsu2_shim_trace("%s(type=%s, name=%s): no PE resources in an ELF", api, type_text, name_text);
}

HRSRC WINAPI FindResourceA(HMODULE module, LPCSTR name, LPCSTR type)
{
    (void)module;
    trace_lookup("FindResourceA", type, name);
    SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return NULL;
}

HRSRC WINAPI FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type)
{
    (void)module; (void)name; (void)type;
    nfsu2_shim_trace("FindResourceW: no PE resources in an ELF");
    SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
    return NULL;
}

HRSRC WINAPI FindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language)
{
    (void)language;
    return FindResourceA(module, name, type);
}

HGLOBAL WINAPI LoadResource(HMODULE module, HRSRC resource)
{
    (void)module; (void)resource;
    /* Unreachable unless a caller ignores FindResource's NULL, which is worth
     * knowing about. */
    NFSU2_STUB("LoadResource called with a resource handle we never issued");
    SetLastError(ERROR_INVALID_HANDLE);
    return NULL;
}

LPVOID WINAPI LockResource(HGLOBAL resource)
{
    (void)resource;
    NFSU2_STUB("LockResource");
    return NULL;
}

BOOL WINAPI FreeResource(HGLOBAL resource)
{
    (void)resource;
    return TRUE;
}

DWORD WINAPI SizeofResource(HMODULE module, HRSRC resource)
{
    (void)module; (void)resource;
    NFSU2_STUB("SizeofResource");
    SetLastError(ERROR_INVALID_HANDLE);
    return 0;
}
