/*
 * ddraw.c - DirectDrawCreate, as an honest failure.
 *
 * The game imports this but does not render with it: DirectDraw was how a 2004
 * title asked "is DirectX installed, and what does the display adapter say it
 * can do" before committing to D3D9, and NFSU2's startup does exactly that check
 * before its real Direct3DCreate9 path.
 *
 * Implementing DirectDraw would mean a second rendering API on top of Vulkan for
 * no visible benefit. Failing cleanly makes the caller fall through to the D3D9
 * path, which is the one that works here.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <ddraw.h>

HRESULT WINAPI DirectDrawCreate(GUID *driver, LPDIRECTDRAW *out, IUnknown *outer)
{
    (void)driver; (void)outer;

    NFSU2_STUB("DirectDrawCreate (D3D9 is the renderer; see src/host and DIRECTX_SCOPE.md)");
    if (out)
        *out = NULL;
    return DDERR_NODIRECTDRAWSUPPORT;
}

HRESULT WINAPI DirectDrawCreateEx(GUID *driver, LPVOID *out, REFIID riid, IUnknown *outer)
{
    (void)driver; (void)riid; (void)outer;

    NFSU2_STUB("DirectDrawCreateEx (D3D9 is the renderer)");
    if (out)
        *out = NULL;
    return DDERR_NODIRECTDRAWSUPPORT;
}
