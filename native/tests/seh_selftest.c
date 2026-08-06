/*
 * seh_selftest.c - the SEH dispatcher, from both ends.
 *
 * The handlers here are ours, written in C, which is the only way to check the
 * dispatcher's side of the contract exactly: what a handler is passed, in what
 * order handlers are called, what happens to the chain during an unwind, and
 * whether a fault can really be resumed from. The other end - our dispatcher
 * calling the *game's* handlers - is in game_functions_selftest.c, because it
 * needs a mapped exe.
 *
 * Everything is i386-specific and unapologetic about it: the chain is a linked
 * list of stack records reached through %fs, and there is no portable version of
 * that.
 *
 * Two of these tests are supposed to kill the process - refusing a corrupt chain
 * has no other honest outcome - so they run in a forked child and the parent
 * checks how it died. The diagnostics they print to stderr are expected output,
 * not failures.
 */
#include <nfsu2/win32_compat.h>

#include <nfsu2/seh.h>
#include <nfsu2/teb.h>

#include <setjmp.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures;

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

#define END_OF_CHAIN ((EXCEPTION_REGISTRATION_RECORD *)~(uintptr_t)0)
#define TEST_CODE    0x20000042u   /* customer bit set, so it is unmistakably ours */

/*
 * The chain head, read and written through %fs the way the game's code does
 * rather than through the TEB pointer - so these tests exercise the same path.
 *
 * Both need the "memory" clobber, and the first version of this file did not
 * have it: publishing a frame through inline asm is invisible to the optimiser
 * otherwise, so GCC deleted the stores that filled in the record - it could see
 * nothing that read them - and the dispatcher then found a registration record
 * with a null handler. The clobber is what says "this asm makes memory
 * observable".
 */
static EXCEPTION_REGISTRATION_RECORD *chain_head(void)
{
    EXCEPTION_REGISTRATION_RECORD *head;

    __asm__ volatile("movl %%fs:0x00, %0" : "=r"(head) :: "memory");
    return head;
}

static void set_chain_head(EXCEPTION_REGISTRATION_RECORD *frame)
{
    __asm__ volatile("movl %0, %%fs:0x00" :: "r"(frame) : "memory");
}

/* --- what the handlers record -------------------------------------------- */

#define MAX_EVENTS 16

struct event {
    const EXCEPTION_REGISTRATION_RECORD *frame;
    DWORD code;
    DWORD flags;
    DWORD parameter_count;
    ULONG_PTR parameters[3];
    int unwinding;
};

static struct event g_events[MAX_EVENTS];
static int g_event_count;

/* The record lives on the dispatcher's stack, so copy what is wanted out of it
 * rather than keeping the pointer. */
static void note(const EXCEPTION_RECORD *record, const EXCEPTION_REGISTRATION_RECORD *frame)
{
    struct event *event;
    DWORD i;

    if (g_event_count < MAX_EVENTS) {
        event = &g_events[g_event_count];
        event->frame = frame;
        event->code = record->ExceptionCode;
        event->flags = record->ExceptionFlags;
        event->unwinding = (record->ExceptionFlags & EXCEPTION_UNWINDING) != 0;
        event->parameter_count = record->NumberParameters;
        for (i = 0; i < record->NumberParameters && i < 3; i++)
            event->parameters[i] = record->ExceptionInformation[i];
    }
    g_event_count++;
}

/* Records everything and declines, which is what most frames do most of the
 * time: the frame that catches is rare, and every frame between it and the throw
 * takes this path. */
static DWORD CDECL decline(PEXCEPTION_RECORD record, EXCEPTION_REGISTRATION_RECORD *frame,
                           PCONTEXT context, EXCEPTION_REGISTRATION_RECORD **dispatcher)
{
    (void)context; (void)dispatcher;
    note(record, frame);
    return ExceptionContinueSearch;
}

/* Says "I fixed it", which for a RaiseException means the raiser just returns. */
static DWORD CDECL accept_and_continue(PEXCEPTION_RECORD record,
                                       EXCEPTION_REGISTRATION_RECORD *frame,
                                       PCONTEXT context,
                                       EXCEPTION_REGISTRATION_RECORD **dispatcher)
{
    (void)context; (void)dispatcher;
    note(record, frame);
    return ExceptionContinueExecution;
}

/* --- 1. dispatch order --------------------------------------------------- */

/*
 * Three frames, innermost first. An array is used deliberately: the records must
 * be at increasing addresses to look like a real chain (the dispatcher checks
 * that Prev points outwards), and separate locals have no guaranteed order.
 */
