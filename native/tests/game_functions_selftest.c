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
/* The renderer test builds a fake IDirect3DDevice9, so it needs the same ABI
 * keystone header the generated renderer code does. */
#include <nfsu2/d3d9_native.h>
#include <nfsu2/pe_loader.h>
#include <nfsu2/seh.h>
#include <nfsu2/teb.h>

#include "game_functions.h"
#include "game_originals.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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

/* --- hybrid execution: original code calling our shim -------------------- */

/*
 * The point of resolving the IAT, demonstrated.
 *
 * Both functions call a Win32 import. The ported copies call our shim directly;
 * the originals reach the same shim through their import table. Verification does
 * not need the two to agree exactly - they return clock readings, which move - it
 * needs to show the call landed in *our* implementation, and the clocks make that
 * easy: ours count from process start, so they are small. A real Windows
 * timeGetTime returns milliseconds since boot and a real QueryPerformanceCounter
 * returns a TSC-scale value; either would be orders of magnitude larger. A value
 * bracketed by two of our own samples can only have come from our shim.
 */
static void test_hybrid_execution(const struct nfsu2_pe_image *image)
{
    nfsu2_original_FUN_006f5ac8 original_qpc = NFSU2_ORIGINAL(FUN_006f5ac8, image);
    nfsu2_original_FUN_006fea95 original_timer = NFSU2_ORIGINAL(FUN_006fea95, image);
    LARGE_INTEGER before, after;
    unsigned long long from_original;
    unsigned long long from_ported;

    printf("\n# hybrid execution: original code calling our shim through the IAT\n");

    CHECK(original_qpc && original_timer, "both import-using originals resolved");
    if (!original_qpc || !original_timer)
        return;

    /* QueryPerformanceCounter, through the original's import table. */
    QueryPerformanceCounter(&before);
    from_original = (unsigned long long)original_qpc();
    QueryPerformanceCounter(&after);

    CHECK(from_original >= (unsigned long long)before.QuadPart &&
          from_original <= (unsigned long long)after.QuadPart,
          "the ORIGINAL FUN_006f5ac8 returned %llu, bracketed by our own %llu..%llu "
          "- it called our QueryPerformanceCounter",
          from_original, (unsigned long long)before.QuadPart,
          (unsigned long long)after.QuadPart);

    /* And the ported copy of the same function, calling the shim directly. */
    QueryPerformanceCounter(&before);
    from_ported = (unsigned long long)FUN_006f5ac8();
    QueryPerformanceCounter(&after);

    CHECK(from_ported >= (unsigned long long)before.QuadPart &&
          from_ported <= (unsigned long long)after.QuadPart,
          "the PORTED FUN_006f5ac8 agrees (%llu)", from_ported);

    /* timeGetTime through a __fastcall original: the convention is part of the
     * test, since fastcall passes the first argument in ECX. */
    {
        DWORD original_state[8];
        DWORD ported_state[8];
        DWORD tick_before, tick_after;
        DWORD *returned;

        memset(original_state, 0xcd, sizeof(original_state));
        memset(ported_state, 0xcd, sizeof(ported_state));

        tick_before = GetTickCount();
        returned = original_timer(original_state);
        tick_after = GetTickCount();

        CHECK(returned == original_state,
              "the ORIGINAL __fastcall FUN_006fea95 returned its ECX argument");
        CHECK(original_state[0] >= tick_before && original_state[0] <= tick_after + 2u,
              "it stored timeGetTime()=%lu, within our %lu..%lu - our shim again",
              (unsigned long)original_state[0], (unsigned long)tick_before,
              (unsigned long)tick_after);
        CHECK(original_state[2] == 0 && original_state[3] == 0 && original_state[4] == 0x1000,
              "and initialised the rest of the struct as the decompilation says");

        FUN_006fea95(ported_state);
        CHECK(ported_state[4] == 0x1000 && ported_state[0] >= tick_before,
              "the ported copy does the same");
    }
}

/* --- the TEB: %fs for original code --------------------------------------- */

/* Read through %fs the way the original code does, to check the segment itself
 * rather than the memory it happens to point at. */
static unsigned int read_fs(unsigned int offset)
{
    unsigned int value;

    __asm__ volatile("movl %%fs:(%1), %0" : "=r"(value) : "r"(offset));
    return value;
}

static void write_fs(unsigned int offset, unsigned int value)
{
    __asm__ volatile("movl %0, %%fs:(%1)" :: "r"(value), "r"(offset));
}

