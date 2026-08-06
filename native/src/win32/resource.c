/*
 * resource.c - PE resource lookup, out of the mapped image.
 *
 * This file used to fail every call, on the reasoning that a native ELF has no
 * .rsrc and that the game only wanted its icon and its version block. The first
 * half is still true and the second half was wrong: the game loads its *shader
 * effects* from resources. `FUN_005d1a60` asks for `IDI_WORLD_FX` and friends,
 * stores the effect it gets back at `this+0x20`, and the next call dereferences it -
 * so a failed lookup here surfaces as a null-pointer fault deep in renderer setup,
 * with nothing in between to suggest resources were involved.
 *
 * No extraction tool turned out to be needed. src/loader/pe_loader.c maps every
 * section of the exe at its own base, `.rsrc` among them, so the resource directory
 * is already in memory in exactly the layout the PE/COFF specification describes -
 * a three-level tree of type, then name, then language. Walking it is the whole
 * implementation, and it is the same data Windows would have been reading.
 *
 * Everything is found relative to the image base, which is what an HMODULE is (see
 * module.c). Nothing here is specific to this game.
 */
#include "shim_internal.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* PE/COFF, all offsets from the specification. */
#define DOS_E_LFANEW           0x3c
#define COFF_SIZE_OF_OPTIONAL  0x10
#define COFF_HEADER_SIZE       0x14
#define OPT_DATA_DIRECTORY     0x60
#define DIRECTORY_ENTRY_RESOURCE 2

/* IMAGE_RESOURCE_DIRECTORY: 16 bytes, then the entries. */
#define DIR_NAMED_COUNT        0x0c
#define DIR_ID_COUNT           0x0e
#define DIR_ENTRIES            0x10

/* IMAGE_RESOURCE_DIRECTORY_ENTRY */
#define ENTRY_NAME             0x00
#define ENTRY_OFFSET           0x04
#define ENTRY_SIZE             0x08

/* IMAGE_RESOURCE_DATA_ENTRY */
#define DATA_RVA               0x00
#define DATA_SIZE              0x04

#define HIGH_BIT               0x80000000u

