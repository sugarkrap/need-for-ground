# Findings

Working notes from reverse-engineering NFS Underground 2's (2004, French
retail release) SafeDisc protection. No game files or binaries are stored
here - addresses/offsets below are specific to that build and will differ
for other releases/languages.

## Protection structure

- `speed2.exe` ships with two extra PE sections, `stxt774` and `stxt371`,
  containing an encrypted/obfuscated SafeDisc stub. The real `.text` is
  encrypted at rest (verified: disassembling it statically produces
  nonsensical opcode soup, not real code).
- The PE entry point points into `stxt371`, not `.text`. The stub:
  1. Resolves a small set of KERNEL32 functions itself (via
     `GetModuleHandleA`/`GetProcAddress`, not the normal import table -
     the import table only lists ~16 decoy functions across the DLLs the
     real game eventually needs, presumably so those DLLs get loaded).
  2. Self-extracts several files into a temp directory (`~e5.NNNN.dir.NNNN\`)
     by reading chunks out of its own exe file - `PfdRun.pfd`, a couple of
     `.tmp` files, one of which is a genuine ~800KB PE DLL.
  3. Loads that DLL for real and runs its `DllMain`.
  4. That DLL does the actual verification. Deep inside (a ~4300-instruction
     routine, decompiled address `0x10054b20` in our emulation's address
     space) it computes some value and compares it against a hardcoded
     32-bit constant (`0x410a02bc` in this build) at approximately
     `0x100134c4`.
  5. If that comparison passes, control returns up through several stack
     frames to the stub, which jumps to the real OEP (`0x75b8d1` in this
     build, `.text`+`0x1b8d1`).

## The checksum is very likely key material, not just a gate

Forcing the `0x100134c4` comparison to "pass" (by directly overwriting the
compared memory with the expected constant) does make the stub take the
success path and jump to the real OEP - but `.text` is still garbage at
that point. Byte-diffing our emulator's `.text` against a real
unwrapped/decrypted reference exe showed 99.6% of bytes differ, with no
repeating pattern (ruling out a trivial XOR obfuscation - this is a real
cipher with proper diffusion).

Conclusion: the checksum computed at `0x100134c4` almost certainly doubles
as (or derives) the decryption key for `.text`, not just a boolean
pass/fail flag. That's consistent with why forcing the comparison gets
you *past the check* but not to *working decrypted code* - and why a
"crack" for this class of protection is realistically produced by running
the real exe with a real disc under a debugger and dumping memory after
genuine hardware-driven decryption completes, then rebuilding a standalone
PE from that - not by patching the comparison.

**We have not solved the actual algorithm.** Standard checksums (CRC32,
Adler32, byte/dword sum, MD5/SHA1-truncated) do not match, confirming it's
a bespoke routine. This is the open thread if anyone wants to pick it back
up: fully reverse `0x10054b20` (and whatever it calls) to determine exactly
what it hashes and whether the input is derivable without genuine disc
hardware, or whether it depends on raw/weak-sector data a plain `.iso`
structurally can't contain.

## What `tools/unwrap.py` does and doesn't do

Given the above, `unwrap.py` does not decrypt anything itself. It performs
the PE-level transformation only:

- drops the `stxt*` stub sections
- rewires `AddressOfEntryPoint` to the real OEP (found via
  `analysis/emulate_stub.py`, independent of any reference file)
- recomputes `SizeOfImage`/section count
- repoints the Import Directory
- sources `.text`/`.rdata`/`.data` *content* from a reference exe whose
  protection has already been removed (matched by identical raw
  offset/size per section - if a reference's section layout doesn't match
  the original's, the tool refuses rather than silently producing garbage)

This reproduces "no-disc-needed" functionality using a known-good
reference for the two payloads we haven't independently derived
(decrypted `.text`, rebuilt import table) - feature-equivalent, not
byte-identical, and fully inspectable/understood for every other part of
the transformation.
