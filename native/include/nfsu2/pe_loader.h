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
 * Imports *are* resolved, on request: nfsu2_pe_resolve_imports() walks the import
 * directory and points each IAT slot at our own shim, so original code can call
 * CreateFileA and land in win32/file.c. That is what makes incremental porting
 * possible - a ported function can call one that is not ported yet, and vice
 * versa.
 *
 * What it deliberately does not do: relocations. This exe has a fixed base and no
 * .reloc, so mapping anywhere other than 0x400000 would break absolute
 * addresses, and the loader fails rather than relocating.
 *
 * The other half of "call anything in the binary" is the TEB: MSVC code reads
 * fs:[0] for SEH and fs:[0x2c] for TLS on every function with a __try. That lives
 * in teb.h and is a separate call - mapping the image and giving the calling
 * thread a %fs are independent, and a thread that only runs leaf functions needs
 * only the former.
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
    unsigned int import_directory; /* RVA of the import table, 0 if absent */
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

struct nfsu2_pe_import_stats {
    int libraries;
    int total;
    int resolved;
    int unresolved;
    int by_ordinal;  /* ordinal imports, which cannot map to an ELF symbol */
};

/*
 * Point every IAT slot at our shim, by name, via dlsym(RTLD_DEFAULT, ...) - the
 * same mechanism GetProcAddress uses (see win32/module.c), which is why the
 * per-DLL visibility macros in win32_dllmacros.h matter here too.
 *
 * Unresolved imports are left as they were and counted rather than faked: a slot
 * pointing at a plausible-looking stub would turn "this API is missing" into a
 * crash somewhere else entirely. Call nfsu2_pe_set_import_reporter() first to see
 * which ones they are.
 *
 * Returns 0 if every named import resolved, -ENOSYS if some did not (the image is
 * still usable - just not for code paths that need them), or another negative
 * errno on a malformed import directory.
 */
int nfsu2_pe_resolve_imports(struct nfsu2_pe_image *image,
                             struct nfsu2_pe_import_stats *stats);

/* Called once per unresolved import, for diagnostics. NULL disables it. */
void nfsu2_pe_set_import_reporter(void (*reporter)(const char *library, const char *symbol));

#endif /* NFSU2_PE_LOADER_H */
