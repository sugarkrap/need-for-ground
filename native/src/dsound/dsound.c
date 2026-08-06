/*
 * dsound.c - DirectSound, answered honestly: there is no driver.
 *
 * Audio is the one genuine platform gap in this port (see the gap list in
 * ../../README.md), and this file does not close it. What it does is stop the gap
 * from presenting as a crash.
 *
 * The game imports exactly one thing from dsound.dll, by ordinal: #1, which is
 * DirectSoundCreate. It was the last unresolved import, and an unresolved import's
 * IAT slot still holds what the file put there - for an ordinal import that is
 * IMAGE_ORDINAL_FLAG | 1, or 0x80000001. So the game's own `jmp [0x78302c]` thunk
 * jumped to address 0x80000001 and died there, with nothing to connect that to
 * audio.
 *
 * DSERR_NODRIVER is the truth, and it is a documented outcome that every Windows
 * game had to handle: a machine with no sound card. Returning it lets the game take
 * the path it already has for that case. Nothing here pretends to produce sound, and
 * nothing here invents a device that would then be asked to play something.
 *
 * When audio is implemented for real, it belongs here: IDirectSound and
 * IDirectSoundBuffer over SDL2's audio device, in the same shape as the SDL-backed
 * dinput8 shim next door.
 */
#include "../win32/shim_internal.h"

#include <string.h>

/*
 * From dsound.h. Spelled out rather than included, because Wine's dsound.h wants
 * the COM interface machinery and this file implements no interfaces yet.
 *
 *   0x88780078 = DSERR_NODRIVER, "no sound driver is available for use"
 */
#define NFSU2_DSERR_NODRIVER 0x88780078u

HRESULT WINAPI DirectSoundCreate(const GUID *device, void **out, void *outer)
{
    (void)device; (void)outer;

    if (out)
        *out = NULL;
    nfsu2_shim_trace("DirectSoundCreate: no audio implementation - answering "
                     "DSERR_NODRIVER, which the game handles as a machine without a "
                     "sound card");
    return (HRESULT)NFSU2_DSERR_NODRIVER;
}

/*
 * dsound.dll's exports are numbered, and this game imports #1 with no name at all,
 * so the loader cannot resolve it by symbol - the same situation as ws2_32 (see
 * ws2_32/winsock.c) and the same answer: an explicit table, consulted by the loader
 * through a weak reference.
 *
 * Only the ordinals actually imported are listed. An ordinal that is missing gets
 * reported as unresolved, which is a true statement about this shim.
 */
void *nfsu2_dsound_lookup(const char *name, unsigned int ordinal)
{
    if (ordinal == 1 || (name && strcmp(name, "DirectSoundCreate") == 0))
        return (void *)DirectSoundCreate;
    return NULL;
}