static void test_dispatch_order(void)
{
    EXCEPTION_REGISTRATION_RECORD frames[3];
    EXCEPTION_REGISTRATION_RECORD *saved = chain_head();
    unsigned int handled_before = nfsu2_seh_handled();
    int i;

    printf("\n# dispatch: the chain, in order, until someone stops\n");

    for (i = 0; i < 3; i++) {
        frames[i].Prev = (i == 2) ? END_OF_CHAIN : &frames[i + 1];
        frames[i].Handler = decline;
    }
    frames[2].Handler = accept_and_continue;

    g_event_count = 0;
    set_chain_head(&frames[0]);
    RaiseException(TEST_CODE, 0, 0, NULL);
    set_chain_head(saved);

    /*
     * Reaching this line is itself an assertion: had nothing accepted, the
     * dispatcher would have reported an unhandled exception and aborted.
     */
    CHECK(g_event_count == 3, "all three handlers ran (%d)", g_event_count);
    CHECK(g_event_count == 3 && g_events[0].frame == &frames[0] &&
          g_events[1].frame == &frames[1] && g_events[2].frame == &frames[2],
          "innermost first, outwards along Prev");
    CHECK(g_event_count > 0 && g_events[0].code == TEST_CODE,
          "the handler got our exception code, 0x%08lx",
          (unsigned long)g_events[0].code);
    CHECK(nfsu2_seh_handled() == handled_before + 1,
          "ContinueExecution counted as handled, and RaiseException returned");
    CHECK(chain_head() == saved, "and the chain is where we left it");
}

/* --- 2. catching, the way a __except block does --------------------------- */

/*
 * A real handler that decides to catch does not return a disposition at all: it
 * calls RtlUnwind to pop everything between the throw and itself, then restores
 * esp/ebp from its own registration record and jumps to the __except block.
 * setjmp/longjmp is the C spelling of that same transfer of control, and using it
 * here means the catch path is exercised rather than described.
 *
 * It is also the only way to test a *non-continuable* exception, since the
 * dispatcher must refuse ExceptionContinueExecution for one.
 */
static jmp_buf g_catch;

static DWORD CDECL catch_here(PEXCEPTION_RECORD record, EXCEPTION_REGISTRATION_RECORD *frame,
                              PCONTEXT context, EXCEPTION_REGISTRATION_RECORD **dispatcher)
{
    (void)context; (void)dispatcher;
    note(record, frame);
    RtlUnwind(frame, NULL, record, NULL);
    longjmp(g_catch, 1);
    return ExceptionContinueSearch; /* not reached */
}

static void test_catch_and_unwind(void)
{
    EXCEPTION_REGISTRATION_RECORD frames[3];
    EXCEPTION_REGISTRATION_RECORD *saved = chain_head();
    ULONG_PTR arguments[3] = { 0x11111111u, 0x22222222u, 0x33333333u };
    int caught;
    int i;

    printf("\n# catching: unwind the frames in between, then transfer control\n");

    for (i = 0; i < 3; i++) {
        frames[i].Prev = (i == 2) ? END_OF_CHAIN : &frames[i + 1];
        frames[i].Handler = decline;
    }
    frames[2].Handler = catch_here;

    g_event_count = 0;
    set_chain_head(&frames[0]);

    caught = setjmp(g_catch);
    if (caught == 0)
        RaiseException(TEST_CODE, EXCEPTION_NONCONTINUABLE, 3, arguments);

    set_chain_head(saved);

    CHECK(caught == 1, "control arrived at the catch point");
    /*
     * Five events: three on the way out looking for a handler, then two more as
     * frames[0] and frames[1] are unwound. frames[2] does not unwind itself.
     */
    CHECK(g_event_count == 5, "3 dispatch + 2 unwind calls (%d)", g_event_count);
    CHECK(g_event_count >= 4 && g_events[3].unwinding && g_events[4].unwinding,
          "the last two carried EXCEPTION_UNWINDING - destructors would run there");
    CHECK(g_event_count >= 5 && g_events[3].frame == &frames[0] &&
          g_events[4].frame == &frames[1],
          "and unwound innermost first, stopping before the catching frame");
    CHECK(g_event_count > 0 && (g_events[0].flags & EXCEPTION_NONCONTINUABLE) != 0,
          "EXCEPTION_NONCONTINUABLE reached the handler (flags 0x%lx)",
          (unsigned long)g_events[0].flags);
    CHECK(g_event_count > 0 && g_events[0].parameter_count == 3 &&
          g_events[0].parameters[0] == 0x11111111u &&
          g_events[0].parameters[2] == 0x33333333u,
          "with all three arguments intact - this is how a C++ throw carries its object");
}

/* --- 3. unwinding on its own --------------------------------------------- */

