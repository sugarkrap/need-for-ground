/*
 * smoke_d3d9.c - end-to-end proof that the target shape works:
 * a native Linux ELF, no Wine loader, talking D3D9 to DXVK Native, which
 * talks Vulkan to the real driver.
 *
 * It creates a window, creates a D3D9 device on it, clears and presents a
 * number of frames, and prints what adapter DXVK reported. If this runs, the
 * ABI decisions in d3d9_native.h are correct on this machine - a mismatch
 * shows up immediately as a crash or garbage adapter string, because every
 * call here goes through the same vtable path the game's renderer will.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/d3d9_native.h>
#include <nfsu2/win32_shim.h>

#include "frame_capture.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_int(const char *s, int fallback)
{
    char *end;
    long v = strtol(s, &end, 10);
    return (*end == '\0' && v > 0 && v < (1 << 24)) ? (int)v : fallback;
}

int main(int argc, char **argv)
{
    int width = 1280, height = 720, frames = 120;
    int readback = 0;
    const char *readback_png = NULL;
    /*
     * Backbuffer size, when it should differ from the window. DXVK can present a
     * backbuffer that exactly matches the swapchain by aliasing the image and
     * skipping its blit shader; forcing a mismatch forces the blit path. That
     * distinction is the point of this flag - see NOTES.md on the black window.
     */
    int backbuffer_width = 0, backbuffer_height = 0;
    int immediate = 0;
    int present_workaround = 0;
    /*
     * Force the rendering to be flushed and waited for before Present, by reading
     * one pixel back. If the window then shows content consistently, the black
     * window is an ordering problem between the app's rendering and DXVK's
     * swapchain blit - not a missing barrier, which validation has now ruled out.
     */
    int sync_each_frame = 0;
    int i;
    SDL_Window *window;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DADAPTER_IDENTIFIER9 ident;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    int presented = 0;
    D3DCOLOR last_colour = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc)
            width = parse_int(argv[++i], width);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            height = parse_int(argv[++i], height);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = parse_int(argv[++i], frames);
        else if (!strcmp(argv[i], "--readback"))
            readback = 1;
        else if (!strcmp(argv[i], "--readback-png") && i + 1 < argc) {
            readback = 1;
            readback_png = argv[++i];
        }
        else if (!strcmp(argv[i], "--immediate"))
            immediate = 1;
        else if (!strcmp(argv[i], "--present-workaround"))
            present_workaround = 1;
        else if (!strcmp(argv[i], "--sync-each-frame"))
            sync_each_frame = 1;
        else if (!strcmp(argv[i], "--backbuffer") && i + 2 < argc) {
            backbuffer_width = parse_int(argv[++i], 0);
            backbuffer_height = parse_int(argv[++i], 0);
        }
        else {
            fprintf(stderr, "usage: %s [--width N] [--height N] [--frames N]\n", argv[0]);
            return 2;
        }
    }

    nfsu2_win32_set_command_line(argc, argv);
    if (nfsu2_win32_init(NULL) != 0) {
        fprintf(stderr, "win32 shim init failed\n");
        return 1;
    }

    /*
     * DXVK Native is built with every WSI backend it could find (SDL3, SDL2,
     * GLFW) and picks one at load time; if it picks a different toolkit than
     * the one that owns our window, it throws a DxvkError out of
     * Direct3DCreate9 with no usable message. Our window is SDL2, so say so -
     * without overriding a deliberate choice from the environment.
     */
    setenv("DXVK_WSI_DRIVER", "SDL2", 0);

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("nfsu2-unwrap: native ELF + DXVK",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width, height, SDL_WINDOW_VULKAN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "Direct3DCreate9 returned NULL "
                        "(is DXVK's WSI backend the same one SDL initialised? "
                        "try DXVK_WSI_DRIVER=SDL2)\n");
        goto fail_window;
    }

    memset(&ident, 0, sizeof(ident));
    hr = IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &ident);
    if (FAILED(hr)) {
        fprintf(stderr, "GetAdapterIdentifier failed: 0x%08lx\n", (unsigned long)hr);
        goto fail_d3d;
    }
    printf("adapter    : %s\n", ident.Description);
    printf("driver     : %s\n", ident.Driver);
    printf("vendor/dev : %04x:%04x\n", (unsigned)ident.VendorId, (unsigned)ident.DeviceId);

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = (UINT)(backbuffer_width ? backbuffer_width : width);
    pp.BackBufferHeight = (UINT)(backbuffer_height ? backbuffer_height : height);
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    /*
     * One, as a real game asks for. Raising this does NOT work around the black
     * window - the D3D9 backbuffer count is not what is wrong; the Vulkan
     * swapchain image count is. See the DXVK_CONFIG below.
     */
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = NFSU2_HWND_FROM_WSI_WINDOW(window);
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

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                                 pp.hDeviceWindow,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                 &pp, &device);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        goto fail_d3d;
    }

    for (i = 0; i < frames; i++) {
        SDL_Event ev;
        D3DVIEWPORT9 vp;
        D3DCOLOR colour;
        float t = (float)i / (float)(frames > 1 ? frames - 1 : 1);

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                goto done;
        }

        /* SetViewport is here on purpose: it is the exact call whose stack
         * cleanup differs between stdcall and cdecl, i.e. the one that would
         * corrupt the stack first if d3d9_native.h were wrong. */
        vp.X = 0;
        vp.Y = 0;
        vp.Width = (DWORD)width;
        vp.Height = (DWORD)height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
        hr = IDirect3DDevice9_SetViewport(device, &vp);
        if (FAILED(hr)) {
            fprintf(stderr, "SetViewport failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }

        colour = D3DCOLOR_XRGB((int)(t * 255.0f), 40, (int)((1.0f - t) * 255.0f));
        last_colour = colour;
        IDirect3DDevice9_Clear(device, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                               colour, 1.0f, 0);
        IDirect3DDevice9_BeginScene(device);
        IDirect3DDevice9_EndScene(device);
        if (sync_each_frame) {
            unsigned char probe[4];

            nfsu2_capture_pixel(device, 0, 0, probe);
        }

        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) {
            fprintf(stderr, "Present failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
        presented++;
    }

    /*
     * Read the rendered pixels back from the GPU. This separates two failures
     * that look identical on screen: "we rendered nothing" and "we rendered
     * correctly but the compositor is not showing it". Without it, a black
     * window is unattributable - and a frame counter proves neither.
     */
    if (readback) {
        unsigned char pixel[4];

        if (nfsu2_capture_pixel(device, width / 2, height / 2, pixel) == 0) {
            printf("readback   : centre pixel B=%u G=%u R=%u  (cleared to B=%u G=%u R=%u)\n",
                   pixel[0], pixel[1], pixel[2],
                   (unsigned)(last_colour & 0xff), (unsigned)((last_colour >> 8) & 0xff),
                   (unsigned)((last_colour >> 16) & 0xff));
            printf("readback   : %s\n",
                   (pixel[0] == (last_colour & 0xff) &&
                    pixel[1] == ((last_colour >> 8) & 0xff) &&
                    pixel[2] == ((last_colour >> 16) & 0xff))
                       ? "MATCH - the GPU rendered what we asked for"
                       : "MISMATCH - rendering itself is wrong");
        } else {
            printf("readback   : could not read the backbuffer\n");
        }
        if (readback_png) {
            printf("readback   : %s\n",
                   nfsu2_capture_png(device, readback_png) == 0
                       ? readback_png : "could not write the PNG");
        }
    }

done:
    printf("presented  : %d frames at %dx%d (%d-bit build)\n",
           presented, width, height, (int)(sizeof(void *) * 8));
    printf("tick count : %lu ms\n", (unsigned long)GetTickCount());

    IDirect3DDevice9_Release(device);
    IDirect3D9_Release(d3d);
    SDL_DestroyWindow(window);
    SDL_Quit();
    nfsu2_win32_shutdown();
    return presented > 0 ? 0 : 1;

fail_d3d:
    IDirect3D9_Release(d3d);
fail_window:
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
}
