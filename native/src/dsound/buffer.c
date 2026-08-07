/*
 * buffer.c - IDirectSoundBuffer.
 *
 * A DirectSound buffer is a circular block of PCM plus a play cursor. The game
 * either fills it once and plays it, or streams into it while it loops - and the
 * second case is why Lock has to hand back two pointers: a region that wraps past
 * the end of the buffer is two runs of memory, and a caller that expects one is a
 * caller that overruns.
 *
 * Nothing here decides *when* audio advances. The mixer does, on the audio device's
 * callback, which is what makes GetCurrentPosition mean anything.
 */
#include "dsound_internal.h"

#include <stdlib.h>
#include <string.h>

static const IDirectSoundBufferVtbl g_buffer_vtbl;
static const IDirectSound3DBufferVtbl g_buffer3d_vtbl;

DWORD nfsu2_dsound_buffer_frames(const struct dsound_buffer *buffer)
{
    if (!buffer->format.nBlockAlign)
        return 0;
    return buffer->samples->bytes / buffer->format.nBlockAlign;
}

int nfsu2_dsound_format_supported(const WAVEFORMATEX *format)
{
    if (!format)
        return 0;
    if (format->wFormatTag != WAVE_FORMAT_PCM)
        return 0;
    if (format->wBitsPerSample != 8 && format->wBitsPerSample != 16)
        return 0;
    if (format->nChannels != 1 && format->nChannels != 2)
        return 0;
    if (!format->nSamplesPerSec || !format->nBlockAlign)
        return 0;
    return 1;
}

/* --- construction --------------------------------------------------------- */

static struct dsound_samples *samples_create(DWORD bytes)
{
    struct dsound_samples *samples = calloc(1, sizeof(*samples));

    if (!samples)
        return NULL;
    /* Zeroed, which for both supported formats is not silence for 8-bit (128 is) -
     * but a buffer the game has not written yet is a buffer it has not played yet,
     * and DirectSound does not promise its contents either. */
    samples->data = calloc(1, bytes ? bytes : 1);
    if (!samples->data) {
        free(samples);
        return NULL;
    }
    samples->bytes = bytes;
    samples->refs = 1;
    return samples;
}

static void samples_release(struct dsound_samples *samples)
{
    if (!samples || --samples->refs > 0)
        return;
    free(samples->data);
    free(samples);
}

struct dsound_buffer *nfsu2_dsound_buffer_create(const DSBUFFERDESC *desc)
{
    struct dsound_buffer *buffer;
    int primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;

    if (!primary) {
        if (!desc->lpwfxFormat || !nfsu2_dsound_format_supported(desc->lpwfxFormat))
            return NULL;
        if (!desc->dwBufferBytes)
            return NULL;
    }

    buffer = calloc(1, sizeof(*buffer));
    if (!buffer)
        return NULL;

    buffer->vtbl = &g_buffer_vtbl;
    buffer->iface3d.vtbl = &g_buffer3d_vtbl;
    buffer->refs = 1;
    buffer->caps_flags = desc->dwFlags;
    buffer->primary = primary;
    buffer->volume = 0;      /* DSBVOLUME_MAX: full scale */
    buffer->pan = 0;
    buffer->frequency = 0;   /* the format's own rate */

    if (primary) {
        /*
         * The primary buffer is the mix destination, and a game creates one to
         * describe the output format rather than to put samples in. It carries no
         * PCM of its own here and is never mixed as a source.
         */
        buffer->format.wFormatTag = WAVE_FORMAT_PCM;
        buffer->format.nChannels = 2;
        buffer->format.nSamplesPerSec = (DWORD)nfsu2_dsound_mixer_rate();
        buffer->format.wBitsPerSample = 16;
        buffer->format.nBlockAlign = 4;
        buffer->format.nAvgBytesPerSec = buffer->format.nSamplesPerSec * 4;
        buffer->samples = samples_create(0);
    } else {
        buffer->format = *desc->lpwfxFormat;
        buffer->format.cbSize = 0; /* no extra format bytes are kept */
        buffer->samples = samples_create(desc->dwBufferBytes);
    }
    if (!buffer->samples) {
        free(buffer);
        return NULL;
    }

