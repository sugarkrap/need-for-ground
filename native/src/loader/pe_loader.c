/*
 * pe_loader.c - the minimal PE section mapper described in pe_loader.h.
 *
 * Headers are parsed by hand from the raw bytes rather than through Wine's PE
 * structures. That is deliberate: this file must not depend on the Win32 type
 * universe, so it can be linked into any harness, and the parsing is a couple of
 * dozen well-defined offsets that are clearer read explicitly than through three
 * layers of typedef.
 */
#include <nfsu2/pe_loader.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Offsets within the PE headers, from the PE/COFF specification. The COFF ones
 * are relative to the start of IMAGE_FILE_HEADER, which is pe_offset + 4 - after
 * the "PE\0\0" signature. Getting that base wrong is easy and shows up as
 * "not an i386 image", because Machine then reads TimeDateStamp's low half.
 */
#define DOS_E_LFANEW            0x3c
#define COFF_MACHINE            0x00
#define COFF_NUMBER_OF_SECTIONS 0x02
#define COFF_SIZE_OF_OPTIONAL   0x10
#define COFF_HEADER_SIZE        0x14
#define OPT_MAGIC               0x00
#define OPT_ENTRY_POINT         0x10
#define OPT_IMAGE_BASE          0x1c
#define OPT_SECTION_ALIGNMENT   0x20
#define OPT_SIZE_OF_IMAGE       0x38
#define OPT_SIZE_OF_HEADERS     0x3c
#define SECTION_HEADER_SIZE     40
#define SECTION_VIRTUAL_SIZE    0x08
#define SECTION_VIRTUAL_ADDRESS 0x0c
#define SECTION_RAW_SIZE        0x10
#define SECTION_RAW_POINTER     0x14
#define SECTION_CHARACTERISTICS 0x24

/* Data directory 1 is the import table; each entry is a VA/size pair. */
#define OPT_DATA_DIRECTORY      0x60
#define DIRECTORY_ENTRY_IMPORT  1

/* IMAGE_IMPORT_DESCRIPTOR */
#define IMPORT_ORIGINAL_FIRST_THUNK 0x00
#define IMPORT_NAME                 0x0c
#define IMPORT_FIRST_THUNK          0x10
#define IMPORT_DESCRIPTOR_SIZE      20

#define IMAGE_ORDINAL_FLAG      0x80000000u

#define IMAGE_FILE_MACHINE_I386 0x014c
#define PE32_MAGIC              0x010b

#define SCN_MEM_EXECUTE 0x20000000u
#define SCN_MEM_READ    0x40000000u
#define SCN_MEM_WRITE   0x80000000u

static unsigned short read16(const unsigned char *p) { return (unsigned short)(p[0] | (p[1] << 8)); }
static unsigned int read32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int fail(char *error, size_t size, int code, const char *fmt, ...)
{
    va_list args;

    if (error && size) {
        va_start(args, fmt);
        vsnprintf(error, size, fmt, args);
        va_end(args);
    }
    return code;
}

/*
 * Tell the shim where the image landed, so GetModuleHandleA(NULL) can return the
 * image base - which is what an HMODULE *is* on Windows, and what MSVC's CRT
 * startup reads a DOS header out of. Weak, like nfsu2_winsock_lookup above, so a
 * harness that maps a PE without linking the shim still builds.
 */
__attribute__((weak)) void nfsu2_module_set_image_base(void *base);

