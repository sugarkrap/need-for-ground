/*
 * game_render.c - the game's own renderer code, driving a real DXVK device.
 *
 * Everything before this ran the ported functions against a *fake* device: a
 * vtable with one entry, checked for what the call carried (see
 * tests/game_functions_selftest.c). That proves the port is faithful; it does not
 * prove the call survives contact with a driver. This does, in a window, on the
 * GPU:
 *
 *   1. a window from our own CreateWindowExA, an IDirect3DDevice9 from DXVK
 *   2. speed2.exe mapped at its own base, imports resolved, a TEB installed
 *   3. the real device written into the game's own device global at 0x870974
 *   4. the PORTED FUN_005b7a30 called, and the state it sets read back out of the
 *      device with GetRenderState
 *   5. the ORIGINAL machine code at 0x5b7a30 called for the same thing, through a
 *      convention shim, and read back the same way
 *
 * Step 5 is the one worth explaining. The original was compiled against Windows'
 * COM ABI, where methods are __stdcall and the callee pops; DXVK Native's are
 * __cdecl. So the original cannot be handed DXVK's device pointer - it would push
 * arguments a cdecl callee never removes, and the stack would drift by 12 bytes
 * per call. It gets a shim object instead: our own vtable of __stdcall thunks that
 * forward to the real device. That is what a partially-ported binary needs in
 * front of every interface it still reaches from original code, and this is the
 * smallest working instance of it.
 *
 * Nothing here embeds game data: the exe is supplied at runtime.
 */
#include <nfsu2/win32_compat.h>

#include <nfsu2/d3d9_native.h>
#include <nfsu2/ghidra_types.h>
#include <nfsu2/pe_loader.h>
#include <nfsu2/seh.h>
#include <nfsu2/teb.h>

#include "game_functions.h"
#include "game_originals.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game's own global for its device, recovered from the code that uses it. */
#define GAME_DEVICE_GLOBAL 0x870974u

static IDirect3DDevice9 *g_device;
static D3DPRESENT_PARAMETERS g_present;
static int g_quit;

/* --- the convention shim, for original code ------------------------------- */

/*
 * A device-shaped object whose vtable is __stdcall. Only the methods original
 * code actually calls need filling in; the rest stay NULL, and a NULL entry is a
 * loud crash rather than a silent wrong answer, which is the right trade while the
 * set of reached methods is still being discovered.
 */
struct shim_device {
    const void **lpVtbl;          /* our own vtable, not DXVK's */
    IDirect3DDevice9 *real;       /* what the thunks forward to */
    int calls;
};

static struct shim_device g_shim;
static const void *g_shim_vtable[128];

static HRESULT __attribute__((stdcall)) shim_set_render_state(struct shim_device *self,
                                                             D3DRENDERSTATETYPE state,
                                                             DWORD value)
{
    self->calls++;
    /* The whole point: __stdcall in, __cdecl out. */
    return IDirect3DDevice9_SetRenderState(self->real, state, value);
}

/*
 * The slot index has to match the vtable the *game* was compiled against, which
 * is the documented D3D9 layout - and the offset the machine code uses is visible
 * in the disassembly (`call [ecx+0xe4]`). Deriving it from our own header keeps
 * the two in step, and the assert says so if they ever disagree.
 */
#define VTABLE_SLOT(member) (offsetof(IDirect3DDevice9Vtbl, member) / sizeof(void *))

static void shim_init(IDirect3DDevice9 *real)
{
    memset(g_shim_vtable, 0, sizeof(g_shim_vtable));
    g_shim_vtable[VTABLE_SLOT(SetRenderState)] = (const void *)shim_set_render_state;
    g_shim.lpVtbl = g_shim_vtable;
    g_shim.real = real;
    g_shim.calls = 0;
}

/* --- window ---------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_CLOSE:
        g_quit = 1;
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
            g_quit = 1;
        return 0;
    default:
        break;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

struct host_vertex {
    float x, y, z, rhw;
    D3DCOLOR colour;
};

/*
 * Something to look at, and a reason for the render state the game code sets to
 * be observable at all: a frame whose entire content is a Clear never reaches the
 * screen (see the README's black-window section).
 */