    nfsu2_dsound_mixer_add(buffer);
    return buffer;
}

/*
 * A duplicate shares the original's PCM and gets its own cursor and its own volume,
 * pan and frequency - which is exactly what a game wants when it plays one recorded
 * sound several times at once.
 */
struct dsound_buffer *nfsu2_dsound_buffer_duplicate(struct dsound_buffer *original)
{
    struct dsound_buffer *buffer = calloc(1, sizeof(*buffer));

    if (!buffer)
        return NULL;

    buffer->vtbl = &g_buffer_vtbl;
    buffer->iface3d.vtbl = &g_buffer3d_vtbl;
    buffer->refs = 1;
    buffer->caps_flags = original->caps_flags;
    buffer->primary = original->primary;
    buffer->format = original->format;
    buffer->volume = original->volume;
    buffer->pan = original->pan;
    buffer->frequency = original->frequency;

    nfsu2_dsound_mixer_lock();
    buffer->samples = original->samples;
    buffer->samples->refs++;
    nfsu2_dsound_mixer_unlock();

    nfsu2_dsound_mixer_add(buffer);
    return buffer;
}

/* --- IUnknown ------------------------------------------------------------- */

static struct dsound_buffer *buffer_from_3d(IDirectSound3DBuffer *self)
{
    return (struct dsound_buffer *)((char *)self - offsetof(struct dsound_buffer, iface3d));
}

static HRESULT WINAPI buffer_QueryInterface(IDirectSoundBuffer *self, REFIID riid, void **out)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!out)
        return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectSoundBuffer)) {
        *out = self;
        IDirectSoundBuffer_AddRef(self);
        return S_OK;
    }

    /*
     * Positional audio. Reported rather than silently refused, because a racing game
     * that wants 3D and cannot have it is a game whose engine and traffic are all at
     * the same place - and that is worth knowing about from a trace rather than
     * guessing at from the sound.
     */
    if (IsEqualGUID(riid, &IID_IDirectSound3DBuffer)) {
        nfsu2_shim_trace("dsound: buffer %p asked for IDirectSound3DBuffer - "
                         "positional audio is not implemented, answering "
                         "E_NOINTERFACE so the caller can fall back to pan",
                         (void *)buffer);
        return E_NOINTERFACE;
    }
    if (IsEqualGUID(riid, &IID_IDirectSoundNotify)) {
        nfsu2_shim_trace("dsound: buffer %p asked for IDirectSoundNotify - position "
                         "notifications are not implemented", (void *)buffer);
        return E_NOINTERFACE;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI buffer_AddRef(IDirectSoundBuffer *self)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    return (ULONG)++buffer->refs;
}

static ULONG WINAPI buffer_Release(IDirectSoundBuffer *self)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;
    LONG remaining = --buffer->refs;

    if (remaining > 0)
        return (ULONG)remaining;

    if (buffer->mixed_frames)
        nfsu2_shim_trace("dsound: buffer %p released after %lu frame(s) mixed",
                         (void *)buffer, buffer->mixed_frames);
    /* Off the mixer's list first: after this the callback cannot reach it. */
    nfsu2_dsound_mixer_remove(buffer);
    nfsu2_dsound_mixer_lock();
    samples_release(buffer->samples);
    nfsu2_dsound_mixer_unlock();
    free(buffer);
    return 0;
}

/* --- state ---------------------------------------------------------------- */

static HRESULT WINAPI buffer_GetCaps(IDirectSoundBuffer *self, LPDSBCAPS caps)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!caps || caps->dwSize < sizeof(*caps))
        return DSERR_INVALIDPARAM;
    caps->dwFlags = buffer->caps_flags;
    caps->dwBufferBytes = buffer->samples->bytes;
    /* Both are documented as meaningful only for hardware-mixed buffers. */
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

/*
 * Where the play cursor is, in bytes. Caller holds the mixer lock.
 */
