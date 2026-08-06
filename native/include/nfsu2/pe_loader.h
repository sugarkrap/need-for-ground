/*
 * pe_loader.h - map a 32-bit PE's sections into this process, at its own base.
 *
 * The point is differential testing. A ported function and the original
 * machine code can then be called with identical inputs and their results
 * compared, which is the only way to verify a port of code whose intended
 * behaviour is not documented anywhere. It is also the first piece of the
 * hybrid-execution path: a partially-ported binary has to be able to call into
 * not-yet-ported original code, and this is how that code gets into memory.
 *
 * What it does: maps every section at ImageBase + VirtualAddress, copies the raw
 * bytes, zero-fills the tail where VirtualSize exceeds SizeOfRawData, and sets
 * per-section protection.
 *
 * What it deliberately does not do yet, because none of it is needed to call a
 * self-contained function, and each would be guesswork until something actually
 * requires it:
 *
 *   - imports. The import directory is not walked, so a function that calls a
 *     Win32 API through the IAT will jump to a zero. The shim it would need is
 *     already here (win32/module.c resolves names), so this is a small step when
 *     something needs it.
 *   - relocations. This exe has a fixed base and no .reloc, so mapping anywhere
 *     other than 0x400000 would break absolute addresses. The loader fails
 *     rather than relocating.
 *   - the TEB and %fs. MSVC code reads fs:[0] for SEH and fs:[0x2c] for TLS. On
 *     i386 Linux %fs already belongs to glibc's TLS, so any function with a
 *     __try block or a __declspec(thread) access reads glibc's data and
 *     misbehaves. Wine solves this with a custom LDT entry via modify_ldt();
 *     that is the next piece of work when a function needs it.
 *
 * Nothing here embeds game data: the caller supplies the path to their own
 * legally-owned exe at runtime.
 */
#ifndef NFSU2_PE_LOADER_H
#define NFSU2_PE_LOADER_H

#include <stddef.h>

struct nfsu2_pe_image {
    unsigned int image_base;   /* the PE's own ImageBase, e.g. 0x400000 */
    unsigned int image_size;   /* SizeOfImage, rounded to a page */
    unsigned int entry_point;  /* absolute VA of AddressOfEntryPoint */
    void *mapping;             /* what was mapped; == (void *)image_base */
    int section_count;
};

/*
 * Map `path` into this process. Returns 0 on success, or a negative errno.
 * `error` receives a human-readable reason on failure when non-NULL.
 */
int nfsu2_pe_load(const char *path, struct nfsu2_pe_image *out,
                  char *error, size_t error_size);

/* Unmap. Safe to call on a zeroed struct. */
void nfsu2_pe_unload(struct nfsu2_pe_image *image);

/*
 * Turn an address as it appears in the decompiled output (an absolute VA, e.g.
 * 0x43ce40) into a callable pointer. Returns NULL if it falls outside the image.
 */
void *nfsu2_pe_function(const struct nfsu2_pe_image *image, unsigned int virtual_address);

#endif /* NFSU2_PE_LOADER_H */
