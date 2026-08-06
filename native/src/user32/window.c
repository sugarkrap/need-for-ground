/*
 * window.c - window classes, window lifecycle, geometry, system metrics.
 */
#include "user32_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- lazy SDL video init ------------------------------------------------ */

int nfsu2_user32_ensure_video(void)
{
    if (SDL_WasInit(SDL_INIT_VIDEO))
        return 0;

    /*
     * DXVK Native builds every WSI backend it finds and picks one at load
     * time; if that is not the toolkit that owns the window, Direct3DCreate9
     * throws an uncaught DxvkError with no message. Our windows are SDL2, so
     * say so - without overriding a deliberate choice from the environment.
     */
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        nfsu2_shim_trace("SDL_Init(VIDEO) failed: %s", SDL_GetError());
        SetLastError(ERROR_NOT_READY);
        return -1;
    }
    /* Win32 delivers characters as WM_CHAR; SDL needs text input switched on
     * for the equivalent SDL_TEXTINPUT events. */
    SDL_StartTextInput();
    return 0;
}

/* --- window class registry --------------------------------------------- */

#define MAX_CLASSES 32

static struct {
    char *name;
    WNDCLASSEXA info;
} g_classes[MAX_CLASSES];

const WNDCLASSEXA *nfsu2_class_find(LPCSTR name)
{
    int i;

    if (!name)
        return NULL;
    for (i = 0; i < MAX_CLASSES; i++) {
        if (g_classes[i].name && strcasecmp(g_classes[i].name, name) == 0)
            return &g_classes[i].info;
    }
    return NULL;
}

ATOM WINAPI RegisterClassExA(const WNDCLASSEXA *cls)
{
    int i;

    if (!cls || !cls->lpszClassName || !cls->lpfnWndProc) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (nfsu2_user32_ensure_video() != 0)
        return 0;
    if (nfsu2_class_find(cls->lpszClassName)) {
        SetLastError(ERROR_CLASS_ALREADY_EXISTS);
        return 0;
    }

    for (i = 0; i < MAX_CLASSES; i++) {
        if (!g_classes[i].name) {
            g_classes[i].name = strdup(cls->lpszClassName);
            if (!g_classes[i].name) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return 0;
            }
            g_classes[i].info = *cls;
            /* The ATOM only has to be non-zero and unique per class. */
            return (ATOM)(i + 1);
        }
    }
    nfsu2_shim_trace("RegisterClassExA: class table full (%d)", MAX_CLASSES);
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

ATOM WINAPI RegisterClassA(const WNDCLASSA *cls)
{
    WNDCLASSEXA ex;

    if (!cls) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    memset(&ex, 0, sizeof(ex));
    ex.cbSize = sizeof(ex);
    ex.style = cls->style;
    ex.lpfnWndProc = cls->lpfnWndProc;
    ex.cbClsExtra = cls->cbClsExtra;
    ex.cbWndExtra = cls->cbWndExtra;
    ex.hInstance = cls->hInstance;
    ex.hIcon = cls->hIcon;
    ex.hCursor = cls->hCursor;
    ex.hbrBackground = cls->hbrBackground;
    ex.lpszMenuName = cls->lpszMenuName;
    ex.lpszClassName = cls->lpszClassName;
    return RegisterClassExA(&ex);
}

BOOL WINAPI UnregisterClassA(LPCSTR name, HINSTANCE instance)
{
    int i;

    (void)instance;
    if (!name) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (i = 0; i < MAX_CLASSES; i++) {
        if (g_classes[i].name && strcasecmp(g_classes[i].name, name) == 0) {
            free(g_classes[i].name);
            g_classes[i].name = NULL;
            memset(&g_classes[i].info, 0, sizeof(g_classes[i].info));
            return TRUE;
        }
    }
    SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
    return FALSE;
}

/* --- window state ------------------------------------------------------ */

struct nfsu2_window *nfsu2_window_state(HWND hwnd)
{
    SDL_Window *sdl = nfsu2_sdl_from_hwnd(hwnd);

    if (!sdl || hwnd == nfsu2_desktop_hwnd())
        return NULL;
    return SDL_GetWindowData(sdl, NFSU2_WINDOW_DATA_KEY);
}

static char g_desktop_sentinel;

HWND nfsu2_desktop_hwnd(void)
{
    return (HWND)(void *)&g_desktop_sentinel;
}

HWND WINAPI GetDesktopWindow(void)
{
    if (nfsu2_user32_ensure_video() != 0)
        return NULL;
    return nfsu2_desktop_hwnd();
}

/* --- creation / destruction -------------------------------------------- */

