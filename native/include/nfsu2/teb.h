/*
 * teb.h - a Thread Environment Block reachable through %fs, for original code.
 *
 * MSVC-compiled 32-bit code reaches the TEB constantly and implicitly:
 *
 *   fs:[0x00]  the SEH handler chain - every function with a __try pushes onto it
 *   fs:[0x2c]  the thread-local storage array, for __declspec(thread)
 *   fs:[0x18]  the TEB's own linear address, which code uses to derive the rest
 *   fs:[0x34]  LastError
 *
 * Without a TEB, any such function writes through whatever %fs happens to hold.
 * That is the wall between "leaf functions work" and "call anything in the
 * binary", because SEH is pervasive in MSVC-compiled C++ - Ghidra spells those
 * accesses as a global called `ExceptionList`.
 *
 * The mechanism, and why i386 makes it easy: on i386 Linux, glibc keeps its own
 * thread pointer in **%gs**, leaving %fs unused - which is exactly why Windows
 * uses %fs there. So a private LDT entry can be pointed at a TEB of our own and
 * loaded into %fs without disturbing libc at all. modify_ldt() is the same
 * mechanism Wine uses.
 *
 * (On x86_64 the roles swap - %fs is glibc's - so this is i386-only and the calls
 * below fail cleanly elsewhere. The port targets i386 anyway.)
 *
 * Per thread: each thread that runs original code needs its own call, because %fs
 * and the TEB are both per-thread.
 */
#ifndef NFSU2_TEB_H
#define NFSU2_TEB_H

#include <stddef.h>

/*
 * Install a TEB for the calling thread and load %fs with a selector for it.
 * Idempotent: a second call on the same thread returns the existing TEB.
 *
 * Returns 0 on success, or a negative errno. `error` receives a readable reason
 * when non-NULL.
 */
int nfsu2_teb_install(char *error, size_t error_size);

/* The calling thread's TEB, or NULL if it has none. */
void *nfsu2_teb_current(void);

/* The %fs selector value in use, or 0. Mostly for diagnostics and tests. */
unsigned short nfsu2_teb_selector(void);

/*
 * Field offsets worth naming, for tests and for ported code that has to agree
 * with the original about where things live.
 */
#define NFSU2_TEB_EXCEPTION_LIST 0x00
#define NFSU2_TEB_STACK_BASE     0x04
#define NFSU2_TEB_STACK_LIMIT    0x08
#define NFSU2_TEB_SELF           0x18
#define NFSU2_TEB_PROCESS_ID     0x20
#define NFSU2_TEB_THREAD_ID      0x24
#define NFSU2_TEB_TLS_POINTER    0x2c
#define NFSU2_TEB_PEB            0x30
#define NFSU2_TEB_LAST_ERROR     0x34

#endif /* NFSU2_TEB_H */