static void draw_quad(IDirect3DDevice9 *device, int width, int height, D3DCOLOR colour)
{
    struct host_vertex quad[4];
    float inset = 64.0f;
    int i;

    quad[0].x = inset;                quad[0].y = inset;
    quad[1].x = (float)width - inset; quad[1].y = inset;
    quad[2].x = inset;                quad[2].y = (float)height - inset;
    quad[3].x = (float)width - inset; quad[3].y = (float)height - inset;
    for (i = 0; i < 4; i++) {
        quad[i].z = 0.0f;
        quad[i].rhw = 1.0f;
        quad[i].colour = colour;
    }

    IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
}

/* --- the game code -------------------------------------------------------- */

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (cond) {                                                             \
            printf("ok   - " __VA_ARGS__);                                      \
            printf("\n");                                                       \
        } else {                                                                \
            printf("FAIL - " __VA_ARGS__);                                      \
            printf("\n");                                                       \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

/*
 * FUN_005b7a30 copies two columns of a matrix into game globals and then calls
 * SetRenderState(D3DRS_ZWRITEENABLE, 0). The argument is a pointer to a pointer to
 * the matrix, which is how the engine holds it.
 */
static void call_game_code(const struct nfsu2_pe_image *image)
{
    nfsu2_original_FUN_005b7a30 original = NFSU2_ORIGINAL(FUN_005b7a30, image);
    IDirect3DDevice9 **device_global = (IDirect3DDevice9 **)(uintptr_t)GAME_DEVICE_GLOBAL;
    undefined4 matrix[16];
    int object[1];
    DWORD state = 0;
    undefined4 result;
    size_t i;

    for (i = 0; i < 16; i++)
        matrix[i] = (undefined4)(0x2000u + i);
    object[0] = (int)(uintptr_t)matrix;

    printf("\n# the game's own renderer code, on a real device\n");
    CHECK(VTABLE_SLOT(SetRenderState) * sizeof(void *) == 0xe4,
          "SetRenderState is vtable offset 0x%zx, the offset the original calls",
          VTABLE_SLOT(SetRenderState) * sizeof(void *));

    /* --- the ported copy: DXVK's device directly, because both are __cdecl. */
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZWRITEENABLE, TRUE);
    *device_global = g_device;
    result = FUN_005b7a30(object);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_ZWRITEENABLE, &state);

    CHECK(result == 1, "the PORTED FUN_005b7a30 returned %lu", (unsigned long)result);
    CHECK(state == 0,
          "and DXVK now reports ZWRITEENABLE=%lu - the game's code reached the driver",
          (unsigned long)state);
    CHECK(*(unsigned int *)(uintptr_t)0x873370u == 0x2000u &&
          *(unsigned int *)(uintptr_t)0x8763c0u == 0x2001u,
          "with its globals written in the mapped image (0x%08x, 0x%08x)",
          *(unsigned int *)(uintptr_t)0x873370u, *(unsigned int *)(uintptr_t)0x8763c0u);

    /* --- the original machine code: through the __stdcall shim. */
    if (!original) {
        CHECK(0, "could not resolve the original at 0x5b7a30");
        return;
    }
    IDirect3DDevice9_SetRenderState(g_device, D3DRS_ZWRITEENABLE, TRUE);
    state = 0xffff;
    *device_global = (IDirect3DDevice9 *)&g_shim;
    result = original(object);
    IDirect3DDevice9_GetRenderState(g_device, D3DRS_ZWRITEENABLE, &state);
    *device_global = g_device;

    CHECK(g_shim.calls == 1, "the ORIGINAL machine code called our __stdcall shim %d time(s)",
          g_shim.calls);
    CHECK(state == 0,
          "and DXVK reports ZWRITEENABLE=%lu again - original code drove Vulkan",
          (unsigned long)state);
    CHECK(result == 1, "the original returned %lu, as the ported copy did",
          (unsigned long)result);
}

/* Name the unresolved imports rather than only counting them: this host links
 * DXVK and user32 but not dinput8, so which ones are missing is a property of the
 * link, and a bare count invites guessing. */
static void report_unresolved(const char *library, const char *symbol)
{
    printf("             unresolved: %s!%s\n", library, symbol);
}

static int parse_int(const char *s, int fallback)
{
    char *end;
    long v = strtol(s, &end, 10);
    return (*end == '\0' && v > 0 && v < (1 << 24)) ? (int)v : fallback;
}

