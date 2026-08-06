/*
 * exception.c - the OS half of i386 structured exception handling.
 *
 * See include/nfsu2/seh.h for what this is for. The short version: the game's
 * handlers are inside the game binary, the dispatcher is not, and this is the
 * dispatcher.
 *
 * The i386 contract, which is entirely register- and stack-based and has no
 * unwind tables anywhere:
 *
 *   fs:[0] -> EXCEPTION_REGISTRATION_RECORD { Prev, Handler }, on the stack,
 *   linked outwards, terminated by -1 rather than NULL.
 *
 *   Handler is __cdecl and gets (record, frame, context, dispatcher) and returns
 *   ExceptionContinueSearch  - not mine, try the next frame
 *   ExceptionContinueExecution - fixed it, resume from the context
 *   ExceptionNestedException / ExceptionCollidedUnwind - a fault happened while
 *   this handler or an unwind was already running.
 *
 * A handler that wants to *handle* something does not say so with a return
 * value. It calls RtlUnwind to pop the frames between here and itself - giving
 * each one EXCEPTION_UNWINDING so destructors run - and then transfers control
 * to its own __except block by restoring esp/ebp from its registration record
 * and jumping. That is why RtlUnwind must return normally to its caller: the
 * caller is the handler, and it is the handler that jumps.
 *
 * One deliberate limitation, stated because it is a real gap rather than an
 * oversight: no sigaltstack. Handlers therefore run on the faulting thread's own
 * stack, which is what Windows does and what makes it safe for a handler to
 * abandon the signal frame by jumping to its __except block. The cost is that
 * EXCEPTION_STACK_OVERFLOW cannot work - a fault on the guard page has no stack
 * left to build a frame on. SA_NODEFER goes with it, so escaping the handler
 * that way does not leave the signal blocked forever.
 */
#include "shim_internal.h"

#include <nfsu2/seh.h>
#include <nfsu2/teb.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

/*
 * i386 only, and not by accident: everything below - a chain of two-dword records
 * at fs:[0], __cdecl handlers, a CONTEXT of 32-bit registers - is the 32-bit
 * contract. Win64 replaced all of it with unwind tables in the executable, which
 * a ported binary would not have. The 64-bit build of this shim exists to catch
 * portability mistakes in the rest of the code, so the entry points below it fail
 * loudly rather than pretending.
 */
#if defined(__i386__)

/* The chain is terminated by -1, not NULL: zero would look like a valid record
 * at address 0, and Windows has always used -1 here. */
#define END_OF_CHAIN ((EXCEPTION_REGISTRATION_RECORD *)~(uintptr_t)0)

static unsigned int g_dispatched;
static unsigned int g_handled;
static int g_installed;

unsigned int nfsu2_seh_dispatched(void) { return g_dispatched; }
unsigned int nfsu2_seh_handled(void) { return g_handled; }

/* --- the chain, which lives in the TEB ----------------------------------- */

static EXCEPTION_REGISTRATION_RECORD **chain_slot(void)
{
    unsigned char *teb = nfsu2_teb_current();

    if (!teb)
        return NULL;
    return (EXCEPTION_REGISTRATION_RECORD **)(teb + NFSU2_TEB_EXCEPTION_LIST);
}

static EXCEPTION_REGISTRATION_RECORD *chain_head(void)
{
    EXCEPTION_REGISTRATION_RECORD **slot = chain_slot();

    return slot ? *slot : END_OF_CHAIN;
}

static void set_chain_head(EXCEPTION_REGISTRATION_RECORD *frame)
{
    EXCEPTION_REGISTRATION_RECORD **slot = chain_slot();

    if (slot)
        *slot = frame;
}

/*
 * Refuse to call a handler out of a chain that does not look like one.
 *
 * This is not paranoia for its own sake: fs:[0] is a *stack* address written by
 * code we did not compile, and if a frame is bogus the alternative to a
 * diagnostic here is calling a function pointer read out of a random stack slot.
 * The three cheap invariants are that the record is inside this thread's stack,
 * 4-byte aligned, and links strictly outwards.
 */
