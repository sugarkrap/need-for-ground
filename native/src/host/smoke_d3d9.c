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

/*
 * Minimal PNG writer: one IDAT of uncompressed deflate "stored" blocks, so no
 * zlib dependency. Not small output, but this is a diagnostic, and a diagnostic
 * that needs a new dependency tends not to get used.
 */
static unsigned long crc32_of(const unsigned char *data, size_t length, unsigned long crc)
{
    static unsigned long table[256];
    size_t i;

    if (!table[1]) {
        unsigned long c;
        int n, k;
        for (n = 0; n < 256; n++) {
            c = (unsigned long)n;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    }
    crc ^= 0xffffffffUL;
    for (i = 0; i < length; i++)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

static void put32(unsigned char *out, unsigned long value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t length)
{
    unsigned char header[8];
    unsigned char crc[4];
    unsigned long value;

    put32(header, (unsigned long)length);
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, f) != 8)
        return -1;
    if (length && fwrite(data, 1, length, f) != length)
        return -1;
    value = crc32_of((const unsigned char *)type, 4, 0);
    if (length)
        value = crc32_of(data, length, value);
    put32(crc, value);
    return fwrite(crc, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png(const char *path, const unsigned char *pixels, int width, int height,
                     int pitch)
{
    static const unsigned char signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw;
    unsigned char *stream;
    size_t raw_size = (size_t)(width * 3 + 1) * (size_t)height;
    size_t stream_size;
    size_t offset = 0;
    size_t written = 0;
    unsigned long adler_a = 1, adler_b = 0;
    FILE *f;
    int y, x;
    int result = -1;

    raw = malloc(raw_size);
    if (!raw)
        return -1;

    /* D3DFMT_X8R8G8B8 is BGRX in memory; PNG wants RGB. */
    for (y = 0; y < height; y++) {
        unsigned char *row = raw + (size_t)y * (size_t)(width * 3 + 1);
        const unsigned char *source = pixels + (size_t)y * (size_t)pitch;

        row[0] = 0; /* filter: none */
        for (x = 0; x < width; x++) {
            row[1 + x * 3 + 0] = source[x * 4 + 2];
            row[1 + x * 3 + 1] = source[x * 4 + 1];
            row[1 + x * 3 + 2] = source[x * 4 + 0];
        }
    }

    for (offset = 0; offset < raw_size; offset++) {
        adler_a = (adler_a + raw[offset]) % 65521;
        adler_b = (adler_b + adler_a) % 65521;
    }

    /* zlib header + stored deflate blocks (max 65535 bytes each) + adler32. */
    stream_size = 2 + ((raw_size + 65534) / 65535) * 5 + raw_size + 4;
    stream = malloc(stream_size);
    if (!stream) {
        free(raw);
        return -1;
    }
    stream[written++] = 0x78;
    stream[written++] = 0x01;
    offset = 0;
    while (offset < raw_size) {
        size_t block = raw_size - offset > 65535 ? 65535 : raw_size - offset;
        int final = (offset + block >= raw_size);

        stream[written++] = (unsigned char)(final ? 1 : 0);
        stream[written++] = (unsigned char)(block & 0xff);
        stream[written++] = (unsigned char)(block >> 8);
        stream[written++] = (unsigned char)(~block & 0xff);
        stream[written++] = (unsigned char)((~block >> 8) & 0xff);
        memcpy(stream + written, raw + offset, block);
        written += block;
        offset += block;
    }
    put32(stream + written, (adler_b << 16) | adler_a);
    written += 4;

    f = fopen(path, "wb");
    if (f) {
        put32(ihdr, (unsigned long)width);
        put32(ihdr + 4, (unsigned long)height);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 2;  /* colour type: truecolour */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        if (fwrite(signature, 1, 8, f) == 8 &&
            write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
            write_chunk(f, "IDAT", stream, written) == 0 &&
            write_chunk(f, "IEND", NULL, 0) == 0)
            result = 0;
        fclose(f);
    }
    free(stream);
    free(raw);
    return result;
}

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
    int immediate = 0;
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
        IDirect3DSurface9 *back = NULL;
        IDirect3DSurface9 *system = NULL;

        if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                                     &back)) &&
            SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(
                device, (UINT)width, (UINT)height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                &system, NULL))) {
            hr = IDirect3DDevice9_GetRenderTargetData(device, back, system);
            if (SUCCEEDED(hr)) {
                D3DLOCKED_RECT locked;

                if (SUCCEEDED(IDirect3DSurface9_LockRect(system, &locked, NULL,
                                                         D3DLOCK_READONLY))) {
                    const unsigned char *row =
                        (const unsigned char *)locked.pBits + (height / 2) * locked.Pitch;
                    const unsigned char *pixel = row + (width / 2) * 4;

                    printf("readback   : centre pixel B=%u G=%u R=%u  (cleared to "
                           "B=%u G=%u R=%u)\n",
                           pixel[0], pixel[1], pixel[2],
                           (unsigned)(last_colour & 0xff),
                           (unsigned)((last_colour >> 8) & 0xff),
                           (unsigned)((last_colour >> 16) & 0xff));
                    printf("readback   : %s\n",
                           (pixel[0] == (last_colour & 0xff) &&
                            pixel[1] == ((last_colour >> 8) & 0xff) &&
                            pixel[2] == ((last_colour >> 16) & 0xff))
                               ? "MATCH - the GPU rendered what we asked for"
                               : "MISMATCH - rendering itself is wrong");
                    /*
                     * Optionally dump the whole surface as a PNG. A pixel value
                     * printed to a terminal is only as convincing as the person
                     * reading it; a file can be opened. It also gives the port a
                     * way to compare rendered output frame by frame later,
                     * independently of any compositor.
                     */
                    if (readback_png) {
                        if (write_png(readback_png, (const unsigned char *)locked.pBits,
                                      width, height, locked.Pitch) == 0)
                            printf("readback   : wrote %s (%dx%d)\n", readback_png,
                                   width, height);
                        else
                            printf("readback   : could not write %s\n", readback_png);
                    }
                    IDirect3DSurface9_UnlockRect(system);
                }
            } else {
                printf("readback   : GetRenderTargetData failed 0x%08lx\n",
                       (unsigned long)hr);
            }
        }
        if (system)
            IDirect3DSurface9_Release(system);
        if (back)
            IDirect3DSurface9_Release(back);
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
