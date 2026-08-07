/*
 * bridge.c - the runtime behind the generated D3D9 thunks.
 *
 * See include/nfsu2/d3d9_bridge.h for the two reasons this exists (calling
 * convention and stack alignment). This file owns three things the generator cannot:
 * the bridge object, the identity map that keeps one bridge per real object, and
 * IUnknown - because refcounting decides when a bridge may be freed.
 */
#include <nfsu2/win32_compat.h>

#include <nfsu2/d3d9_bridge.h>
#include <nfsu2/d3d9_native.h>
#include <nfsu2/win32_shim.h>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * What every thunk is: __stdcall, because that is what the game's call site does,
 * and force_align_arg_pointer, because the callee is GCC code that will `movaps` to
 * a stack slot and MSVC does not align the stack to 16 bytes. Dropping either one
 * is a crash, and the second one is a crash four frames deep in someone else's
 * library.
 */
#define NFSU2_D3D9_THUNK __attribute__((stdcall, force_align_arg_pointer))

struct nfsu2_d3d9_bridge {
    const void **lpVtbl;              /* our stdcall vtable - must be first */
    void *real;                       /* the DXVK object */
    const void *const *real_vtbl;     /* its cdecl vtable */
    enum nfsu2_d3d9_iface iface;
    unsigned int retired;             /* the wrapped object is gone; see the quarantine */
};

/* Defined below the generated file, which supplies the name table. Declared here
 * because the diagnostics throughout this file name the interface they are about. */
static const char *iface_name(enum nfsu2_d3d9_iface iface);

/* --- the identity map ----------------------------------------------------- */

/*
 * One bridge per real object, and it matters: `GetBackBuffer` called twice returns
 * the same surface, and a game that caches interface pointers compares them. Two
 * bridges for one object would also mean two independent refcount views of it.
 *
 * Open-addressed, locked, and **grown on demand**. It used to be a fixed 2048
 * entries, on the reasoning that a frame's worth of D3D9 objects is hundreds rather
 * than thousands. Loading a track disproves that: one run wraps 8354 objects and
 * overflowed the table 5435 times.
 *
 * Overflowing was not a degraded mode, it was a wild jump. An untracked bridge is
 * invisible to bridge_find_locked, so nfsu2_d3d9_unwrap cannot tell it from a plain
 * data pointer and hands *the bridge* to DXVK, which reads its first word as a
 * vtable - our stdcall table - and calls a slot of it as one of its own methods.
 * The result was a jump to whatever that slot held. Growing is the fix; the trace
 * that said "table is full" was the only warning, and it was easy to read as
 * bookkeeping rather than as the cause.
 */
#define BRIDGE_INITIAL_SLOTS 4096

static struct nfsu2_d3d9_bridge **g_bridges;
static size_t g_bridge_slots;
static unsigned int g_bridge_count;
static unsigned int g_bridge_calls;
static unsigned int g_bridge_untracked;
static pthread_mutex_t g_bridge_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t bridge_slot(const void *real, size_t slots)
{
    uintptr_t value = (uintptr_t)real >> 4;

    value *= 2654435761u;
    return (size_t)value & (slots - 1);
}

static struct nfsu2_d3d9_bridge *bridge_find_locked(const void *real)
{
    size_t start;
    size_t probe;

    if (!g_bridges)
        return NULL;
    start = bridge_slot(real, g_bridge_slots);
    for (probe = 0; probe < g_bridge_slots; probe++) {
        size_t index = (start + probe) & (g_bridge_slots - 1);

        if (!g_bridges[index])
            return NULL;
        if (g_bridges[index]->real == real)
            return g_bridges[index];
    }
    return NULL;
}

/*
 * Put a bridge in the table as it stands. Never grows, so it is safe to call while
 * walking the table - which bridge_forget does when it closes a probe cluster.
 */