static int frame_is_plausible(const EXCEPTION_REGISTRATION_RECORD *frame, const char **why)
{
    unsigned char *teb = nfsu2_teb_current();
    uintptr_t address = (uintptr_t)frame;
    uintptr_t base, limit;

    if (address & 3u) {
        *why = "not 4-byte aligned";
        return 0;
    }
    if (!frame->Handler) {
        *why = "null handler";
        return 0;
    }
    if (frame->Prev != END_OF_CHAIN && (uintptr_t)frame->Prev <= address) {
        *why = "Prev does not point outwards";
        return 0;
    }
    if (teb) {
        limit = *(uintptr_t *)(teb + NFSU2_TEB_STACK_LIMIT);
        base = *(uintptr_t *)(teb + NFSU2_TEB_STACK_BASE);
        if (base && (address < limit || address + sizeof(*frame) > base)) {
            *why = "outside this thread's stack";
            return 0;
        }
    }
    return 1;
}

/* --- CONTEXT ------------------------------------------------------------- */

/*
 * A CONTEXT for a synchronous RaiseException. Only the registers a handler can
 * reasonably want are filled: the callee-saved set, the segments, and the
 * instruction pointer the caller passes in. Esp and Ebp are this frame's rather
 * than the raiser's, which is a real approximation and harmless in practice -
 * MSVC's handlers take the frame they need from the registration record, not
 * from the context.
 */
static void capture_context(CONTEXT *context, void *eip)
{
    memset(context, 0, sizeof(*context));
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    __asm__ volatile("movl %%edi, %0\n\t"
                     "movl %%esi, %1\n\t"
                     "movl %%ebx, %2\n\t"
                     "movl %%ebp, %3\n\t"
                     "movl %%esp, %4"
                     : "=m"(context->Edi), "=m"(context->Esi), "=m"(context->Ebx),
                       "=m"(context->Ebp), "=m"(context->Esp));
    __asm__ volatile("movw %%cs, %0\n\t"
                     "movw %%ss, %1\n\t"
                     "movw %%ds, %2\n\t"
                     "movw %%es, %3\n\t"
                     "movw %%fs, %4\n\t"
                     "movw %%gs, %5"
                     : "=m"(context->SegCs), "=m"(context->SegSs), "=m"(context->SegDs),
                       "=m"(context->SegEs), "=m"(context->SegFs), "=m"(context->SegGs));
    context->Eip = (DWORD)(uintptr_t)eip;
}

static void context_from_ucontext(CONTEXT *context, const ucontext_t *uc)
{
    const greg_t *r = uc->uc_mcontext.gregs;

    memset(context, 0, sizeof(*context));
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS;
    context->Edi = (DWORD)r[REG_EDI];
    context->Esi = (DWORD)r[REG_ESI];
    context->Ebx = (DWORD)r[REG_EBX];
    context->Edx = (DWORD)r[REG_EDX];
    context->Ecx = (DWORD)r[REG_ECX];
    context->Eax = (DWORD)r[REG_EAX];
    context->Ebp = (DWORD)r[REG_EBP];
    context->Eip = (DWORD)r[REG_EIP];
    context->Esp = (DWORD)r[REG_UESP];
    context->EFlags = (DWORD)r[REG_EFL];
    context->SegCs = (DWORD)r[REG_CS];
    context->SegSs = (DWORD)r[REG_SS];
    context->SegDs = (DWORD)r[REG_DS];
    context->SegEs = (DWORD)r[REG_ES];
    context->SegFs = (DWORD)r[REG_FS];
    context->SegGs = (DWORD)r[REG_GS];
}

/*
 * And back, for ExceptionContinueExecution: returning from the signal handler
 * makes the kernel reload the registers from here, so a handler that repaired
 * the fault - or just moved Eip past it - resumes as if nothing happened.
 */
static void ucontext_from_context(ucontext_t *uc, const CONTEXT *context)
{
    greg_t *r = uc->uc_mcontext.gregs;

    r[REG_EDI] = (greg_t)context->Edi;
    r[REG_ESI] = (greg_t)context->Esi;
    r[REG_EBX] = (greg_t)context->Ebx;
    r[REG_EDX] = (greg_t)context->Edx;
    r[REG_ECX] = (greg_t)context->Ecx;
    r[REG_EAX] = (greg_t)context->Eax;
    r[REG_EBP] = (greg_t)context->Ebp;
    r[REG_EIP] = (greg_t)context->Eip;
    r[REG_UESP] = (greg_t)context->Esp;
    r[REG_EFL] = (greg_t)context->EFlags;
}

