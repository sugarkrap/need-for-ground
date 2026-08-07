/*
 * d3d9_bridge.h - let the game's own code call DXVK Native.
 *
 * Two things stop the game from calling DXVK directly, and neither is negotiable:
 *
 *   1. **Calling convention.** MSVC compiled the game against Windows' COM ABI,
 *      where a method is __stdcall and the *callee* pops the arguments. DXVK Native
 *      defines STDMETHODCALLTYPE as empty, so its methods are __cdecl and the caller
 *      pops. Hand the game a DXVK pointer and the stack drifts by the argument size
 *      on every call.
 *   2. **Stack alignment.** MSVC-compiled i386 code does not keep the stack 16-byte
 *      aligned. GCC-compiled code assumes it is, and DXVK is full of `movaps` on
 *      stack slots. That is not a theoretical hazard: the game reached
 *      Direct3DCreate9 and died on `movaps %xmm0,-0x178(%ebp)` four frames into
 *      DxvkInstance's constructor.
 *
 * So every entry point the game reaches goes through a thunk that is __stdcall *and*
 * carries force_align_arg_pointer, and forwards to DXVK's __cdecl method. That is
 * one thunk per method across the D3D9 interfaces, which is why they are generated
 * (tools/generate_d3d9_bridge.ts) rather than written.
 *
 * COM pointers cross this boundary in both directions, so they are translated in
 * both: an interface the game receives is wrapped before it can touch it, and a
 * wrapped pointer the game passes back is unwrapped before DXVK sees it. Identity is
 * preserved - the same real object always maps to the same bridge - because callers
 * do compare interface pointers.
 *
 * The game reaches this through its import table: the loader resolves
 * d3d9.dll!Direct3DCreate9 to nfsu2_d3d9_lookup's answer, *not* to DXVK's exported
 * symbol. That matters for the rest of native/: our own C hosts keep calling DXVK's
 * cdecl entry point directly, and nothing here shadows it.
 */
#ifndef NFSU2_D3D9_BRIDGE_H
#define NFSU2_D3D9_BRIDGE_H

/*
 * Resolve a d3d9.dll export for the loader. Returns NULL for anything not bridged,
 * which the loader reports as unresolved rather than faking.
 */
void *nfsu2_d3d9_lookup(const char *name);

/* Interfaces the bridge knows, generated from the SDK headers. */
enum nfsu2_d3d9_iface {
    NFSU2_D3D9_IFACE_NONE = 0,
#define NFSU2_D3D9_IFACE(name) NFSU2_D3D9_IFACE_##name,
#include "d3d9_bridge_ifaces.h"
#undef NFSU2_D3D9_IFACE
    NFSU2_D3D9_IFACE_COUNT
};

/*
 * Wrap a DXVK object so game code can call it; returns the same bridge for the same
 * object. NULL in, NULL out.
 */
void *nfsu2_d3d9_wrap(void *real, enum nfsu2_d3d9_iface iface);

/* The real object behind a bridge, or the pointer itself if it is not one of ours -
 * game code passes plain data through the same parameter slots. */
void *nfsu2_d3d9_unwrap(void *maybe_bridge);

/* How many bridges exist, and how many calls have crossed. Diagnostics. */
unsigned int nfsu2_d3d9_bridge_count(void);

/* How many times a released interface was used again. Non-zero means a refcount
 * disagreement between the game and DXVK, and the trace names the interface. */
unsigned int nfsu2_d3d9_use_after_release(void);

#endif /* NFSU2_D3D9_BRIDGE_H */