HWND WINAPI CreateWindowExA(DWORD ex_style, LPCSTR class_name, LPCSTR window_name,
                            DWORD style, int x, int y, int width, int height,
                            HWND parent, HMENU menu, HINSTANCE instance, LPVOID param)
{
    const WNDCLASSEXA *cls;
    struct nfsu2_window *state;
    SDL_Window *sdl;
    Uint32 flags;
    CREATESTRUCTA cs;
    int pos_x, pos_y;

    (void)parent; (void)menu;

    if (nfsu2_user32_ensure_video() != 0)
        return NULL;

    cls = nfsu2_class_find(class_name);
    if (!cls) {
        nfsu2_shim_trace("CreateWindowExA: unknown class '%s'", class_name ? class_name : "(null)");
        SetLastError(ERROR_CANNOT_FIND_WND_CLASS);
        return NULL;
    }

    /*
     * width/height are treated as the *client* size, which is also what
     * SDL_CreateWindow takes. That pairs with AdjustWindowRect() below being
     * an identity transform: the game adjusts a client rect, passes the result
     * here, and ends up with the client size it originally wanted.
     */
    if (width <= 0)
        width = 640;
    if (height <= 0)
        height = 480;

    flags = SDL_WINDOW_VULKAN; /* required for DXVK to make a surface */
    if (style & WS_VISIBLE)
        flags |= SDL_WINDOW_SHOWN;
    else
        flags |= SDL_WINDOW_HIDDEN;
    /* A WS_POPUP window with no caption is the game's borderless/fullscreen
     * shape; anything else gets normal decorations. */
    if ((style & WS_POPUP) && !(style & WS_CAPTION))
        flags |= SDL_WINDOW_BORDERLESS;
    if (style & WS_THICKFRAME)
        flags |= SDL_WINDOW_RESIZABLE;
    if (style & WS_MINIMIZE)
        flags |= SDL_WINDOW_MINIMIZED;

    pos_x = (x == (int)CW_USEDEFAULT) ? (int)SDL_WINDOWPOS_CENTERED : x;
    pos_y = (y == (int)CW_USEDEFAULT) ? (int)SDL_WINDOWPOS_CENTERED : y;

    sdl = SDL_CreateWindow(window_name ? window_name : "", pos_x, pos_y, width, height, flags);
    if (!sdl) {
        nfsu2_shim_trace("SDL_CreateWindow failed: %s", SDL_GetError());
        SetLastError(ERROR_CANNOT_MAKE);
        return NULL;
    }

    state = calloc(1, sizeof(*state));
    if (!state) {
        SDL_DestroyWindow(sdl);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    state->sdl = sdl;
    state->wndproc = cls->lpfnWndProc;
    state->class_name = strdup(cls->lpszClassName);
    state->style = style;
    state->ex_style = ex_style;
    state->instance = instance ? instance : cls->hInstance;
    SDL_SetWindowData(sdl, NFSU2_WINDOW_DATA_KEY, state);

    /* WM_CREATE is delivered synchronously, as on Windows: the game's wndproc
     * commonly does its one-time setup there and returning -1 must abort. */
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param;
    cs.hInstance = state->instance;
    cs.hMenu = menu;
    cs.hwndParent = parent;
    cs.cy = height;
    cs.cx = width;
    cs.y = y;
    cs.x = x;
    cs.style = (LONG)style;
    cs.lpszName = window_name;
    cs.lpszClass = class_name;
    cs.dwExStyle = ex_style;

    if (state->wndproc(nfsu2_hwnd_from_sdl(sdl), WM_CREATE, 0, (LPARAM)&cs) == (LRESULT)-1) {
        nfsu2_shim_trace("CreateWindowExA: WM_CREATE handler returned -1");
        DestroyWindow(nfsu2_hwnd_from_sdl(sdl));
        return NULL;
    }

    SetLastError(ERROR_SUCCESS);
    return nfsu2_hwnd_from_sdl(sdl);
}

/* No CreateWindowA here: Wine's winuser.h defines it as a macro over
 * CreateWindowExA (winuser.h:4005), same as the Windows SDK does. */

BOOL WINAPI DestroyWindow(HWND hwnd)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);

    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    if (state->destroyed)
        return TRUE;
    state->destroyed = 1;

    state->wndproc(hwnd, WM_DESTROY, 0, 0);
    nfsu2_msg_drop_window(hwnd);

    SDL_SetWindowData(state->sdl, NFSU2_WINDOW_DATA_KEY, NULL);
    SDL_DestroyWindow(state->sdl);
    free(state->class_name);
    free(state);
    return TRUE;
}