static int bridge_place_locked(struct nfsu2_d3d9_bridge *bridge)
{
    size_t start;
    size_t probe;

    if (!g_bridges)
        return -1;
    start = bridge_slot(bridge->real, g_bridge_slots);
    for (probe = 0; probe < g_bridge_slots; probe++) {
        size_t index = (start + probe) & (g_bridge_slots - 1);

        if (!g_bridges[index]) {
            g_bridges[index] = bridge;
            g_bridge_count++;
            return 0;
        }
    }
    return -1;
}

/* Double the table and re-place everything. Returns 0 if the table got bigger. */
static int bridge_grow_locked(void)
{
    struct nfsu2_d3d9_bridge **old = g_bridges;
    size_t old_slots = g_bridge_slots;
    size_t i;

    g_bridge_slots = old_slots ? old_slots * 2 : BRIDGE_INITIAL_SLOTS;
    g_bridges = calloc(g_bridge_slots, sizeof(*g_bridges));
    if (!g_bridges) {
        /* Keep what we had rather than lose every mapping at once. */
        g_bridges = old;
        g_bridge_slots = old_slots;
        return -1;
    }
    g_bridge_count = 0;
    for (i = 0; i < old_slots; i++) {
        if (old[i])
            bridge_place_locked(old[i]);
    }
    free(old);
    return 0;
}

static void bridge_insert_locked(struct nfsu2_d3d9_bridge *bridge)
{
    /* Grown at three-quarters full, because linear probing degrades before it
     * fills, and again if a place somehow still fails. */
    if (!g_bridges || (g_bridge_count + 1) * 4 > g_bridge_slots * 3)
        bridge_grow_locked();
    if (bridge_place_locked(bridge) == 0)
        return;
    if (bridge_grow_locked() == 0 && bridge_place_locked(bridge) == 0)
        return;

    /*
     * Out of memory for the table. The bridge is still returned to the game, since
     * calls through it work; what is lost is the ability to recognise it later, so
     * this is loud and counted rather than a one-line note.
     */
    g_bridge_untracked++;
    nfsu2_shim_trace("d3d9 bridge: CANNOT TRACK %s %p - out of memory for the "
                     "identity map (%u untracked). Unwrapping will not recognise "
                     "it and DXVK may be handed the bridge itself.",
                     iface_name(bridge->iface), bridge->real, g_bridge_untracked);
}