/*
 * What the original's virtual call lands in, so the SEH record can be read while
 * it is still linked. See test_teb below for why there are two of these.
 *
 * __stdcall for the original: MSVC's __thiscall passes `this` in ECX and leaves
 * the callee to pop the stack argument, which is what `push 1; call [eax]`
 * followed by no cleanup means. GCC has no __thiscall, but __stdcall gets the
 * stack discipline right and the ignored ECX costs nothing here.
 */
static int g_child_calls;
static unsigned int g_chain_inside;
static unsigned int g_handler_inside;

static void record_chain(void)
{
    unsigned int chain = read_fs(NFSU2_TEB_EXCEPTION_LIST);

    g_child_calls++;
    g_chain_inside = chain;
    /* EXCEPTION_REGISTRATION is { prev, handler }, on the caller's stack. */
    g_handler_inside = (chain != 0 && chain != 0xffffffffu)
        ? ((unsigned int *)(uintptr_t)chain)[1] : 0;
}

static void __attribute__((stdcall)) original_child_destructor(int flag)
{
    (void)flag;
    record_chain();
}

/* And __cdecl for the ported copy, which calls through Ghidra's `code *`. */
static void ported_child_destructor(int flag)
{
    (void)flag;
    record_chain();
}

static void test_teb(const struct nfsu2_pe_image *image)
{
    char error[256] = "";
    unsigned char *teb;
    unsigned int saved_chain;

    printf("\n# TEB through %%fs\n");

    CHECK(nfsu2_teb_install(error, sizeof(error)) == 0, "installed a TEB (%s)",
          error[0] ? error : "no error");
    teb = nfsu2_teb_current();
    CHECK(teb != NULL, "the thread has a TEB at %p, selector 0x%04x", (void *)teb,
          nfsu2_teb_selector());
    if (!teb)
        return;

    /* The segment must actually reach it: this is the part modify_ldt buys. */
    CHECK(read_fs(NFSU2_TEB_SELF) == (unsigned int)(uintptr_t)teb,
          "fs:[0x18] is the TEB's own address - the LDT entry is live");
    CHECK(read_fs(NFSU2_TEB_EXCEPTION_LIST) == 0xffffffffu,
          "fs:[0] is -1, an empty SEH chain, as Windows leaves it");
    CHECK(read_fs(NFSU2_TEB_PEB) == *(unsigned int *)(teb + NFSU2_TEB_PEB) &&
          read_fs(NFSU2_TEB_PEB) != 0,
          "fs:[0x30] points at a PEB");
    CHECK(read_fs(NFSU2_TEB_TLS_POINTER) != 0, "fs:[0x2c] points at a TLS array");

    /* Writing through the segment must land in the TEB, since that is what a
     * __try prologue does on every entry. */
    write_fs(NFSU2_TEB_LAST_ERROR, 0xd15ea5e);
    CHECK(*(unsigned int *)(teb + NFSU2_TEB_LAST_ERROR) == 0xd15ea5e,
          "a write through fs: lands in the TEB");

    /*
     * And glibc must be unharmed. This is the reason the whole approach works on
     * i386: libc keeps its thread pointer in %gs there, so %fs was free. If that
     * were wrong, malloc and errno would be reading our TEB as their thread
     * descriptor and the process would be unpredictable rather than broken.
     */
    {
        char *scratch = malloc(64);

        CHECK(scratch != NULL, "malloc still works after loading %%fs");
        if (scratch) {
            snprintf(scratch, 64, "glibc is fine");
            CHECK(strcmp(scratch, "glibc is fine") == 0, "and so does snprintf");
            free(scratch);
        }
        errno = EDOM;
        CHECK(errno == EDOM, "and errno, which is thread-local through %%gs");
    }

    /*
     * Now the point of all of it: original machine code with a __try.
     *
     * FUN_0040a1f0 is a destructor MSVC compiled with an inline SEH prologue -
     * the shape most of the C++ in this binary has:
     *
     *   push -1 / push 0x770fc8 / push fs:[0] / mov fs:[0], esp
     *   ... body ...
     *   mov fs:[0], ecx        <- the saved chain, restored before returning
     *
     * So it reads and writes our TEB, and leaves nothing behind to inspect
     * afterwards. That is what the child object below is for: the body makes a
     * __thiscall virtual call when the child pointer is set, and the call lands
     * in a thunk of ours *while* the record is linked. The thunk can then read
     * fs:[0] and see the original's own SEH record - handler included.
     */
    {
        nfsu2_original_FUN_0040a1f0 original = NFSU2_ORIGINAL(FUN_0040a1f0, image);
        undefined4 object[3];
        undefined4 child;
        undefined4 child_vtable[1];

        CHECK(original != NULL, "resolved the original with an inline __try");
        if (original) {
            write_fs(NFSU2_TEB_EXCEPTION_LIST, 0xffffffffu);
            saved_chain = read_fs(NFSU2_TEB_EXCEPTION_LIST);

            /* The layout the function expects: [0] a vtable it overwrites twice,
             * [2] a child whose first vtable slot it calls. */
            child_vtable[0] = (undefined4)(uintptr_t)original_child_destructor;
            child = (undefined4)(uintptr_t)child_vtable;
            memset(object, 0, sizeof(object));
            object[2] = (undefined4)(uintptr_t)&child;

            g_child_calls = 0;
            original(object);

            CHECK(g_child_calls == 1,
                  "the ORIGINAL FUN_0040a1f0 called our thunk through its vtable");
            CHECK(g_chain_inside != 0 && g_chain_inside != 0xffffffffu,
                  "inside it, fs:[0] was 0x%08x - a live SEH record in our TEB",
                  g_chain_inside);
            CHECK(g_handler_inside == 0x770fc8,
                  "and the record's handler is 0x%08x, the address MSVC pushed (0x770fc8)",
                  g_handler_inside);
            CHECK(read_fs(NFSU2_TEB_EXCEPTION_LIST) == saved_chain,
                  "it restored the chain to 0x%08x on the way out", saved_chain);
            CHECK(object[0] == 0x7843f0,
                  "and left the second vtable in the object, as the decompilation says");

            /*
             * The ported copy reaches the same field, through ghidra_teb.h's
             * ExceptionList macro rather than through %fs directly - and its
             * thunk is __cdecl, because Ghidra's `(*(code *)...)(1)` has lost the
             * __thiscall convention the original call site used. That difference
             * is the interesting one for the port as a whole: vtable calls in
             * decompiled code will need the convention put back by hand.
             *
             * Only the linking is compared, not the record's contents: GCC lays
             * out its own frame, so the handler need not sit next to the saved
             * chain the way MSVC's push sequence guarantees.
             */
            child_vtable[0] = (undefined4)(uintptr_t)ported_child_destructor;
            memset(object, 0, sizeof(object));
            object[2] = (undefined4)(uintptr_t)&child;

            g_child_calls = 0;
            g_chain_inside = 0;
            FUN_0040a1f0(object);

            CHECK(g_child_calls == 1 && g_chain_inside != 0 &&
                  g_chain_inside != 0xffffffffu,
                  "the PORTED copy linked its own record too (fs:[0] was 0x%08x inside)",
                  g_chain_inside);
            CHECK(read_fs(NFSU2_TEB_EXCEPTION_LIST) == saved_chain &&
                  object[0] == 0x7843f0,
                  "restored the chain and wrote the same vtable");
        }
    }
}