/* --- dispatch ------------------------------------------------------------ */

static const char *disposition_name(DWORD disposition)
{
    switch (disposition) {
    case ExceptionContinueExecution: return "ContinueExecution";
    case ExceptionContinueSearch:    return "ContinueSearch";
    case ExceptionNestedException:   return "NestedException";
    case ExceptionCollidedUnwind:    return "CollidedUnwind";
    default:                         return "invalid";
    }
}

/*
 * Walk the chain. Returns non-zero when a handler said "resume from this
 * context" - the only disposition that means the exception is over. A handler
 * that intends to run a __except block never gets that far: it unwinds and
 * jumps, and this loop is left behind with the rest of the abandoned frames.
 */
static int dispatch(EXCEPTION_RECORD *record, CONTEXT *context)
{
    EXCEPTION_REGISTRATION_RECORD *frame;
    EXCEPTION_REGISTRATION_RECORD *nested_below = NULL;
    EXCEPTION_REGISTRATION_RECORD *dispatcher;
    const char *why = "";
    DWORD disposition;

    g_dispatched++;

    if (!nfsu2_teb_current()) {
        fprintf(stderr, "[nfsu2] exception 0x%08lx on a thread with no TEB - "
                        "call nfsu2_teb_install() there\n",
                (unsigned long)record->ExceptionCode);
        return 0;
    }

    for (frame = chain_head(); frame != END_OF_CHAIN && frame != NULL; frame = frame->Prev) {
        if (!frame_is_plausible(frame, &why)) {
            fprintf(stderr, "[nfsu2] SEH chain is corrupt at %p: %s\n", (void *)frame, why);
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            return 0;
        }

        /*
         * EXCEPTION_NESTED_CALL tells a handler that it is being asked about an
         * exception that happened inside another handler further in, which some
         * handlers refuse to touch.
         */
        if (nested_below && frame <= nested_below)
            record->ExceptionFlags |= EXCEPTION_NESTED_CALL;

        dispatcher = frame;
        disposition = frame->Handler(record, frame, context, &dispatcher);

        switch (disposition) {
        case ExceptionContinueExecution:
            if (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) {
                fprintf(stderr, "[nfsu2] handler %p wants to continue a "
                                "non-continuable exception 0x%08lx\n",
                        (void *)frame->Handler, (unsigned long)record->ExceptionCode);
                return 0;
            }
            g_handled++;
            return 1;
        case ExceptionContinueSearch:
            break;
        case ExceptionNestedException:
            /* The handler reported a nested fault and told us how far in it
             * got; frames at or inside that point must be told. */
            if (dispatcher > nested_below)
                nested_below = dispatcher;
            break;
        case ExceptionCollidedUnwind:
            /* Only meaningful during an unwind, and RtlUnwind handles it. */
            fprintf(stderr, "[nfsu2] CollidedUnwind outside an unwind, at %p\n",
                    (void *)frame);
            return 0;
        default:
            fprintf(stderr, "[nfsu2] handler %p returned %lu (%s)\n",
                    (void *)frame->Handler, (unsigned long)disposition,
                    disposition_name(disposition));
            return 0;
        }
    }
    return 0;
}

/* --- the last resort ----------------------------------------------------- */

static void report_unhandled(const EXCEPTION_RECORD *record, const CONTEXT *context)
{
    DWORD i;

    fprintf(stderr, "\n[nfsu2] unhandled exception 0x%08lx at %p\n",
            (unsigned long)record->ExceptionCode, record->ExceptionAddress);
    if (record->ExceptionCode == 0xe06d7363u)
        fprintf(stderr, "        that is MSVC's C++ throw, with no matching catch in the chain\n");
    for (i = 0; i < record->NumberParameters && i < 4; i++) {
        fprintf(stderr, "        parameter[%lu] = 0x%lx\n", (unsigned long)i,
                (unsigned long)record->ExceptionInformation[i]);
    }
    fprintf(stderr, "        eip %08lx esp %08lx ebp %08lx eax %08lx\n",
            (unsigned long)context->Eip, (unsigned long)context->Esp,
            (unsigned long)context->Ebp, (unsigned long)context->Eax);
    fprintf(stderr, "        %u exception(s) dispatched, %u handled so far\n",
            g_dispatched, g_handled);
}