static DWORD play_bytes_locked(const struct dsound_buffer *buffer)
{
    DWORD position = (DWORD)buffer->play_cursor * buffer->format.nBlockAlign;

    if (buffer->samples->bytes && position >= buffer->samples->bytes)
        position = buffer->samples->bytes - buffer->format.nBlockAlign;
    return position;
}

/*
 * The write cursor: the first byte it is safe to write, which DirectSound places
 * *ahead* of the play cursor by however much the driver has already committed.
 *
 * This being equal to the play cursor is not a harmless simplification, and it was
 * the whole reason this game stayed silent with a correct-looking mixer. A streaming
 * engine computes its writable region as everything that is not committed:
 *
 *     available = (play - write + size) % size
 *
 * With write == play that is exactly zero, every time it asks, forever. The game
 * armed its pump, the pump ran, it read the cursors, concluded there was no room, and
 * wrote nothing - while the buffer looped over the silence it had been cleared to.
 *
 * The lead comes from the mixer rather than a constant, because there it is a real
 * quantity: whatever the next audio callback will consume.
 */
static DWORD write_bytes_locked(const struct dsound_buffer *buffer)
{
    DWORD size = buffer->samples->bytes;
    DWORD lead;

    if (!size || !buffer->format.nSamplesPerSec)
        return 0;
    lead = (DWORD)((unsigned long long)buffer->format.nSamplesPerSec *
                   (unsigned long long)nfsu2_dsound_mixer_commit_ms() / 1000ull) *
           buffer->format.nBlockAlign;
    /* A lead as long as the buffer would leave nothing writable, which is the bug
     * this function exists to avoid; keep it to at most half. */
    if (lead > size / 2)
        lead = size / 2;
    lead -= lead % buffer->format.nBlockAlign;
    return (play_bytes_locked(buffer) + lead) % size;
}

static HRESULT WINAPI buffer_GetCurrentPosition(IDirectSoundBuffer *self, LPDWORD play,
                                                LPDWORD write)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    nfsu2_dsound_mixer_lock();
    if (play)
        *play = play_bytes_locked(buffer);
    if (write)
        *write = write_bytes_locked(buffer);
    nfsu2_dsound_mixer_unlock();
    return DS_OK;
}

static HRESULT WINAPI buffer_GetFormat(IDirectSoundBuffer *self, LPWAVEFORMATEX format,
                                       DWORD allocated, LPDWORD written)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;
    DWORD needed = sizeof(WAVEFORMATEX);

    if (!format) {
        /* Size query. */
        if (written)
            *written = needed;
        return DS_OK;
    }
    if (allocated < needed)
        return DSERR_INVALIDPARAM;
    memcpy(format, &buffer->format, needed);
    if (written)
        *written = needed;
    return DS_OK;
}

static HRESULT WINAPI buffer_GetVolume(IDirectSoundBuffer *self, LPLONG volume)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!volume)
        return DSERR_INVALIDPARAM;
    *volume = buffer->volume;
    return DS_OK;
}

static HRESULT WINAPI buffer_GetPan(IDirectSoundBuffer *self, LPLONG pan)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!pan)
        return DSERR_INVALIDPARAM;
    *pan = buffer->pan;
    return DS_OK;
}

static HRESULT WINAPI buffer_GetFrequency(IDirectSoundBuffer *self, LPDWORD frequency)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!frequency)
        return DSERR_INVALIDPARAM;
    *frequency = buffer->frequency ? buffer->frequency : buffer->format.nSamplesPerSec;
    return DS_OK;
}

static HRESULT WINAPI buffer_GetStatus(IDirectSoundBuffer *self, LPDWORD status)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!status)
        return DSERR_INVALIDPARAM;
    nfsu2_dsound_mixer_lock();
    *status = 0;
    if (buffer->playing)
        *status |= DSBSTATUS_PLAYING;
    if (buffer->looping)
        *status |= DSBSTATUS_LOOPING;
    nfsu2_dsound_mixer_unlock();
    /* Never DSBSTATUS_BUFFERLOST: nothing here can lose a buffer, so Restore has
     * nothing to do and a caller that checks will never be told to call it. */
    return DS_OK;
}