static void bridge_forget(struct nfsu2_d3d9_bridge *bridge)
{
    size_t start;
    size_t probe;

    pthread_mutex_lock(&g_bridge_lock);
    if (!g_bridges) {
        pthread_mutex_unlock(&g_bridge_lock);
        return;
    }
    start = bridge_slot(bridge->real, g_bridge_slots);
    for (probe = 0; probe < g_bridge_slots; probe++) {
        size_t index = (start + probe) & (g_bridge_slots - 1);

        if (!g_bridges[index])
            break;
        if (g_bridges[index] == bridge) {
            g_bridges[index] = NULL;
            g_bridge_count--;
            /* Re-place the rest of the cluster, since linear probing cannot see
             * past a hole. bridge_place_locked, not bridge_insert_locked: growing
             * here would rehash the table this loop is walking. There is room, a
             * slot having just been freed. */
            for (probe = probe + 1; probe < g_bridge_slots; probe++) {
                size_t next = (start + probe) & (g_bridge_slots - 1);
                struct nfsu2_d3d9_bridge *moved = g_bridges[next];

                if (!moved)
                    break;
                g_bridges[next] = NULL;
                g_bridge_count--;
                bridge_place_locked(moved);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_bridge_lock);
}

/* --- the quarantine: a double release should be a report, not a wild jump --- */

/*
 * When a bridge's object reaches refcount zero the bridge has to stop existing -
 * but freeing it immediately means a release that arrives afterwards reads a
 * recycled 16-byte chunk, and glibc has by then written its own tcache bookkeeping
 * into the very field that holds the vtable pointer. The call goes to whatever that
 * word happens to be. That is how this presented: an unmapped-address jump out of
 * nfsu2_d3d9_Release, with nothing to say which interface it had been.
 *
 * So retirement is deferred. A retired bridge is marked, kept, and only freed once
 * this many further retirements have happened - long enough for a stray release to
 * land on a bridge that can still describe itself, and bounded, so it is not a leak.
 * The cost is 1024 * 20 bytes.
 */
#define BRIDGE_QUARANTINE 1024

static struct nfsu2_d3d9_bridge *g_retired[BRIDGE_QUARANTINE];
static unsigned int g_retired_next;
static unsigned int g_use_after_release;

static void bridge_retire(struct nfsu2_d3d9_bridge *bridge)
{
    struct nfsu2_d3d9_bridge *evicted;

    pthread_mutex_lock(&g_bridge_lock);
    bridge->retired = 1;
    /* real and real_vtbl are deliberately left alone: they are what makes the
     * report below able to name the interface and the object it wrapped. */
    evicted = g_retired[g_retired_next];
    g_retired[g_retired_next] = bridge;
    g_retired_next = (g_retired_next + 1) % BRIDGE_QUARANTINE;
    pthread_mutex_unlock(&g_bridge_lock);

    free(evicted);
}

/*
 * A call on a bridge whose object is gone. Reported once per occurrence with
 * everything known about it, and answered as a released interface would be, so the
 * caller carries on instead of dying four frames into someone else's library.
 */
static unsigned bridge_use_after_release(struct nfsu2_d3d9_bridge *self, const char *api)
{
    g_use_after_release++;
    nfsu2_shim_trace("d3d9 bridge: USE AFTER RELEASE - %s on %s bridge %p, whose "
                     "object %p was already destroyed (%u so far)",
                     api, iface_name(self->iface), (void *)self, self->real,
                     g_use_after_release);
    return 0;
}

unsigned int nfsu2_d3d9_use_after_release(void)
{
    return g_use_after_release;
}

static int is_bridge(const void *pointer)
{
    int found;

    if (!pointer)
        return 0;
    pthread_mutex_lock(&g_bridge_lock);
    found = bridge_find_locked(((const struct nfsu2_d3d9_bridge *)pointer)->real) == pointer;
    pthread_mutex_unlock(&g_bridge_lock);
    return found;
}

/* --- IUnknown ------------------------------------------------------------- */

/* Declared before the generated file, which puts them in every vtable. */
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_QueryInterface(struct nfsu2_d3d9_bridge *self,
                                                          unsigned iid, unsigned out);
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_AddRef(struct nfsu2_d3d9_bridge *self);
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_Release(struct nfsu2_d3d9_bridge *self);

/* --- state block recording depth ------------------------------------------ */

/*
 * BeginStateBlock fails with D3DERR_INVALIDCALL for exactly one reason: a state
 * block is already being recorded. This game gets that answer 41 times a run, which
 * looked alarming enough to be worth settling, because the two readings of it are
 * very different. If a recorder were left open, every state-setting call after it
 * would be captured into the block instead of reaching the device, and whatever was
 * being drawn would be wrong from then on.
 *
 * Measured, and it is the harmless reading. Over a run to the main menu the counts
 * are 326 begun and 326 ended, and the pattern around each refusal is
 *
 *     BeginStateBlock  ok      depth 1
 *     BeginStateBlock  refused
 *     BeginStateBlock  refused
 *     EndStateBlock    ok      depth 0
 *
 * so the game asks twice, ignores the answer it cannot have, and closes the block it
 * did open. Windows refuses the same call the same way. The FAILED lines in a trace
 * are noise, not a lead - which is worth writing down, since they read as a lead.
 *
 * The counters stay because they are what proves it, and because an unbalanced depth
 * would be a real and otherwise invisible bug. NFSU2_D3D9_TRACE_STATE_BLOCKS=1 shows
 * every Begin and End if the balance ever needs re-checking.
 */
static int g_state_block_depth;
static int g_state_block_nested;
static int g_state_block_unbalanced;
static unsigned int g_state_block_begins;
static unsigned int g_state_block_ends;
static int g_trace_state_blocks = -1;

/* Every Begin and End, for finding where the two stop pairing. Off by default:
 * there are hundreds of them and only the divergence is interesting. */
static int trace_state_blocks(void)
{
    if (g_trace_state_blocks < 0) {
        const char *value = getenv("NFSU2_D3D9_TRACE_STATE_BLOCKS");

        g_trace_state_blocks = (value && *value && *value != '0') ? 1 : 0;
    }
    return g_trace_state_blocks;
}

static void state_block_begin(unsigned int result)
{
    if (result & 0x80000000u) {
        g_state_block_nested++;
        nfsu2_shim_trace("d3d9: BeginStateBlock refused - %d recording(s) already open "
                         "(%d refused so far, %u begun / %u ended)",
                         g_state_block_depth, g_state_block_nested,
                         g_state_block_begins, g_state_block_ends);
        return;
    }
    g_state_block_depth++;
    g_state_block_begins++;
    if (trace_state_blocks())
        nfsu2_shim_trace("d3d9: BeginStateBlock #%u ok, depth now %d",
                         g_state_block_begins, g_state_block_depth);
}

static void state_block_end(unsigned int result)
{
    if (result & 0x80000000u)
        return;
    g_state_block_ends++;
    if (trace_state_blocks())
        nfsu2_shim_trace("d3d9: EndStateBlock #%u ok, depth was %d",
                         g_state_block_ends, g_state_block_depth);
    if (g_state_block_depth > 0) {
        g_state_block_depth--;
        return;
    }
    /* An End with nothing open. Counted rather than ignored: it would mean the
     * depth this file thinks it is tracking has drifted from DXVK's. */
    g_state_block_unbalanced++;
}

unsigned int nfsu2_d3d9_state_block_depth(void)
{
    return (unsigned int)(g_state_block_depth < 0 ? 0 : g_state_block_depth);
}

/* --- the FPU, which Direct3D9 owns and DXVK Native does not touch ---------- */

/*
 * Direct3D9's CreateDevice, on Windows and on x86, reprograms the x87 unit:
 * single precision, round to nearest, and every floating-point exception masked.
 * It is documented, it is what D3DCREATE_FPU_PRESERVE exists to opt out of, and
 * it is not optional behaviour a game has to ask for - it just happens.
 *
 * DXVK Native does not do it, and the omission is not harmless. This game leaves
 * the control word at 0xe72 - 53-bit precision, round toward zero, with invalid
 * operation, divide-by-zero and overflow *unmasked*. Every thread DXVK creates
 * inherits that word, so the NVIDIA driver raised an invalid-operation trap on
 * dxvk-submit and the process took a SIGFPE on a thread that has no TEB and no
 * business handling Win32 exceptions. On Windows the same code cannot fault,
 * because D3D9 masked those exceptions before the driver ever ran.
 *
 * Doing it at CreateDevice is what makes it reach DXVK's worker threads: they are
 * created inside that call, and a new thread inherits the creating thread's x87
 * state. MXCSR is set too - SSE exceptions were already masked here, but relying
 * on that would be relying on an accident.
 *
 * Same shape as Wine's d3d_fpu_setup, and for the same reason.
 */
#define NFSU2_D3DCREATE_FPU_PRESERVE 0x00000002u

static void fpu_setup(unsigned int behaviour)
{
    if (behaviour & NFSU2_D3DCREATE_FPU_PRESERVE) {
        nfsu2_shim_trace("d3d9 bridge: D3DCREATE_FPU_PRESERVE - leaving the FPU as "
                         "the game set it");
        return;
    }
#if defined(__i386__) || defined(__x86_64__)
    {
        unsigned short control;
        unsigned int mxcsr;

        __asm__ __volatile__("fnstcw %0" : "=m"(control));
        /*
         * Clear bits 0-5 (the exception masks) and 8-11 (precision and rounding
         * control), then set all six masks. That leaves PC = 00 (24-bit single)
         * and RC = 00 (round to nearest), which is what D3D9 installs.
         */
        control = (unsigned short)((control & ~0x0f3fu) | 0x003fu);
        __asm__ __volatile__("fldcw %0" : : "m"(control));

        __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
        mxcsr |= 0x1f80u; /* the six SSE exception masks */
        __asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr));

        nfsu2_shim_trace("d3d9 bridge: FPU set the way D3D9 does it - single "
                         "precision, round to nearest, all exceptions masked "
                         "(x87 control word 0x%04x)", control);
    }
#else
    nfsu2_shim_trace("d3d9 bridge: no FPU setup on this architecture");
#endif
}

