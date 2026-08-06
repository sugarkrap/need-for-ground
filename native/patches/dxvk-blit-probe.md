# dxvk-blit-probe: read the swapchain image back inside DXVK

The last experiment that can split the black-window problem (see `../README.md`
and `../../NOTES.md`). Everything observable from outside DXVK has been checked
and is correct, so the question left is narrow:

> After `DxvkSwapchainBlitter::present()` has drawn the game's backbuffer *and*
> the HUD into the acquired swapchain image, does that image contain the game's
> pixels?

- **Yes** → the blit works and the fault is in presentation or compositing below
  DXVK. On this machine that means the NVIDIA driver, XWayland, or the Wayland
  compositor. The HUD being visible while the blit is not would then mean the
  compositor is showing an *older* image than the one just presented.
- **No** → the blit draw genuinely emits no fragments, despite valid Vulkan,
  correct rects, correct images and a real draw call.

## How it works

The probe copies the swapchain image into a host-visible buffer inside the same
command list that did the blit, then reads that buffer **several frames later**.
Reading later is deliberate: it needs no fence and no stall, because a submission
from ten frames ago has certainly completed. It reports the centre pixel and a
non-black count through DXVK's own logger, so there is no file plumbing in the
render path.

`NFSU2_BLIT_PROBE=1` turns it on; without it the code is inert.

## Applying it

The DXVK checkout under `native/third_party/` is gitignored - it is fetched by
`native/tools/build_dxvk_native.sh` - so the patch lives here instead:

```sh
cd native/third_party/dxvk
git apply ../../patches/dxvk-blit-probe.patch
ninja -C build-native-32
cp -P build-native-32/src/d3d9/libdxvk_d3d9.so.0.30002 \
      ../dxvk-native/lib32/
cd ../../..
NFSU2_BLIT_PROBE=1 ./native/build32/nfsu2-smoke-d3d9 --frames 40 --width 400 --height 300
```

To get back to a clean library:

```sh
cd native/third_party/dxvk && git checkout src/dxvk/dxvk_swapchain_blitter.cpp \
    src/dxvk/dxvk_swapchain_blitter.h && ninja -C build-native-32
```

`NFSU2_BLIT_PROBE=2` captures the **source** image instead - the D3D9 backbuffer.
That is the control, and running it first is not optional: the first version of
this probe reported an empty image for a source that demonstrably had content, and
without the control that would have been read as a DXVK bug.

## What it found

```
NFSU2_BLIT_PROBE=2 (source image, the control)
  buffer slice offset=0 size=480000 mapPtr=...
  first bytes 0 0 0 0        <- the 0xAB fill was overwritten, so the copy landed
  centre pixel B=0 G=0 R=0   non-black 0/120000

NFSU2_BLIT_PROBE=1 (swapchain image), with DXVK_HUD=fps
  centre pixel B=0 G=0 R=0   non-black 252/120000   <- 252 = the HUD's text
```

The 0xAB fill matters: the buffer is filled with 0xAB before the copy, so
zeroes on read-back mean the copy really did land and really did read zeroes,
rather than the probe looking in the wrong place.

So: **the image the blitter samples is black at blit time**, even though
`GetRenderTargetData` on the same D3D9 backbuffer returns the cleared colour. The
blit draw is not broken - it runs, and faithfully writes the black it is given.
The HUD's 252 pixels land in the same image, in the same render pass, which is why
the HUD is visible over a black window.

That reframes the whole problem. It is not a blit or a presentation fault; the
rendering the application did is not in the image the swapchain path reads. The
next thing to look at is DXVK's D3D9 backbuffer rotation - `D3D9SwapChainEx`
`Swap()`s the back buffers after each present, so which `VkImage` backs
`m_backBuffers[0]` at blit time is worth logging against the image the device's
render target points at.

## Two mistakes this probe made first

Worth keeping, because both produced confident wrong answers:

1. **No host-read barrier.** The copy was valid and validation was clean, but
   nothing made the written bytes visible to the CPU, so the buffer read back as
   zeroes. Fixed with a `VkBufferMemoryBarrier2` to `HOST_READ`/`HOST` - included
   in the patch.
2. **No control.** With the barrier still missing, the probe said "the presented
   image is empty" and that got reported as "the blit draw produced nothing". The
   source-image control (`=2`) is what exposed it: the probe returned zeroes for an
   image that could not possibly be empty.
