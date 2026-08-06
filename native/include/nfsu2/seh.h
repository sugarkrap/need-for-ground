/*
 * seh.h - structured exception handling for original code.
 *
 * The game binary carries its own exception machinery: every function with a
 * __try or a C++ try links a record onto fs:[0], and the handler in that record
 * is code inside the exe - MSVC's __except_handler3 and __CxxFrameHandler are
 * statically linked, and each function with C++ EH gets a ten-byte thunk
 * (`mov eax, <FuncInfo>; jmp __CxxFrameHandler`) whose address is what the
 * prologue pushes. None of that is ours to implement.
 *
 * What *is* ours is the operating system's half of the contract:
 *
 *   - when a fault happens, build an EXCEPTION_RECORD and a CONTEXT and call
 *     each handler in the fs:[0] chain until one of them says stop
 *   - RaiseException, which is how MSVC's `throw` (code 0xe06d7363) and the
 *     game's own error paths start an exception
 *   - RtlUnwind, which is what a handler calls to pop the frames between the
 *     throw and the catch, giving each one a chance to run its destructors
 *
 * So: signals in, handler chain out. See src/win32/exception.c.
 *
 * This needs a TEB (teb.h) on every thread that raises or handles anything -
 * the chain head lives at fs:[0] and there is nowhere else to keep it.
 */
#ifndef NFSU2_SEH_H
#define NFSU2_SEH_H

#include <stddef.h>

/*
 * Install the fault handlers - SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP -
 * that turn a machine fault into a Win32 exception. Process-wide and idempotent;
 * unlike nfsu2_teb_install() this is not per-thread.
 *
 * Returns 0, or a negative errno with a reason in `error`.
 *
 * Without this, faults still crash the process exactly as they do now: only
 * RaiseException and RtlUnwind work, which is enough for code that throws
 * explicitly but not for code that dereferences a null pointer inside a __try.
 */
int nfsu2_seh_install(char *error, size_t error_size);

/* Undo it, restoring the default disposition. Mostly for tests. */
void nfsu2_seh_remove(void);

/*
 * How many exceptions have been dispatched, and how many were handled by
 * someone in the chain. Diagnostics - a port that silently swallows faults is
 * worse than one that crashes, so these are worth being able to print.
 */
unsigned int nfsu2_seh_dispatched(void);
unsigned int nfsu2_seh_handled(void);

#endif /* NFSU2_SEH_H */