static unsigned int read32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned short read16(const unsigned char *p)
{
    return (unsigned short)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

/*
 * An identifier is either a small integer packed into the pointer
 * (MAKEINTRESOURCE) or a string. IS_INTRESOURCE is the documented test.
 */
static int identifier_is_id(LPCSTR value)
{
    return ((uintptr_t)value >> 16) == 0;
}

/* The image the module handle refers to, or NULL if it is not one we know. */
static const unsigned char *image_for(HMODULE module)
{
    HMODULE self = GetModuleHandleA(NULL);

    if (!self)
        return NULL;
    if (module && module != self) {
        nfsu2_shim_trace("resources: module %p is not the executable", (void *)module);
        return NULL;
    }
    return (const unsigned char *)self;
}

/* The .rsrc directory, and its size, inside the mapped image. */
static const unsigned char *resource_root(const unsigned char *image, unsigned int *size)
{
    unsigned int pe_offset, optional_size, rva;
    const unsigned char *optional;

    if (image[0] != 'M' || image[1] != 'Z')
        return NULL;
    pe_offset = read32(image + DOS_E_LFANEW);
    if (memcmp(image + pe_offset, "PE\0\0", 4) != 0)
        return NULL;
    optional_size = read16(image + pe_offset + 4 + COFF_SIZE_OF_OPTIONAL);
    if (optional_size < OPT_DATA_DIRECTORY + (DIRECTORY_ENTRY_RESOURCE + 1) * 8)
        return NULL;
    optional = image + pe_offset + 4 + COFF_HEADER_SIZE;
    rva = read32(optional + OPT_DATA_DIRECTORY + DIRECTORY_ENTRY_RESOURCE * 8);
    if (size)
        *size = read32(optional + OPT_DATA_DIRECTORY + DIRECTORY_ENTRY_RESOURCE * 8 + 4);
    if (!rva)
        return NULL;
    return image + rva;
}

/*
 * Compare a resource directory entry's name against an ANSI identifier. Names in
 * the tree are UTF-16 with a leading length and are *not* null-terminated;
 * comparison is case-insensitive, as Windows does it.
 */
static int name_matches(const unsigned char *root, unsigned int name_offset, LPCSTR wanted)
{
    const unsigned char *entry = root + (name_offset & ~HIGH_BIT);
    unsigned short length = read16(entry);
    unsigned short i;

    for (i = 0; i < length; i++) {
        unsigned short wide = read16(entry + 2 + i * 2);

        if (!wanted[i])
            return 0;
        if (wide > 0x7f)
            return 0; /* nothing in this game's tree is non-ASCII */
        if (tolower((unsigned char)wide) != tolower((unsigned char)wanted[i]))
            return 0;
    }
    return wanted[length] == '\0';
}

/*
 * One level of the tree. Returns the entry's OffsetToData field, or 0 if absent -
 * an offset of 0 cannot be valid, since the directory itself is there.
 */
static unsigned int find_entry(const unsigned char *root, const unsigned char *directory,
                               LPCSTR identifier)
{
    unsigned int named = read16(directory + DIR_NAMED_COUNT);
    unsigned int ids = read16(directory + DIR_ID_COUNT);
    const unsigned char *entries = directory + DIR_ENTRIES;
    unsigned int i;

    if (identifier_is_id(identifier)) {
        unsigned int wanted = (unsigned int)(uintptr_t)identifier & 0xffff;

        /* Integer entries follow the named ones and are sorted, but a linear scan
         * over a few dozen is not worth a binary search. */
        for (i = named; i < named + ids; i++) {
            const unsigned char *entry = entries + i * ENTRY_SIZE;

            if ((read32(entry + ENTRY_NAME) & 0xffff) == wanted)
                return read32(entry + ENTRY_OFFSET);
        }
        return 0;
    }

    for (i = 0; i < named; i++) {
        const unsigned char *entry = entries + i * ENTRY_SIZE;
        unsigned int name = read32(entry + ENTRY_NAME);

        if ((name & HIGH_BIT) && name_matches(root, name, identifier))
            return read32(entry + ENTRY_OFFSET);
    }
    return 0;
}

/* The first entry of a directory, whatever it is - used for the language level,
 * where the game does not care and Windows would pick by locale. */
static unsigned int first_entry(const unsigned char *directory)
{
    unsigned int count = (unsigned int)read16(directory + DIR_NAMED_COUNT) +
                         (unsigned int)read16(directory + DIR_ID_COUNT);

    if (count == 0)
        return 0;
    return read32(directory + DIR_ENTRIES + ENTRY_OFFSET);
}

static void trace_lookup(const char *api, LPCSTR type, LPCSTR name, const void *found)
{
    char type_text[64];
    char name_text[64];

    if (!nfsu2_shim_trace_enabled())
        return;
    if (identifier_is_id(type))
        snprintf(type_text, sizeof(type_text), "#%u", (unsigned)(uintptr_t)type & 0xffff);
    else
        snprintf(type_text, sizeof(type_text), "%s", type);
    if (identifier_is_id(name))
        snprintf(name_text, sizeof(name_text), "#%u", (unsigned)(uintptr_t)name & 0xffff);
    else
        snprintf(name_text, sizeof(name_text), "%s", name);
    nfsu2_shim_trace("%s(%s, %s) = %p", api, type_text, name_text, found);
}

static HRSRC find_resource(HMODULE module, LPCSTR name, LPCSTR type)
{
    const unsigned char *image = image_for(module);
    const unsigned char *root;
    unsigned int offset;
    const unsigned char *level;

    if (!image)
        return NULL;
    root = resource_root(image, NULL);
    if (!root) {
        nfsu2_shim_trace("resources: the image has no resource directory");
        return NULL;
    }

    /* Type, then name, then language - the tree's three levels. */
    offset = find_entry(root, root, type);
    if (!offset || !(offset & HIGH_BIT))
        return NULL;
    level = root + (offset & ~HIGH_BIT);

    offset = find_entry(root, level, name);
    if (!offset)
        return NULL;
    if (offset & HIGH_BIT) {
        level = root + (offset & ~HIGH_BIT);
        offset = first_entry(level);
        if (!offset || (offset & HIGH_BIT))
            return NULL;
    }
    /* A leaf: IMAGE_RESOURCE_DATA_ENTRY, which is what an HRSRC is here. */
    return (HRSRC)(void *)(root + offset);
}

HRSRC WINAPI FindResourceA(HMODULE module, LPCSTR name, LPCSTR type)
{
    HRSRC found = find_resource(module, name, type);

    trace_lookup("FindResourceA", type, name, found);
    if (!found)
        SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
    return found;
}

HRSRC WINAPI FindResourceExA(HMODULE module, LPCSTR type, LPCSTR name, WORD language)
{
    /* The language level is taken as it comes: this tree has one entry per
     * resource, and picking by locale would need a locale to pick with. */
    (void)language;
    return FindResourceA(module, name, type);
}

HRSRC WINAPI FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type)
{
    /*
     * Not implemented, and not by omission: the identifiers would have to be
     * narrowed to compare against the tree, and nothing in this game imports the
     * wide form. It says so rather than returning a resource it did not look for.
     */
    (void)module; (void)name; (void)type;
    NFSU2_STUB("FindResourceW");
    SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
    return NULL;
}

/*
 * LoadResource and LockResource are two steps of one thing here. On Windows the
 * first commits the pages and the second gives a pointer; in a mapped image the
 * bytes are already there, so both resolve to the same address and FreeResource has
 * nothing to undo.
 */
HGLOBAL WINAPI LoadResource(HMODULE module, HRSRC resource)
{
    const unsigned char *image = image_for(module);
    const unsigned char *data = (const unsigned char *)resource;

    if (!image || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    return (HGLOBAL)(void *)(image + read32(data + DATA_RVA));
}

LPVOID WINAPI LockResource(HGLOBAL resource)
{
    return (LPVOID)resource;
}

BOOL WINAPI FreeResource(HGLOBAL resource)
{
    (void)resource;
    return TRUE;
}

DWORD WINAPI SizeofResource(HMODULE module, HRSRC resource)
{
    const unsigned char *data = (const unsigned char *)resource;

    (void)module;
    if (!data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return read32(data + DATA_SIZE);
}
