/*
 * user32_selftest.c - window, message-queue and input-translation checks.
 *
 * Needs a display; exits 77 (meson's SKIP) when there is none, so the suite
 * still passes on a headless machine instead of reporting a false failure.
 *
 * Input translation is tested by pushing synthetic SDL events into the queue
 * and checking what comes out of PeekMessageA. That is the seam most likely to
 * be silently wrong (a scancode table is easy to get subtly misaligned) and the
 * hardest to notice from inside game code, where a wrong VK just means a
 * control that does nothing.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_wm_create_seen;
static int g_dispatched_message;
static WPARAM g_dispatched_wparam;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            printf("ok   - " __VA_ARGS__);                                      \
            printf("\n");                                                       \
        } else {                                                                \
            printf("FAIL - " __VA_ARGS__);                                      \
            printf("  (%s:%d, GetLastError=%lu)\n", __FILE__, __LINE__,         \
                   (unsigned long)GetLastError());                              \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static LRESULT WINAPI test_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CREATE) {
        g_wm_create_seen++;
        return 0;
    }
    if (message == WM_APP) {
        g_dispatched_message = (int)message;
        g_dispatched_wparam = wparam;
        return 42;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

/* Drain anything the window manager posted (focus, size, expose) so the
 * synthetic-event checks below see a predictable queue. */
static void drain_messages(void)
{
    MSG msg;
    int guard = 0;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && ++guard < 512) {
        if (msg.message == WM_QUIT)
            break;
    }
}

static int push_key(SDL_Scancode scancode, Uint32 type, Uint32 window_id)
{
    SDL_Event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.key.type = type;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.windowID = window_id;
    ev.key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = scancode;
    ev.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
    return SDL_PushEvent(&ev) == 1;
}