/*
 * Is the game asking to lock more of a buffer than the buffer has?
 *
 * The hypothesis this tests: our heap canary proves nothing overruns a block *we*
 * allocated, so the corruption may be of memory DXVK owns - and Lock is the one
 * place DXVK hands the game a raw pointer to write through. If the game asks for a
 * region larger than the buffer, whatever DXVK returns is smaller than what the game
 * then fills.
 *
 * Read-only: it asks the real object for its descriptor and compares. Nothing is
 * written into DXVK's memory, because that is the thing under suspicion.
 *
 * D3DVERTEXBUFFER_DESC and D3DINDEXBUFFER_DESC both start Format, Type, Usage, Pool,
 * Size - so Size is the fifth dword either way.
 */
static void audit_buffer_lock(struct nfsu2_d3d9_bridge *self, unsigned int getdesc_slot,
                              unsigned int offset, unsigned int size)
{
    unsigned int desc[8];
    unsigned int result;
    unsigned int capacity;

    memset(desc, 0, sizeof(desc));
    result = ((unsigned (*)(void *, unsigned))self->real_vtbl[getdesc_slot])
        (self->real, (unsigned)(uintptr_t)desc);
    if (result & 0x80000000u)
        return; /* no descriptor, nothing to compare against */

    capacity = desc[4];
    /* SizeToLock 0 means "from the offset to the end", which cannot overrun. */
    if (size == 0)
        return;
    if ((unsigned long long)offset + size > (unsigned long long)capacity) {
        nfsu2_shim_trace("d3d9 LOCK PAST END: %s asked for offset %u + %u bytes of a "
                         "%u-byte buffer - %llu byte(s) beyond it",
                         iface_name(self->iface), offset, size, capacity,
                         (unsigned long long)offset + size - capacity);
    }
}

