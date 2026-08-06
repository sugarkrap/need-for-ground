/*
 * ghidra_types.h - the types Ghidra's decompiler emits, as real C types.
 *
 * Bulk-decompiled output (analysis/bulk_decompile.py -> decompiled/, not
 * committed) is full of `undefined4`, `byte`, `uint`, `code` and friends.
 * Include this header at the top of a ported translation unit so the
 * pseudocode compiles with as little hand-editing as possible; then narrow
 * the types function by function as they get understood.
 *
 * ILP32 only, on purpose. Ghidra's output for a 32-bit PE stores pointers in
 * `int`/`uint` locals, indexes structs by literal byte offsets, and assumes
 * sizeof(long) == 4. None of that survives a 64-bit build, so refuse to
 * compile rather than silently miscompile.
 */
#ifndef NFSU2_GHIDRA_TYPES_H
#define NFSU2_GHIDRA_TYPES_H

#if !defined(__i386__)
#error "decompiled game code is 32-bit; build with -m32 (see native/cross/linux32.txt)"
#endif

#include <stdint.h>
#include <stddef.h>

/* Ghidra's "I don't know what this is, but it is N bytes wide" types. */
typedef uint8_t  undefined;
typedef uint8_t  undefined1;
typedef uint16_t undefined2;
typedef uint32_t undefined3; /* only ever appears inside padding arrays */
typedef uint32_t undefined4;
typedef uint64_t undefined5;
typedef uint64_t undefined6;
typedef uint64_t undefined7;
typedef uint64_t undefined8;

/* Ghidra's built-in aliases. `byte` matches Wine's rpcndr.h typedef, and the
 * u* aliases match glibc's <sys/types.h> ones under _DEFAULT_SOURCE, so these
 * are compatible redeclarations rather than conflicts. */
typedef unsigned char      byte;
typedef unsigned short     word;
typedef unsigned int       dword;
typedef unsigned long long qword;
typedef unsigned int       uint;
typedef unsigned short     ushort;
typedef unsigned long      ulong;
typedef long long          longlong;
typedef unsigned long long ulonglong;
typedef signed char        sbyte;

/* `code` is Ghidra's placeholder for "instructions live here"; it shows up as
 * `code *` for function pointers it could not type. */
typedef void code;

/* x87 80-bit intermediates in the original MSVC output. */
typedef long double float10;

/*
 * Not defined here on purpose: unkbyte9/unkuint10/unkfloat* and similar
 * odd-width placeholders. They only appear where Ghidra failed to model a
 * SIMD/x87 access, and any typedef we invent would compile while computing
 * the wrong thing. Fix those functions by hand (usually by re-typing the
 * variable in Ghidra and re-exporting; see analysis/retype_and_decompile.py).
 */

#endif /* NFSU2_GHIDRA_TYPES_H */