int nfsu2_pe_load(const char *path, struct nfsu2_pe_image *out, char *error, size_t error_size)
{
    unsigned char *file = NULL;
    struct stat st;
    int fd;
    unsigned int pe_offset, section_table, image_base, image_size, page_size;
    unsigned int import_directory = 0;
    unsigned int size_of_headers = 0;
    unsigned short section_count, optional_size;
    void *mapping;
    int i;

    if (!path || !out)
        return fail(error, error_size, -EINVAL, "no path");
    memset(out, 0, sizeof(*out));

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fail(error, error_size, -errno, "cannot open %s: %s", path, strerror(errno));
    if (fstat(fd, &st) != 0 || st.st_size < 0x200) {
        close(fd);
        return fail(error, error_size, -EINVAL, "%s is not a PE image", path);
    }

    file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED)
        return fail(error, error_size, -errno, "cannot map %s: %s", path, strerror(errno));

    if (file[0] != 'M' || file[1] != 'Z') {
        munmap(file, (size_t)st.st_size);
        return fail(error, error_size, -EINVAL, "no MZ signature");
    }
    pe_offset = read32(file + DOS_E_LFANEW);
    if (pe_offset + COFF_HEADER_SIZE + 0x40 > (unsigned int)st.st_size ||
        memcmp(file + pe_offset, "PE\0\0", 4) != 0) {
        munmap(file, (size_t)st.st_size);
        return fail(error, error_size, -EINVAL, "no PE signature");
    }

    if (read16(file + pe_offset + 4 + COFF_MACHINE) != IMAGE_FILE_MACHINE_I386) {
        munmap(file, (size_t)st.st_size);
        return fail(error, error_size, -ENOEXEC, "not an i386 image");
    }
    section_count = read16(file + pe_offset + 4 + COFF_NUMBER_OF_SECTIONS);
    optional_size = read16(file + pe_offset + 4 + COFF_SIZE_OF_OPTIONAL);

    {
        const unsigned char *opt = file + pe_offset + 4 + COFF_HEADER_SIZE;

        if (read16(opt + OPT_MAGIC) != PE32_MAGIC) {
            munmap(file, (size_t)st.st_size);
            return fail(error, error_size, -ENOEXEC, "not a PE32 image (PE32+ is 64-bit)");
        }
        image_base = read32(opt + OPT_IMAGE_BASE);
        image_size = read32(opt + OPT_SIZE_OF_IMAGE);
        size_of_headers = read32(opt + OPT_SIZE_OF_HEADERS);
        out->entry_point = image_base + read32(opt + OPT_ENTRY_POINT);
        import_directory = read32(opt + OPT_DATA_DIRECTORY
          + DIRECTORY_ENTRY_IMPORT * 8);
    }
    section_table = pe_offset + 4 + COFF_HEADER_SIZE + optional_size;

    page_size = (unsigned int)sysconf(_SC_PAGESIZE);
    image_size = (image_size + page_size - 1) & ~(page_size - 1);

    /*
     * MAP_FIXED_NOREPLACE, not MAP_FIXED: this exe has no .reloc, so it can only
     * run at its own ImageBase - but silently unmapping whatever already lives
     * there would be far worse than failing. In a 32-bit process 0x400000 is
     * normally free; a PIE loaded low is the case that would collide.
     */
    mapping = mmap((void *)(uintptr_t)image_base, image_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapping == MAP_FAILED) {
        int saved = errno;
        munmap(file, (size_t)st.st_size);
        return fail(error, error_size, -saved,
                    "cannot map %u KiB at the image base 0x%x: %s "
                    "(no .reloc, so it cannot be moved)",
                    image_size / 1024, image_base, strerror(saved));
    }
    if (mapping != (void *)(uintptr_t)image_base) {
        munmap(mapping, image_size);
        munmap(file, (size_t)st.st_size);
        return fail(error, error_size, -ENOMEM, "0x%x is already occupied", image_base);
    }

    /*
     * The headers, at the image base, exactly as Windows maps them - and this is
     * not for completeness. An HMODULE *is* the image base there, so code that
     * asks for its own module handle gets a pointer it can read a DOS header out
     * of, and MSVC's CRT startup does precisely that: GetModuleHandleA(NULL) and
     * then `cmp WORD PTR [eax], 0x5a4d` for 'MZ', followed by a walk to the PE
     * header. Without the headers mapped it reads zeroes and takes an error path
     * that goes nowhere good.
     */
    if (size_of_headers > 0 && (size_t)size_of_headers <= (size_t)st.st_size) {
        size_t copy = size_of_headers;

        if (copy > image_size)
            copy = image_size;
        memcpy(mapping, file, copy);
    }

    for (i = 0; i < section_count; i++) {
        const unsigned char *header = file + section_table + (unsigned int)i * SECTION_HEADER_SIZE;
        unsigned int virtual_size = read32(header + SECTION_VIRTUAL_SIZE);
        unsigned int virtual_address = read32(header + SECTION_VIRTUAL_ADDRESS);
        unsigned int raw_size = read32(header + SECTION_RAW_SIZE);
        unsigned int raw_pointer = read32(header + SECTION_RAW_POINTER);
        unsigned char *destination = (unsigned char *)mapping + virtual_address;
        unsigned int copy = raw_size < virtual_size ? raw_size : virtual_size;

        if (virtual_address + virtual_size > image_size)
            continue; /* a section outside SizeOfImage is malformed; skip it */
        if (raw_pointer + copy > (unsigned int)st.st_size)
            continue;

        if (copy)
            memcpy(destination, file + raw_pointer, copy);
        /* The mapping is fresh anonymous memory, so the VirtualSize tail is
         * already zero - .bss needs nothing further. */
    }

    /* Protection is applied after all copying, so an executable section can be
     * written first and only then sealed. */
    for (i = 0; i < section_count; i++) {
        const unsigned char *header = file + section_table + (unsigned int)i * SECTION_HEADER_SIZE;
        unsigned int virtual_size = read32(header + SECTION_VIRTUAL_SIZE);
        unsigned int virtual_address = read32(header + SECTION_VIRTUAL_ADDRESS);
        unsigned int characteristics = read32(header + SECTION_CHARACTERISTICS);
        unsigned char *destination = (unsigned char *)mapping + virtual_address;
        unsigned int length = (virtual_size + page_size - 1) & ~(page_size - 1);
        int protection = 0;

        if (!length || virtual_address + length > image_size)
            continue;
        if (characteristics & SCN_MEM_READ)
            protection |= PROT_READ;
        if (characteristics & SCN_MEM_WRITE)
            protection |= PROT_WRITE;
        if (characteristics & SCN_MEM_EXECUTE)
            protection |= PROT_EXEC;
        if (!protection)
            protection = PROT_READ;

        if (mprotect(destination, length, protection) != 0) {
            int saved = errno;
            munmap(mapping, image_size);
            munmap(file, (size_t)st.st_size);
            return fail(error, error_size, -saved, "mprotect failed: %s", strerror(saved));
        }
    }

    munmap(file, (size_t)st.st_size);

    out->image_base = image_base;
    out->image_size = image_size;
    out->mapping = mapping;
    if (nfsu2_module_set_image_base)
        nfsu2_module_set_image_base(mapping);
    out->section_count = section_count;
    out->import_directory = import_directory;
    return 0;
}

