/*
 * dsound.c - DirectSoundCreate and IDirectSound, over SDL2.
 *
 * This file used to answer DSERR_NODRIVER, on the honest grounds that there was no
 * audio implementation and that "no sound card" is a case every Windows game had to
 * handle. That was the right placeholder and it is no longer the truth: buffer.c and
 * mixer.c are the implementation, and this hands out the object that reaches them.
 *
 * The game imports one symbol from dsound.dll - ordinal #1, DirectSoundCreate - and
 * an ordinal import cannot be resolved by name, so the loader consults
 * nfsu2_dsound_lookup through a weak reference. Two definitions of that function
 * exist: this one, and the DSERR_NODRIVER answer in nodriver.c that is linked when
 * SDL2 is not available. This one is strong and wins wherever both are present.
 */
#include "dsound_internal.h"

#include <stdlib.h>
#include <string.h>

struct dsound {
    const IDirectSoundVtbl *vtbl;   /* must be first: this is lpVtbl */
    LONG refs;
    DWORD cooperative_level;
};

static const IDirectSoundVtbl g_dsound_vtbl;

/* --- IUnknown ------------------------------------------------------------- */

static HRESULT WINAPI dsound_QueryInterface(IDirectSound *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectSound)) {
        *out = self;
        IDirectSound_AddRef(self);
        return S_OK;
    }
    /*
     * Notably IID_IDirectSound8: an object from DirectSoundCreate is not one, and
     * saying so is what sends a caller down its DirectSound-7 path rather than
     * letting it call IDirectSound8 methods through a vtable that has none.
     */
    nfsu2_shim_trace("dsound: QueryInterface for an interface this object is not");
    return E_NOINTERFACE;
}

static ULONG WINAPI dsound_AddRef(IDirectSound *self)
{
    struct dsound *ds = (struct dsound *)self;

    return (ULONG)++ds->refs;
}

static ULONG WINAPI dsound_Release(IDirectSound *self)
{
    struct dsound *ds = (struct dsound *)self;
    LONG remaining = --ds->refs;

    if (remaining > 0)
        return (ULONG)remaining;
    /*
     * The device is deliberately left running. Buffers outlive the IDirectSound that
     * made them - a game may release this object and keep playing - and there is no
     * ownership here that says otherwise. nfsu2_dsound_mixer_stop exists for a host
     * that wants to shut down cleanly.
     */
    free(ds);
    return 0;
}

/* --- buffers -------------------------------------------------------------- */

static HRESULT WINAPI dsound_CreateSoundBuffer(IDirectSound *self, LPCDSBUFFERDESC desc,
                                              LPDIRECTSOUNDBUFFER *out, IUnknown *outer)
{
    struct dsound_buffer *buffer;

    (void)self;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (outer)
        return DSERR_NOAGGREGATION;
    if (!desc || desc->dwSize < sizeof(DSBUFFERDESC))
        return DSERR_INVALIDPARAM;

    /*
     * A format the mixer cannot read is refused here rather than accepted and played
     * as noise. DSERR_BADFORMAT is what DirectSound returns for it, and the trace
     * says which format it was, because that is the thing worth knowing.
     */
    if (!(desc->dwFlags & DSBCAPS_PRIMARYBUFFER) && desc->lpwfxFormat &&
        !nfsu2_dsound_format_supported(desc->lpwfxFormat)) {
        nfsu2_shim_trace("dsound CreateSoundBuffer: unsupported format - tag %u, "
                         "%lu Hz, %u channel(s), %u-bit",
                         desc->lpwfxFormat->wFormatTag,
                         (unsigned long)desc->lpwfxFormat->nSamplesPerSec,
                         desc->lpwfxFormat->nChannels,
                         desc->lpwfxFormat->wBitsPerSample);
        return DSERR_BADFORMAT;
    }

    if (nfsu2_dsound_mixer_start() != 0)
        return DSERR_NODRIVER;

    buffer = nfsu2_dsound_buffer_create(desc);
    if (!buffer)
        return DSERR_INVALIDPARAM;

    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER)
        nfsu2_shim_trace("dsound: primary buffer created");
    else
        nfsu2_shim_trace("dsound: buffer %p - %lu bytes, %lu Hz, %u channel(s), "
                         "%u-bit%s", (void *)buffer, (unsigned long)desc->dwBufferBytes,
                         (unsigned long)desc->lpwfxFormat->nSamplesPerSec,
                         desc->lpwfxFormat->nChannels,
                         desc->lpwfxFormat->wBitsPerSample,
                         (desc->dwFlags & DSBCAPS_CTRL3D) ? " (wants 3D)" : "");

    *out = (LPDIRECTSOUNDBUFFER)buffer;
    return DS_OK;
}

static HRESULT WINAPI dsound_DuplicateSoundBuffer(IDirectSound *self,
                                                  LPDIRECTSOUNDBUFFER original,
                                                  LPDIRECTSOUNDBUFFER *out)
{
    struct dsound_buffer *duplicate;

    (void)self;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!original)
        return DSERR_INVALIDPARAM;

    duplicate = nfsu2_dsound_buffer_duplicate((struct dsound_buffer *)original);
    if (!duplicate)
        return DSERR_OUTOFMEMORY;
    *out = (LPDIRECTSOUNDBUFFER)duplicate;
    return DS_OK;
}