/*
 * The top-level filter, which is the game's own crash handler if it registered
 * one (see SetUnhandledExceptionFilter in process.c). Returns non-zero if it
 * asked to resume execution.
 */
static int run_top_level_filter(EXCEPTION_RECORD *record, CONTEXT *context)
{
    LPTOP_LEVEL_EXCEPTION_FILTER filter = nfsu2_top_level_filter();
    EXCEPTION_POINTERS pointers;
    LONG result;

    if (!filter)
        return 0;

    pointers.ExceptionRecord = record;
    pointers.ContextRecord = context;
    result = filter(&pointers);
    if (result == EXCEPTION_CONTINUE_EXECUTION &&
        !(record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
        g_handled++;
        return 1;
    }
    return 0;
}

/* --- RaiseException / RtlUnwind ------------------------------------------ */

VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD argument_count,
                           const ULONG_PTR *arguments)
{
    EXCEPTION_RECORD record;
    CONTEXT context;
    DWORD i;

    memset(&record, 0, sizeof(record));
    record.ExceptionCode = code;
    record.ExceptionFlags = flags & EXCEPTION_NONCONTINUABLE;
    record.ExceptionAddress = __builtin_return_address(0);
    record.NumberParameters = argument_count > EXCEPTION_MAXIMUM_PARAMETERS
        ? EXCEPTION_MAXIMUM_PARAMETERS : argument_count;
    if (arguments) {
        for (i = 0; i < record.NumberParameters; i++)
            record.ExceptionInformation[i] = arguments[i];
    } else {
        record.NumberParameters = 0;
    }

    capture_context(&context, record.ExceptionAddress);

    if (dispatch(&record, &context))
        return; /* a handler repaired it and asked to continue */

    report_unhandled(&record, &context);
    if (run_top_level_filter(&record, &context))
        return;

    /*
     * abort() rather than exit(): a core dump is the only artefact that says
     * where this came from, and by here the chain has already been walked, so
     * nothing is going to handle it.
     */
    abort();
}

/*
 * RtlUnwind: pop frames from the head of the chain up to (but not including)
 * end_frame, giving each handler a chance to run its destructors, then return to
 * the caller - which is the handler that decided to catch, and which jumps to
 * its own __except block once we return.
 *
 * target_ip is ignored here, as it is on Windows/i386: the caller does the
 * transfer of control itself. It exists in the signature because ntdll's does.
 */
VOID WINAPI RtlUnwind(PVOID end_frame_pointer, PVOID target_ip,
                      PEXCEPTION_RECORD record, PVOID value)
{
    EXCEPTION_REGISTRATION_RECORD *end_frame = end_frame_pointer;
    EXCEPTION_REGISTRATION_RECORD *frame;
    EXCEPTION_REGISTRATION_RECORD *dispatcher;
    EXCEPTION_RECORD synthetic;
    CONTEXT context;
    const char *why = "";
    DWORD disposition;

    (void)target_ip;
    (void)value;

    if (!nfsu2_teb_current()) {
        fprintf(stderr, "[nfsu2] RtlUnwind on a thread with no TEB\n");
        abort();
    }

    if (!record) {
        memset(&synthetic, 0, sizeof(synthetic));
        /* 0xc0000027 - STATUS_UNWIND, from ntstatus.h, which is not in the
         * default windows.h include set. */
        synthetic.ExceptionCode = 0xc0000027u;
        synthetic.ExceptionAddress = __builtin_return_address(0);
        record = &synthetic;
    }
    record->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (!end_frame)
        record->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;

    capture_context(&context, __builtin_return_address(0));

    while ((frame = chain_head()) != END_OF_CHAIN && frame != NULL && frame != end_frame) {
        if (!frame_is_plausible(frame, &why)) {
            fprintf(stderr, "[nfsu2] SEH chain is corrupt at %p during unwind: %s\n",
                    (void *)frame, why);
            abort();
        }
        /*
         * The target must be *outwards* of where we are. If it is not, the
         * handler asked to unwind to a frame that is not in the chain, and
         * continuing would pop the whole stack looking for it.
         */
        if (end_frame && frame > end_frame) {
            fprintf(stderr, "[nfsu2] unwind target %p is not in the chain "
                            "(passed %p on the way out)\n",
                    (void *)end_frame, (void *)frame);
            abort();
        }

        dispatcher = frame;
        disposition = frame->Handler(record, frame, &context, &dispatcher);

        if (disposition == ExceptionCollidedUnwind) {
            /* Another unwind was already in progress through this frame; it
             * tells us where to carry on from. */
            set_chain_head(dispatcher);
            continue;
        }
        if (disposition != ExceptionContinueSearch) {
            fprintf(stderr, "[nfsu2] handler %p returned %s during unwind - "
                            "only ContinueSearch is legal there\n",
                    (void *)frame->Handler, disposition_name(disposition));
            abort();
        }

        /* Pop it, unless the handler already did. */
        if (chain_head() == frame)
            set_chain_head(frame->Prev);
    }

    set_chain_head(end_frame ? end_frame : END_OF_CHAIN);
    record->ExceptionFlags &= ~EXCEPTION_UNWINDING;
}

LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *info)
{
    if (info && info->ExceptionRecord) {
        fprintf(stderr, "[nfsu2] UnhandledExceptionFilter: exception 0x%08lx at %p\n",
                (unsigned long)info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
    }
    /*
     * Windows hands the exception to a debugger first if there is one, and the
     * game's __except filters use the return value to decide whether to run
     * their own crash reporter. EXCEPTION_EXECUTE_HANDLER is the answer that
     * lets them do so; EXCEPTION_CONTINUE_SEARCH would send the exception
     * looking for a handler further out that does not exist.
     */
    if (IsDebuggerPresent())
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* --- faults -------------------------------------------------------------- */

static DWORD exception_code_for(int signal_number, const siginfo_t *info,
                               const ucontext_t *uc, ULONG_PTR *parameters,
                               DWORD *parameter_count)
{
    *parameter_count = 0;

    switch (signal_number) {
    case SIGSEGV:
        /*
         * Windows reports "read or write, and at what address". The kernel does
         * not put the direction in siginfo, but the page-fault error code it
         * pushed is in the context: bit 1 set means the access was a write.
         */
        parameters[0] = (uc->uc_mcontext.gregs[REG_ERR] & 2u) ? 1u : 0u;
        parameters[1] = (ULONG_PTR)(uintptr_t)info->si_addr;
        *parameter_count = 2;
        return EXCEPTION_ACCESS_VIOLATION;
    case SIGBUS:
        parameters[0] = 0;
        parameters[1] = (ULONG_PTR)(uintptr_t)info->si_addr;
        *parameter_count = 2;
        return EXCEPTION_DATATYPE_MISALIGNMENT;
    case SIGILL:
        return info->si_code == ILL_PRVOPC || info->si_code == ILL_PRVREG
            ? EXCEPTION_PRIV_INSTRUCTION : EXCEPTION_ILLEGAL_INSTRUCTION;
    case SIGTRAP:
        return EXCEPTION_BREAKPOINT;
    case SIGFPE:
        switch (info->si_code) {
        case FPE_INTDIV: return EXCEPTION_INT_DIVIDE_BY_ZERO;
        case FPE_INTOVF: return EXCEPTION_INT_OVERFLOW;
        case FPE_FLTDIV: return EXCEPTION_FLT_DIVIDE_BY_ZERO;
        case FPE_FLTOVF: return EXCEPTION_FLT_OVERFLOW;
        case FPE_FLTUND: return EXCEPTION_FLT_UNDERFLOW;
        case FPE_FLTRES: return EXCEPTION_FLT_INEXACT_RESULT;
        case FPE_FLTINV: return EXCEPTION_FLT_INVALID_OPERATION;
        case FPE_FLTSUB: return EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        default:         return EXCEPTION_FLT_INVALID_OPERATION;
        }
    default:
        return EXCEPTION_ILLEGAL_INSTRUCTION;
    }
}

static void fault_handler(int signal_number, siginfo_t *info, void *ucontext_raw)
{
    ucontext_t *uc = ucontext_raw;
    EXCEPTION_RECORD record;
    CONTEXT context;
    struct sigaction default_action;

    context_from_ucontext(&context, uc);

    memset(&record, 0, sizeof(record));
    record.ExceptionCode = exception_code_for(signal_number, info, uc,
                                              record.ExceptionInformation,
                                              &record.NumberParameters);
    record.ExceptionAddress = (PVOID)(uintptr_t)context.Eip;

    if (dispatch(&record, &context)) {
        ucontext_from_context(uc, &context);
        return;
    }

    report_unhandled(&record, &context);
    if (run_top_level_filter(&record, &context)) {
        ucontext_from_context(uc, &context);
        return;
    }

    /*
     * Nothing handled it. Rather than abort() here - which would bury the
     * faulting frame under this handler's - put the default disposition back and
     * return: SIGSEGV, SIGBUS and SIGILL re-execute the faulting instruction, so
     * the process dies exactly where it went wrong and the core dump is worth
     * having. The others are not guaranteed to re-fault, so they get abort().
     */
    switch (signal_number) {
    case SIGSEGV:
    case SIGBUS:
    case SIGILL:
        memset(&default_action, 0, sizeof(default_action));
        default_action.sa_handler = SIG_DFL;
        sigaction(signal_number, &default_action, NULL);
        return;
    default:
        abort();
    }
}

static const int g_fault_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };

int nfsu2_seh_install(char *error, size_t error_size)
{
    struct sigaction action;
    size_t i;

    if (g_installed)
        return 0;

    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault_handler;
    sigemptyset(&action.sa_mask);
    /*
     * No SA_ONSTACK, and SA_NODEFER with it - see the header comment. Handlers
     * run on the faulting stack because a handler is allowed to leave by jumping
     * to its __except block, and SA_NODEFER means doing so does not leave the
     * signal blocked for the rest of the process's life.
     */
    action.sa_flags = SA_SIGINFO | SA_NODEFER;

    for (i = 0; i < sizeof(g_fault_signals) / sizeof(g_fault_signals[0]); i++) {
        if (sigaction(g_fault_signals[i], &action, NULL) != 0) {
            if (error) {
                snprintf(error, error_size, "sigaction(%d) failed: %s",
                         g_fault_signals[i], strerror(errno));
            }
            nfsu2_seh_remove();
            return -errno;
        }
    }
    g_installed = 1;
    return 0;
}

void nfsu2_seh_remove(void)
{
    struct sigaction action;
    size_t i;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    for (i = 0; i < sizeof(g_fault_signals) / sizeof(g_fault_signals[0]); i++)
        sigaction(g_fault_signals[i], &action, NULL);
    g_installed = 0;
}

#else /* !__i386__ */

/*
 * The loud-failure versions. An exception here is a crash, and the useful thing
 * is that it is a crash *with a message* at the point it happened: silently
 * continuing after a failed RtlUnwind would corrupt state and produce a second,
 * meaningless crash somewhere else.
 */
VOID WINAPI RaiseException(DWORD code, DWORD flags, DWORD argument_count,
                           const ULONG_PTR *arguments)
{
    (void)flags; (void)argument_count; (void)arguments;
    fprintf(stderr, "\n[nfsu2] RaiseException(0x%08lx): SEH is i386-only here\n",
            (unsigned long)code);
    abort();
}

VOID WINAPI RtlUnwind(PVOID end_frame_pointer, PVOID target_ip,
                      PEXCEPTION_RECORD record, PVOID value)
{
    (void)end_frame_pointer; (void)target_ip; (void)record; (void)value;
    fprintf(stderr, "\n[nfsu2] RtlUnwind: SEH is i386-only here\n");
    abort();
}

LONG WINAPI UnhandledExceptionFilter(struct _EXCEPTION_POINTERS *info)
{
    if (info && info->ExceptionRecord) {
        fprintf(stderr, "[nfsu2] unhandled exception 0x%08lx at %p\n",
                (unsigned long)info->ExceptionRecord->ExceptionCode,
                info->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int nfsu2_seh_install(char *error, size_t error_size)
{
    if (error)
        snprintf(error, error_size, "the fs:[0] handler chain is i386-only");
    return -ENOTSUP;
}

void nfsu2_seh_remove(void) { }

unsigned int nfsu2_seh_dispatched(void) { return 0; }
unsigned int nfsu2_seh_handled(void) { return 0; }

#endif /* __i386__ */