BOOL WINAPI ShowWindow(HWND hwnd, int cmd)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);
    BOOL was_visible;

    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    was_visible = (SDL_GetWindowFlags(state->sdl) & SDL_WINDOW_SHOWN) ? TRUE : FALSE;

    switch (cmd) {
    case SW_HIDE:
        SDL_HideWindow(state->sdl);
        break;
    case SW_MINIMIZE:
    case SW_SHOWMINIMIZED:
    case SW_SHOWMINNOACTIVE:
        SDL_MinimizeWindow(state->sdl);
        break;
    case SW_MAXIMIZE:
        SDL_MaximizeWindow(state->sdl);
        break;
    case SW_RESTORE:
        SDL_RestoreWindow(state->sdl);
        SDL_ShowWindow(state->sdl);
        break;
    default: /* SW_SHOW, SW_SHOWNORMAL, SW_SHOWDEFAULT, ... */
        SDL_ShowWindow(state->sdl);
        break;
    }
    return was_visible;
}

BOOL WINAPI UpdateWindow(HWND hwnd)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);

    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    /* Windows would flush a pending WM_PAINT here. There is no GDI backing
     * store to repaint, and the renderer presents every frame regardless, so
     * delivering WM_PAINT is enough to keep a wndproc that counts on it. */
    state->wndproc(hwnd, WM_PAINT, 0, 0);
    return TRUE;
}

/* --- geometry ---------------------------------------------------------- */

static void desktop_rect(RECT *rect)
{
    SDL_DisplayMode mode;

    rect->left = 0;
    rect->top = 0;
    if (SDL_GetDesktopDisplayMode(0, &mode) == 0) {
        rect->right = mode.w;
        rect->bottom = mode.h;
    } else {
        rect->right = 1920;
        rect->bottom = 1080;
    }
}

BOOL WINAPI GetClientRect(HWND hwnd, LPRECT rect)
{
    struct nfsu2_window *state;
    int w = 0, h = 0;

    if (!rect) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (hwnd == nfsu2_desktop_hwnd()) {
        desktop_rect(rect);
        return TRUE;
    }
    state = nfsu2_window_state(hwnd);
    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    SDL_GetWindowSize(state->sdl, &w, &h);
    rect->left = 0;
    rect->top = 0;
    rect->right = w;
    rect->bottom = h;
    return TRUE;
}

BOOL WINAPI GetWindowRect(HWND hwnd, LPRECT rect)
{
    struct nfsu2_window *state;
    int x = 0, y = 0, w = 0, h = 0;

    if (!rect) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (hwnd == nfsu2_desktop_hwnd()) {
        desktop_rect(rect);
        return TRUE;
    }
    state = nfsu2_window_state(hwnd);
    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return FALSE;
    }
    SDL_GetWindowPosition(state->sdl, &x, &y);
    SDL_GetWindowSize(state->sdl, &w, &h);
    /* Client rect in screen coordinates: decorations are not counted, which
     * matches CreateWindowExA treating its size as the client size. */
    rect->left = x;
    rect->top = y;
    rect->right = x + w;
    rect->bottom = y + h;
    return TRUE;
}

BOOL WINAPI AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu)
{
    (void)style; (void)menu;
    if (!rect) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    /*
     * Identity on purpose. The Win32 idiom is "adjust a client rect to the
     * window rect that yields it, then pass that to CreateWindowExA", and our
     * CreateWindowExA takes a client size (SDL_CreateWindow does too). Adding
     * a decoration margin here would make the client that much too small.
     */
    return TRUE;
}

BOOL WINAPI AdjustWindowRectEx(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style)
{
    (void)ex_style;
    return AdjustWindowRect(rect, style, menu);
}

BOOL WINAPI SetRect(LPRECT rect, int left, int top, int right, int bottom)
{
    if (!rect) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    rect->left = left;
    rect->top = top;
    rect->right = right;
    rect->bottom = bottom;
    return TRUE;
}

BOOL WINAPI IsIconic(HWND hwnd)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);

    if (!state)
        return FALSE;
    return (SDL_GetWindowFlags(state->sdl) & SDL_WINDOW_MINIMIZED) ? TRUE : FALSE;
}

/* --- window long values ------------------------------------------------ */

