/*
 * user32_internal.h - internals of the SDL2-backed user32 shim.
 *
 * Central decision: **HWND is an SDL_Window\***, not a handle of our own.
 *
 * DXVK Native's WSI layer casts HWND straight to the backend window pointer
 * (see include/nfsu2/d3d9_native.h). The game takes the HWND that
 * CreateWindowExA gave it and puts it in D3DPRESENT_PARAMETERS.hDeviceWindow,
 * so if HWND were a struct of ours, DXVK would dereference it as an
 * SDL_Window and crash. Making them the same pointer keeps that path working
 * with zero translation, and HWND is an opaque pointer to callers anyway.
 *
 * Per-window state hangs off the SDL_Window via SDL_SetWindowData, so there is
 * no side table to keep in sync.
 */
#ifndef NFSU2_USER32_INTERNAL_H
#define NFSU2_USER32_INTERNAL_H

#include "../shim_dll_macros.h"

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <SDL2/SDL.h>

#define NFSU2_WINDOW_DATA_KEY "nfsu2.window"

struct nfsu2_window {
    SDL_Window *sdl;
    WNDPROC wndproc;
    char *class_name;
    DWORD style;
    DWORD ex_style;
    LONG user_data;
    HINSTANCE instance;
    /* Set between DestroyWindow and the SDL window actually going away, so a
     * queued message for it can be dropped instead of dispatched. */
    int destroyed;
};

/* HWND <-> SDL_Window are the same pointer; these exist to make every place
 * that relies on that fact grep-able. */
static inline HWND nfsu2_hwnd_from_sdl(SDL_Window *w) { return (HWND)(void *)w; }
static inline SDL_Window *nfsu2_sdl_from_hwnd(HWND h) { return (SDL_Window *)(void *)h; }

struct nfsu2_window *nfsu2_window_state(HWND hwnd);

/* Lazily SDL_Init(SDL_INIT_VIDEO): the game never calls SDL itself, so the
 * first user32 entry point it touches has to do it. Returns 0 on success. */
int nfsu2_user32_ensure_video(void);

/* The sentinel GetDesktopWindow() returns. Not a real window; GetClientRect
 * and GetWindowRect special-case it to the desktop bounds. */
HWND nfsu2_desktop_hwnd(void);

/* Class registry (window.c). */
const WNDCLASSEXA *nfsu2_class_find(LPCSTR name);

/* Message plumbing (message.c). */
void nfsu2_msg_pump_sdl(void);
BOOL nfsu2_msg_post(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
void nfsu2_msg_drop_window(HWND hwnd);

/* SDL scancode -> Win32 virtual-key code (keymap.c). 0 if unmapped. */
WPARAM nfsu2_vk_from_scancode(SDL_Scancode scancode);

/* Current mouse-button/modifier flags for the wParam of mouse messages. */
WPARAM nfsu2_mouse_key_flags(void);

#endif /* NFSU2_USER32_INTERNAL_H */
