/*
 * d3d9_native.h - Wine's D3D9 declarations, retargeted to DXVK Native's ABI.
 *
 * Why this header exists
 * ---------------------
 * DXVK Native (upstream dxvk, built without mingw) provides d3d9 as a plain
 * Linux shared object, libdxvk_d3d9.so, exporting Direct3DCreate9 and COM
 * objects with C++ vtables. Its own compat headers
 * (dxvk/include/native/windows/windows_base.h) define
 *
 *     #define STDMETHODCALLTYPE
 *     #define __stdcall
 *     #define WINAPI
 *
 * i.e. *empty*. On i386 that means every D3D9 entry point and every vtable
 * slot is cdecl (caller pops the arguments), not stdcall (callee pops).
 *
 * Wine's d3d9.h declares the same interfaces via STDMETHOD(...), which
 * expands through basetyps.h to STDMETHODCALLTYPE -> WINAPI -> stdcall on
 * i386 (and ms_abi on x86_64). Using it unmodified against DXVK Native would
 * compile and link fine and then corrupt the stack on the very first
 * SetViewport call - each one leaking or double-popping 4-8 bytes.
 *
 * Verified with gcc -m32 -O1 -S on `dev->lpVtbl->SetViewport(dev, vp)`:
 *
 *     wine headers as-is   ... call *188(%edx) ; addl $20, %esp   (callee popped 8 -> stdcall)
 *     with this header     ... call *188(%edx) ; addl $28, %esp   (caller popped 8 -> cdecl)
 *
 * So rather than vendoring DXVK's headers or generating ~300 stdcall->cdecl
 * thunks, we take Wine's *types* (which are the DX9 SDK layout, identical to
 * what DXVK mirrors) and neutralise the two convention macros for the
 * duration of the d3d9 includes only. Zero glue code, zero DXVK patches.
 *
 * The same trick is required at -m64, where Wine's WINAPI is ms_abi but DXVK
 * Native is SysV.
 *
 * Consequence for the eventual own-Vulkan-renderer (see DIRECTX_SCOPE.md):
 * whatever we implement behind these interfaces must also be cdecl/SysV, i.e.
 * it must be compiled against this header, not against raw d3d9.h.
 */
#ifndef NFSU2_D3D9_NATIVE_H
#define NFSU2_D3D9_NATIVE_H

#include <nfsu2/win32_compat.h>

/* Neutralise the Win32 calling conventions for the D3D9 declarations. */
#undef WINAPI
#define WINAPI
#undef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE

#include <d3d9.h>
#include <d3d9types.h>
#include <d3d9caps.h>

/*
 * Restore Wine's own definitions (minwindef.h:157 / basetyps.h:29). The
 * __stdcall macro itself is still defined by minwindef.h for every target,
 * so this is exactly the original meaning, not an approximation.
 */
#undef WINAPI
#define WINAPI __stdcall
#undef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE WINAPI

/*
 * DXVK Native's WSI layer casts HWND straight to the backend's window pointer
 * (SDL_Window, SDL3's SDL_Window, or GLFWwindow - see
 * dxvk/src/wsi/native/wsi_window_sdl2.cpp). Wine's HWND is a distinct pointer
 * type, so the cast has to be spelled out; do it through this macro so the
 * WSI-backend assumption is grep-able in one place.
 */
#define NFSU2_HWND_FROM_WSI_WINDOW(w) ((HWND)(void *)(w))

#endif /* NFSU2_D3D9_NATIVE_H */