LONG WINAPI GetWindowLongA(HWND hwnd, int index)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);

    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    switch (index) {
    case GWL_STYLE:     return (LONG)state->style;
    case GWL_EXSTYLE:   return (LONG)state->ex_style;
    case GWL_USERDATA:  return state->user_data;
    case GWL_WNDPROC:   return (LONG)(LONG_PTR)state->wndproc;
    case GWL_HINSTANCE: return (LONG)(LONG_PTR)state->instance;
    default:
        nfsu2_shim_trace("GetWindowLongA(%d): unhandled index", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
}

LONG WINAPI SetWindowLongA(HWND hwnd, int index, LONG value)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);
    LONG previous;

    if (!state) {
        SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        return 0;
    }
    previous = GetWindowLongA(hwnd, index);

    switch (index) {
    case GWL_STYLE:
        state->style = (DWORD)value;
        /*
         * The game restyles its window when toggling fullscreen, so the border
         * has to follow. Everything else in the style word is either advisory
         * here or already fixed at creation.
         */
        SDL_SetWindowBordered(state->sdl,
                              ((state->style & WS_POPUP) && !(state->style & WS_CAPTION))
                                  ? SDL_FALSE : SDL_TRUE);
        SDL_SetWindowResizable(state->sdl,
                               (state->style & WS_THICKFRAME) ? SDL_TRUE : SDL_FALSE);
        break;
    case GWL_EXSTYLE:
        state->ex_style = (DWORD)value;
        break;
    case GWL_USERDATA:
        state->user_data = value;
        break;
    case GWL_WNDPROC:
        /* Subclassing: legitimate and used by the game's own input hook. */
        state->wndproc = (WNDPROC)(LONG_PTR)value;
        break;
    default:
        nfsu2_shim_trace("SetWindowLongA(%d): unhandled index", index);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return previous;
}

/* --- focus / activation ------------------------------------------------ */

HWND WINAPI GetForegroundWindow(void)
{
    SDL_Window *sdl;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return NULL;
    sdl = SDL_GetKeyboardFocus();
    return sdl ? nfsu2_hwnd_from_sdl(sdl) : NULL;
}

BOOL WINAPI SetForegroundWindow(HWND hwnd)
{
    struct nfsu2_window *state = nfsu2_window_state(hwnd);

    if (!state)
        return FALSE;
    SDL_RaiseWindow(state->sdl);
    return TRUE;
}

HWND WINAPI SetActiveWindow(HWND hwnd)
{
    HWND previous = GetForegroundWindow();

    SetForegroundWindow(hwnd);
    return previous;
}

HWND WINAPI SetFocus(HWND hwnd)
{
    HWND previous = GetForegroundWindow();

    /* No sub-window focus model here: a window either has the platform's
     * keyboard focus or it does not. */
    SetForegroundWindow(hwnd);
    return previous;
}

/* --- system metrics ---------------------------------------------------- */

int WINAPI GetSystemMetrics(int index)
{
    RECT desktop;

    if (nfsu2_user32_ensure_video() != 0)
        return 0;
    desktop_rect(&desktop);

    switch (index) {
    case SM_CXSCREEN:
    case SM_CXFULLSCREEN:
    case SM_CXMAXIMIZED:
    case SM_CXVIRTUALSCREEN:
        return desktop.right;
    case SM_CYSCREEN:
    case SM_CYFULLSCREEN:
    case SM_CYMAXIMIZED:
    case SM_CYVIRTUALSCREEN:
        return desktop.bottom;
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
        return 0;
    case SM_CMONITORS:
        return SDL_GetNumVideoDisplays() > 0 ? SDL_GetNumVideoDisplays() : 1;
    case SM_CMOUSEBUTTONS:
        return 3;
    case SM_MOUSEWHEELPRESENT:
        return 1;
    case SM_SWAPBUTTON:
        return 0;
    /* Decoration sizes: reported as zero to stay consistent with
     * AdjustWindowRect being an identity transform. */
    case SM_CXBORDER:
    case SM_CYBORDER:
    case SM_CXFRAME:
    case SM_CYFRAME:
    case SM_CYCAPTION:
    case SM_CYMENU:
        return 0;
    case SM_CXCURSOR:
    case SM_CYCURSOR:
        return 32;
    default:
        nfsu2_shim_trace("GetSystemMetrics(%d): unhandled, returning 0", index);
        return 0;
    }
}

/* --- painting ---------------------------------------------------------- */

/*
 * No GDI here. BeginPaint has to return a non-NULL HDC and a sane rcPaint or a
 * wndproc's WM_PAINT branch will bail out or divide by zero; anything actually
 * drawn through that HDC by the gdi32 entry points (still unimplemented) is
 * dropped. The renderer presents every frame, so nothing depends on it.
 */
static char g_fake_dc;

HDC WINAPI BeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
    if (!ps) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    NFSU2_STUB("BeginPaint (no GDI backing)");
    memset(ps, 0, sizeof(*ps));
    ps->hdc = (HDC)(void *)&g_fake_dc;
    GetClientRect(hwnd, &ps->rcPaint);
    ps->fErase = FALSE;
    return ps->hdc;
}

BOOL WINAPI EndPaint(HWND hwnd, const PAINTSTRUCT *ps)
{
    (void)hwnd; (void)ps;
    return TRUE;
}
