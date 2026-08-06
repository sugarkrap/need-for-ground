/*
 * d3d9_abi_selftest.c - checks that d3d9_native.h really produces the calling
 * convention DXVK Native expects, without needing DXVK (or a GPU) present.
 *
 * It builds a fake IDirect3DDevice9 whose vtable slots point at plain C
 * functions and calls them through Wine's own C dispatch macros. Two things
 * are then true only if the convention is right:
 *
 *   - the arguments arrive intact (a stdcall/cdecl mismatch shifts the stack);
 *   - the caller's stack is still balanced after many calls in a loop, which a
 *     4-or-8-byte-per-call drift would destroy.
 *
 * A convention mismatch against real DXVK cannot be proven from inside our own
 * process - for that, run nfsu2-smoke-d3d9, which drives the real library.
 * What this test does catch is a regression in the header itself, which is the
 * failure mode most likely to be introduced by accident later.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/d3d9_native.h>

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_call_count;
static D3DVIEWPORT9 g_last_viewport;
static IDirect3DDevice9 *g_last_self;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            printf("ok   - %s\n", msg);                     \
        } else {                                            \
            printf("FAIL - %s (line %d)\n", msg, __LINE__); \
            g_failures++;                                   \
        }                                                   \
    } while (0)

/*
 * Note the absence of WINAPI/STDMETHODCALLTYPE: inside the world
 * d3d9_native.h establishes, a D3D9 vtable slot is a plain (cdecl on i386,
 * SysV on x86_64) function, which is exactly how DXVK Native compiles them.
 * If this file compiled with a stdcall macro still in effect, the assignment
 * into the vtable below would not type-check.
 */
static HRESULT fake_set_viewport(IDirect3DDevice9 *self, const D3DVIEWPORT9 *vp)
{
    g_call_count++;
    g_last_self = self;
    g_last_viewport = *vp;
    return D3D_OK;
}

static HRESULT fake_clear(IDirect3DDevice9 *self, DWORD count, const D3DRECT *rects,
                          DWORD flags, D3DCOLOR colour, float z, DWORD stencil)
{
    (void)self; (void)rects;
    g_call_count++;
    /* Encode every argument into the viewport struct so the caller can verify
     * that a seven-argument call marshals correctly too. */
    g_last_viewport.X = count;
    g_last_viewport.Y = flags;
    g_last_viewport.Width = colour;
    g_last_viewport.Height = stencil;
    g_last_viewport.MinZ = z;
    return D3D_OK;
}

int main(void)
{
    IDirect3DDevice9Vtbl vtbl;
    IDirect3DDevice9 device;
    IDirect3DDevice9 *dev = &device;
    D3DVIEWPORT9 vp = { 11, 22, 2560, 1080, 0.25f, 0.75f };
    volatile unsigned canary_before, canary_after;
    unsigned char stack_probe[64];
    int i;

    memset(&vtbl, 0, sizeof(vtbl));
    vtbl.SetViewport = fake_set_viewport;
    vtbl.Clear = fake_clear;
    device.lpVtbl = &vtbl;

    memset(stack_probe, 0x5a, sizeof(stack_probe));
    canary_before = 0xc0ffee;

    CHECK(IDirect3DDevice9_SetViewport(dev, &vp) == D3D_OK,
          "call through a D3D9 vtable slot returns correctly");
    CHECK(g_last_self == dev, "the `this` pointer arrives as the first argument");
    CHECK(g_last_viewport.X == 11 && g_last_viewport.Y == 22 &&
          g_last_viewport.Width == 2560 && g_last_viewport.Height == 1080,
          "D3DVIEWPORT9 contents survive the call");
    CHECK(g_last_viewport.MinZ == 0.25f && g_last_viewport.MaxZ == 0.75f,
          "float fields survive the call");

    IDirect3DDevice9_Clear(dev, 3, NULL, D3DCLEAR_TARGET, 0xdeadbeef, 0.5f, 7);
    CHECK(g_last_viewport.X == 3 && g_last_viewport.Y == D3DCLEAR_TARGET &&
          g_last_viewport.Width == 0xdeadbeef && g_last_viewport.Height == 7 &&
          g_last_viewport.MinZ == 0.5f,
          "a seven-argument vtable call marshals every argument");

    /* Stack-drift check: 100k calls would move esp by ~800KB if the caller and
     * callee disagreed about who pops the arguments. */
    for (i = 0; i < 100000; i++)
        IDirect3DDevice9_SetViewport(dev, &vp);
    canary_after = canary_before;

    CHECK(g_call_count == 100002, "all 100002 calls were dispatched");
    CHECK(canary_after == 0xc0ffee, "caller's stack frame is intact after 100k calls");
    for (i = 0; i < (int)sizeof(stack_probe); i++) {
        if (stack_probe[i] != 0x5a) {
            CHECK(0, "stack probe buffer was not clobbered");
            break;
        }
    }
    if (i == (int)sizeof(stack_probe))
        CHECK(1, "stack probe buffer was not clobbered");

    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
