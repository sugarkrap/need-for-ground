/*
 * game_functions_selftest.c - runs the game's own code, natively.
 *
 * Two independent verifications per function, which is the point:
 *
 *  1. **Against a reference.** The CRT functions are checked against libc, and
 *     the game's own maths against a straightforward recomputation. This catches
 *     a port that compiles but computes the wrong thing.
 *  2. **Against the original machine code.** When an unwrapped speed2.exe is
 *     available (--exe, or $NFSU2_EXE), its sections are mapped into this
 *     process and the *same inputs* are pushed through both the ported C and the
 *     original compiled function, then compared byte for byte. That is the only
 *     verification that does not depend on someone having guessed the intended
 *     behaviour correctly, and it is what makes porting the rest tractable.
 *
 * The ported functions themselves are not in the repo: they are generated from
 * your local decompiled/ output by native/tools/import_decompiled.py, because
 * decompiled code is a derivative of the copyrighted binary. This file, the
 * manifest and the tooling are ours.
 *
 * Exits 77 (meson SKIP) when the generated functions are absent.
 */
#include <nfsu2/ghidra_types.h>
#include <nfsu2/pe_loader.h>

#include "game_functions.h"
#include "game_originals.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_differential_checks;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            printf("ok   - " __VA_ARGS__);                                      \
            printf("\n");                                                       \
        } else {                                                                \
            printf("FAIL - " __VA_ARGS__);                                      \
            printf("  (%s:%d)\n", __FILE__, __LINE__);                          \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

/*
 * No addresses or function-pointer typedefs here: game_originals.h is generated
 * from the manifest, so the address and the recovered calling convention of each
 * original arrive with it. NFSU2_ORIGINAL(symbol, image) resolves one.
 */

/* --- 1. against a reference --------------------------------------------- */

/*
 * Truncation is the behaviour under test here - strncpy's contract is precisely
 * what it does when the source does not fit - so gcc's warning about it is
 * noise in this one function.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"

static void test_against_reference(void)
{
    char ported[32];
    char reference[32];
    float a[3] = { 1.0f, 2.0f, 3.0f };
    float b[3] = { 4.0f, 6.0f, 15.0f };
    float destination[4] = { 0 };
    float source[3] = { 2.0f, 3.0f, 4.0f };
    undefined4 copy_in[8];
    undefined4 copy_out[8];
    size_t i;

    printf("\n# ported code vs a reference implementation\n");

    /* strncpy's whole contract is the padding behaviour, so check a short source
     * (must zero-fill to n) and an exact-length one (must not terminate). */
    memset(ported, 0xaa, sizeof(ported));
    memset(reference, 0xaa, sizeof(reference));
    _strncpy(ported, "drift", 16);
    strncpy(reference, "drift", 16);
    CHECK(memcmp(ported, reference, 16) == 0,
          "_strncpy(\"drift\", 16) matches libc, padding included");

    memset(ported, 0xaa, sizeof(ported));
    memset(reference, 0xaa, sizeof(reference));
    _strncpy(ported, "underground", 5);
    strncpy(reference, "underground", 5);
    CHECK(memcmp(ported, reference, 8) == 0,
          "_strncpy with n < strlen leaves the string unterminated, as libc does");

    /* The word-at-a-time path only engages on an aligned source, and the
     * unaligned prologue is separate code - exercise both. */
    {
        char buffer[24] = "xxxxxxxxxxxxxxxxxxxxxxx";
        char *unaligned = buffer + 1;
        memset(ported, 0xaa, sizeof(ported));
        memset(reference, 0xaa, sizeof(reference));
        memcpy(unaligned, "misaligned", 11);
        _strncpy(ported, unaligned, 20);
        strncpy(reference, unaligned, 20);
        CHECK(memcmp(ported, reference, 20) == 0,
              "_strncpy from an unaligned source matches (the byte prologue path)");
    }

    CHECK(_strrchr("data/frontend/fe_art.bun", '/') ==
              strrchr("data/frontend/fe_art.bun", '/'),
          "_strrchr finds the last separator at the same address libc does");
    CHECK(_strrchr("no-separator-here", '/') == NULL,
          "_strrchr returns NULL when the character is absent");
    CHECK(_strrchr("trailing/", '/') == strrchr("trailing/", '/'),
          "_strrchr handles a trailing match");

    /* The engine's inlined memcpy: dwords then a byte tail, so use a length that
     * is not a multiple of four. */
    for (i = 0; i < 8; i++) {
        copy_in[i] = (undefined4)(0x11111111u * (i + 1));
        copy_out[i] = 0;
    }
    FUN_00401000(copy_out, copy_in, 4 * 6 + 3);
    CHECK(memcmp(copy_out, copy_in, 27) == 0 && copy_out[6] >> 24 == 0,
          "FUN_00401000 copies 27 bytes exactly - dword body plus a 3-byte tail");

    /* 3D distance: (3,4,12) has length 13, which is exact in binary floating
     * point, so this can be compared without a tolerance argument. */
    CHECK((double)FUN_0043ce40(a, b) == 13.0,
          "FUN_0043ce40 is euclidean distance: |(1,2,3)-(4,6,15)| == 13");
    CHECK((double)FUN_0043ce40(a, a) == 0.0, "and zero for identical points");

    /* The vector op: destination[0] is a dot product, the rest are carried. */
    FUN_0048b710(source, destination, 2.0f, 3.0f, 4.0f, 5.0f);
    CHECK(destination[0] == 2.0f * 2.0f + 3.0f * 3.0f + 4.0f * 4.0f,
          "FUN_0048b710 writes the dot product (expected 29, got %g)",
          (double)destination[0]);
    CHECK(destination[1] == 3.0f && destination[2] == 4.0f && destination[3] == 5.0f,
          "and copies the remaining three components through");
}

