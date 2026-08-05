#!/usr/bin/env python3
"""Patch NFS Underground 2's hardcoded 4:3-era resolution/FOV values to
support an arbitrary target resolution/aspect ratio, baked directly into
the exe (not a runtime DirectX hook).

Addresses below are specific to the 2004 French retail build (same one
`tools/unwrap.py`'s default OEP applies to) - found via live debugging and
cross-checked against Ghidra's decompilation, see NOTES.md.

Two independent hardcoded sites need patching, found by tracing which
in-memory resolution struct actually feeds the projection matrix's
FOV/aspect computation for the real gameplay camera (most on-screen
"resolution" settings in this game - like the one in the video options
menu - only affect the D3D backbuffer/UI cameras, not the world camera's
field of view):

  - `FUN_005bf210` (0x5bf210): a switch table of 6 fixed display
    resolutions (640x480 up to 1600x1200) selected by a game-settings
    index. Feeds the D3D backbuffer size and two UI/overlay cameras
    (slots 0 and 1). Patching the entry actually selected at runtime
    removes pillarboxing, but alone just stretches the 3D view - it does
    NOT reach the real gameplay camera.
  - `FUN_005b9ea0` (0x5b9ea0): unconditionally (no mode-flag gating,
    unlike a similar-looking but dead-in-practice write in
    `FUN_005c1de0`) initializes a small per-camera-context array with
    hardcoded reference resolutions - 320x240 for camera slot 4, which is
    confirmed (via live tracing during an actual race) to be the real
    driving camera. This is the value that actually determines the
    projection matrix's FOV/aspect ratio - patching only this one, height
    held fixed at 240 to preserve vertical FOV (the standard "Hor+"
    widescreen approach), width recomputed for the target aspect ratio,
    is what actually fixes the in-race field of view.

Both are applied together by default since the first without the second
just stretches the image; pass --skip-resolution-table or
--skip-camera-fov to omit one for experimentation.
"""
import argparse
import struct

import pefile

# (description, VA of the immediate operand, expected original value)
RESOLUTION_TABLE_WIDTH = (0x5BF28B, 1280)
RESOLUTION_TABLE_HEIGHT = (0x5BF291, 1024)
CAMERA_FOV_WIDTH = (0x5B9F55, 320)
CAMERA_FOV_HEIGHT = (0x5B9F5C, 240)
CAMERA_FOV_REFERENCE_HEIGHT = 240  # kept fixed; only width is recomputed


def va_to_file_offset(pe: pefile.PE, va: int) -> int:
    return pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)


def patch_dword(data: bytearray, pe: pefile.PE, va: int, expected_old: int, new: int, label: str) -> None:
    off = va_to_file_offset(pe, va)
    old = struct.unpack_from("<I", data, off)[0]
    if old == new:
        print(f"{label}: already {new} at VA {hex(va)}, leaving as-is")
        return
    if old != expected_old:
        raise ValueError(
            f"{label}: value at VA {hex(va)} is {old}, expected either the "
            f"known original ({expected_old}) or the target ({new}) - "
            "refusing to patch an exe that doesn't match what this tool expects"
        )
    struct.pack_into("<I", data, off, new)
    print(f"{label}: VA {hex(va)} {old} -> {new}")


def patch_widescreen(exe_path: str, output_path: str, width: int, height: int,
                      patch_resolution_table: bool, patch_camera_fov: bool) -> None:
    pe = pefile.PE(exe_path, fast_load=True)
    data = bytearray(open(exe_path, "rb").read())

    if patch_resolution_table:
        rva, expected = RESOLUTION_TABLE_WIDTH
        patch_dword(data, pe, rva, expected, width, "resolution table width")
        rva, expected = RESOLUTION_TABLE_HEIGHT
        patch_dword(data, pe, rva, expected, height, "resolution table height")

    if patch_camera_fov:
        fov_width = round(CAMERA_FOV_REFERENCE_HEIGHT * width / height)
        rva, expected = CAMERA_FOV_WIDTH
        patch_dword(data, pe, rva, expected, fov_width, "camera FOV reference width")
        rva, expected = CAMERA_FOV_HEIGHT
        patch_dword(data, pe, rva, expected, CAMERA_FOV_REFERENCE_HEIGHT, "camera FOV reference height")

    with open(output_path, "wb") as f:
        f.write(data)
    print(f"\nWrote {output_path} ({len(data)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", required=True, help="path to the (already SafeDisc-unwrapped) input exe")
    parser.add_argument("--output", required=True, help="path to write the patched exe to (can be the same path)")
    parser.add_argument("--width", required=True, type=int, help="target display width, e.g. 2560")
    parser.add_argument("--height", required=True, type=int, help="target display height, e.g. 1080")
    parser.add_argument("--skip-resolution-table", action="store_true",
                         help="don't patch FUN_005bf210 (backbuffer/UI cameras)")
    parser.add_argument("--skip-camera-fov", action="store_true",
                         help="don't patch FUN_005b9ea0 (the real gameplay camera's FOV/aspect)")
    args = parser.parse_args()
    patch_widescreen(
        args.exe, args.output, args.width, args.height,
        patch_resolution_table=not args.skip_resolution_table,
        patch_camera_fov=not args.skip_camera_fov,
    )


if __name__ == "__main__":
    main()
