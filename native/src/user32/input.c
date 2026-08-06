/*
 * input.c - cursor visibility, capture, position, and the icon/cursor handles.
 */
#include "user32_internal.h"

#include <string.h>

/* --- cursor visibility -------------------------------------------------- */

/*
 * ShowCursor is a counter on Windows, not a boolean: the cursor is visible
 * while the count is >= 0, and every hide must be matched by a show. Game code
 * relies on that (it hides on entering the 3D view and shows on leaving), so
 * the count has to be modelled rather than passed through to SDL directly.
 */
static int g_cursor_count;

int WINAPI ShowCursor(BOOL show)
{
    g_cursor_count += show ? 1 : -1;

    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_ShowCursor(g_cursor_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    return g_cursor_count;
}

/* --- cursor / icon handles --------------------------------------------- */

/*
 * There is no GDI object model here. LoadCursorA and LoadIconA hand back
 * distinct non-NULL pseudo-handles so the caller's null checks pass and
 * WNDCLASSEXA can carry them, and SetCursor maps the ones we recognise onto an
 * SDL system cursor. Custom cursors from resources are not supported.
 */
static char g_cursor_arrow, g_cursor_wait, g_cursor_other, g_icon;

HCURSOR WINAPI LoadCursorA(HINSTANCE instance, LPCSTR name)
{
    if (instance) {
        /* A cursor out of the exe's resources: nothing to load it from. */
        NFSU2_STUB("LoadCursorA from module resources");
        return (HCURSOR)(void *)&g_cursor_other;
    }
    if (name == IDC_WAIT || name == IDC_APPSTARTING)
        return (HCURSOR)(void *)&g_cursor_wait;
    if (name == IDC_ARROW)
        return (HCURSOR)(void *)&g_cursor_arrow;
    return (HCURSOR)(void *)&g_cursor_other;
}

HICON WINAPI LoadIconA(HINSTANCE instance, LPCSTR name)
{
    (void)instance; (void)name;
    /* The window icon comes from the desktop environment here; SDL can set one
     * from a surface, which we have no way to decode a Win32 resource into. */
    return (HICON)(void *)&g_icon;
}

HCURSOR WINAPI SetCursor(HCURSOR cursor)
{
    static HCURSOR current;
    HCURSOR previous = current;
    SDL_Cursor *sdl_cursor = NULL;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return previous;

    if (cursor == (HCURSOR)(void *)&g_cursor_wait)
        sdl_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_WAIT);
    else if (cursor)
        sdl_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);

    if (sdl_cursor) {
        /* SDL_SetCursor does not take ownership and freeing the active cursor
         * is undefined, so the previous one is released after the swap. */
        SDL_Cursor *old = SDL_GetCursor();
        SDL_SetCursor(sdl_cursor);
        if (old && old != SDL_GetDefaultCursor())
            SDL_FreeCursor(old);
    }
    current = cursor;
    return previous;
}

/* --- position and capture ---------------------------------------------- */

BOOL WINAPI GetCursorPos(LPPOINT point)
{
    int x = 0, y = 0;

    if (!point) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (nfsu2_user32_ensure_video() != 0)
        return FALSE;

    /* Screen coordinates, matching Win32; SDL_GetMouseState would be relative
     * to the focused window. */
    SDL_GetGlobalMouseState(&x, &y);
    point->x = x;
    point->y = y;
    return TRUE;
}

BOOL WINAPI SetCursorPos(int x, int y)
{
    if (nfsu2_user32_ensure_video() != 0)
        return FALSE;
    return SDL_WarpMouseGlobal(x, y) == 0 ? TRUE : FALSE;
}

static HWND g_capture_window;

HWND WINAPI SetCapture(HWND hwnd)
{
    HWND previous = g_capture_window;

    if (!nfsu2_window_state(hwnd)) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return previous;
    }
    /* Capture means "keep sending me mouse messages even outside the window",
     * which is SDL's mouse capture, not relative mode. */
    if (SDL_CaptureMouse(SDL_TRUE) != 0)
        nfsu2_shim_trace("SDL_CaptureMouse failed: %s", SDL_GetError());
    g_capture_window = hwnd;
    return previous;
}

BOOL WINAPI ReleaseCapture(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO))
        SDL_CaptureMouse(SDL_FALSE);
    g_capture_window = NULL;
    return TRUE;
}

HWND WINAPI GetCapture(void)
{
    return g_capture_window;
}