int main(int argc, char **argv)
{
    int width = 1280, height = 720, frames = 600;
    const char *exe = getenv("NFSU2_EXE");
    struct nfsu2_pe_image image;
    struct nfsu2_pe_import_stats stats;
    char error[256] = "";
    WNDCLASSEXA cls;
    HWND hwnd;
    IDirect3D9 *d3d;
    D3DPRESENT_PARAMETERS pp;
    RECT client;
    HRESULT hr;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = parse_int(argv[++i], width);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = parse_int(argv[++i], height);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = parse_int(argv[++i], frames);
        else {
            fprintf(stderr, "usage: %s [--exe PATH] [--width N] [--height N] [--frames N]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!exe || !*exe) {
        fprintf(stderr, "no exe: pass --exe PATH or set NFSU2_EXE\n");
        return 2;
    }

    /* The TEB first: original code with a __try reaches fs:[0] on entry. */
    if (nfsu2_teb_install(error, sizeof(error)) != 0) {
        fprintf(stderr, "TEB: %s\n", error);
        return 1;
    }
    if (nfsu2_seh_install(error, sizeof(error)) != 0)
        fprintf(stderr, "SEH unavailable (%s) - faults will not be translated\n", error);

    if (nfsu2_pe_load(exe, &image, error, sizeof(error)) != 0) {
        fprintf(stderr, "%s: %s\n", exe, error);
        return 1;
    }
    printf("mapped     : %s\n", exe);
    nfsu2_pe_set_import_reporter(report_unresolved);
    nfsu2_pe_resolve_imports(&image, &stats);
    nfsu2_pe_set_import_reporter(NULL);
    printf("imports    : %d of %d resolved (%d by ordinal)\n",
           stats.resolved, stats.total, stats.by_ordinal);

    memset(&cls, 0, sizeof(cls));
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = wndproc;
    cls.hInstance = GetModuleHandleA(NULL);
    cls.lpszClassName = "nfsu2_game_render";
    if (!RegisterClassExA(&cls)) {
        fprintf(stderr, "RegisterClassExA failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    client.left = 0; client.top = 0; client.right = width; client.bottom = height;
    AdjustWindowRect(&client, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, cls.lpszClassName, "nfsu2-unwrap: game code on DXVK",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           client.right - client.left, client.bottom - client.top,
                           NULL, NULL, NULL, NULL);
    if (!hwnd) {
        fprintf(stderr, "CreateWindowExA failed: %lu\n", (unsigned long)GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    GetClientRect(hwnd, &client);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        fprintf(stderr, "Direct3DCreate9 returned NULL\n");
        return 1;
    }

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = (UINT)(client.right - client.left);
    pp.BackBufferHeight = (UINT)(client.bottom - client.top);
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device);
    if (FAILED(hr)) {
        fprintf(stderr, "CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    g_present = pp;
    shim_init(g_device);

    /* The game's code runs once, with the results printed, before the loop so the
     * window is up and the device is real when it does. */
    call_game_code(&image);

    printf("\nrendering %d frame(s) - the game's ZWRITEENABLE is re-applied every "
           "frame by its own code\n", frames);
    {
        MSG message;
        int presented = 0;

        while (!g_quit && presented < frames) {
            while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (g_quit)
                break;

            IDirect3DDevice9_Clear(g_device, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
                                   D3DCOLOR_XRGB(24, 32, 72), 1.0f, 0);
            if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_device))) {
                undefined4 matrix[16];
                int object[1];
                size_t k;

                for (k = 0; k < 16; k++)
                    matrix[k] = (undefined4)k;
                object[0] = (int)(uintptr_t)matrix;

                /* Ported game code, every frame, on the live device. */
                FUN_005b7a30(object);
                draw_quad(g_device, (int)pp.BackBufferWidth, (int)pp.BackBufferHeight,
                          D3DCOLOR_XRGB(232, 196, 64));
                IDirect3DDevice9_EndScene(g_device);
            }
            if (FAILED(IDirect3DDevice9_Present(g_device, NULL, NULL, NULL, NULL)))
                break;
            presented++;
        }
        printf("presented  : %d frame(s)\n", presented);
    }

    IDirect3DDevice9_Release(g_device);
    IDirect3D9_Release(d3d);
    DestroyWindow(hwnd);
    nfsu2_pe_unload(&image);

    printf("\n%s (%d check%s, %d failure%s)\n", g_failures ? "FAILED" : "PASSED",
           g_checks, g_checks == 1 ? "" : "s", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
