# DirectX-touching function inventory

Auto-generated from `triage.json` - 99 functions total (54 direct, 45 transitive). This is the concrete starting scope for the D3D9->Vulkan renderer port.

## Direct callers

Functions that call a DirectX interface method themselves.

- `FUN_005b7a30` @ `0x005b7a30` - SetRenderState
- `FUN_005b7ad0` @ `0x005b7ad0` - SetRenderState
- `FUN_005b93a0` @ `0x005b93a0` - CreateVertexDeclaration
- `FUN_005b97c0` @ `0x005b97c0` - BeginScene, Clear, CreateDevice, EndScene, Present
- `FUN_005b99e0` @ `0x005b99e0` - SetTexture
- `FUN_005b9cb0` @ `0x005b9cb0` - CreateDepthStencilSurface, CreateTexture, GetBackBuffer, GetDepthStencilSurface
- `FUN_005b9ea0` @ `0x005b9ea0` - GetDepthStencilSurface
- `FUN_005ba070` @ `0x005ba070` - SetDepthStencilSurface, SetRenderTarget, SetViewport
- `FUN_005ba160` @ `0x005ba160` - CreateCubeTexture, CreateDepthStencilSurface
- `FUN_005bae60` @ `0x005bae60` - CreateIndexBuffer, CreateVertexBuffer
- `FUN_005bb390` @ `0x005bb390` - CreateIndexBuffer, CreateVertexBuffer
- `FUN_005bb480` @ `0x005bb480` - Clear, CreateRenderTarget, CreateTexture, SetDepthStencilSurface, SetRenderTarget
- `FUN_005bc210` @ `0x005bc210` - CreateTexture
- `FUN_005bc7b0` @ `0x005bc7b0` - SetRenderState
- `FUN_005bc800` @ `0x005bc800` - SetRenderState
- `FUN_005bc870` @ `0x005bc870` - GetLight, SetRenderState, SetTexture
- `FUN_005bcd80` @ `0x005bcd80` - LightEnable, SetLight
- `FUN_005bd0e0` @ `0x005bd0e0` - SetMaterial
- `FUN_005bda20` @ `0x005bda20` - SetTransform
- `FUN_005be1b0` @ `0x005be1b0` - EnumAdapterModes, GetAdapterIdentifier, GetAdapterModeCount
- `FUN_005c2030` @ `0x005c2030` - SetRenderState, SetSamplerState, SetTextureStageState, SetVertexDeclaration
- `FUN_005c23b0` @ `0x005c23b0` - DrawPrimitiveUP, SetRenderState, SetSamplerState
- `FUN_005c2ad0` @ `0x005c2ad0` - CreateAdditionalSwapChain, CreateRenderTarget, GetSwapChain, Release, ShowCursor, StretchRect
- `FUN_005c2d20` @ `0x005c2d20` - CreateRenderTarget, ShowCursor, StretchRect
- `FUN_005c4c20` @ `0x005c4c20` - DrawPrimitiveUP, SetFVF, SetRenderState, SetTextureStageState, SetTransform
- `FUN_005c5180` @ `0x005c5180` - DrawPrimitiveUP
- `FUN_005c5350` @ `0x005c5350` - DrawPrimitiveUP
- `FUN_005c53e0` @ `0x005c53e0` - DrawIndexedPrimitive, SetIndices, SetRenderState, SetStreamSource
- `FUN_005c5800` @ `0x005c5800` - DrawPrimitiveUP, SetDepthStencilSurface, SetFVF, SetRenderTarget, SetVertexDeclaration, SetViewport
- `FUN_005c5b70` @ `0x005c5b70` - DrawPrimitiveUP, SetDepthStencilSurface, SetRenderTarget, SetVertexDeclaration, SetViewport, UpdateSurface
- `FUN_005c6120` @ `0x005c6120` - DrawPrimitiveUP, SetDepthStencilSurface, SetRenderTarget, SetVertexDeclaration, SetViewport
- `FUN_005c64d0` @ `0x005c64d0` - SetRenderState
- `FUN_005c67e0` @ `0x005c67e0` - DrawPrimitiveUP, SetDepthStencilSurface, SetRenderTarget, SetVertexDeclaration, SetViewport
- `FUN_005c6d40` @ `0x005c6d40` - SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetTexture, SetVertexDeclaration
- `FUN_005c7070` @ `0x005c7070` - DrawIndexedPrimitive, SetDepthStencilSurface, SetIndices, SetRenderTarget, SetStreamSource, SetTexture, SetTransform, SetVertexDeclaration, SetViewport
- `FUN_005c7460` @ `0x005c7460` - SetRenderState
- `FUN_005c76b0` @ `0x005c76b0` - DrawPrimitiveUP, SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetVertexDeclaration
- `FUN_005ca6d0` @ `0x005ca6d0` - CreateVertexBuffer, SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetSamplerState, SetTexture, SetVertexDeclaration, SetViewport, StretchRect
- `FUN_005cb700` @ `0x005cb700` - SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetVertexDeclaration, SetViewport, StretchRect
- `FUN_005cbb40` @ `0x005cbb40` - SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetSamplerState, SetVertexDeclaration, SetViewport
- `FUN_005cbe80` @ `0x005cbe80` - CreateIndexBuffer, CreateVertexBuffer
- `FUN_005cc060` @ `0x005cc060` - SetRenderState, StretchRect
- `FUN_005cc4b0` @ `0x005cc4b0` - SetRenderState, StretchRect
- `FUN_005cc600` @ `0x005cc600` - SetDepthStencilSurface, SetRenderState, SetRenderTarget, SetViewport
- `FUN_005cdda0` @ `0x005cdda0` - CreateTexture
- `FUN_005ce4d0` @ `0x005ce4d0` - CreateVertexBuffer
- `FUN_005ce990` @ `0x005ce990` - GetDeviceCaps, LightEnable
- `FUN_005cf030` @ `0x005cf030` - CreateVertexBuffer, DrawIndexedPrimitive, DrawPrimitiveUP, SetIndices, SetRenderState, SetStreamSource, SetVertexDeclaration
- `FUN_005cf570` @ `0x005cf570` - Release
- `FUN_005d1870` @ `0x005d1870` - Reset
- `FUN_005d1a60` @ `0x005d1a60` - CreateVertexDeclaration
- `FUN_005d1e30` @ `0x005d1e30` - GetAdapterIdentifier, GetDeviceCaps
- `FUN_005d2130` @ `0x005d2130` - CheckDeviceMultiSampleType, GetAdapterDisplayMode, GetAvailableTextureMem, GetDeviceCaps
- `FUN_005d24a0` @ `0x005d24a0` - BeginScene, Clear, EndScene, Present, TestCooperativeLevel

