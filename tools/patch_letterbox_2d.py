#!/usr/bin/env python3
"""Letterbox the game's 2D quad rendering (menu background, splash video,
and other UI elements drawn through the same shared utilities) instead of
letting it stretch to fill a widened backbuffer.

Background: `patch_widescreen.py` fixes the real 3D gameplay camera's FOV
correctly (proper "Hor+" widening). But NFS Underground 2's 2D/UI quad
drawing has no aspect-ratio logic at all - it was written once, in 2004,
for 4:3, and always fills whatever viewport happens to be active with a
plain 0..1 normalized quad. Widening the backbuffer (needed for the 3D fix)
directly widens what these quads stretch into.

There's no existing "shrink the viewport for 2D" call anywhere to patch -
so this doesn't recompute an existing constant like the other patches do.
It injects new code (see `letterbox_2d.s`, assembled here via GNU `as`/
`ld`) that computes a centered, 4:3-correct viewport from the *live*
backbuffer resolution (so it stays correct if the resolution changes at
runtime) and calls IDirect3DDevice9::SetViewport. A pure-constant fix was
tried first (the UI coordinate system's X scale is a static, compile-time
2/640 value) but doesn't work: the formula pins pixel-0 to the screen's
left edge unconditionally, so adjusting only the scale shrinks the *right*
edge inward instead of centering - true centering needs the viewport
itself adjusted.

Five hooks are involved, split by call frequency and, for the three
per-element hooks, scoped to the exact duration of one draw call rather
than left active until the next frame. Getting there took several
confirmed-live wrong turns - see NOTES.md for the full postmortem - the
short version: an earlier design left the viewport narrowed after each
2D-quad draw until the *next* frame's reset, on the theory that nothing
else draws in between. That's false often enough to matter (a bloom/
post-process pass, or simply DXVK/wined3d's own internal state) and
produced a persistent torn/garbled artifact in the bar regions that
survived eight independent fix attempts targeting the bar-clear itself
(rect math, timing, render target, scissor state, XMM preservation, a
wrong "some quads must stay full-width" heuristic, stack alignment) before
the real cause - viewport state leaking between unrelated draws - was
identified and fixed structurally instead of hidden.
  - Three hooks on the generic 2D quad-draw functions (found by tracing
    what's actually active during splash/menu, see NOTES.md) - two for
    video playback (`FUN_005c5b70`, `FUN_005c6d40`), plus `FUN_005c4c20`,
    the general-purpose "draw one UI element" function responsible for the
    static title/menu screen itself (confirmed live via a breakpoint sweep
    across every direct DirectX-calling function while sitting on that
    screen: it's by far the most frequently invoked one there, ~1900
    calls/frame). Each swaps its own return address for a small
    post-hook stub before calling `set_narrow_viewport`: the original
    function runs and returns as normal, but its `ret` lands in our stub
    first, which calls `set_wide_viewport` to restore the full backbuffer
    viewport, then jumps to the real original caller. This means the
    narrowed viewport is active for the minimum possible window - exactly
    one draw call - rather than leaking into whatever runs next.
    `FUN_005c4c20` additionally gates on its own param_3 ([esp+0xc] at
    entry): full-screen effects (e.g. a race-start wipe/transition) pass a
    distinct value there and must not be letterboxed or return-swapped at
    all - confirmed live via parameter sampling (only 0/1 seen for normal
    menu-element calls, a third value in the function's own decompiled
    source is specifically for those effects).
  - One hook right after the game's own existing frame-end
    Clear(Count=0, pRects=NULL) call inside FUN_005d24a0 (confirmed via
    decompiled source + live disassembly to be the main render loop's
    per-frame backbuffer clear, called exactly once per frame right after
    Present()) - calls `set_full_viewport_and_clear_bars`, which widens to
    the full backbuffer, forces scissor test off, and clears just the two
    bar rects to black (D3D9's Clear() is clipped by both the current
    viewport and an active scissor rect, so without this the bars can show
    stale/uninitialized or garbled content).
  - One hook at the tail of FUN_005d1870, the device Reset() handler
    (reached both when Reset() succeeds immediately and after a successful
    retry, right after the game's own post-reset re-initialization
    finishes) - also calls `set_full_viewport_and_clear_bars`, since a
    compositor-triggered redraw (e.g. a workspace switch) can force a
    device-lost/Reset() cycle whose freshly recreated swapchain image(s)
    were never touched by the once-per-frame hook above.

Every call into a real D3D9 method (SetViewport/Clear/SetRenderState) is
preceded by forcing the stack to 16-byte alignment and restored exactly
after - live-sampled evidence found the stack was misaligned relative to
that boundary in ~19% of calls, which the compiled DXVK/wined3d code (built
with SSE-optimization assumptions this hand-written injected code doesn't
automatically satisfy) is not guaranteed to tolerate.

The injected code lives in a confirmed-empty 2319-byte padding gap at the
end of `.text` (between the last real function and the section's raw-size
alignment) - no existing bytes are displaced beyond the five hook
prologues. The per-element hooks also keep a few bytes of scratch data
(saved return addresses for the trampoline swap described above) inside
that same cave; since `.text` is normally CODE|EXECUTE|READ only, this
patcher also flips on IMAGE_SCN_MEM_WRITE for that section - confirmed
live that omitting this faults immediately (a real page-protection SIGSEGV
on the first attempted write) rather than silently failing.

Usage:
    python3 patch_letterbox_2d.py --exe /path/to/widescreen-patched/speed2.exe \\
        --output speed2_letterboxed.exe
"""
import argparse
import subprocess
import struct
import tempfile
import os

