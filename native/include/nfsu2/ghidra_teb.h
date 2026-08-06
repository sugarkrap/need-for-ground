/*
 * ghidra_teb.h - Ghidra's pseudo-globals for TEB fields.
 *
 * A 32-bit MSVC function with a __try reads and writes fs:[0], and the decompiler
 * presents that as a global called `ExceptionList`. Ported code has to reach the
 * *same storage* the original reaches through %fs, or the two keep separate SEH
 * chains and a chain built by one gets unwound by the other - the sort of
 * divergence that stays invisible until something throws.
 *
 * So these resolve to the live TEB's fields rather than to variables of our own.
 * That costs a call per access; correctness first, and nothing here is hot enough
 * for it to matter.
 *
 * **Include this after <windows.h>**, which is why it is not part of
 * ghidra_types.h. Wine's NT_TIB has a *member* called ExceptionList, so defining
 * the macro first rewrites the struct declaration and the headers stop compiling.
 */
#ifndef NFSU2_GHIDRA_TEB_H
#define NFSU2_GHIDRA_TEB_H

#include <nfsu2/teb.h>

#define NFSU2_TEB_FIELD(offset, type) \
    (*(type *)(void *)((char *)nfsu2_teb_current() + (offset)))

#define ExceptionList NFSU2_TEB_FIELD(NFSU2_TEB_EXCEPTION_LIST, void *)
#define StackBase     NFSU2_TEB_FIELD(NFSU2_TEB_STACK_BASE, void *)
#define StackLimit    NFSU2_TEB_FIELD(NFSU2_TEB_STACK_LIMIT, void *)

#endif /* NFSU2_GHIDRA_TEB_H */