static HRESULT WINAPI buffer_Initialize(IDirectSoundBuffer *self, LPDIRECTSOUND ds,
                                        LPCDSBUFFERDESC desc)
{
    (void)self; (void)ds; (void)desc;
    /* Only meaningful for a buffer created by CoCreateInstance, which is not a route
     * anything here offers. DirectSound returns this for an already-initialised one. */
    return DSERR_ALREADYINITIALIZED;
}

/* --- the circular part ---------------------------------------------------- */

static HRESULT WINAPI buffer_Lock(IDirectSoundBuffer *self, DWORD offset, DWORD bytes,
                                  LPVOID *ptr1, LPDWORD bytes1, LPVOID *ptr2,
                                  LPDWORD bytes2, DWORD flags)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;
    DWORD size = buffer->samples->bytes;
    DWORD first;

    if (!ptr1 || !bytes1)
        return DSERR_INVALIDPARAM;
    *ptr1 = NULL;
    *bytes1 = 0;
    if (ptr2)
        *ptr2 = NULL;
    if (bytes2)
        *bytes2 = 0;
    if (!size)
        return DSERR_INVALIDPARAM;

    if (flags & DSBLOCK_FROMWRITECURSOR) {
        /* The real write cursor, derived from the play cursor - not a record of
         * where the last Unlock finished. DirectSound has no such state: its write
         * cursor moves with the audio, whatever the application has written. */
        nfsu2_dsound_mixer_lock();
        offset = write_bytes_locked(buffer);
        nfsu2_dsound_mixer_unlock();
    }
    if (flags & DSBLOCK_ENTIREBUFFER)
        bytes = size;
    if (offset >= size)
        return DSERR_INVALIDPARAM;
    if (bytes > size)
        bytes = size;

    /*
     * The whole point of the two-pointer signature: a region that runs off the end
     * continues at the start, and the caller is told about both halves. Returning
     * one pointer and the full length would invite a write past the buffer.
     */
    first = size - offset;
    if (first > bytes)
        first = bytes;

    *ptr1 = buffer->samples->data + offset;
    *bytes1 = first;
    if (bytes > first && ptr2 && bytes2) {
        *ptr2 = buffer->samples->data;
        *bytes2 = bytes - first;
    }

    if (!buffer->traced_lock) {
        buffer->traced_lock = 1;
        nfsu2_shim_trace("dsound: buffer %p written through Lock (offset %lu, %lu "
                         "bytes%s) - the game is filling it",
                         (void *)buffer, (unsigned long)offset, (unsigned long)bytes,
                         (flags & DSBLOCK_FROMWRITECURSOR) ? ", from write cursor" : "");
    }
    buffer->locked = 1;
    return DS_OK;
}

static HRESULT WINAPI buffer_Unlock(IDirectSoundBuffer *self, LPVOID ptr1, DWORD bytes1,
                                    LPVOID ptr2, DWORD bytes2)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    (void)ptr1; (void)ptr2;
    /*
     * There is nothing to copy back - Lock handed out the real memory - and nothing
     * to advance either: the write cursor is derived from the play cursor, so it is
     * the audio device that moves it, not the act of writing. All that is left is to
     * notice an Unlock without a Lock.
     */
    (void)bytes1; (void)bytes2;
    if (!buffer->locked)
        return DSERR_INVALIDPARAM;
    buffer->locked = 0;
    return DS_OK;
}

/* --- transport ------------------------------------------------------------ */

static HRESULT WINAPI buffer_Play(IDirectSoundBuffer *self, DWORD reserved1,
                                  DWORD priority, DWORD flags)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    (void)reserved1; (void)priority;
    nfsu2_dsound_mixer_lock();
    buffer->playing = 1;
    buffer->looping = (flags & DSBPLAY_LOOPING) != 0;
    nfsu2_dsound_mixer_unlock();
    nfsu2_shim_trace("dsound: buffer %p playing%s", (void *)buffer,
                     (flags & DSBPLAY_LOOPING) ? " (looping)" : "");
    return DS_OK;
}