static void test_unwind_bounds(void)
{
    EXCEPTION_REGISTRATION_RECORD frames[4];
    EXCEPTION_REGISTRATION_RECORD *saved = chain_head();
    unsigned int dispatched_before;
    int i;

    printf("\n# RtlUnwind on its own\n");

    for (i = 0; i < 4; i++) {
        frames[i].Prev = (i == 3) ? END_OF_CHAIN : &frames[i + 1];
        frames[i].Handler = decline;
    }

    g_event_count = 0;
    dispatched_before = nfsu2_seh_dispatched();
    set_chain_head(&frames[0]);
    RtlUnwind(&frames[2], NULL, NULL, NULL);

    CHECK(g_event_count == 2, "unwinding to frames[2] ran the two inside it (%d)",
          g_event_count);
    CHECK(chain_head() == &frames[2], "fs:[0] is the target frame afterwards (%p)",
          (void *)chain_head());
    CHECK(nfsu2_seh_dispatched() == dispatched_before,
          "an unwind is not a dispatch: that counter is untouched (%u)",
          nfsu2_seh_dispatched());

    /* An exit unwind - no target - empties the chain. */
    g_event_count = 0;
    RtlUnwind(NULL, NULL, NULL, NULL);
    CHECK(g_event_count == 2, "an exit unwind ran the remaining two frames (%d)",
          g_event_count);
    CHECK(chain_head() == END_OF_CHAIN, "and left the chain terminated");
    CHECK(g_event_count >= 1 && (g_events[0].flags & EXCEPTION_EXIT_UNWIND) != 0,
          "flagged EXCEPTION_EXIT_UNWIND (0x%lx), which tells a handler the thread "
          "is going away", (unsigned long)g_events[0].flags);

    set_chain_head(saved);
}

/* --- 4. a real fault, repaired and resumed -------------------------------- */

static void *g_guard_page;
static size_t g_page_size;
static int g_fault_seen;
static DWORD g_fault_code;
static ULONG_PTR g_fault_address;
static ULONG_PTR g_fault_was_write;
static DWORD g_fault_eip;

/*
 * Repairs the fault and asks to resume, which re-executes the faulting
 * instruction - now that the page it wrote to is writable, it succeeds.
 *
 * This is what ExceptionContinueExecution is *for*: guard pages, copy-on-write
 * emulation, and the "commit this region on first touch" pattern. It also avoids
 * the trap the first version of this test fell into - moving Eip to a label taken
 * with GCC's `&&label` - because at -O2 the label's block was laid out *before*
 * the faulting store, so resuming there fell straight back into the fault and
 * looped forever. Repairing the memory needs no address arithmetic at all.
 */
static DWORD CDECL repair_and_retry(PEXCEPTION_RECORD record,
                                    EXCEPTION_REGISTRATION_RECORD *frame,
                                    PCONTEXT context,
                                    EXCEPTION_REGISTRATION_RECORD **dispatcher)
{
    (void)frame; (void)dispatcher;

    g_fault_seen++;
    g_fault_code = record->ExceptionCode;
    g_fault_eip = context->Eip;
    if (record->NumberParameters >= 2) {
        g_fault_was_write = record->ExceptionInformation[0];
        g_fault_address = record->ExceptionInformation[1];
    }
    if (g_fault_seen > 4)
        return ExceptionContinueSearch; /* refuse to loop if the repair fails */
    if (mprotect(g_guard_page, g_page_size, PROT_READ | PROT_WRITE) != 0)
        return ExceptionContinueSearch;
    return ExceptionContinueExecution;
}