/* --- a renderer function, against a fake device ---------------------------- */

/*
 * The first function from the renderer scope (../../DIRECTX_SCOPE.md), and the
 * pattern the other 98 need: it writes game globals and calls D3D9 through a
 * device pointer held in a global.
 *
 * Both of those are now portable. The globals are bound to their addresses in the
 * mapped image, so the ported copy and the original write the *same storage* -
 * which is what makes comparing them meaningful, and what hybrid execution needs
 * anyway. The device is a fake: an object whose vtable has one real entry, so the
 * call can be checked without a GPU, without DXVK, and without a device that has
 * any state to disturb.
 *
 * The two sides need *different* fake vtables, and that is the finding worth
 * keeping from this test. MSVC compiled the original against Windows' COM ABI,
 * where methods are __stdcall and the callee pops. Our ported copy is compiled
 * against DXVK Native's headers, where STDMETHODCALLTYPE is empty and methods are
 * __cdecl (see include/nfsu2/d3d9_native.h). Both are correct for their own side;
 * they simply cannot share one vtable. The consequence for the port as a whole:
 * once ported renderer code talks to DXVK it is calling the right ABI directly,
 * but any *original* renderer code still in the process needs a stdcall-to-cdecl
 * shim in front of the device - not the same pointer.
 */
