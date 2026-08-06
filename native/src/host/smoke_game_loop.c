/*
 * smoke_game_loop.c - the shape the ported game will actually have.
 *
 * Unlike smoke_d3d9.c, which drives SDL directly, this host touches SDL
 * nowhere: it registers a window class, creates a window, hands that HWND to
 * D3D9, and runs a PeekMessageA/DispatchMessageA loop - i.e. exactly the
 * sequence NFSU2's own startup performs. That makes it the real test of two
 * things the unit tests cannot cover:
 *
 *   1. HWND being an SDL_Window* holds all the way through DXVK's WSI layer
 *      when the window came from *our* CreateWindowExA rather than SDL_
 *      CreateWindow.
 *   2. Input and window events survive the SDL -> WM_* translation and arrive
 *      at a stdcall WNDPROC, which is the same convention boundary as
 *      CreateThread's entry point.
 *
 * Escape or closing the window exits, as it would in the game.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/d3d9_native.h>
#include <nfsu2/win32_shim.h>

#include "frame_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IDirect3DDevice9 *g_device;
static D3DPRESENT_PARAMETERS g_present;
static int g_running = 1;
static int g_frames_presented;
static int g_keys_seen;
static int g_mouse_moves_seen;
static int g_activate_seen;
/* Set by WM_SIZE, acted on in the frame loop - resetting a device from inside a
 * message handler would do it in the middle of a frame. */
static int g_pending_width;
static int g_pending_height;
static int g_capture_requested;
static int g_captures_written;

static LRESULT WINAPI window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CREATE:
        printf("wndproc    : WM_CREATE\n");
        return 0;
    case WM_KEYDOWN:
        g_keys_seen++;
        printf("wndproc    : WM_KEYDOWN vk=0x%02x%s\n", (unsigned)wparam,
               wparam == VK_ESCAPE ? " (escape -> quit)" : "");
        if (wparam == VK_ESCAPE)
            PostQuitMessage(0);
        /*
         * F12 writes the backbuffer to a PNG. On a system where the window shows
         * black despite correct rendering (see NOTES.md), this is the only way to
         * see what the GPU actually produced while poking at the window.
         */
        if (wparam == VK_F12)
            g_capture_requested = 1;
        return 0;
    case WM_MOUSEMOVE:
        g_mouse_moves_seen++;
        return 0;
    case WM_ACTIVATEAPP:
        g_activate_seen++;
        printf("wndproc    : WM_ACTIVATEAPP active=%d\n", (int)wparam);
        return 0;
    case WM_SIZE: {
        int width = (int)(lparam & 0xffff);
        int height = (int)((lparam >> 16) & 0xffff);

        printf("wndproc    : WM_SIZE %dx%d%s\n", width, height,
               wparam == SIZE_MINIMIZED ? " (minimised)" : "");
        /*
         * A resize means the backbuffer no longer matches the window. Without a
         * Reset the swapchain and the backbuffer disagree about size and the
         * window goes black - which is exactly what Alt+Enter produced before
         * this was handled. Queued rather than done here: Reset must not happen
         * part-way through a frame.
         */
        if (wparam != SIZE_MINIMIZED && width > 0 && height > 0) {
            g_pending_width = width;
            g_pending_height = height;
        }
        return 0;
    }
    case WM_CLOSE:
        printf("wndproc    : WM_CLOSE\n");
        g_running = 0;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_running = 0;
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
}

static int parse_int(const char *s, int fallback)
{
    char *end;
    long v = strtol(s, &end, 10);
    return (*end == '\0' && v > 0 && v < (1 << 24)) ? (int)v : fallback;
}