/* --- guarded locks: catch a write past a locked region ---------------------- */

/*
 * NFSU2_D3D9_GUARD_LOCKS=1 hands the game *our* memory for the duration of a buffer
 * lock, with an unmapped page right after it, and copies the contents into DXVK's
 * pointer at Unlock.
 *
 * Why a bounce buffer rather than a canary: the memory DXVK returns from Lock sits
 * inside a larger allocation of its own, so there is nowhere safe to put a guard -
 * and a canary only reports *that* something was written past the end, long after the
 * instruction that did it. With an unmapped page, the offending store faults where it
 * happens and the backtrace names the code. That is the difference between knowing
 * there is a bug and knowing where it is.
 *
 * Off by default: it costs a copy per lock, and it changes what pointer the game sees.
 */
struct guarded_lock {
    struct nfsu2_d3d9_bridge *owner;
    void *mapping;        /* our region plus one unmapped page */
    size_t mapping_size;
    void *given;          /* what the game got: the end of our region, page-aligned */
    void *real_bits;      /* what DXVK gave us */
    size_t size;
};

#define MAX_GUARDED_LOCKS 32

static struct guarded_lock g_guarded[MAX_GUARDED_LOCKS];
static int g_guard_locks = -1;

static int guard_locks_enabled(void)
{
    if (g_guard_locks < 0) {
        const char *value = getenv("NFSU2_D3D9_GUARD_LOCKS");

        g_guard_locks = (value && *value && *value != '0') ? 1 : 0;
        if (g_guard_locks)
            nfsu2_shim_trace("d3d9 bridge: guarded locks are ON - the game writes into "
                             "our memory, with an unmapped page after it");
    }
    return g_guard_locks;
}

/*
 * The region is placed so that its *end* is page-aligned and the next page is
 * unmapped: an overrun of even one byte lands in the hole. Under-running is not what
 * is being looked for here.
 */