import pefile

CAVE_VA = 0x7826F1
CAVE_SIZE = 0x90F  # 2319 bytes, confirmed zero in the unpatched exe

HOOK_5C5B70_VA = 0x5C5B70
HOOK_5C5B70_ORIG = bytes.fromhex("81ec64010000")  # sub esp, 0x164
TRAMPOLINE_5C5B70_VA = 0x78298B

HOOK_5C6D40_VA = 0x5C6D40
HOOK_5C6D40_ORIG = bytes.fromhex("558bec83e4f0")  # push ebp; mov ebp,esp; and esp,0xfffffff0
TRAMPOLINE_5C6D40_VA = 0x7829B3

HOOK_5C4C20_VA = 0x5C4C20
HOOK_5C4C20_ORIG = bytes.fromhex("558bec83e4f0")  # push ebp; mov ebp,esp; and esp,0xfffffff0
TRAMPOLINE_5C4C20_VA = 0x7829DB

HOOK_5D288F_VA = 0x5D288F
HOOK_5D288F_ORIG = bytes.fromhex("e8cc5eebff")  # call 0x488760 (frame-end Clear(), inside FUN_005d24a0)
TRAMPOLINE_5D288F_VA = 0x782A0A

HOOK_5D1A50_VA = 0x5D1A50
HOOK_5D1A50_ORIG = bytes.fromhex("5f8bc65e83c408")  # pop edi; mov eax,esi; pop esi; add esp,8 (FUN_005d1870 Reset() epilogue)
TRAMPOLINE_5D1A50_VA = 0x782A19

# End of the assembled program (one past the last trampoline's `jmp`),
# verified by disassembling the linked output - see NOTES.md. `ld
# --oformat binary` pads its output well beyond this with stray alignment
# bytes that aren't part of our program and must not be written out.
CODE_END_VA = 0x782A26

ASM_SOURCE = os.path.join(os.path.dirname(__file__), "..", "patches", "letterbox_2d.s")


def assemble_cave() -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, "letterbox.o")
        binf = os.path.join(tmp, "letterbox.bin")
        subprocess.run(["as", "--32", ASM_SOURCE, "-o", obj], check=True, capture_output=True)
        subprocess.run(
            ["ld", "-m", "elf_i386", "-Ttext", hex(CAVE_VA), "--oformat", "binary", "-o", binf, obj],
            check=True, capture_output=True,
        )
        with open(binf, "rb") as f:
            data = f.read()
    return data[:CODE_END_VA - CAVE_VA]


def jmp_rel32(from_va: int, to_va: int) -> bytes:
    disp = to_va - (from_va + 5)
    return b"\xe9" + struct.pack("<i", disp)


def va_to_file_offset(pe: pefile.PE, va: int) -> int:
    return pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)


