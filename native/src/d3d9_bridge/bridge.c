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
};

/* --- the identity map ----------------------------------------------------- */

/*
 * One bridge per real object, and it matters: `GetBackBuffer` called twice returns
 * the same surface, and a game that caches interface pointers compares them. Two
 * bridges for one object would also mean two independent refcount views of it.
 *
 * Open-addressed, locked, and generous - a frame's worth of D3D9 objects is
 * hundreds, not thousands.
 */
#define BRIDGE_SLOTS 2048

static struct nfsu2_d3d9_bridge *g_bridges[BRIDGE_SLOTS];
static unsigned int g_bridge_count;
static unsigned int g_bridge_calls;
static pthread_mutex_t g_bridge_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t bridge_slot(const void *real)
{
    uintptr_t value = (uintptr_t)real >> 4;

    value *= 2654435761u;
    return (size_t)(value & (BRIDGE_SLOTS - 1));
}

static struct nfsu2_d3d9_bridge *bridge_find_locked(const void *real)
{
    size_t start = bridge_slot(real);
    size_t probe;

    for (probe = 0; probe < BRIDGE_SLOTS; probe++) {
        size_t index = (start + probe) & (BRIDGE_SLOTS - 1);

        if (!g_bridges[index])
            return NULL;
        if (g_bridges[index]->real == real)
            return g_bridges[index];
    }
    return NULL;
}

static void bridge_insert_locked(struct nfsu2_d3d9_bridge *bridge)
{
    size_t start = bridge_slot(bridge->real);
    size_t probe;

    for (probe = 0; probe < BRIDGE_SLOTS; probe++) {
        size_t index = (start + probe) & (BRIDGE_SLOTS - 1);

        if (!g_bridges[index]) {
            g_bridges[index] = bridge;
            g_bridge_count++;
            return;
        }
    }
    nfsu2_shim_trace("d3d9 bridge table is full (%d) - not tracking %p",
                     BRIDGE_SLOTS, bridge->real);
}

static void bridge_forget(struct nfsu2_d3d9_bridge *bridge)
{
    size_t start = bridge_slot(bridge->real);
    size_t probe;

    pthread_mutex_lock(&g_bridge_lock);
    for (probe = 0; probe < BRIDGE_SLOTS; probe++) {
        size_t index = (start + probe) & (BRIDGE_SLOTS - 1);

        if (!g_bridges[index])
            break;
        if (g_bridges[index] == bridge) {
            g_bridges[index] = NULL;
            g_bridge_count--;
            /* Re-place the rest of the cluster, since linear probing cannot see
             * past a hole. */
            for (probe = probe + 1; probe < BRIDGE_SLOTS; probe++) {
                size_t next = (start + probe) & (BRIDGE_SLOTS - 1);
                struct nfsu2_d3d9_bridge *moved = g_bridges[next];

                if (!moved)
                    break;
                g_bridges[next] = NULL;
                g_bridge_count--;
                bridge_insert_locked(moved);
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_bridge_lock);
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

#include "bridge_generated.h"

static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_AddRef(struct nfsu2_d3d9_bridge *self)
{
    g_bridge_calls++;
    return ((unsigned (*)(void *))self->real_vtbl[1])(self->real);
}

/*
 * The bridge lives exactly as long as the object it wraps. Freeing it earlier would
 * hand the game a dangling vtable; freeing it later would leak one per object per
 * frame in a game that creates and destroys surfaces constantly.
 */
static unsigned NFSU2_D3D9_THUNK nfsu2_d3d9_Release(struct nfsu2_d3d9_bridge *self)
{
    unsigned remaining;

    g_bridge_calls++;
    remaining = ((unsigned (*)(void *))self->real_vtbl[2])(self->real);
    if (remaining == 0) {
        bridge_forget(self);
        free(self);
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