#pragma GCC diagnostic pop

/* --- imports ------------------------------------------------------------- */

static int g_unresolved;
static char g_unresolved_list[24][64];

static void report_unresolved(const char *library, const char *symbol)
{
    if (g_unresolved < 24) {
        snprintf(g_unresolved_list[g_unresolved], sizeof(g_unresolved_list[0]),
                 "%s!%s", library, symbol);
    }
    g_unresolved++;
}

/*
 * Resolving the IAT is what makes incremental porting possible: original code can
 * call our shim, so a ported function may call an unported one and vice versa.
 */
static void test_imports(struct nfsu2_pe_image *image)
{
    struct nfsu2_pe_import_stats stats;
    int rc;
    int i;

    printf("\n# import resolution\n");

    nfsu2_pe_set_import_reporter(report_unresolved);
    rc = nfsu2_pe_resolve_imports(image, &stats);
    nfsu2_pe_set_import_reporter(NULL);

    printf("     %d libraries, %d imports: %d resolved, %d unresolved (%d by ordinal)\n",
           stats.libraries, stats.total, stats.resolved, stats.unresolved, stats.by_ordinal);
    for (i = 0; i < g_unresolved && i < 24; i++)
        printf("     unresolved: %s\n", g_unresolved_list[i]);

    CHECK(stats.libraries >= 10 && stats.total >= 200,
          "walked the import directory: %d libraries, %d imports",
          stats.libraries, stats.total);
    CHECK(stats.resolved > 0, "resolved %d imports against our own shim", stats.resolved);
    CHECK(rc == 0 || rc == -ENOSYS, "resolve returned a sane status (%d)", rc);

    /*
     * The decisive check: an IAT slot must now hold the address of *our* function.
     * Anything else - a plausible stub, a leftover bound address - would turn a
     * missing API into a crash somewhere unrelated.
     */
    {
        unsigned int *iat = NULL;
        unsigned int rva;
        int found = 0;

        /* KERNEL32's IAT, from the import directory dump; find GetTickCount's slot
         * by looking for our own address in the kernel32 block. */
        for (rva = 0x383000u; rva < 0x383500u && !found; rva += 4u) {
            iat = (unsigned int *)((unsigned char *)image->mapping + rva);
            if (*iat == (unsigned int)(uintptr_t)&GetTickCount)
                found = 1;
        }
        CHECK(found, "an IAT slot points at our own GetTickCount (%p)", (void *)&GetTickCount);
    }
}

/* --- 2. against the original machine code -------------------------------- */