int main(int argc, char **argv)
{
    int width = 1280, height = 720, frames = 180;
    int i;
    int immediate = 0;
    int present_workaround = 0;
    WNDCLASSEXA cls;
    HWND hwnd;
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS pp;
    D3DADAPTER_IDENTIFIER9 ident;
    HRESULT hr;
    RECT client;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc)
            width = parse_int(argv[++i], width);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            height = parse_int(argv[++i], height);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = parse_int(argv[++i], frames);
        else if (!strcmp(argv[i], "--immediate"))
            immediate = 1;
        else if (!strcmp(argv[i], "--present-workaround"))
            present_workaround = 1;
        else {
            fprintf(stderr, "usage: %s [--width N] [--height N] [--frames N] "
                            "[--immediate]\n", argv[0]);
            return 2;
        }
    }

    /*
     * DXVK Native presents swapchain images that were never rendered into on this
     * system: the window is black (or, with more images, flashes) while the
     * backbuffer verifiably holds the right pixels - press F12 in the interactive
     * host, or use --readback-png, to see it.
     *
     * `dxvk.numBackBuffers = 3` changes the symptom rather than fixing it: content
     * then reaches the screen in roughly one frame in ten, i.e. it flashes. That is
     * not a default worth shipping - a flashing window is worse than a black one -
     * so it is behind --present-workaround for diagnosis. Raising
     * D3DPRESENT_PARAMETERS.BackBufferCount does nothing; it is the Vulkan
     * swapchain image count that matters, which is what points at DXVK rather than
     * at anything here. NOTES.md records what has been ruled out.
     */
    if (present_workaround)
        setenv("DXVK_CONFIG", "dxvk.numBackBuffers = 3", 0);

    nfsu2_win32_set_command_line(argc, argv);
    if (nfsu2_win32_init(NULL) != 0) {
        fprintf(stderr, "win32 shim init failed\n");
        return 1;
    }

    printf("screen     : %dx%d (GetSystemMetrics)\n",
           GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    cls.lpfnWndProc = window_proc;
    cls.hCursor = LoadCursorA(NULL, IDC_ARROW);
    cls.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    cls.lpszClassName = "NFSU2_NATIVE_WINDOW";
    if (!RegisterClassExA(&cls)) {
        fprintf(stderr, "RegisterClassExA failed: %lu "
                        "(no display? try under a compositor)\n",
                (unsigned long)GetLastError());
        return 1;
    }

    /* The Win32 idiom: adjust a client rect, then create with the result. */
    client.left = 0;
    client.top = 0;
    client.right = width;
    client.bottom = height;
    AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd = CreateWindowExA(0, cls.lpszClassName, "nfsu2-unwrap: native window + DXVK",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           client.right - client.left, client.bottom - client.top,
                           NULL, NULL, NULL, NULL);
    if (!hwnd) {
        fprintf(stderr, "CreateWindowExA failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    GetClientRect(hwnd, &client);
    printf("client     : %ldx%ld (requested %dx%d)\n",
           (long)(client.right - client.left), (long)(client.bottom - client.top),
           width, height);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "Direct3DCreate9 returned NULL\n");
        DestroyWindow(hwnd);
        return 1;
    }

    memset(&ident, 0, sizeof(ident));
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &ident)))
        printf("adapter    : %s\n", ident.Description);

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = (UINT)(client.right - client.left);
    pp.BackBufferHeight = (UINT)(client.bottom - client.top);
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    /*
     * One, as a real game asks for. Raising this does NOT work around the black
     * window - the D3D9 backbuffer count is not what is wrong; the Vulkan
     * swapchain image count is. See the DXVK_CONFIG below.
     */
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd; /* our HWND, straight into DXVK's WSI */
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    /*
     * Vsync, not IMMEDIATE. A real game presents on the refresh; IMMEDIATE here
     * ran at ~4300 fps, which is not a realistic test and turned out to matter -
     * see --immediate to compare.
     */
    pp.PresentationInterval = immediate ? D3DPRESENT_INTERVAL_IMMEDIATE
                                       : D3DPRESENT_INTERVAL_ONE;

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device);
    g_present = pp;
    if (FAILED(hr)) {
        fprintf(stderr, "CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        IDirect3D9_Release(d3d);
        DestroyWindow(hwnd);
        return 1;
    }

    /* The game's frame loop: drain messages, then render. */
    {
        DWORD started = GetTickCount();
        DWORD last_report = started;
        int frames_at_report = 0;

        while (g_running && g_frames_presented < frames) {
            MSG msg;
            DWORD now;
            float phase;

            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_running = 0;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            if (!g_running)
                break;

            /* Apply a queued resize before touching the device. */
            if (g_pending_width && g_pending_height) {
                g_present.BackBufferWidth = (UINT)g_pending_width;
                g_present.BackBufferHeight = (UINT)g_pending_height;
                g_pending_width = 0;
                g_pending_height = 0;

                hr = IDirect3DDevice9_Reset(g_device, &g_present);
                printf("device     : Reset to %ux%u -> 0x%08lx\n",
                       g_present.BackBufferWidth, g_present.BackBufferHeight,
                       (unsigned long)hr);
                if (FAILED(hr))
                    break;
            }

            /*
             * The lost-device dance, which a real game must do and which a
             * compositor triggers in practice (see NOTES.md on the letterbox
             * patch): while lost, do not render; when it says NOTRESET, reset.
             */
            hr = IDirect3DDevice9_TestCooperativeLevel(g_device);
            if (hr == D3DERR_DEVICELOST) {
                Sleep(20);
                continue;
            }
            if (hr == D3DERR_DEVICENOTRESET) {
                printf("device     : lost, resetting\n");
                if (FAILED(IDirect3DDevice9_Reset(g_device, &g_present)))
                    break;
            }

            /*
             * Animated from the clock rather than from the frame index: a
             * frame-index ramp over a large --frames sits at its starting colour
             * for minutes, which looks exactly like a hung renderer.
             */
            now = GetTickCount();
            phase = (float)((now - started) % 3000u) / 3000.0f;

            IDirect3DDevice9_Clear(g_device, 0, NULL,
                                   D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                                   D3DCOLOR_XRGB((int)(60.0f + 195.0f * phase), 40,
                                                 (int)(195.0f - 155.0f * phase)),
                                   1.0f, 0);
            IDirect3DDevice9_BeginScene(g_device);
            IDirect3DDevice9_EndScene(g_device);
            if (FAILED(IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL)))
                break;
            g_frames_presented++;

            if (g_capture_requested) {
                char path[64];

                g_capture_requested = 0;
                snprintf(path, sizeof(path), "/tmp/nfsu2-frame-%d.png", ++g_captures_written);
                printf("capture    : %s\n",
                       nfsu2_capture_png(g_device, path) == 0 ? path : "failed");
            }

            /* Liveness in the title bar, through our own SetWindowTextA. */
            if (now - last_report >= 1000) {
                char title[128];

                snprintf(title, sizeof(title),
                         "nfsu2 native + DXVK - %ux%u - %d fps - %d frames - F12 capture, Esc quit",
                         g_present.BackBufferWidth, g_present.BackBufferHeight,
                         g_frames_presented - frames_at_report, g_frames_presented);
                SetWindowTextA(hwnd, title);
                last_report = now;
                frames_at_report = g_frames_presented;
            }
        }
    }

    printf("presented  : %d frames (%d-bit build)\n", g_frames_presented,
           (int)(sizeof(void *) * 8));
    printf("events     : %d key, %d mouse-move, %d activate\n",
           g_keys_seen, g_mouse_moves_seen, g_activate_seen);

    IDirect3DDevice9_Release(g_device);
    IDirect3D9_Release(d3d);
    if (g_running)
        DestroyWindow(hwnd);
    UnregisterClassA(cls.lpszClassName, NULL);
    nfsu2_win32_shutdown();
    return g_frames_presented > 0 ? 0 : 1;
}
