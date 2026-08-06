/*
 * win32_compat.h - single entry point for Win32 types/prototypes when
 * building native Linux ELF objects.
 *
 * We do not use winelib (winegcc + libwine + the wine loader). We use only
 * Wine's *headers* (LGPL-2.1, /usr/include/wine/windows on Arch) as the
 * declaration source, and compile straight to a native ELF with gcc/clang.
 * Wine's headers are self-contained enough for this: they compile cleanly at
 * both -m32 and -m64 against glibc, and they can be mixed in the same
 * translation unit with <stdio.h>, <SDL2/SDL.h>, etc.
 *
 * Calling conventions on this platform, as Wine's minwindef.h defines them:
 *
 *   i386   WINAPI = __attribute__((stdcall)) __attribute__((force_align_arg_pointer))
 *   x86_64 WINAPI = __attribute__((ms_abi))
 *
 * That is deliberate and we keep it: every Win32 function we shim is defined
 * WINAPI, so a call site in ported game code has the same ABI the original
 * MSVC-compiled code had. It also keeps the door open for hybrid execution
 * (running not-yet-ported original machine code in-process, whose calls to
 * imports are stdcall by construction).
 *
 * The one place this convention must NOT be used is COM interfaces provided
 * by DXVK Native, which is compiled with STDMETHODCALLTYPE empty. See
 * d3d9_native.h - that header is the only place allowed to touch WINAPI.
 */
#ifndef NFSU2_WIN32_COMPAT_H
#define NFSU2_WIN32_COMPAT_H

#ifdef _WIN32
#error "nfsu2 native port targets Linux ELF; do not build these sources for Win32"
#endif

#if !defined(__i386__) && !defined(__x86_64__)
#error "unsupported architecture (i386 is the real target; x86_64 builds exist only to validate the toolchain)"
#endif

#include <windows.h>

/*
 * The Win64 SDK dropped the legacy GWL_USERDATA / GWL_WNDPROC / ... names in
 * favour of the pointer-sized GWLP_* ones, and Wine's headers follow it. On
 * Win32 - which is what the game is - the two sets are the same values, and the
 * game uses the legacy names. Alias them so identical sources still compile in
 * a 64-bit toolchain-validation build. Guarded individually because Wine keeps
 * some of the legacy names (GWL_ID) even at 64-bit.
 */
#ifndef GWL_USERDATA
#  define GWL_USERDATA   GWLP_USERDATA
#endif
#ifndef GWL_WNDPROC
#  define GWL_WNDPROC    GWLP_WNDPROC
#endif
#ifndef GWL_HINSTANCE
#  define GWL_HINSTANCE  GWLP_HINSTANCE
#endif
#ifndef GWL_HWNDPARENT
#  define GWL_HWNDPARENT GWLP_HWNDPARENT
#endif
#ifndef GWL_ID
#  define GWL_ID         GWLP_ID
#endif

/*
 * Variadic Win32 exports (wsprintfA and anything else declared WINAPIV) need
 * these. Wine's headers give us the __ms_va_list *type* but not the matching
 * va_start/va_end, because those live in Wine's internal headers.
 *
 * It matters only at 64-bit, where WINAPIV resolves to ms_abi and
 * __ms_va_list is __builtin_ms_va_list - a different type from the SysV
 * va_list, so the standard va_start would not initialise it. At 32-bit both are
 * the same thing and these collapse to the ordinary macros.
 */
#if defined(__x86_64__)
#  define NFSU2_MS_VA_START(list, last) __builtin_ms_va_start(list, last)
#  define NFSU2_MS_VA_END(list)         __builtin_ms_va_end(list)
#else
#  define NFSU2_MS_VA_START(list, last) va_start(list, last)
#  define NFSU2_MS_VA_END(list)         va_end(list)
#endif

/*
 * Extra convention macros for porting decompiled code verbatim. Ghidra
 * annotates the original functions with __cdecl / __stdcall / __thiscall /
 * __fastcall; keeping those annotations on the ported C prototypes is what
 * makes a partially-ported binary (some functions ours, some still original
 * machine code) able to call across the boundary.
 */
#if defined(__i386__)
#  define NFSU2_CDECL     __attribute__((cdecl))
#  define NFSU2_STDCALL   __attribute__((stdcall))
#  define NFSU2_THISCALL  __attribute__((thiscall))
#  define NFSU2_FASTCALL  __attribute__((fastcall))
#else
#  define NFSU2_CDECL
#  define NFSU2_STDCALL
#  define NFSU2_THISCALL
#  define NFSU2_FASTCALL
#endif

#endif /* NFSU2_WIN32_COMPAT_H */
