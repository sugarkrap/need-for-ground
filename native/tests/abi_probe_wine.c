/*
 * abi_probe_wine.c - dump the D3D9 layout as Wine's headers describe it.
 *
 * Paired with abi_probe_dxvk.cpp (same dump, from DXVK Native's own headers)
 * and compared by tools/abi_diff.py. The whole native port rests on those two
 * views being identical, so it is worth checking mechanically rather than
 * assuming - a silent divergence here would look like random rendering
 * corruption much later.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/d3d9_native.h>

#include <stddef.h>
#include <stdio.h>

#define DUMP_SIZE(type)        printf("size %s %zu\n", #type, sizeof(type))
#define DUMP_OFFSET(type, mem) printf("offset %s.%s %zu\n", #type, #mem, offsetof(type, mem))
#define DUMP_VALUE(name)       printf("value %s %ld\n", #name, (long)(name))

int main(void)
{
    printf("pointer_size %zu\n", sizeof(void *));

    DUMP_SIZE(D3DMATRIX);
    DUMP_SIZE(D3DVIEWPORT9);
    DUMP_SIZE(D3DPRESENT_PARAMETERS);
    DUMP_SIZE(D3DADAPTER_IDENTIFIER9);
    DUMP_SIZE(D3DCAPS9);
    DUMP_SIZE(D3DDISPLAYMODE);
    DUMP_SIZE(D3DSURFACE_DESC);
    DUMP_SIZE(D3DVOLUME_DESC);
    DUMP_SIZE(D3DLOCKED_RECT);
    DUMP_SIZE(D3DVERTEXELEMENT9);
    DUMP_SIZE(D3DMATERIAL9);
    DUMP_SIZE(D3DLIGHT9);
    DUMP_SIZE(D3DRECT);
    DUMP_SIZE(D3DGAMMARAMP);

    DUMP_OFFSET(D3DPRESENT_PARAMETERS, BackBufferWidth);
    DUMP_OFFSET(D3DPRESENT_PARAMETERS, BackBufferFormat);
    DUMP_OFFSET(D3DPRESENT_PARAMETERS, hDeviceWindow);
    DUMP_OFFSET(D3DPRESENT_PARAMETERS, Windowed);
    DUMP_OFFSET(D3DPRESENT_PARAMETERS, EnableAutoDepthStencil);
    DUMP_OFFSET(D3DPRESENT_PARAMETERS, PresentationInterval);
    DUMP_OFFSET(D3DVIEWPORT9, MinZ);
    DUMP_OFFSET(D3DVIEWPORT9, MaxZ);
    DUMP_OFFSET(D3DCAPS9, MaxTextureWidth);
    DUMP_OFFSET(D3DCAPS9, MaxVertexShaderConst);
    DUMP_OFFSET(D3DCAPS9, PixelShaderVersion);
    DUMP_OFFSET(D3DADAPTER_IDENTIFIER9, VendorId);
    DUMP_OFFSET(D3DSURFACE_DESC, Width);
    DUMP_OFFSET(D3DLIGHT9, Attenuation0);
    DUMP_OFFSET(D3DMATERIAL9, Power);

    /* Enum/constant values the game passes across the boundary as literals. */
    DUMP_VALUE(D3DFMT_X8R8G8B8);
    DUMP_VALUE(D3DFMT_A8R8G8B8);
    DUMP_VALUE(D3DFMT_D24S8);
    DUMP_VALUE(D3DFMT_DXT1);
    DUMP_VALUE(D3DTS_VIEW);
    DUMP_VALUE(D3DTS_PROJECTION);
    DUMP_VALUE(D3DRS_ALPHABLENDENABLE);
    DUMP_VALUE(D3DRS_SCISSORTESTENABLE);
    DUMP_VALUE(D3DTSS_TEXCOORDINDEX);
    DUMP_VALUE(D3DSAMP_ADDRESSU);
    DUMP_VALUE(D3DPT_TRIANGLELIST);
    DUMP_VALUE(D3DDECLTYPE_FLOAT3);
    DUMP_VALUE(D3DPOOL_MANAGED);
    DUMP_VALUE(D3DUSAGE_RENDERTARGET);
    DUMP_VALUE(D3DCLEAR_TARGET);
    DUMP_VALUE(D3DSWAPEFFECT_DISCARD);
    DUMP_VALUE(D3DDEVTYPE_HAL);
    DUMP_VALUE(D3D_SDK_VERSION);

    return 0;
}
