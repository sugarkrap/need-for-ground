"""Vtable method orders for the COM interfaces this game uses, straight from
the public DirectX SDK headers (verified against apitrace/dxsdk's mirror of
Microsoft's actual d3d9.h/ddraw.h - not reverse-engineered, just encoding a
documented ABI). Order matters: it maps directly to vtable slot offsets.
"""

IUNKNOWN = ["QueryInterface", "AddRef", "Release"]

IDIRECT3D9 = IUNKNOWN + [
    "RegisterSoftwareDevice", "GetAdapterCount", "GetAdapterIdentifier",
    "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode",
    "CheckDeviceType", "CheckDeviceFormat", "CheckDeviceMultiSampleType",
    "CheckDepthStencilMatch", "CheckDeviceFormatConversion", "GetDeviceCaps",
    "GetAdapterMonitor", "CreateDevice",
]

IDIRECT3DDEVICE9 = IUNKNOWN + [
    "TestCooperativeLevel", "GetAvailableTextureMem", "EvictManagedResources",
    "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor",
    "CreateAdditionalSwapChain", "GetSwapChain", "GetNumberOfSwapChains",
    "Reset", "Present", "GetBackBuffer", "GetRasterStatus",
    "SetDialogBoxMode", "SetGammaRamp", "GetGammaRamp", "CreateTexture",
    "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer",
    "CreateIndexBuffer", "CreateRenderTarget", "CreateDepthStencilSurface",
    "UpdateSurface", "UpdateTexture", "GetRenderTargetData",
    "GetFrontBufferData", "StretchRect", "ColorFill",
    "CreateOffscreenPlainSurface", "SetRenderTarget", "GetRenderTarget",
    "SetDepthStencilSurface", "GetDepthStencilSurface", "BeginScene",
    "EndScene", "Clear", "SetTransform", "GetTransform", "MultiplyTransform",
    "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight",
    "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane",
    "GetClipPlane", "SetRenderState", "GetRenderState", "CreateStateBlock",
    "BeginStateBlock", "EndStateBlock", "SetClipStatus", "GetClipStatus",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState",
    "GetSamplerState", "SetSamplerState", "ValidateDevice",
    "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette",
    "GetCurrentTexturePalette", "SetScissorRect", "GetScissorRect",
    "SetSoftwareVertexProcessing", "GetSoftwareVertexProcessing",
    "SetNPatchMode", "GetNPatchMode", "DrawPrimitive", "DrawIndexedPrimitive",
    "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices",
    "CreateVertexDeclaration", "SetVertexDeclaration", "GetVertexDeclaration",
    "SetFVF", "GetFVF", "CreateVertexShader", "SetVertexShader",
    "GetVertexShader", "SetVertexShaderConstantF", "GetVertexShaderConstantF",
    "SetVertexShaderConstantI", "GetVertexShaderConstantI",
    "SetVertexShaderConstantB", "GetVertexShaderConstantB",
    "SetStreamSource", "GetStreamSource", "SetStreamSourceFreq",
    "GetStreamSourceFreq", "SetIndices", "GetIndices", "CreatePixelShader",
    "SetPixelShader", "GetPixelShader", "SetPixelShaderConstantF",
    "GetPixelShaderConstantF", "SetPixelShaderConstantI",
    "GetPixelShaderConstantI", "SetPixelShaderConstantB",
    "GetPixelShaderConstantB", "DrawRectPatch", "DrawTriPatch",
    "DeletePatch", "CreateQuery",
]

IDIRECTDRAW = IUNKNOWN + [
    "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces",
    "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes",
    "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
    "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
]

IDIRECTDRAW7 = IDIRECTDRAW + [
    "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces",
    "TestCooperativeLevel", "GetDeviceIdentifier", "StartModeTest",
    "EvaluateMode",
]

INTERFACES = {
    "IDirect3D9": IDIRECT3D9,
    "IDirect3DDevice9": IDIRECT3DDEVICE9,
    "IDirectDraw": IDIRECTDRAW,
    "IDirectDraw7": IDIRECTDRAW7,
}
