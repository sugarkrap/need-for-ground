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

#include <stdbool.h> /* Ghidra emits `bool`, `true` and `false` */
#include <stddef.h>
#include <stdint.h>

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

/*
 * `code` is Ghidra's placeholder for "instructions live here"; it appears as
 * `code *` wherever the decompiler found a function pointer it could not type.
 *
 * Declared as an *unprototyped function* type, not as void. Ghidra emits calls
 * straight through it - `(*param_3)(a, b)` for a comparison callback, say - and
 * with `typedef void code;` that is a call through a void lvalue, which does not
 * compile. An unprototyped function type accepts a call with any arguments,
 * which is exactly the "I do not know this signature" semantics Ghidra means.
 *
 * Measured effect on the corpus (native/tools/survey_decompiled.py, 500-function
 * sample): 68.8% -> 69.4% compiling as-is. Small, and instructive - it clears the
 * error in all 62 affected functions, but most of them then fail on a *second*
 * problem that was hidden behind it. Fixing one class of decompiler-output
 * breakage mostly reveals the next one in the same function.
 *
 * K&R function types are gone in C23, so this pins the port to gnu11/gnu17 until
 * those call sites get real signatures - which they need anyway.
 */
typedef void code();

/* x87 80-bit intermediates in the original MSVC output. */
typedef long double float10;

/*
 * Not defined here on purpose: unkbyte9/unkuint10/unkfloat* and similar
 * odd-width placeholders. They only appear where Ghidra failed to model a
 * SIMD/x87 access, and any typedef we invent would compile while computing
 * the wrong thing. Fix those functions by hand (usually by re-typing the
 * variable in Ghidra and re-exporting; see analysis/retype_and_decompile.py).
 */

/*
 * Ghidra's pseudo-intrinsics.
 *
 * The decompiler emits these where a machine instruction has no direct C
 * spelling. They are not functions in the original binary - they describe what
 * the instruction did - so each has to be given semantics that match the x86
 * behaviour exactly, or ported code compiles and computes something subtly
 * different. Measured frequency across this game's self-contained functions:
 * ABS 27, SBORROW4 18, NAN 12, SQRT 9, CARRY4 3.
 */

/* fsqrt / sqrtss. Type-generic so it works on float, double and float10. */
#define SQRT(x) __builtin_sqrtl((long double)(x))

/* fabs / andps with a sign mask. */
#define ABS(x)  __builtin_fabsl((long double)(x))

/*
 * frndint: round to the nearest integer using the *current* x87 rounding mode,
 * which is round-to-nearest-even unless the game changed it. rint() is that, and
 * nearbyint() would not be - it ignores the mode.
 */
#define ROUND(x) __builtin_rintl((long double)(x))

/*
 * fucom-style unordered compare: true when either operand is NaN. Ghidra emits
 * it with one argument for the "is this NaN" case.
 */
#define NAN(x)  (__builtin_isnan((double)(x)) != 0)

/*
 * The x86 flag predicates, as the CPU computes them for `sub`/`cmp`. Ghidra
 * emits these when code branches on a flag in a way that does not reduce to a
 * plain comparison - most often MSVC's branchless float-compare idioms.
 *
 * SBORROW4(a, b) is the *signed overflow* flag of a - b, i.e. whether the
 * subtraction overflowed: not the borrow, despite the name. CARRY4(a, b) is the
 * unsigned carry out of a + b.
 */
#define SBORROW4(a, b) (__builtin_sub_overflow_p((int)(a), (int)(b), (int)0) != 0)
#define CARRY4(a, b)   (__builtin_add_overflow_p((uint)(a), (uint)(b), (uint)0) != 0)

/*
 * Bit-slicing helpers. CONCATxy joins an x-byte high half with a y-byte low
 * half; SUBxy takes the y-byte slice of an x-byte value starting at a byte
 * offset; ZEXT/SEXT widen. Only the widths this game's functions actually use
 * are defined - an unused one added on speculation is an unused one nobody
 * checked.
 */
#define CONCAT13(hi, lo) ((uint)(((uint)(byte)(hi) << 24) | ((uint)(lo) & 0xffffff)))
#define CONCAT22(hi, lo) ((uint)(((uint)(ushort)(hi) << 16) | (ushort)(lo)))
#define CONCAT31(hi, lo) ((uint)(((uint)(hi) << 8) | (byte)(lo)))
#define CONCAT44(hi, lo) ((ulonglong)(((ulonglong)(uint)(hi) << 32) | (uint)(lo)))

#define SUB41(v, off) ((byte)((uint)(v) >> (8 * (off))))
#define SUB42(v, off) ((ushort)((uint)(v) >> (8 * (off))))
#define SUB84(v, off) ((uint)((ulonglong)(v) >> (8 * (off))))

#define ZEXT14(v) ((uint)(byte)(v))
#define ZEXT24(v) ((uint)(ushort)(v))
#define ZEXT48(v) ((ulonglong)(uint)(v))
#define SEXT14(v) ((int)(char)(v))
#define SEXT24(v) ((int)(short)(v))
#define SEXT48(v) ((longlong)(int)(v))

#endif /* NFSU2_GHIDRA_TYPES_H */
