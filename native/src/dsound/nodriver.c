/*
 * nodriver.c - DirectSound when there is no mixer to reach.
 *
 * The real implementation (dsound.c, buffer.c, mixer.c) needs SDL2, and this tree
 * builds without it - see the sdl2.found() branch in meson.build. This file is what
 * a build like that links instead, and what it says is true of it: there is no audio
 * driver here.
 *
 * DSERR_NODRIVER is not an invention. It is a documented outcome that every Windows
 * game had to handle, because a machine with no sound card returns it, and this game
 * takes the path it already has for that case. Nothing here pretends to produce
 * sound, and nothing invents a device that would then be asked to play something.
 *
 * Both symbols are weak, so linking the SDL2-backed library overrides them with the
 * real thing. Without them, the game's only dsound import - ordinal #1 - would stay
 * unresolved, and its own `jmp [0x78302c]` thunk would jump to what the file left in
 * that slot: IMAGE_ORDINAL_FLAG | 1, or 0x80000001.
 */
#include "../win32/shim_internal.h"

#include <string.h>

/* From dsound.h. Spelled out rather than included, because this file implements no
 * interfaces and pulling in the COM machinery for one constant is not worth it.
 *
 *   0x88780078 = DSERR_NODRIVER, "no sound driver is available for use"
 */
#define NFSU2_DSERR_NODRIVER 0x88780078u

__attribute__((weak))
HRESULT WINAPI DirectSoundCreate(const GUID *device, void **out, void *outer)
{
    (void)device; (void)outer;

    if (out)
        *out = NULL;
    nfsu2_shim_trace("DirectSoundCreate: this build has no audio backend - answering "
                     "DSERR_NODRIVER, which the game handles as a machine without a "
                     "sound card");
    return (HRESULT)NFSU2_DSERR_NODRIVER;
}

__attribute__((weak))
void *nfsu2_dsound_lookup(const char *name, unsigned int ordinal)
{
    if (ordinal == 1 || (name && strcmp(name, "DirectSoundCreate") == 0))
        return (void *)DirectSoundCreate;
    return NULL;
}