static HRESULT WINAPI buffer_Stop(IDirectSoundBuffer *self)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    nfsu2_dsound_mixer_lock();
    /* Stop leaves the cursor where it is; only SetCurrentPosition moves it. A
     * caller that stops and plays again expects to continue. */
    buffer->playing = 0;
    nfsu2_dsound_mixer_unlock();
    nfsu2_shim_trace("dsound: buffer %p stopped after %lu frame(s) mixed",
                     (void *)buffer, buffer->mixed_frames);
    return DS_OK;
}

static HRESULT WINAPI buffer_SetCurrentPosition(IDirectSoundBuffer *self, DWORD position)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (!buffer->format.nBlockAlign)
        return DSERR_INVALIDPARAM;
    if (buffer->samples->bytes && position >= buffer->samples->bytes)
        return DSERR_INVALIDPARAM;
    nfsu2_dsound_mixer_lock();
    buffer->play_cursor = (double)(position / buffer->format.nBlockAlign);
    nfsu2_dsound_mixer_unlock();
    return DS_OK;
}

static HRESULT WINAPI buffer_SetFormat(IDirectSoundBuffer *self, LPCWAVEFORMATEX format)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    /* Only the primary buffer's format may be set, which is what DirectSound says
     * and what a game uses this for: describing the output mix. */
    if (!buffer->primary)
        return DSERR_INVALIDCALL;
    if (!format)
        return DSERR_INVALIDPARAM;
    nfsu2_shim_trace("dsound: primary format set to %lu Hz, %u channel(s), %u-bit - "
                     "the mixer stays at %d Hz and converts",
                     (unsigned long)format->nSamplesPerSec, format->nChannels,
                     format->wBitsPerSample, nfsu2_dsound_mixer_rate());
    return DS_OK;
}

static HRESULT WINAPI buffer_SetVolume(IDirectSoundBuffer *self, LONG volume)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (volume > DSBVOLUME_MAX || volume < DSBVOLUME_MIN)
        return DSERR_INVALIDPARAM;
    nfsu2_dsound_mixer_lock();
    buffer->volume = volume;
    nfsu2_dsound_mixer_unlock();
    return DS_OK;
}

static HRESULT WINAPI buffer_SetPan(IDirectSoundBuffer *self, LONG pan)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    if (pan > DSBPAN_RIGHT || pan < DSBPAN_LEFT)
        return DSERR_INVALIDPARAM;
    nfsu2_dsound_mixer_lock();
    buffer->pan = pan;
    nfsu2_dsound_mixer_unlock();
    return DS_OK;
}

static HRESULT WINAPI buffer_SetFrequency(IDirectSoundBuffer *self, DWORD frequency)
{
    struct dsound_buffer *buffer = (struct dsound_buffer *)self;

    /* Zero means "back to the format's own rate", which is how a game undoes a
     * pitch shift. The range is DirectSound's documented one. */
    if (frequency && (frequency < DSBFREQUENCY_MIN || frequency > DSBFREQUENCY_MAX))
        return DSERR_INVALIDPARAM;
    nfsu2_dsound_mixer_lock();
    buffer->frequency = frequency;
    nfsu2_dsound_mixer_unlock();
    return DS_OK;
}

static HRESULT WINAPI buffer_Restore(IDirectSoundBuffer *self)
{
    (void)self;
    /* Nothing here can lose a buffer's memory, so there is never anything to
     * restore and saying so is the truth rather than a stub. */
    return DS_OK;
}

static const IDirectSoundBufferVtbl g_buffer_vtbl = {
    buffer_QueryInterface,
    buffer_AddRef,
    buffer_Release,
    buffer_GetCaps,
    buffer_GetCurrentPosition,
    buffer_GetFormat,
    buffer_GetVolume,
    buffer_GetPan,
    buffer_GetFrequency,
    buffer_GetStatus,
    buffer_Initialize,
    buffer_Lock,
    buffer_Play,
    buffer_SetCurrentPosition,
    buffer_SetFormat,
    buffer_SetVolume,
    buffer_SetPan,
    buffer_SetFrequency,
    buffer_Stop,
    buffer_Unlock,
    buffer_Restore,
};

/* --- IDirectSound3DBuffer ------------------------------------------------- */

