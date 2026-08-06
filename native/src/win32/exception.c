/*
 * exception.c - the SEH entry points, as loud failures.
 *
 * Structured exception handling has no equivalent here. Implementing it means
 * translating SIGSEGV/SIGFPE into an EXCEPTION_RECORD, walking the game's
 * registered handler chain off the TEB, and unwinding frames the compiler did
 * not generate unwind data for - a project of its own, and one that only pays
 * off once the port is running.
 *
 * Until then, an exception is a crash, and the useful thing is that it is a
 * crash *with a message* at the point it happened. Silently continuing after a
 * failed RtlUnwind would corrupt state and produce a second, meaningless crash
 * somewhere else - which is exactly the debugging experience worth avoiding.
 */
#include "shim_internal.h"

#include <stdio.h>
#include <stdlib.h>

VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD argument_count,
                           const ULONG_PTR *arguments)
{
    DWORD i;

    fprintf(stderr, "\n[nfsu2] RaiseException(0x%08lx) with no SEH support\n",
            (unsigned long)code);
    fprintf(stderr, "        flags 0x%08lx, %lu argument(s)",
            (unsigned long)flags, (unsigned long)argument_count);
    if (arguments) {
        for (i = 0; i < argument_count && i < 4; i++)
            fprintf(stderr, " 0x%lx", (unsigned long)arguments[i]);
    }
    fprintf(stderr, "\n        (C++ throws arrive here too: code 0xe06d7363 is MSVC's)\n");

    /*
     * abort() rather than exit(): it leaves a core dump, which is the only
     * artefact that says where this came from.
     */
    abort();
}

/*
 * RtlUnwind is what MSVC's exception machinery calls to pop frames. Returning
 * from it would resume execution in a frame that was supposed to be gone.
 */
VOID WINAPI RtlUnwind(PVOID end_frame, PVOID target_ip, PEXCEPTION_RECORD record, PVOID value)
{
    (void)end_frame; (void)target_ip; (void)value;

    fprintf(stderr, "\n[nfsu2] RtlUnwind with no SEH support");
    if (record)
        fprintf(stderr, " (exception code 0x%08lx)", (unsigned long)record->ExceptionCode);
    fprintf(stderr, "\n");
    abort();
}

LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *info)
{
    if (info && info->ExceptionRecord) {
        fprintf(stderr, "[nfsu2] unhandled exception 0x%08lx at %p\n",
                (unsigned long)info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
    }
    /*
     * EXCEPTION_EXECUTE_HANDLER tells the caller "stop, do not continue" - the
     * honest answer when there is no handler to run. EXCEPTION_CONTINUE_SEARCH
     * would send it looking for a handler that does not exist.
     */
    return EXCEPTION_EXECUTE_HANDLER;
}