IMAGE_SCN_MEM_WRITE = 0x80000000


def ensure_text_section_writable(pe: pefile.PE, data: bytearray) -> None:
    """The injected routine keeps a few bytes of scratch data (saved return
    addresses for the per-element viewport-restore trampolines) inside the
    .text code cave. .text is normally CODE|EXECUTE|READ only (confirmed
    live: writing there faulted with an actual page-protection SIGSEGV,
    `mov [saved_ret_5c4c20], eax` at the very first attempt) - add
    IMAGE_SCN_MEM_WRITE to its section characteristics so the cave's
    scratch slots are actually writable at runtime."""
    for section in pe.sections:
        if section.Name.rstrip(b"\x00") != b".text":
            continue
        off = section.get_file_offset() + 36  # Characteristics field within IMAGE_SECTION_HEADER
        current = struct.unpack("<I", data[off:off + 4])[0]
        if current & IMAGE_SCN_MEM_WRITE:
            print(".text section already writable")
            return
        data[off:off + 4] = struct.pack("<I", current | IMAGE_SCN_MEM_WRITE)
        print(f".text section: added IMAGE_SCN_MEM_WRITE (characteristics {current:#x} -> {current | IMAGE_SCN_MEM_WRITE:#x})")
        return
    raise ValueError(".text section not found - cannot make it writable")


def patch(exe_path: str, output_path: str) -> None:
    pe = pefile.PE(exe_path, fast_load=True)
    data = bytearray(open(exe_path, "rb").read())

    ensure_text_section_writable(pe, data)

    cave_off = va_to_file_offset(pe, CAVE_VA)
    existing_cave = bytes(data[cave_off:cave_off + CAVE_SIZE])
    code = assemble_cave()

    if not (all(b == 0 for b in existing_cave) or existing_cave[:len(code)] == code):
        raise ValueError(
            "code cave at VA %#x is neither empty nor already patched with this exact code - "
            "refusing to overwrite unknown content" % CAVE_VA
        )
    data[cave_off:cave_off + len(code)] = code
    print(f"Wrote {len(code)}-byte injected routine at VA {hex(CAVE_VA)} "
          f"({CAVE_SIZE - len(code)} bytes of cave left unused)")

    for hook_va, orig, tramp_va, label in (
        (HOOK_5C5B70_VA, HOOK_5C5B70_ORIG, TRAMPOLINE_5C5B70_VA, "FUN_005c5b70"),
        (HOOK_5C6D40_VA, HOOK_5C6D40_ORIG, TRAMPOLINE_5C6D40_VA, "FUN_005c6d40"),
        (HOOK_5C4C20_VA, HOOK_5C4C20_ORIG, TRAMPOLINE_5C4C20_VA, "FUN_005c4c20"),
        (HOOK_5D288F_VA, HOOK_5D288F_ORIG, TRAMPOLINE_5D288F_VA, "FUN_005d24a0 frame-end clear"),
        (HOOK_5D1A50_VA, HOOK_5D1A50_ORIG, TRAMPOLINE_5D1A50_VA, "FUN_005d1870 Reset() epilogue"),
    ):
        off = va_to_file_offset(pe, hook_va)
        current = bytes(data[off:off + len(orig)])
        new_bytes = jmp_rel32(hook_va, tramp_va) + b"\x90" * (len(orig) - 5)
        if current == new_bytes:
            print(f"{label}: hook already applied, leaving as-is")
            continue
        if current != orig:
            raise ValueError(
                f"{label}: prologue at VA {hex(hook_va)} is {current.hex()}, expected either the "
                f"known original ({orig.hex()}) or the hook ({new_bytes.hex()}) - refusing to patch"
            )
        data[off:off + len(new_bytes)] = new_bytes
        print(f"{label}: hooked VA {hex(hook_va)} -> trampoline at {hex(tramp_va)}")

    with open(output_path, "wb") as f:
        f.write(data)
    print(f"\nWrote {output_path} ({len(data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to the widescreen-patched input exe")
    parser.add_argument("--output", required=True, help="path to write the patched exe to (can be the same path)")
    args = parser.parse_args()
    patch(args.exe, args.output)


if __name__ == "__main__":
    main()