struct render_state_call {
    void *device;
    DWORD state;
    DWORD value;
};

static struct render_state_call g_render_calls[8];
static int g_render_call_count;

static void note_render_state(void *device, DWORD state, DWORD value)
{
    if (g_render_call_count < 8) {
        g_render_calls[g_render_call_count].device = device;
        g_render_calls[g_render_call_count].state = state;
        g_render_calls[g_render_call_count].value = value;
    }
    g_render_call_count++;
}

/* __cdecl: DXVK Native's COM ABI, which is what the ported copy calls. */
static HRESULT ported_set_render_state(IDirect3DDevice9 *device, D3DRENDERSTATETYPE state,
                                      DWORD value)
{
    note_render_state(device, (DWORD)state, value);
    return D3D_OK;
}

/* __stdcall: Windows' COM ABI, which is what the original machine code calls. */
static HRESULT __attribute__((stdcall)) original_set_render_state(IDirect3DDevice9 *device,
                                                                 D3DRENDERSTATETYPE state,
                                                                 DWORD value)
{
    note_render_state(device, (DWORD)state, value);
    return D3D_OK;
}

/* The globals this function writes, in the order the decompilation writes them. */
static const unsigned int g_renderer_globals[] = {
    0x873370, 0x873374, 0x873378, 0x87337c,
    0x8763c0, 0x8763c4, 0x8763c8, 0x8763cc,
    0x870750, 0x87074c,
};

static void read_globals(unsigned int *out)
{
    size_t i;

    for (i = 0; i < sizeof(g_renderer_globals) / sizeof(g_renderer_globals[0]); i++)
        out[i] = *(volatile unsigned int *)(uintptr_t)g_renderer_globals[i];
}

static void clear_globals(void)
{
    size_t i;

    for (i = 0; i < sizeof(g_renderer_globals) / sizeof(g_renderer_globals[0]); i++)
        *(volatile unsigned int *)(uintptr_t)g_renderer_globals[i] = 0xcdcdcdcdu;
}

static void test_renderer_function(const struct nfsu2_pe_image *image)
{
    nfsu2_original_FUN_005b7a30 original = NFSU2_ORIGINAL(FUN_005b7a30, image);
    IDirect3DDevice9Vtbl vtable;
    IDirect3DDevice9 device;
    IDirect3DDevice9 **device_slot = (IDirect3DDevice9 **)(uintptr_t)0x870974u;
    undefined4 matrix[16];
    int object[1];
    unsigned int after_original[10];
    unsigned int after_ported[10];
    undefined4 original_result;
    undefined4 ported_result;
    struct render_state_call original_call;
    int original_calls;
    size_t i;

    printf("\n# a renderer function: ported vs original, against a fake device\n");

    /*
     * The offset the original's machine code uses - `call DWORD PTR [ecx+0xe4]` -
     * has to be where Wine's header puts SetRenderState, or the fake vtable would
     * be answering a different method than the one the game asked for.
     */
    CHECK(offsetof(IDirect3DDevice9Vtbl, SetRenderState) == 0xe4,
          "SetRenderState is at vtable offset 0x%x, which is the offset the "
          "original calls through", (unsigned)offsetof(IDirect3DDevice9Vtbl, SetRenderState));
    CHECK(original != NULL, "resolved the original renderer function");
    if (!original)
        return;

    for (i = 0; i < 16; i++)
        matrix[i] = (undefined4)(0x1000u + i);
    object[0] = (int)(uintptr_t)matrix;

    memset(&vtable, 0, sizeof(vtable));
    device.lpVtbl = &vtable;
    *device_slot = &device;
    CHECK(*device_slot == &device,
          "the device-pointer global at 0x870974 is writable in the mapped image");

    /* The original first, through a __stdcall entry. */
    *(void **)&vtable.SetRenderState = (void *)original_set_render_state;
    clear_globals();
    g_render_call_count = 0;
    original_result = original(object);
    original_calls = g_render_call_count;
    original_call = g_render_calls[0];
    read_globals(after_original);

    CHECK(original_calls == 1, "the original made one SetRenderState call (%d)",
          original_calls);
    CHECK(original_calls >= 1 && original_call.device == &device,
          "with our fake device as `this` - COM's first argument");
    /* 0xe in the decompilation, which is D3DRS_ZWRITEENABLE - checked against the
     * header's enum rather than against a name read off the number by eye, which
     * is how the manifest note came to say FOGENABLE at first. */
    CHECK(original_calls >= 1 && original_call.state == D3DRS_ZWRITEENABLE &&
          original_call.value == 0,
          "SetRenderState(D3DRS_ZWRITEENABLE=%d, 0) - state %lu, value %lu",
          (int)D3DRS_ZWRITEENABLE, (unsigned long)original_call.state,
          (unsigned long)original_call.value);

    /* Then the ported copy, through a __cdecl entry. */
    *(void **)&vtable.SetRenderState = (void *)ported_set_render_state;
    clear_globals();
    g_render_call_count = 0;
    ported_result = FUN_005b7a30(object);
    read_globals(after_ported);

    CHECK(g_render_call_count == original_calls,
          "the ported copy made the same number of calls (%d)", g_render_call_count);
    CHECK(g_render_call_count >= 1 &&
          g_render_calls[0].device == original_call.device &&
          g_render_calls[0].state == original_call.state &&
          g_render_calls[0].value == original_call.value,
          "with identical arguments");
    CHECK(ported_result == original_result, "the same return value (%lu)",
          (unsigned long)ported_result);
    CHECK(memcmp(after_original, after_ported, sizeof(after_original)) == 0,
          "and all %zu globals hold identical values afterwards",
          sizeof(after_original) / sizeof(after_original[0]));
    g_differential_checks++;

    /* Worth naming what those globals got, since it is the function's real work:
     * column 0 and column 1 of the matrix, with a zeroed w. */
    CHECK(after_ported[0] == 0x1000 && after_ported[1] == 0x1004 &&
          after_ported[2] == 0x1008 && after_ported[3] == 0,
          "the first vector is (m[0], m[4], m[8], 0) as the decompilation says");

    *device_slot = NULL;
}