static void guard_lock_post(struct nfsu2_d3d9_bridge *self, unsigned int getdesc_slot,
                            unsigned int offset, unsigned int requested,
                            unsigned int out, unsigned int result)
{
    unsigned int desc[8];
    void **bits = (void **)(uintptr_t)out;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size;
    size_t body;
    unsigned char *mapping;
    int slot;

    if (!guard_locks_enabled() || (result & 0x80000000u) || !bits || !*bits)
        return;

    memset(desc, 0, sizeof(desc));
    if (((unsigned (*)(void *, unsigned))self->real_vtbl[getdesc_slot])
            (self->real, (unsigned)(uintptr_t)desc) & 0x80000000u)
        return;

    size = requested ? requested : (desc[4] > offset ? desc[4] - offset : 0);
    if (!size)
        return;

    for (slot = 0; slot < MAX_GUARDED_LOCKS; slot++) {
        if (!g_guarded[slot].mapping)
            break;
    }
    if (slot == MAX_GUARDED_LOCKS)
        return; /* too many at once: leave this one alone rather than lose track */

    body = ((size + page - 1) / page) * page;
    mapping = mmap(NULL, body + page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return;
    if (mprotect(mapping + body, page, PROT_NONE) != 0) {
        munmap(mapping, body + page);
        return;
    }

    g_guarded[slot].owner = self;
    g_guarded[slot].mapping = mapping;
    g_guarded[slot].mapping_size = body + page;
    g_guarded[slot].given = mapping + body - size; /* so the end abuts the dead page */
    g_guarded[slot].real_bits = *bits;
    g_guarded[slot].size = size;

    /* What DXVK already has, so a partial write by the game does not lose the rest. */
    memcpy(g_guarded[slot].given, *bits, size);
    *bits = g_guarded[slot].given;
}

/* Copy back and release, before DXVK is told the lock is over. */
static void guard_unlock_pre(struct nfsu2_d3d9_bridge *self)
{
    int slot;

    if (!guard_locks_enabled())
        return;
    for (slot = 0; slot < MAX_GUARDED_LOCKS; slot++) {
        struct guarded_lock *lock = &g_guarded[slot];

        if (lock->mapping && lock->owner == self) {
            memcpy(lock->real_bits, lock->given, lock->size);
            munmap(lock->mapping, lock->mapping_size);
            memset(lock, 0, sizeof(*lock));
            return;
        }
    }
}

#include "bridge_generated.h"

static const char *iface_name(enum nfsu2_d3d9_iface iface)
{
    if (iface <= NFSU2_D3D9_IFACE_NONE || iface >= NFSU2_D3D9_IFACE_COUNT)
        return "?";
    return g_iface_names[iface];
}

static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_AddRef(struct nfsu2_d3d9_bridge *self)
{
    g_bridge_calls++;
    if (self->retired)
        return bridge_use_after_release(self, "AddRef");
    return ((unsigned (*)(void *))self->real_vtbl[1])(self->real);
}

/*
 * The bridge lives exactly as long as the object it wraps. Freeing it earlier would
 * hand the game a dangling vtable; freeing it later would leak one per object per
 * frame in a game that creates and destroys surfaces constantly. Retirement goes
 * through the quarantine so that a release arriving after the last one is a report
 * rather than a jump into a recycled heap chunk.
 */
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_Release(struct nfsu2_d3d9_bridge *self)
{
    unsigned remaining;

    g_bridge_calls++;
    if (self->retired)
        return bridge_use_after_release(self, "Release");
    remaining = ((unsigned (*)(void *))self->real_vtbl[2])(self->real);
    if (remaining == 0) {
        bridge_forget(self);
        bridge_retire(self);
    }
    return remaining;
}

/*
 * QueryInterface is the one place a bridge cannot be sure what it is handing back.
 * In practice D3D9 code asks an object for itself (the same pointer comes back),
 * which is answerable; anything else would need the IID decoded, and inventing a
 * wrapper of the wrong type would be worse than saying so.
 */
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_QueryInterface(struct nfsu2_d3d9_bridge *self,
                                                          unsigned iid, unsigned out)
{
    unsigned result;
    void **slot = (void **)(uintptr_t)out;

    g_bridge_calls++;
    if (self->retired) {
        if (slot)
            *slot = NULL;
        /* E_NOINTERFACE, which is the truth about an object that no longer exists. */
        bridge_use_after_release(self, "QueryInterface");
        return 0x80004002u;
    }
    result = ((unsigned (*)(void *, unsigned, unsigned))self->real_vtbl[0])
        (self->real, iid, out);
    if (result == 0 && slot && *slot) {
        if (*slot == self->real) {
            *slot = self;
        } else {
            nfsu2_shim_trace("d3d9 bridge: QueryInterface returned a different object "
                             "(%p from a %s) - it is NOT wrapped, and calling through "
                             "it will fault", *slot, g_iface_names[self->iface]);
        }
    }
    return result;
}

/* --- wrap / unwrap -------------------------------------------------------- */

void *nfsu2_d3d9_wrap(void *real, enum nfsu2_d3d9_iface iface)
{
    struct nfsu2_d3d9_bridge *bridge;

    if (!real)
        return NULL;
    if (iface <= NFSU2_D3D9_IFACE_NONE || iface >= NFSU2_D3D9_IFACE_COUNT ||
        !g_vtables[iface]) {
        nfsu2_shim_trace("d3d9 bridge: no vtable for interface %d", (int)iface);
        return real;
    }

    pthread_mutex_lock(&g_bridge_lock);
    bridge = bridge_find_locked(real);
    if (bridge) {
        pthread_mutex_unlock(&g_bridge_lock);
        return bridge;
    }
    pthread_mutex_unlock(&g_bridge_lock);

    bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return real; /* out of memory: better the raw pointer than NULL */
    bridge->real = real;
    bridge->real_vtbl = *(const void *const **)real;
    bridge->iface = iface;
    bridge->lpVtbl = g_vtables[iface];

    pthread_mutex_lock(&g_bridge_lock);
    /* Someone may have wrapped it while we were allocating. */
    {
        struct nfsu2_d3d9_bridge *raced = bridge_find_locked(real);

        if (raced) {
            pthread_mutex_unlock(&g_bridge_lock);
            free(bridge);
            return raced;
        }
        bridge_insert_locked(bridge);
    }
    pthread_mutex_unlock(&g_bridge_lock);

    nfsu2_shim_trace("d3d9 bridge: wrapped %s %p as %p", g_iface_names[iface], real,
                     (void *)bridge);
    return bridge;
}

void *nfsu2_d3d9_unwrap(void *maybe_bridge)
{
    if (!maybe_bridge)
        return NULL;
    /*
     * Reading through the pointer to test it is safe here only because every value
     * that reaches this path is one the game got from us or a plain data pointer it
     * owns; both are readable. The identity map is what makes the answer definite.
     */
    if (is_bridge(maybe_bridge))
        return ((struct nfsu2_d3d9_bridge *)maybe_bridge)->real;
    return maybe_bridge;
}

unsigned int nfsu2_d3d9_bridge_count(void)
{
    return g_bridge_count;
}

/* --- the entry point the game imports ------------------------------------- */

/*
 * Deliberately not called Direct3DCreate9. Naming it that would put it in this
 * binary's dynamic symbol table, where it would shadow DXVK's for *our own* hosts -
 * which call the cdecl entry point directly and correctly. The game gets here
 * through its import table instead: the loader asks nfsu2_d3d9_lookup.
 */
static void *NFSU2_D3D9_THUNK bridged_Direct3DCreate9(unsigned sdk_version)
{
    IDirect3D9 *real = Direct3DCreate9(sdk_version);

    if (!real) {
        nfsu2_shim_trace("d3d9 bridge: Direct3DCreate9(%u) returned NULL", sdk_version);
        return NULL;
    }
    return nfsu2_d3d9_wrap(real, NFSU2_D3D9_IFACE_IDirect3D9);
}

void *nfsu2_d3d9_lookup(const char *name)
{
    if (!name)
        return NULL;
    if (strcmp(name, "Direct3DCreate9") == 0)
        return (void *)bridged_Direct3DCreate9;
    return NULL;
}
