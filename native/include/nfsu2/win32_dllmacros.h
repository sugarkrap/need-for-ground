/*
 * win32_dllmacros.h - "this program implements these DLLs", for Wine's headers.
 *
 * Include this before *any* Wine header. win32_compat.h does it for you; the
 * only reason it is a separate header is <winsock2.h>, which has to be included
 * before windows.h (it defines _WINSOCKAPI_ so windows.h skips the older
 * winsock.h) and therefore before win32_compat.h - so ws2_32/winsock.c needs the
 * macros without pulling in windows.h yet.
 *
 * What these do: switch WINBASEAPI / WINUSERAPI / WINGDIAPI / WINADVAPI /
 * WINMMAPI / WINSOCK_API_LINKAGE from DECLSPEC_IMPORT to nothing or
 * DECLSPEC_EXPORT. That is not a trick - it is the truth here. Every Win32 entry
 * point is a definition in this same binary, not an import from another module.
 *
 * Why it must apply to every translation unit, not just the shim's own sources:
 * under GCC on a non-PE target DECLSPEC_IMPORT expands to
 * __attribute__((visibility("hidden"))), and ELF resolves a symbol's visibility
 * to the *most restrictive* of all its declarations and its definition. So one
 * consumer compiled without these macros - a ported game file calling
 * CreateFileA, say - makes CreateFileA hidden for the whole program, dropping it
 * from .dynsym, and GetProcAddress (implemented over dlsym; see
 * win32/module.c) silently stops finding it. The symptom is a shim that works
 * for direct calls and fails only for the code paths that resolve imports
 * dynamically, which is a thoroughly unpleasant thing to debug.
 */
#ifndef NFSU2_WIN32_DLLMACROS_H
#define NFSU2_WIN32_DLLMACROS_H

#ifdef _WIN32
#error "nfsu2 native port targets Linux ELF; do not build these sources for Win32"
#endif

#define _KERNEL32_
#define _USER32_
#define _ADVAPI32_
#define _WINMM_
#define _GDI32_
#define _SHELL32_
/* RtlUnwind and friends are declared NTSYSAPI, which is DECLSPEC_IMPORT unless
 * this is set - the same hidden-visibility trap as the others. */
#define _NTSYSTEM_

/*
 * _WS2_32_ is deliberately NOT defined, so the Winsock entry points keep Wine's
 * hidden-by-default visibility.
 *
 * They must not reach the executable's dynamic symbol table, because their names
 * are also libc's: socket, bind, listen, accept, connect, send, recv, select and
 * the rest. Symbols exported by an executable take precedence over shared
 * libraries in the global scope, so exporting them means SDL, libX11 and
 * anything else in the process calling select() lands in our Winsock select -
 * which reads its fd_set as a count-plus-array structure and walks off the end
 * of a POSIX bitmask. That presented as an immediate SIGSEGV before main() even
 * printed anything.
 *
 * Hidden visibility costs nothing real here: the imports resolve at link time
 * for direct calls, which is how the game uses them. The only loss is
 * GetProcAddress("socket"), and nothing does that - Winsock is imported
 * normally. See ws2_32/posix_net.c for the same collision in its link-time form.
 */

#endif /* NFSU2_WIN32_DLLMACROS_H */
