/*
 * abi_probe_dxvk.cpp - the same layout dump as abi_probe_wine.c, but taken
 * from DXVK Native's own headers, i.e. exactly what libdxvk_d3d9.so was
 * compiled against. tools/abi_diff.py compares the two outputs.
 *
 * C++ because DXVK's native headers declare COM interfaces as C++ classes;
 * the plain data structs we care about are shared with the C view.
 */
#include <windows.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdio>

#define DUMP_SIZE(type)        std::printf("size %s %zu\n", #type, sizeof(type))
#define DUMP_OFFSET(type, mem) std::printf("offset %s.%s %zu\n", #type, #mem, offsetof(type, mem))
#define DUMP_VALUE(name)       std::printf("value %s %ld\n", #name, (long)(name))

int main()
{
    std::printf("pointer_size %zu\n", sizeof(void *));

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