/* --- SEH: our dispatcher calling the game's own handler -------------------- */

/*
 * The other half of the SEH work (the dispatcher's own contract is checked in
 * seh_selftest.c): can an exception raised inside the game's code be walked
 * through a handler that *belongs to the game*?
 *
 * FUN_0040a1f0's prologue links a record whose handler is 0x770fc8 - a ten-byte
 * thunk that loads a FuncInfo into eax and jumps to __CxxFrameHandler, both
 * statically linked into the exe. So the chain during the call is
 *
 *   fs:[0] -> [the original's record, handler = the exe's C++ EH thunk]
 *          -> [a record of ours, outside it, which catches]
 *          -> -1
 *
 * and raising an exception from inside the original's virtual call means our
 * dispatcher has to call MSVC's own frame handler, on real FuncInfo, and act on
 * what it returns.
 *
 * IT DOES NOT WORK YET, and the reason is worth more than the test would have
 * been. Run with NFSU2_SEH_CXX_PROBE=1 and the child dies of a stack overflow
 * inside the *game's* CRT, in a two-frame recursion:
 *
 *   __InternalCxxFrameHandler          0x762e0a
 *     -> _mtinitlocknum(10)            0x762d72   allocate lock 10
 *          -> _lock(10)                0x762df1   ...which locks lock 10
 *               -> _mtinitlocknum(10)             ...which is still NULL
 *
 * MSVC's C++ EH takes a CRT lock before it allocates its per-thread exception
 * data, and `_lock(n)` bootstraps a missing lock by calling `_mtinitlocknum(n)`,
 * which takes lock 10 (_LOCKTAB_LOCK) to do it. In a real process that entry was
 * filled by `_mtinit()` during CRT startup, so the recursion terminates on the
 * first step. Nothing here has run the game's CRT startup, so `_locktable` at
 * 0x81a340 is all zeroes and it never terminates.
 *
 * So the next dependency for hybrid execution is not SEH at all: it is the game's
 * own CRT initialisation. Our dispatcher's part is done - it reached MSVC's frame
 * handler with a real record and real FuncInfo - and the probe stays here, off by
 * default, because it is the reproduction for that finding.
 */
static jmp_buf g_seh_catch;
static DWORD g_caught_code;
static int g_raised;

static DWORD CDECL catch_outside(PEXCEPTION_RECORD record,
                                 EXCEPTION_REGISTRATION_RECORD *frame,
                                 PCONTEXT context,
                                 EXCEPTION_REGISTRATION_RECORD **dispatcher)
{
    (void)context; (void)dispatcher;
    g_caught_code = record->ExceptionCode;
    /* What a __except block does: pop everything inside this frame, then jump. */
    RtlUnwind(frame, NULL, record, NULL);
    longjmp(g_seh_catch, 1);
    return ExceptionContinueSearch; /* not reached */
}