## Transitive callers

Functions that reach a direct caller through the call graph (chain shown to the nearest direct call).

- `FUN_00433280` @ `0x00433280`: FUN_00433280 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_00488950` @ `0x00488950`: FUN_00488950 -> FUN_005ce140 -> FUN_005cdda0
- `FUN_0048ccc0` @ `0x0048ccc0`: FUN_0048ccc0 -> FUN_005d2130
- `FUN_004908b0` @ `0x004908b0`: FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_004917f0` @ `0x004917f0`: FUN_004917f0 -> FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_00493b00` @ `0x00493b00`: FUN_00493b00 -> FUN_005cbe80
- `FUN_004941c0` @ `0x004941c0`: FUN_004941c0 -> FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_004949e0` @ `0x004949e0`: FUN_004949e0 -> FUN_005cdda0
- `FUN_00494e30` @ `0x00494e30`: FUN_00494e30 -> FUN_004949e0 -> FUN_005cdda0
- `FUN_004ceac0` @ `0x004ceac0`: FUN_004ceac0 -> FUN_005ce140 -> FUN_005cdda0
- `FUN_0050aea0` @ `0x0050aea0`: FUN_0050aea0 -> FUN_005c4c20
- `FUN_0050d730` @ `0x0050d730`: FUN_0050d730 -> FUN_005c5350
- `FUN_0050d8f0` @ `0x0050d8f0`: FUN_0050d8f0 -> FUN_0050d730 -> FUN_005c5350
- `FUN_0051a0c0` @ `0x0051a0c0`: FUN_0051a0c0 -> FUN_005c7aa0 -> FUN_005c4c20
- `FUN_00537460` @ `0x00537460`: FUN_00537460 -> FUN_0050aea0 -> FUN_005c4c20
- `FUN_005bb030` @ `0x005bb030`: FUN_005bb030 -> FUN_005bc870
- `FUN_005c0a10` @ `0x005c0a10`: FUN_005c0a10 -> FUN_005bc870
- `FUN_005c6d20` @ `0x005c6d20`: FUN_005c6d20 -> FUN_005c5b70
- `FUN_005c7a40` @ `0x005c7a40`: FUN_005c7a40 -> FUN_005c5b70
- `FUN_005c7a80` @ `0x005c7a80`: FUN_005c7a80 -> FUN_005c5180
- `FUN_005c7aa0` @ `0x005c7aa0`: FUN_005c7aa0 -> FUN_005c4c20
- `FUN_005cb9c0` @ `0x005cb9c0`: FUN_005cb9c0 -> FUN_005c4c20
- `FUN_005cbe50` @ `0x005cbe50`: FUN_005cbe50 -> FUN_005c53e0
- `FUN_005cc5a0` @ `0x005cc5a0`: FUN_005cc5a0 -> FUN_005c5b70
- `FUN_005cc8d0` @ `0x005cc8d0`: FUN_005cc8d0 -> FUN_005c67e0
- `FUN_005cdd20` @ `0x005cdd20`: FUN_005cdd20 -> FUN_005c53e0
- `FUN_005ce140` @ `0x005ce140`: FUN_005ce140 -> FUN_005cdda0
- `FUN_005cea10` @ `0x005cea10`: FUN_005cea10 -> FUN_005cdda0
- `FUN_005d2d20` @ `0x005d2d20`: FUN_005d2d20 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_005d7810` @ `0x005d7810`: FUN_005d7810 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_005d79a0` @ `0x005d79a0`: FUN_005d79a0 -> FUN_005d2d20 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_005d7ac0` @ `0x005d7ac0`: FUN_005d7ac0 -> FUN_005d79a0 -> FUN_005d2d20 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_005dc0b0` @ `0x005dc0b0`: FUN_005dc0b0 -> FUN_005d7810 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0060d220` @ `0x0060d220`: FUN_0060d220 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0060d440` @ `0x0060d440`: FUN_0060d440 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_006136c0` @ `0x006136c0`: FUN_006136c0 -> FUN_005bb030 -> FUN_005bc870
- `FUN_006150e0` @ `0x006150e0`: FUN_006150e0 -> FUN_0060d220 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_00615280` @ `0x00615280`: FUN_00615280 -> FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_00615660` @ `0x00615660`: FUN_00615660 -> FUN_0060d440 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0061a8e0` @ `0x0061a8e0`: FUN_0061a8e0 -> FUN_00615660 -> FUN_0060d440 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0061a930` @ `0x0061a930`: FUN_0061a930 -> FUN_00615280 -> FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0061aae0` @ `0x0061aae0`: FUN_0061aae0 -> FUN_00615280 -> FUN_004908b0 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0061ae40` @ `0x0061ae40`: FUN_0061ae40 -> FUN_00615660 -> FUN_0060d440 -> FUN_005c7a80 -> FUN_005c5180
- `FUN_0061ed70` @ `0x0061ed70`: FUN_0061ed70 -> FUN_006136c0 -> FUN_005bb030 -> FUN_005bc870
- `FUN_00620250` @ `0x00620250`: FUN_00620250 -> FUN_005c7a80 -> FUN_005c5180