/* --- imports ------------------------------------------------------------ */

static void (*g_import_reporter)(const char *library, const char *symbol);

/*
 * ws2_32's entry points are hidden on purpose (their names collide with libc's),
 * so dlsym cannot see them, and the game imports most of Winsock by ordinal
 * anyway. The shim provides an explicit table; weak, so a harness that does not
 * link the sockets shim still builds.
 */
__attribute__((weak)) void *nfsu2_winsock_lookup(const char *name, unsigned ordinal);

/*
 * And d3d9.dll, for a different reason: DXVK *does* export Direct3DCreate9, so
 * dlsym would find it - and it is the wrong one. It is __cdecl and assumes a
 * 16-byte-aligned stack, neither of which is true of the game's call. The bridge
 * (src/d3d9_bridge/) provides a __stdcall, stack-realigning entry point that returns
 * wrapped interfaces, and it must win. Weak, so hosts without the bridge still link.
 */
__attribute__((weak)) void *nfsu2_d3d9_lookup(const char *name);

static int library_is(const char *library, const char *name)
{
    size_t i;

    for (i = 0; library[i] && name[i]; i++) {
        char a = library[i], b = name[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = (char)(b - 'A' + 'a');
        if (a != b)
            return 0;
    }
    return library[i] == name[i];
}

void nfsu2_pe_set_import_reporter(void (*reporter)(const char *library, const char *symbol))
{
    g_import_reporter = reporter;
}

/* Bounds-checked pointer to an RVA inside the mapped image. */
static void *at_rva(const struct nfsu2_pe_image *image, unsigned int rva, size_t need)
{
    if (!rva || rva >= image->image_size || rva + need > image->image_size)
        return NULL;
    return (unsigned char *)image->mapping + rva;
}

int nfsu2_pe_resolve_imports(struct nfsu2_pe_image *image, struct nfsu2_pe_import_stats *stats)
{
    struct nfsu2_pe_import_stats local = { 0, 0, 0, 0, 0 };
    unsigned int page_size = (unsigned int)sysconf(_SC_PAGESIZE);
    const unsigned char *descriptor;

    if (!image || !image->mapping)
        return -EINVAL;
    if (!image->import_directory) {
        if (stats)
            *stats = local;
        return 0; /* nothing to do; not an error */
    }

    descriptor = at_rva(image, image->import_directory, IMPORT_DESCRIPTOR_SIZE);
    if (!descriptor)
        return -EINVAL;

    for (;; descriptor += IMPORT_DESCRIPTOR_SIZE) {
        unsigned int lookup_rva = read32(descriptor + IMPORT_ORIGINAL_FIRST_THUNK);
        unsigned int iat_rva = read32(descriptor + IMPORT_FIRST_THUNK);
        unsigned int name_rva = read32(descriptor + IMPORT_NAME);
        const char *library;
        unsigned int *iat;
        const unsigned char *lookup;
        unsigned int index;

        if (!lookup_rva && !iat_rva)
            break; /* the terminating all-zero descriptor */

        library = at_rva(image, name_rva, 1);
        if (!library)
            library = "(unnamed)";

        /*
         * The lookup table (OriginalFirstThunk) holds the names and the IAT holds
         * the addresses. They start identical, but a bound import has the IAT
         * pre-filled with addresses from another machine - so names must come
         * from the lookup table when it exists, or they would be read as
         * pointers.
         */
        lookup = at_rva(image, lookup_rva ? lookup_rva : iat_rva, 4);
        iat = at_rva(image, iat_rva, 4);
        if (!lookup || !iat)
            return -EINVAL;

        /*
         * The IAT usually lives in read-only .rdata: on Windows the loader fills
         * it in before applying protection. Ours applied protection already, so
         * make the pages writable for the patch. They stay writable, which is
         * what Windows does for an IAT the loader has touched anyway.
         */
        {
            unsigned char *page = (unsigned char *)((uintptr_t)iat & ~(uintptr_t)(page_size - 1));
            size_t span = (size_t)((unsigned char *)iat - page) + 4096u * 4u;

            if (mprotect(page, span, PROT_READ | PROT_WRITE) != 0)
                return -errno;
        }

        local.libraries++;

        for (index = 0; ; index++) {
            unsigned int entry = read32(lookup + (size_t)index * 4u);
            const unsigned char *hint;
            const char *symbol;
            void *address;

            if (!entry)
                break;
            local.total++;

            if (entry & IMAGE_ORDINAL_FLAG) {
                /* An ordinal has no name to look up; see win32/module.c. */
                unsigned ordinal = entry & 0xffffu;
                void *by_ordinal = NULL;

                local.by_ordinal++;

                if (nfsu2_winsock_lookup && library_is(library, "ws2_32.dll"))
                    by_ordinal = nfsu2_winsock_lookup(NULL, ordinal);

                if (by_ordinal) {
                    iat[index] = (unsigned int)(uintptr_t)by_ordinal;
                    local.resolved++;
                    continue;
                }

                local.unresolved++;
                if (g_import_reporter) {
                    char text[32];

                    snprintf(text, sizeof(text), "#%u (ordinal)", ordinal);
                    g_import_reporter(library, text);
                }
                continue;
            }

            hint = at_rva(image, entry, 3);
            if (!hint) {
                local.unresolved++;
                continue;
            }
            symbol = (const char *)hint + 2; /* skip the ordinal hint */

            /*
             * The bridge goes *first* for d3d9.dll, not as a fallback: dlsym would
             * find DXVK's own export, which is the wrong ABI for this caller.
             */
            address = NULL;
            if (nfsu2_d3d9_lookup && library_is(library, "d3d9.dll"))
                address = nfsu2_d3d9_lookup(symbol);
            if (!address)
                address = dlsym(RTLD_DEFAULT, symbol);
            if (!address && nfsu2_winsock_lookup && library_is(library, "ws2_32.dll"))
                address = nfsu2_winsock_lookup(symbol, 0);
            if (!address) {
                local.unresolved++;
                if (g_import_reporter)
                    g_import_reporter(library, symbol);
                continue;
            }

            iat[index] = (unsigned int)(uintptr_t)address;
            local.resolved++;
        }
    }

    if (stats)
        *stats = local;
    return local.unresolved ? -ENOSYS : 0;
}

void nfsu2_pe_unload(struct nfsu2_pe_image *image)
{
    if (!image || !image->mapping)
        return;
    munmap(image->mapping, image->image_size);
    memset(image, 0, sizeof(*image));
}

void *nfsu2_pe_function(const struct nfsu2_pe_image *image, unsigned int virtual_address)
{
    if (!image || !image->mapping)
        return NULL;
    if (virtual_address < image->image_base ||
        virtual_address >= image->image_base + image->image_size)
        return NULL;
    return (void *)(uintptr_t)virtual_address;
}