int main(void)
{
    WNDCLASSEXA cls;
    HWND hwnd, desktop;
    MSG msg;
    RECT rect;
    LONG previous;
    int cursor_count;

    /* Probe for a usable display before anything else. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SKIP - no usable display (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_Quit();

    if (nfsu2_win32_init(NULL) != 0) {
        printf("FAIL - shim init\n");
        return 1;
    }

    /* --- system metrics ------------------------------------------------- */
    CHECK(GetSystemMetrics(SM_CXSCREEN) > 0 && GetSystemMetrics(SM_CYSCREEN) > 0,
          "GetSystemMetrics reports a screen size (%dx%d)",
          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    desktop = GetDesktopWindow();
    CHECK(desktop != NULL && GetClientRect(desktop, &rect) &&
          rect.right == GetSystemMetrics(SM_CXSCREEN),
          "GetClientRect(GetDesktopWindow()) matches the screen width");

    /* --- class registration --------------------------------------------- */
    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = test_proc;
    cls.hCursor = LoadCursorA(NULL, IDC_ARROW);
    cls.lpszClassName = "NFSU2_TEST_CLASS";

    CHECK(RegisterClassExA(&cls) != 0, "RegisterClassExA");
    CHECK(RegisterClassExA(&cls) == 0 && GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
          "re-registering the same class fails with ERROR_CLASS_ALREADY_EXISTS");
    CHECK(cls.hCursor != NULL, "LoadCursorA(IDC_ARROW) returns a usable handle");

    /* --- window creation ------------------------------------------------ */
    rect.left = 0;
    rect.top = 0;
    rect.right = 800;
    rect.bottom = 600;
    CHECK(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE) &&
          rect.right - rect.left == 800 && rect.bottom - rect.top == 600,
          "AdjustWindowRect is an identity transform (client size in, same out)");

    hwnd = CreateWindowExA(0, cls.lpszClassName, "nfsu2 user32 selftest",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           800, 600, NULL, NULL, NULL, NULL);
    CHECK(hwnd != NULL, "CreateWindowExA");
    if (!hwnd) {
        printf("\nFAILED (cannot continue without a window)\n");
        return 1;
    }
    CHECK(g_wm_create_seen == 1, "WM_CREATE was delivered synchronously");
    CHECK((HWND)(void *)SDL_GetWindowFromID(SDL_GetWindowID((SDL_Window *)(void *)hwnd)) == hwnd,
          "HWND is the SDL_Window pointer (what DXVK's WSI requires)");

    CHECK(GetClientRect(hwnd, &rect) && rect.right == 800 && rect.bottom == 600,
          "GetClientRect returns the requested client size (%ldx%ld)",
          (long)rect.right, (long)rect.bottom);
    CHECK(GetWindowRect(hwnd, &rect) && rect.right - rect.left == 800,
          "GetWindowRect width matches");
    CHECK(!IsIconic(hwnd), "a freshly created window is not iconic");

    ShowWindow(hwnd, SW_SHOW);
    drain_messages();

    /* --- window longs --------------------------------------------------- */
    previous = SetWindowLongA(hwnd, GWL_USERDATA, 0x1234);
    CHECK(previous == 0 && GetWindowLongA(hwnd, GWL_USERDATA) == 0x1234,
          "GWL_USERDATA round-trips and returns the previous value");
    CHECK((GetWindowLongA(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW,
          "GWL_STYLE reports the creation style");

    /* --- posted messages ------------------------------------------------ */
    CHECK(PostMessageA(hwnd, WM_APP, 0xabc, 0xdef), "PostMessageA");
    CHECK(PeekMessageA(&msg, NULL, 0, 0, 0) && msg.message == WM_APP,
          "PeekMessageA without PM_REMOVE sees it");
    CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && msg.message == WM_APP &&
          msg.wParam == 0xabc && msg.lParam == 0xdef && msg.hwnd == hwnd,
          "PeekMessageA(PM_REMOVE) returns it intact");
    CHECK(DispatchMessageA(&msg) == 42 && g_dispatched_message == WM_APP &&
          g_dispatched_wparam == 0xabc,
          "DispatchMessageA reaches the WNDPROC and returns its result");

    PostMessageA(hwnd, WM_APP, 1, 0);
    CHECK(!PeekMessageA(&msg, NULL, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE),
          "a message-range filter excludes non-matching messages");
    CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && msg.message == WM_APP,
          "the filtered-out message is still queued afterwards");

    /* --- key translation ------------------------------------------------ */
    drain_messages();
    {
        Uint32 window_id = SDL_GetWindowID((SDL_Window *)(void *)hwnd);
        int pushed = push_key(SDL_SCANCODE_W, SDL_KEYDOWN, window_id);

        CHECK(pushed, "pushed a synthetic SDL_KEYDOWN for scancode W");
        CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) &&
              msg.message == WM_KEYDOWN && msg.wParam == 'W',
              "SDL scancode W translates to WM_KEYDOWN with VK 'W'");

        push_key(SDL_SCANCODE_W, SDL_KEYUP, window_id);
        CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) &&
              msg.message == WM_KEYUP && msg.wParam == 'W' &&
              (msg.lParam & (1L << 31)) != 0,
              "SDL_KEYUP translates to WM_KEYUP with the transition bit set");

        push_key(SDL_SCANCODE_ESCAPE, SDL_KEYDOWN, window_id);
        CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && msg.wParam == VK_ESCAPE,
              "escape translates to VK_ESCAPE");

        push_key(SDL_SCANCODE_LEFT, SDL_KEYDOWN, window_id);
        CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && msg.wParam == VK_LEFT,
              "left arrow translates to VK_LEFT");

        push_key(SDL_SCANCODE_F5, SDL_KEYDOWN, window_id);
        CHECK(PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && msg.wParam == VK_F5,
              "F5 translates to VK_F5");
    }

    /* --- cursor counter -------------------------------------------------- */
    cursor_count = ShowCursor(FALSE);
    CHECK(cursor_count == -1, "ShowCursor(FALSE) returns -1 (count, not boolean)");
    CHECK(ShowCursor(FALSE) == -2, "a second hide decrements again");
    CHECK(ShowCursor(TRUE) == -1 && ShowCursor(TRUE) == 0,
          "matching shows bring the count back to zero");

    CHECK(GetCursorPos((LPPOINT)&rect), "GetCursorPos");

    /* --- quit / destroy -------------------------------------------------- */
    drain_messages();
    PostQuitMessage(7);
    CHECK(!GetMessageA(&msg, NULL, 0, 0) && msg.message == WM_QUIT && msg.wParam == 7,
          "PostQuitMessage makes GetMessageA return FALSE with WM_QUIT");

    CHECK(DestroyWindow(hwnd), "DestroyWindow");
    CHECK(!GetClientRect(hwnd, &rect), "the HWND is invalid after DestroyWindow");
    CHECK(UnregisterClassA(cls.lpszClassName, NULL), "UnregisterClassA");

    nfsu2_win32_shutdown();
    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