/*
 * Declared but not offered: QueryInterface answers E_NOINTERFACE for
 * IID_IDirectSound3DBuffer, so nothing can reach these. The vtable exists so that
 * implementing positional audio is filling these in rather than restructuring the
 * buffer, and so that the sub-object's offset is real and testable now.
 */
static HRESULT WINAPI buffer3d_QueryInterface(IDirectSound3DBuffer *self, REFIID riid,
                                              void **out)
{
    return buffer_QueryInterface((IDirectSoundBuffer *)buffer_from_3d(self), riid, out);
}

static ULONG WINAPI buffer3d_AddRef(IDirectSound3DBuffer *self)
{
    return buffer_AddRef((IDirectSoundBuffer *)buffer_from_3d(self));
}

static ULONG WINAPI buffer3d_Release(IDirectSound3DBuffer *self)
{
    return buffer_Release((IDirectSoundBuffer *)buffer_from_3d(self));
}

#define NFSU2_DS3D_UNIMPLEMENTED(name, signature, args)                          \
    static HRESULT WINAPI name signature                                         \
    {                                                                            \
        (void)self; args;                                                        \
        NFSU2_STUB("dsound " #name);                                             \
        return DSERR_UNSUPPORTED;                                                \
    }

NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetAllParameters,
    (IDirectSound3DBuffer *self, LPDS3DBUFFER p), (void)p)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetConeAngles,
    (IDirectSound3DBuffer *self, LPDWORD a, LPDWORD b), (void)a; (void)b)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetConeOrientation,
    (IDirectSound3DBuffer *self, LPD3DVECTOR v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetConeOutsideVolume,
    (IDirectSound3DBuffer *self, LPLONG v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetMaxDistance,
    (IDirectSound3DBuffer *self, D3DVALUE *v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetMinDistance,
    (IDirectSound3DBuffer *self, D3DVALUE *v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetMode,
    (IDirectSound3DBuffer *self, LPDWORD v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetPosition,
    (IDirectSound3DBuffer *self, LPD3DVECTOR v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_GetVelocity,
    (IDirectSound3DBuffer *self, LPD3DVECTOR v), (void)v)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetAllParameters,
    (IDirectSound3DBuffer *self, LPCDS3DBUFFER p, DWORD a), (void)p; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetConeAngles,
    (IDirectSound3DBuffer *self, DWORD a, DWORD b, DWORD c), (void)a; (void)b; (void)c)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetConeOrientation,
    (IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD a),
    (void)x; (void)y; (void)z; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetConeOutsideVolume,
    (IDirectSound3DBuffer *self, LONG v, DWORD a), (void)v; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetMaxDistance,
    (IDirectSound3DBuffer *self, D3DVALUE v, DWORD a), (void)v; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetMinDistance,
    (IDirectSound3DBuffer *self, D3DVALUE v, DWORD a), (void)v; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetMode,
    (IDirectSound3DBuffer *self, DWORD m, DWORD a), (void)m; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetPosition,
    (IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD a),
    (void)x; (void)y; (void)z; (void)a)
NFSU2_DS3D_UNIMPLEMENTED(buffer3d_SetVelocity,
    (IDirectSound3DBuffer *self, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD a),
    (void)x; (void)y; (void)z; (void)a)

static const IDirectSound3DBufferVtbl g_buffer3d_vtbl = {
    buffer3d_QueryInterface,
    buffer3d_AddRef,
    buffer3d_Release,
    buffer3d_GetAllParameters,
    buffer3d_GetConeAngles,
    buffer3d_GetConeOrientation,
    buffer3d_GetConeOutsideVolume,
    buffer3d_GetMaxDistance,
    buffer3d_GetMinDistance,
    buffer3d_GetMode,
    buffer3d_GetPosition,
    buffer3d_GetVelocity,
    buffer3d_SetAllParameters,
    buffer3d_SetConeAngles,
    buffer3d_SetConeOrientation,
    buffer3d_SetConeOutsideVolume,
    buffer3d_SetMaxDistance,
    buffer3d_SetMinDistance,
    buffer3d_SetMode,
    buffer3d_SetPosition,
    buffer3d_SetVelocity,
};
