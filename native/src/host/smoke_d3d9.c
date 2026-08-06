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
    int i;
    SDL_Window *window;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DADAPTER_IDENTIFIER9 ident;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    int presented = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc)
            width = parse_int(argv[++i], width);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            height = parse_int(argv[++i], height);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = parse_int(argv[++i], frames);
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
    pp.BackBufferWidth = width;
    pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = NFSU2_HWND_FROM_WSI_WINDOW(window);
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

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
        IDirect3DDevice9_Clear(device, 0, NULL,
                               D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                               colour, 1.0f, 0);
        IDirect3DDevice9_BeginScene(device);
        IDirect3DDevice9_EndScene(device);
        hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) {
            fprintf(stderr, "Present failed: 0x%08lx\n", (unsigned long)hr);
            break;
        }
        presented++;
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
