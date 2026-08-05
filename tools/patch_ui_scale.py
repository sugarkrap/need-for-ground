#!/usr/bin/env python3
"""SUPERSEDED - kept for reference, not the active fix. See
`tools/patch_letterbox_2d.py` instead.

This scales the UI's X coordinate constant, on the theory that it would
letterbox 2D content the same way `patch_widescreen.py` corrects hardcoded
resolution values elsewhere. It doesn't work: live-tested, it produced a
LEFT-anchored result, not a centered one (content flush-left, a single
black bar on the right only). The underlying NDC formula,
`x_ndc = corner_x * scaleX - 1.0`, pins pixel-0 to the screen's left edge
unconditionally regardless of scaleX (the corner_x=0 term always vanishes),
so adjusting the scale alone can only shrink the *right* edge inward, never
center. True centering needs the D3D9 viewport itself narrowed to a
centered rect, which requires code injection (no existing "shrink the
viewport for 2D" call exists to patch a constant into) - see
`patches/letterbox_2d.s` / `tools/patch_letterbox_2d.py`.

Original docstring, describing the (disproven) approach below:

Letterbox the 2D UI coordinate system (menu backgrounds, logos, text,
and most other 2D sprite elements) instead of letting it stretch to fill a
widened backbuffer.

`FUN_005c4c20` (0x5c4c20) is the general-purpose "draw a UI element" quad
function - confirmed live (via a breakpoint sweep across every direct
DirectX-calling function while sitting on the game's static title screen)
to be by far the most frequently invoked draw-capable function there,
running in lockstep with `FUN_005bc870`. It converts each element's stored
corner coordinates into NDC via two global scale constants:

    x_ndc = corner_x * DAT_0079ac14 - 1.0
    y_ndc = -(corner_y * DAT_0079ac18 - 1.0)

Both are read-only at runtime everywhere in the binary (confirmed via
Ghidra's reference manager: every reference to either address, across all
functions that touch them, is a READ) - they're plain compile-time float
constants in `.rdata`, not something computed from the live resolution.
Their values are exactly 2/640 and 2/480: this UI system was authored once,
in 2004, against a fixed 640x480 (4:3) coordinate space, and NDC -1..1
always spans the *entire* active viewport regardless of its real aspect -
so on a wider-than-4:3 backbuffer this space visibly stretches sideways.

Unlike the widescreen camera/FOV fix, this doesn't need code injection: the
constant itself is what needs correcting, the same way `patch_widescreen.py`
corrects hardcoded resolution values elsewhere. `DAT_0079ac18` (Y) is left
untouched - the UI still fills the full height, matching the "Hor+" style
used by the camera fix. `DAT_0079ac14` (X) is recomputed so the 640-wide
authored coordinate space keeps its original *physical* pixel density
relative to Y, centering it (letterboxing) within the wider actual screen
instead of stretching corner-to-corner.

Usage:
    python3 patch_ui_scale.py --exe /path/to/patched/speed2.exe \\
        --output speed2_ui_letterboxed.exe --width 2560 --height 1080
"""
import argparse
import struct

import pefile

X_SCALE_VA = 0x79AC14
Y_SCALE_VA = 0x79AC18
REFERENCE_WIDTH = 640
REFERENCE_HEIGHT = 480


def va_to_file_offset(pe: pefile.PE, va: int) -> int:
    return pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)


def patch(exe_path: str, output_path: str, width: int, height: int) -> None:
    pe = pefile.PE(exe_path, fast_load=True)
    data = bytearray(open(exe_path, "rb").read())

    off = va_to_file_offset(pe, X_SCALE_VA)
    old = struct.unpack_from("<f", data, off)[0]
    expected_original = 2.0 / REFERENCE_WIDTH

    y_scale = 2.0 / REFERENCE_HEIGHT
    new = y_scale * (height / width)

    if abs(old - new) < 1e-9:
        print(f"UI X scale: already {new} at VA {hex(X_SCALE_VA)}, leaving as-is")
    else:
        if abs(old - expected_original) > 1e-9:
            raise ValueError(
                f"UI X scale at VA {hex(X_SCALE_VA)} is {old}, expected either the "
                f"known original ({expected_original}) or the target ({new}) - "
                "refusing to patch an exe that doesn't match what this tool expects"
            )
        struct.pack_into("<f", data, off, new)
        print(f"UI X scale: VA {hex(X_SCALE_VA)} {old} -> {new} "
              f"(centers the 640-wide authored UI space for a {width}x{height} backbuffer)")

    with open(output_path, "wb") as f:
        f.write(data)
    print(f"\nWrote {output_path} ({len(data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to the input exe")
    parser.add_argument("--output", required=True, help="path to write the patched exe to (can be the same path)")
    parser.add_argument("--width", required=True, type=int, help="target display width, e.g. 2560")
    parser.add_argument("--height", required=True, type=int, help="target display height, e.g. 1080")
    args = parser.parse_args()
    patch(args.exe, args.output, args.width, args.height)


if __name__ == "__main__":
    main()
