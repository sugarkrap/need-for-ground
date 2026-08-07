/*
 * dsound_internal.h - shared internals of the SDL2-backed DirectSound shim.
 *
 * The game imports exactly one thing from dsound.dll - ordinal #1, which is
 * DirectSoundCreate - so this is DirectSound, not DirectSound8, and everything
 * reached from here is reached through that object's vtables.
 *
 * Like dinput8 next door, we *implement* these interfaces rather than wrap someone
 * else's, so the vtables are ordinary WINAPI (stdcall on i386) and there is no
 * calling-convention accommodation to make. Contrast d3d9_bridge, which exists
 * entirely because DXVK's vtables are cdecl.
 */
#ifndef NFSU2_DSOUND_INTERNAL_H
#define NFSU2_DSOUND_INTERNAL_H

#include "../win32/shim_internal.h"

#include <dsound.h>

#include <SDL2/SDL.h>

/*
 * PCM shared between a buffer and its duplicates. DuplicateSoundBuffer is how a
 * game plays the same sound several times at once, and it is documented to share
 * the original's memory - so a duplicate costs a refcount here, not a copy. A
 * racing game duplicates constantly.
 */
struct dsound_samples {
    unsigned char *data;
    DWORD bytes;
    int refs;
};

/*
 * One IDirectSoundBuffer. The 3D interface is a separate sub-object with its own
 * vtable pointer, because QueryInterface has to hand back something whose first
 * word is the 3D vtable; buffer_from_3d() gets back to the container.
 */
struct dsound_buffer {
    const IDirectSoundBufferVtbl *vtbl;   /* must be first: this is lpVtbl */
    struct { const IDirectSound3DBufferVtbl *vtbl; } iface3d;

    LONG refs;
    struct dsound_samples *samples;
    WAVEFORMATEX format;
    DWORD caps_flags;                     /* DSBCAPS_* as the descriptor asked */
    int primary;

    /*
     * The play cursor is kept in fractional *frames* rather than bytes, because
     * the mixer advances it by the ratio between this buffer's sample rate and the
     * device's and that ratio is not a whole number. GetCurrentPosition converts.
     */
    double play_cursor;
    int playing;
    int looping;

    LONG volume;                          /* hundredths of a dB, 0 = full */
    LONG pan;                             /* -10000 left .. 10000 right */
    DWORD frequency;                      /* 0 means "the format's own rate" */

    /* Set once Lock has handed out pointers, so Unlock can be checked. */
    int locked;
    /* Traced once, so "the game is writing audio into this" is answerable without
     * a line per Lock in a streaming loop that runs every few milliseconds. */
    int traced_lock;

    /*
     * How many frames the mixer has actually taken from this buffer. Counted in the
     * audio callback and reported from Stop and Release, because it is the only
     * evidence that audio reached the device that does not involve listening - and
     * because a game whose sound is silent for some other reason (its own mix muted,
     * nothing written into the buffer) looks identical from the outside. Nothing is
     * printed from the callback: that would be file I/O on the audio thread.
     */
    unsigned long mixed_frames;
};

/* mixer.c: the SDL audio device and the mixing callback. */
int nfsu2_dsound_mixer_start(void);
void nfsu2_dsound_mixer_stop(void);
void nfsu2_dsound_mixer_add(struct dsound_buffer *buffer);
void nfsu2_dsound_mixer_remove(struct dsound_buffer *buffer);
/* Held around any read or write of a buffer's play state, including by the audio
 * callback while it mixes. */
void nfsu2_dsound_mixer_lock(void);
void nfsu2_dsound_mixer_unlock(void);
/* What the device is running at, for diagnostics. */
int nfsu2_dsound_mixer_rate(void);
/* How far ahead of the play cursor is already committed, in milliseconds - which is
 * what the DirectSound write cursor reports. */
int nfsu2_dsound_mixer_commit_ms(void);

/* buffer.c */
struct dsound_buffer *nfsu2_dsound_buffer_create(const DSBUFFERDESC *desc);
struct dsound_buffer *nfsu2_dsound_buffer_duplicate(struct dsound_buffer *original);
DWORD nfsu2_dsound_buffer_frames(const struct dsound_buffer *buffer);

/* Is this format one the mixer can read? 8-bit unsigned or 16-bit signed PCM,
 * mono or stereo - which is all DirectSound secondary buffers ever were. */
int nfsu2_dsound_format_supported(const WAVEFORMATEX *format);

#endif /* NFSU2_DSOUND_INTERNAL_H */