static void test_fault_becomes_exception(void)
{
    EXCEPTION_REGISTRATION_RECORD frame;
    EXCEPTION_REGISTRATION_RECORD *saved = chain_head();
    volatile int *guarded;
    char error[256] = "";

    printf("\n# a real SIGSEGV, as an exception, repaired and resumed\n");

    CHECK(nfsu2_seh_install(error, sizeof(error)) == 0, "installed the fault handlers (%s)",
          error[0] ? error : "no error");

    g_page_size = (size_t)sysconf(_SC_PAGESIZE);
    g_guard_page = mmap(NULL, g_page_size, PROT_NONE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(g_guard_page != MAP_FAILED, "mapped an unwritable page to fault on");
    if (g_guard_page == MAP_FAILED)
        return;

    frame.Prev = END_OF_CHAIN;
    frame.Handler = repair_and_retry;
    g_fault_seen = 0;
    set_chain_head(&frame);

    guarded = g_guard_page;
    *guarded = 0x1234; /* faults; the handler makes the page writable; retried */

    set_chain_head(saved);

    CHECK(g_fault_seen == 1, "the handler saw the fault once (%d)", g_fault_seen);
    CHECK(g_fault_code == (DWORD)EXCEPTION_ACCESS_VIOLATION,
          "as EXCEPTION_ACCESS_VIOLATION (0x%08lx)", (unsigned long)g_fault_code);
    CHECK(g_fault_address == (ULONG_PTR)(uintptr_t)g_guard_page,
          "at the address that faulted (0x%lx)", (unsigned long)g_fault_address);
    CHECK(g_fault_was_write == 1,
          "reported as a write, which comes from the page-fault error code in the context");
    CHECK(g_fault_eip != 0, "with the faulting instruction's address in Eip (0x%08lx)",
          (unsigned long)g_fault_eip);
    CHECK(*guarded == 0x1234,
          "and the store completed on the retry - ExceptionContinueExecution works");

    munmap(g_guard_page, g_page_size);
    nfsu2_seh_remove();
}

/* --- 5. the two failures that have to be fatal --------------------------- */

/*
 * Following a corrupt chain means calling a function pointer read out of a
 * random stack slot, and unwinding to a frame that is not in the chain means
 * popping the whole stack looking for it. Both have to stop the process, so both
 * are checked in a child: the parent asserts on *how* it died.
 */
static int run_in_child(void (*body)(void))
{
    pid_t child = fork();
    int status = 0;

    if (child < 0)
        return -1;
    if (child == 0) {
        /* stdout is the test log; the child's own output would interleave. */
        fflush(stdout);
        body();
        _exit(0); /* reaching here is the failure the parent is looking for */
    }
    if (waitpid(child, &status, 0) < 0)
        return -1;
    return status;
}

static void body_corrupt_chain(void)
{
    static EXCEPTION_REGISTRATION_RECORD frames[2];
    EXCEPTION_REGISTRATION_RECORD *inner = &frames[1];

    /* Prev pointing *inwards* is what an overwritten chain looks like. */
    frames[1].Prev = &frames[0];
    frames[1].Handler = decline;
    frames[0].Prev = END_OF_CHAIN;
    frames[0].Handler = accept_and_continue;

    /* Static storage, so the records are outside the stack too - either check
     * is enough to refuse them, and this way the test does not depend on which
     * one fires first. */
    set_chain_head(inner);
    RaiseException(TEST_CODE, 0, 0, NULL);
}

static void body_bad_unwind_target(void)
{
    EXCEPTION_REGISTRATION_RECORD frames[2];
    /* Through uintptr_t, and volatile, because GCC recognises pointer
     * arithmetic that leaves an object and warns about it - here the address
     * being out of bounds is the entire point. */
    volatile uintptr_t below = (uintptr_t)&frames[0] - 16;
    void *inwards_of_the_chain = (void *)below;
    int i;

    for (i = 0; i < 2; i++) {
        frames[i].Prev = (i == 1) ? END_OF_CHAIN : &frames[i + 1];
        frames[i].Handler = decline;
    }

    set_chain_head(&frames[0]);
    /* A target *inwards* of the innermost frame can never be reached by walking
     * outwards, so the unwind would pop the whole stack looking for it. */
    RtlUnwind(inwards_of_the_chain, NULL, NULL, NULL);
}

static void test_fatal_paths(void)
{
    int status;

    printf("\n# the failures that must be fatal (the child's diagnostics follow)\n");

    status = run_in_child(body_corrupt_chain);
    CHECK(status >= 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "a corrupt chain aborts rather than following the bad link (status 0x%x)",
          status);

    status = run_in_child(body_bad_unwind_target);
    CHECK(status >= 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT,
          "an unwind target that is not in the chain aborts (status 0x%x)", status);
}

int main(void)
{
    char error[256] = "";

    /* Unbuffered: this test deliberately aborts child processes and can crash
     * outright while debugging the dispatcher, and block-buffered output would
     * be lost exactly when it is most wanted. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("# nfsu2 SEH selftest (%d-bit)\n", (int)(sizeof(void *) * 8));

    if (nfsu2_teb_install(error, sizeof(error)) != 0) {
        printf("SKIP - no TEB available: %s\n", error);
        return 77;
    }
    /* An empty chain to start from, as Windows leaves it. */
    set_chain_head(END_OF_CHAIN);

    test_dispatch_order();
    test_catch_and_unwind();
    test_unwind_bounds();
    test_fault_becomes_exception();
    test_fatal_paths();

    printf("\n%s (%d failure%s, %u exception(s) dispatched, %u handled)\n",
           g_failures == 0 ? "PASSED" : "FAILED", g_failures,
           g_failures == 1 ? "" : "s", nfsu2_seh_dispatched(), nfsu2_seh_handled());
    return g_failures == 0 ? 0 : 1;
}