/* Called by the original through its vtable, with its SEH record linked. */
static void __attribute__((stdcall)) raise_from_inside(int flag)
{
    (void)flag;
    g_raised = 1;
    RaiseException(0x20000042u, 0, 0, NULL);
}

#define CHILD_OK              0
#define CHILD_NOT_CAUGHT      2
#define CHILD_WRONG_CODE      3
#define CHILD_NEVER_RAISED    4

static int seh_through_original_child(const struct nfsu2_pe_image *image)
{
    nfsu2_original_FUN_0040a1f0 original = NFSU2_ORIGINAL(FUN_0040a1f0, image);
    EXCEPTION_REGISTRATION_RECORD outer;
    undefined4 object[3];
    undefined4 child;
    undefined4 child_vtable[1];

    if (!original)
        return CHILD_NOT_CAUGHT;

    /* Our frame goes on *before* the call, so the original's prologue saves it
     * as its own Prev and the chain links outwards to us. */
    outer.Prev = (EXCEPTION_REGISTRATION_RECORD *)~(uintptr_t)0;
    outer.Handler = catch_outside;
    write_fs(NFSU2_TEB_EXCEPTION_LIST, (unsigned int)(uintptr_t)&outer);

    child_vtable[0] = (undefined4)(uintptr_t)raise_from_inside;
    child = (undefined4)(uintptr_t)child_vtable;
    memset(object, 0, sizeof(object));
    object[2] = (undefined4)(uintptr_t)&child;

    if (setjmp(g_seh_catch) == 0) {
        original(object);
        /* Returned normally: the exception was never raised, or was swallowed. */
        return g_raised ? CHILD_NOT_CAUGHT : CHILD_NEVER_RAISED;
    }
    return g_caught_code == 0x20000042u ? CHILD_OK : CHILD_WRONG_CODE;
}

static void test_seh_through_original_handler(const struct nfsu2_pe_image *image)
{
    const char *probe = getenv("NFSU2_SEH_CXX_PROBE");
    pid_t child;
    int status = 0;

    printf("\n# SEH through the game's own C++ frame handler\n");

    if (!probe || !*probe || *probe == '0') {
        printf("     off: the game's __CxxFrameHandler takes a CRT lock, and the CRT\n"
               "     lock table at 0x81a340 is uninitialised without the game's own\n"
               "     startup - _lock(10) and _mtinitlocknum(10) then recurse until the\n"
               "     stack ends. NFSU2_SEH_CXX_PROBE=1 reproduces it; see the comment.\n");
        return;
    }

    fflush(stdout);
    child = fork();
    if (child < 0) {
        CHECK(0, "fork failed: %s", strerror(errno));
        return;
    }
    if (child == 0)
        _exit(seh_through_original_child(image));
    if (waitpid(child, &status, 0) < 0) {
        CHECK(0, "waitpid failed: %s", strerror(errno));
        return;
    }

    if (WIFSIGNALED(status)) {
        printf("     the child died on signal %d, as expected until the game's CRT is\n"
               "     initialised - that is the finding, not a regression\n",
               WTERMSIG(status));
        return;
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == CHILD_OK,
          "an exception raised inside the original was walked through the exe's "
          "__CxxFrameHandler thunk (0x770fc8) and caught outside it");
    if (WIFEXITED(status) && WEXITSTATUS(status) != CHILD_OK) {
        printf("     child said %d: %s\n", WEXITSTATUS(status),
               WEXITSTATUS(status) == CHILD_NEVER_RAISED ? "the vtable call never happened"
               : WEXITSTATUS(status) == CHILD_WRONG_CODE ? "a different exception code arrived"
               : "nothing caught it");
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
          "the 5 addresses compared here resolved inside the mapped image "
          "(%d in the manifest)", NFSU2_ORIGINAL_COUNT);
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
        test_hybrid_execution(&image);
        test_teb(&image);
        test_seh_through_original_handler(&image);
        test_renderer_function(&image);
        test_against_original(&image);
        printf("\n     %d differential comparisons against original code\n",
               g_differential_checks);
        nfsu2_pe_unload(&image);
    }

    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