static void test_against_original(const struct nfsu2_pe_image *image)
{
    nfsu2_original__strncpy original_ncpy = NFSU2_ORIGINAL(_strncpy, image);
    nfsu2_original__strrchr original_rchr = NFSU2_ORIGINAL(_strrchr, image);
    nfsu2_original_FUN_00401000 original_copy = NFSU2_ORIGINAL(FUN_00401000, image);
    nfsu2_original_FUN_0043ce40 original_dist = NFSU2_ORIGINAL(FUN_0043ce40, image);
    nfsu2_original_FUN_0048b710 original_dotp = NFSU2_ORIGINAL(FUN_0048b710, image);

    printf("\n# ported code vs the ORIGINAL machine code, same inputs\n");

    CHECK(original_ncpy && original_rchr && original_copy && original_dist && original_dotp,
          "all %d manifest addresses resolved inside the mapped image",
          NFSU2_ORIGINAL_COUNT);
    if (!original_ncpy || !original_dist)
        return;

    /* strncpy over a set of lengths and alignments, since the original switches
     * strategy on both. */
    {
        static const char *inputs[] = { "", "a", "abc", "aligned!", "a longer string here" };
        size_t lengths[] = { 1, 3, 4, 5, 8, 12, 21 };
        size_t input_index, length_index, offset;
        int mismatches = 0;

        for (input_index = 0; input_index < sizeof(inputs) / sizeof(inputs[0]); input_index++) {
            for (length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]);
                 length_index++) {
                for (offset = 0; offset < 4; offset++) {
                    char source_buffer[32];
                    char ported[48];
                    char original[48];
                    char *source = source_buffer + offset;

                    memset(source_buffer, 0, sizeof(source_buffer));
                    memcpy(source, inputs[input_index], strlen(inputs[input_index]) + 1);
                    memset(ported, 0x5a, sizeof(ported));
                    memset(original, 0x5a, sizeof(original));

                    _strncpy(ported, source, lengths[length_index]);
                    original_ncpy(original, source, lengths[length_index]);
                    g_differential_checks++;
                    if (memcmp(ported, original, sizeof(ported)) != 0)
                        mismatches++;
                }
            }
        }
        CHECK(mismatches == 0,
              "_strncpy matches the original across %d input/length/alignment combinations",
              5 * 7 * 4);
    }

    {
        static const char *haystacks[] = { "", "/", "a/b/c", "no-slash", "trailing/" };
        size_t i;
        int mismatches = 0;

        for (i = 0; i < sizeof(haystacks) / sizeof(haystacks[0]); i++) {
            char buffer[32];

            snprintf(buffer, sizeof(buffer), "%s", haystacks[i]);
            g_differential_checks++;
            if (_strrchr(buffer, '/') != original_rchr(buffer, '/'))
                mismatches++;
        }
        CHECK(mismatches == 0, "_strrchr matches the original on %zu haystacks",
              sizeof(haystacks) / sizeof(haystacks[0]));
    }

    {
        undefined4 source[16];
        undefined4 ported[16];
        undefined4 original[16];
        uint length;
        int mismatches = 0;
        size_t i;

        for (i = 0; i < 16; i++)
            source[i] = (undefined4)(0x01020304u * (i + 1));

        for (length = 0; length <= 40; length++) {
            memset(ported, 0xcd, sizeof(ported));
            memset(original, 0xcd, sizeof(original));
            FUN_00401000(ported, source, length);
            original_copy(original, source, length);
            g_differential_checks++;
            if (memcmp(ported, original, sizeof(ported)) != 0)
                mismatches++;
        }
        CHECK(mismatches == 0, "FUN_00401000 matches the original for every length 0..40");
    }

    {
        int mismatches = 0;
        int i;

        for (i = 0; i < 64; i++) {
            float first[3], second[3];
            long double ported, original;
            int axis;

            for (axis = 0; axis < 3; axis++) {
                first[axis] = (float)((i * 7 % 23) - 11) * 0.75f;
                second[axis] = (float)((i * 13 + axis) % 19) * 1.25f;
            }
            ported = FUN_0043ce40(first, second);
            original = original_dist(first, second);
            g_differential_checks++;
            /*
             * Bit-exact, not approximate. Both compute in x87 80-bit precision
             * (that is what float10 means), so anything other than equality
             * would mean the port changed the arithmetic - which is exactly what
             * this test exists to catch.
             */
            if (ported != original)
                mismatches++;
        }
        CHECK(mismatches == 0,
              "FUN_0043ce40 is bit-identical to the original over 64 point pairs");
    }

    {
        int mismatches = 0;
        int i;

        for (i = 0; i < 32; i++) {
            float source[3] = { (float)i, (float)i * 0.5f, (float)i * -2.0f };
            float ported[4] = { 0, 0, 0, 0 };
            float original[4] = { 0, 0, 0, 0 };

            FUN_0048b710(source, ported, (float)i, 2.0f, 3.0f, 4.0f);
            original_dotp(source, original, (float)i, 2.0f, 3.0f, 4.0f);
            g_differential_checks++;
            if (memcmp(ported, original, sizeof(ported)) != 0)
                mismatches++;
        }
        CHECK(mismatches == 0,
              "FUN_0048b710 matches the original over 32 inputs "
              "(including the __thiscall ECX argument)");
    }
}

int main(int argc, char **argv)
{
    struct nfsu2_pe_image image;
    const char *exe = getenv("NFSU2_EXE");
    char error[256] = "";
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exe") && i + 1 < argc)
            exe = argv[++i];
    }

    test_against_reference();

    if (!exe || !*exe) {
        printf("\n# no exe supplied (--exe PATH or $NFSU2_EXE)\n");
        printf("     the differential comparison against the original machine code\n"
               "     needs an unwrapped speed2.exe; skipping that half\n");
    } else if (nfsu2_pe_load(exe, &image, error, sizeof(error)) != 0) {
        printf("\n# could not map %s: %s\n", exe, error);
        g_failures++;
    } else {
        printf("\n# mapped %s\n", exe);
        printf("     base 0x%x, %u KiB, %d sections, entry 0x%x\n",
               image.image_base, image.image_size / 1024, image.section_count,
               image.entry_point);
        test_imports(&image);
        test_against_original(&image);
        printf("\n     %d differential comparisons against original code\n",
               g_differential_checks);
        nfsu2_pe_unload(&image);
    }

    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
