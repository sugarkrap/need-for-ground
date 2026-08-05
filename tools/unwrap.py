#!/usr/bin/env python3
"""Strip a SafeDisc protection wrapper from an NFS Underground 2 executable.

Current capability: PATCHING ONLY. This tool performs the PE-level
transformation (drop the encrypted stub sections, rewire the entry point,
recompute image size, repoint the import directory) given:

  - the original, still-protected exe
  - a REFERENCE exe whose .text/.rdata/.data are already decrypted, used as
    the source for the three content payloads this tool does not itself
    decrypt (that requires either the disc-verification-derived key, or an
    already-unwrapped reference to copy known-good bytes from)

It does not (yet) independently derive the decryption key or the entry
point address for an unknown build - those still require the emulation
workflow in ../analysis to trace a specific exe's SafeDisc stub by hand.
"""
import argparse
import struct
import sys

import pefile


def align_up(x: int, a: int = 0x1000) -> int:
    return (x + a - 1) & ~(a - 1)


def section_by_name(pe: pefile.PE, name: str) -> pefile.SectionStructure:
    for s in pe.sections:
        if s.Name.decode(errors="replace").strip("\x00") == name:
            return s
    raise KeyError(f"section {name!r} not found")


def is_safedisc_stub(section: pefile.SectionStructure) -> bool:
    name = section.Name.decode(errors="replace").strip("\x00")
    return name.startswith("stxt")


def unwrap(original_path: str, reference_path: str, output_path: str, entry_rva: int) -> None:
    orig = pefile.PE(original_path)
    ref = pefile.PE(reference_path)

    stub_names = {s.Name for s in orig.sections if is_safedisc_stub(s)}
    keep_sections = [s for s in orig.sections if s.Name not in stub_names]
    removed = [s for s in orig.sections if s.Name in stub_names]
    print(f"Removing {len(removed)} SafeDisc stub section(s): "
          f"{[s.Name.decode(errors='replace').strip(chr(0)) for s in removed]}")
    print(f"Keeping {len(keep_sections)} section(s): "
          f"{[s.Name.decode(errors='replace').strip(chr(0)) or '(unnamed)' for s in keep_sections]}")

    with open(original_path, "rb") as f:
        raw = bytearray(f.read())

    # source the payloads we don't independently re-derive, from the reference
    for name in (".text", ".rdata", ".data"):
        orig_s = section_by_name(orig, name)
        ref_s = section_by_name(ref, name)
        if orig_s.SizeOfRawData != ref_s.SizeOfRawData or orig_s.PointerToRawData != ref_s.PointerToRawData:
            raise ValueError(
                f"{name}: layout differs between original and reference "
                f"(orig raw={hex(orig_s.SizeOfRawData)}@{hex(orig_s.PointerToRawData)}, "
                f"ref raw={hex(ref_s.SizeOfRawData)}@{hex(ref_s.PointerToRawData)}) - "
                "this reference isn't a drop-in match for this original"
            )
        ref_data = ref_s.get_data()
        off = orig_s.PointerToRawData
        raw[off: off + len(ref_data)] = ref_data
        print(f"Replaced {name} content ({len(ref_data)} bytes) at file offset {hex(off)}")

    # drop the stub sections' raw data by truncating after the last kept section
    last_kept = max(keep_sections, key=lambda s: s.PointerToRawData)
    new_size = last_kept.PointerToRawData + last_kept.SizeOfRawData
    print(f"Truncating file from {len(raw)} to {new_size} bytes")
    raw = raw[:new_size]

    e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    coff_off = e_lfanew + 4
    opt_off = coff_off + 20

    struct.pack_into("<H", raw, coff_off + 2, len(keep_sections))

    struct.pack_into("<I", raw, opt_off + 16, entry_rva)
    print(f"Set AddressOfEntryPoint = {hex(entry_rva)} "
          f"(VA {hex(orig.OPTIONAL_HEADER.ImageBase + entry_rva)})")

    size_of_image = align_up(max(s.VirtualAddress + s.Misc_VirtualSize for s in keep_sections))
    struct.pack_into("<I", raw, opt_off + 56, size_of_image)
    print(f"Set SizeOfImage = {hex(size_of_image)}")

    ref_import_dir = ref.OPTIONAL_HEADER.DATA_DIRECTORY[1]
    struct.pack_into("<II", raw, opt_off + 104, ref_import_dir.VirtualAddress, ref_import_dir.Size)
    print(f"Set Import Directory = RVA {hex(ref_import_dir.VirtualAddress)} size {hex(ref_import_dir.Size)}")

    sec_table_off = opt_off + orig.FILE_HEADER.SizeOfOptionalHeader
    section_bytes = raw[sec_table_off: sec_table_off + 40 * len(orig.sections)]
    kept_bytes = bytearray()
    for i, s in enumerate(orig.sections):
        if s.Name not in stub_names:
            kept_bytes += section_bytes[i * 40:(i + 1) * 40]
    kept_bytes += b"\x00" * (40 * (len(orig.sections) - len(keep_sections)))
    raw[sec_table_off: sec_table_off + len(kept_bytes)] = kept_bytes

    with open(output_path, "wb") as f:
        f.write(raw)
    print(f"\nWrote {output_path} ({len(raw)} bytes)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--original", required=True, help="path to the protected input exe")
    parser.add_argument("--reference", required=True, help="path to an exe with decrypted .text/.rdata/.data to source content from")
    parser.add_argument("--output", required=True, help="path to write the unwrapped exe to")
    parser.add_argument("--entry-rva", required=True, type=lambda x: int(x, 0),
                         help="the real OEP as an RVA (e.g. 0x35b8d1) - find this via the emulation workflow in ../analysis first")
    args = parser.parse_args()
    unwrap(args.original, args.reference, args.output, args.entry_rva)


if __name__ == "__main__":
    main()