/* --- capabilities and configuration -------------------------------------- */

static HRESULT WINAPI dsound_GetCaps(IDirectSound *self, LPDSCAPS caps)
{
    (void)self;
    if (!caps || caps->dwSize < sizeof(*caps))
        return DSERR_INVALIDPARAM;

    memset((char *)caps + sizeof(DWORD), 0, caps->dwSize - sizeof(DWORD));
    /*
     * Everything is mixed in software, so the honest answer is a device with the
     * certified formats and no hardware mixing channels at all. A game that reads
     * dwFreeHwMixingAllBuffers to decide how many hardware voices it can have will
     * correctly conclude none, and use software buffers - which is what these are.
     */
    caps->dwFlags = DSCAPS_CONTINUOUSRATE | DSCAPS_PRIMARY16BIT | DSCAPS_PRIMARYSTEREO |
                    DSCAPS_SECONDARY16BIT | DSCAPS_SECONDARYSTEREO |
                    DSCAPS_PRIMARY8BIT | DSCAPS_SECONDARY8BIT;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = DSBFREQUENCY_MAX;
    caps->dwPrimaryBuffers = 1;
    return DS_OK;
}

static HRESULT WINAPI dsound_SetCooperativeLevel(IDirectSound *self, HWND window, DWORD level)
{
    struct dsound *ds = (struct dsound *)self;

    (void)window;
    /*
     * On Windows this decides whether the application may change the primary
     * buffer's format and what happens to its audio when it loses focus. Neither
     * applies: there is one process, one device, and SDL keeps playing. Recorded
     * because GetCooperativeLevel does not exist for a caller to read it back.
     */
    ds->cooperative_level = level;
    return DS_OK;
}

static HRESULT WINAPI dsound_Compact(IDirectSound *self)
{
    (void)self;
    /* Defragmenting on-board sound memory. There is none. */
    return DS_OK;
}

static HRESULT WINAPI dsound_GetSpeakerConfig(IDirectSound *self, LPDWORD config)
{
    (void)self;
    if (!config)
        return DSERR_INVALIDPARAM;
    *config = DSSPEAKER_STEREO;
    return DS_OK;
}

static HRESULT WINAPI dsound_SetSpeakerConfig(IDirectSound *self, DWORD config)
{
    (void)self;
    /* Accepted and ignored: the mixer is stereo, and a caller asking for something
     * else is asking about the user's speakers rather than about this buffer. */
    nfsu2_shim_trace("dsound SetSpeakerConfig(0x%lx): the mixer is stereo",
                     (unsigned long)config);
    return DS_OK;
}

static HRESULT WINAPI dsound_Initialize(IDirectSound *self, LPCGUID device)
{
    (void)self; (void)device;
    /* DirectSoundCreate has already done it. */
    return DSERR_ALREADYINITIALIZED;
}

static const IDirectSoundVtbl g_dsound_vtbl = {
    dsound_QueryInterface,
    dsound_AddRef,
    dsound_Release,
    dsound_CreateSoundBuffer,
    dsound_GetCaps,
    dsound_DuplicateSoundBuffer,
    dsound_SetCooperativeLevel,
    dsound_Compact,
    dsound_GetSpeakerConfig,
    dsound_SetSpeakerConfig,
    dsound_Initialize,
};

/* --- the entry point ------------------------------------------------------ */

HRESULT WINAPI DirectSoundCreate(const GUID *device, IDirectSound **out, IUnknown *outer)
{
    struct dsound *ds;

    (void)device;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (outer)
        return DSERR_NOAGGREGATION;

    /*
     * The device is opened here rather than lazily, so that a machine with no
     * working audio reports DSERR_NODRIVER from the call that asks for audio - which
     * is the answer this file used to give unconditionally, and which the game
     * handles as a machine without a sound card.
     */
    if (nfsu2_dsound_mixer_start() != 0)
        return DSERR_NODRIVER;

    ds = calloc(1, sizeof(*ds));
    if (!ds)
        return DSERR_OUTOFMEMORY;
    ds->vtbl = &g_dsound_vtbl;
    ds->refs = 1;

    nfsu2_shim_trace("DirectSoundCreate: mixing at %d Hz", nfsu2_dsound_mixer_rate());
    *out = (IDirectSound *)ds;
    return DS_OK;
}

/*
 * dsound.dll's exports are numbered and this game imports #1 with no name, so the
 * loader cannot resolve it by symbol - the same situation as ws2_32 and the same
 * answer: an explicit table, consulted through a weak reference. Only the ordinals
 * actually imported are listed; a missing one is reported as unresolved, which is a
 * true statement about this shim.
 */
void *nfsu2_dsound_lookup(const char *name, unsigned int ordinal)
{
    if (ordinal == 1 || (name && strcmp(name, "DirectSoundCreate") == 0))
        return (void *)DirectSoundCreate;
    return NULL;
}
