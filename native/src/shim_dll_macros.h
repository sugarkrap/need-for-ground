/*
 * shim_dll_macros.h - must be included before any Wine header, by every
 * translation unit that *implements* Win32 entry points.
 *
 * Wine decorates imported prototypes with DECLSPEC_IMPORT, which under GCC on
 * a non-PE target is __attribute__((visibility("hidden"))). A definition
 * inherits that from the declaration, so a shim built without these macros
 * links but leaves its symbols out of the dynamic symbol table - and
 * GetProcAddress (implemented over dlsym, see win32/module.c) silently finds
 * nothing.
 *
 * Defining the per-DLL "I am implementing this DLL" macros is the same
 * mechanism Wine uses when building its own kernel32/user32, and switches
 * those prototypes to default visibility.
 */
#ifndef NFSU2_SHIM_DLL_MACROS_H
#define NFSU2_SHIM_DLL_MACROS_H

#define _KERNEL32_
#define _USER32_
#define _ADVAPI32_
#define _WINMM_

#endif /* NFSU2_SHIM_DLL_MACROS_H */
