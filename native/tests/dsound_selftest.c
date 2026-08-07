/*
 * dsound_selftest.c - DirectSound over SDL2.
 *
 * Needs a working audio device (exits 77 = meson SKIP otherwise), because the play
 * cursor is driven by the device's callback and there is nothing to test without one.
 *
 * The two things worth testing are the ones a game actually depends on and that are
 * easy to get subtly wrong:
 *
 *   - Lock's two-pointer contract. A region that runs off the end of the buffer
 *     continues at the start, and a caller told only about the first half writes past
 *     the end. This is checked at the wrap, not just in the middle.
 *   - The play cursor advancing in real time. A game streams by asking where the
 *     cursor is and writing ahead of it, so a cursor that does not move turns into a
 *     game that stalls or one that overwrites unplayed audio - a silent failure that
 *     has nothing to do with whether any sound came out.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <dsound.h>

#include <SDL2/SDL.h>

#include <stdio.h>
#include <string.h>

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

/* 16-bit stereo at 22050, which is the shape most of this game's audio is. */
static void fill_format(WAVEFORMATEX *format)
{
    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = 2;
    format->nSamplesPerSec = 22050;
    format->wBitsPerSample = 16;
    format->nBlockAlign = 4;
    format->nAvgBytesPerSec = 22050 * 4;
}

int main(void)
{
    WAVEFORMATEX format;
    DSBUFFERDESC desc;
    IDirectSound *ds = NULL;
    IDirectSoundBuffer *buffer = NULL;
    IDirectSoundBuffer *duplicate = NULL;
    IDirectSoundBuffer *primary = NULL;
    void *ptr1;
    void *ptr2;
    DWORD bytes1;
    DWORD bytes2;
    DWORD play;
    DWORD write;
    DWORD status;
    DWORD first;
    LONG value;
    HRESULT hr;
    const DWORD size = 22050 * 4; /* one second */

    setvbuf(stdout, NULL, _IONBF, 0);

    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        printf("no audio subsystem (%s) - skipping\n", SDL_GetError());
        return 77;
    }

    hr = DirectSoundCreate(NULL, &ds, NULL);
    if (hr == (HRESULT)0x88780078 /* DSERR_NODRIVER */) {
        printf("no audio device - skipping\n");
        SDL_Quit();
        return 77;
    }
    CHECK(hr == DS_OK && ds != NULL, "DirectSoundCreate");
    if (!ds) {
        SDL_Quit();
        return 1;
    }

    CHECK(IDirectSound_SetCooperativeLevel(ds, NULL, DSSCL_PRIORITY) == DS_OK,
          "SetCooperativeLevel(DSSCL_PRIORITY)");

    /* --- the primary buffer, which a game makes to describe the output ---- */
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    CHECK(IDirectSound_CreateSoundBuffer(ds, &desc, &primary, NULL) == DS_OK && primary,
          "a primary buffer needs no format and no size");
    if (primary) {
        fill_format(&format);
        CHECK(IDirectSoundBuffer_SetFormat(primary, &format) == DS_OK,
              "SetFormat on the primary buffer is accepted");
    }

    /* --- a format the mixer cannot read is refused, not played as noise --- */
    fill_format(&format);
    format.wBitsPerSample = 24;
    format.nBlockAlign = 6;
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwBufferBytes = size;
    desc.lpwfxFormat = &format;
    {
        IDirectSoundBuffer *rejected = NULL;

        CHECK(IDirectSound_CreateSoundBuffer(ds, &desc, &rejected, NULL) == DSERR_BADFORMAT &&
              !rejected, "24-bit PCM is refused with DSERR_BADFORMAT");
    }

    /* --- a real secondary buffer ------------------------------------------ */
    fill_format(&format);
    memset(&desc, 0, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY |
                   DSBCAPS_GETCURRENTPOSITION2;
    desc.dwBufferBytes = size;
    desc.lpwfxFormat = &format;
    CHECK(IDirectSound_CreateSoundBuffer(ds, &desc, &buffer, NULL) == DS_OK && buffer,
          "CreateSoundBuffer, 16-bit stereo 22050 Hz, one second");
    if (!buffer) {
        IDirectSound_Release(ds);
        SDL_Quit();
        return 1;
    }

    /* --- format round trip ------------------------------------------------ */
    {
        WAVEFORMATEX got;
        DWORD written = 0;

        memset(&got, 0, sizeof(got));
        CHECK(IDirectSoundBuffer_GetFormat(buffer, &got, sizeof(got), &written) == DS_OK &&
              written == sizeof(WAVEFORMATEX) && got.nSamplesPerSec == 22050 &&
              got.nChannels == 2 && got.wBitsPerSample == 16,
              "GetFormat returns what CreateSoundBuffer was given");
    }

    /* --- Lock in the middle: one region ----------------------------------- */
    ptr1 = ptr2 = NULL;
    bytes1 = bytes2 = 0;
    CHECK(IDirectSoundBuffer_Lock(buffer, 0, 1024, &ptr1, &bytes1, &ptr2, &bytes2, 0) == DS_OK &&
          ptr1 && bytes1 == 1024 && !ptr2 && bytes2 == 0,
          "Lock inside the buffer returns one region of exactly the size asked");
    memset(ptr1, 0, bytes1);
    CHECK(IDirectSoundBuffer_Unlock(buffer, ptr1, bytes1, ptr2, bytes2) == DS_OK, "Unlock");

    /* --- Lock across the end: two regions, and they add up ---------------- */
    first = 256;
    ptr1 = ptr2 = NULL;
    bytes1 = bytes2 = 0;
    CHECK(IDirectSoundBuffer_Lock(buffer, size - first, first + 512, &ptr1, &bytes1,
                                  &ptr2, &bytes2, 0) == DS_OK &&
          ptr1 && bytes1 == first && ptr2 && bytes2 == 512 &&
          bytes1 + bytes2 == first + 512,
          "Lock across the end splits into %lu + %lu bytes",
          (unsigned long)bytes1, (unsigned long)bytes2);
    CHECK(ptr1 != NULL && ptr2 != NULL && (char *)ptr2 < (char *)ptr1,
          "the second region is the start of the buffer, not past the end of it");
    if (ptr1 && ptr2) {
        memset(ptr1, 0, bytes1);
        memset(ptr2, 0, bytes2);
    }
    CHECK(IDirectSoundBuffer_Unlock(buffer, ptr1, bytes1, ptr2, bytes2) == DS_OK,
          "Unlock both regions");

    /* --- DSBLOCK_ENTIREBUFFER --------------------------------------------- */
    ptr1 = ptr2 = NULL;
    bytes1 = bytes2 = 0;
    CHECK(IDirectSoundBuffer_Lock(buffer, 0, 0, &ptr1, &bytes1, &ptr2, &bytes2,
                                  DSBLOCK_ENTIREBUFFER) == DS_OK && bytes1 == size,
          "DSBLOCK_ENTIREBUFFER locks all %lu bytes", (unsigned long)size);
    IDirectSoundBuffer_Unlock(buffer, ptr1, bytes1, ptr2, bytes2);

    /* --- volume, pan and frequency round trip ----------------------------- */
    CHECK(IDirectSoundBuffer_SetVolume(buffer, -2000) == DS_OK &&
          IDirectSoundBuffer_GetVolume(buffer, &value) == DS_OK && value == -2000,
          "SetVolume/GetVolume round trip");
    CHECK(IDirectSoundBuffer_SetVolume(buffer, 1) == DSERR_INVALIDPARAM,
          "a volume above DSBVOLUME_MAX is refused");
    CHECK(IDirectSoundBuffer_SetPan(buffer, DSBPAN_RIGHT) == DS_OK &&
          IDirectSoundBuffer_GetPan(buffer, &value) == DS_OK && value == DSBPAN_RIGHT,
          "SetPan/GetPan round trip");
    CHECK(IDirectSoundBuffer_SetFrequency(buffer, 11025) == DS_OK &&
          IDirectSoundBuffer_GetFrequency(buffer, &play) == DS_OK && play == 11025,
          "SetFrequency/GetFrequency round trip");
    CHECK(IDirectSoundBuffer_SetFrequency(buffer, 0) == DS_OK &&
          IDirectSoundBuffer_GetFrequency(buffer, &play) == DS_OK && play == 22050,
          "frequency 0 means the format's own rate");

    /* Back to neutral before the timing check, so nothing is silent or shifted. */
    IDirectSoundBuffer_SetVolume(buffer, 0);
    IDirectSoundBuffer_SetPan(buffer, 0);

    /* --- the play cursor has to advance in real time ---------------------- */
    CHECK(IDirectSoundBuffer_GetStatus(buffer, &status) == DS_OK && status == 0,
          "a new buffer is not playing");
    CHECK(IDirectSoundBuffer_SetCurrentPosition(buffer, 0) == DS_OK, "SetCurrentPosition(0)");
    CHECK(IDirectSoundBuffer_Play(buffer, 0, 0, DSBPLAY_LOOPING) == DS_OK, "Play(LOOPING)");
    CHECK(IDirectSoundBuffer_GetStatus(buffer, &status) == DS_OK &&
          (status & DSBSTATUS_PLAYING) && (status & DSBSTATUS_LOOPING),
          "GetStatus reports PLAYING and LOOPING");

    SDL_Delay(250);
    CHECK(IDirectSoundBuffer_GetCurrentPosition(buffer, &play, &write) == DS_OK,
          "GetCurrentPosition while playing");
    /*
     * A quarter of a second of 22050 Hz stereo is about 22050 bytes. The bounds are
     * wide because this is a real audio device on a machine doing other things; what
     * is being tested is that the cursor moved at roughly the right speed, not that
     * the scheduler is precise.
     */
    CHECK(play > 4000 && play < 40000,
          "the play cursor advanced to %lu bytes after 250 ms (expected ~22050)",
          (unsigned long)play);
    CHECK(play % format.nBlockAlign == 0,
          "the play cursor is frame-aligned (%lu %% %u == 0)",
          (unsigned long)play, format.nBlockAlign);

    CHECK(IDirectSoundBuffer_Stop(buffer) == DS_OK, "Stop");
    CHECK(IDirectSoundBuffer_GetCurrentPosition(buffer, &write, NULL) == DS_OK,
          "GetCurrentPosition after Stop");
    CHECK(IDirectSoundBuffer_GetStatus(buffer, &status) == DS_OK &&
          !(status & DSBSTATUS_PLAYING), "Stop clears DSBSTATUS_PLAYING");
    {
        DWORD after = 0;

        SDL_Delay(100);
        IDirectSoundBuffer_GetCurrentPosition(buffer, &after, NULL);
        CHECK(after == write, "a stopped cursor stays where it was (%lu)",
              (unsigned long)after);
    }

    /* --- a duplicate shares the samples and keeps its own cursor ---------- */
    CHECK(IDirectSound_DuplicateSoundBuffer(ds, buffer, &duplicate) == DS_OK && duplicate,
          "DuplicateSoundBuffer");
    if (duplicate) {
        CHECK(duplicate != buffer, "the duplicate is a distinct object");
        CHECK(IDirectSoundBuffer_SetVolume(duplicate, -500) == DS_OK &&
              IDirectSoundBuffer_GetVolume(buffer, &value) == DS_OK && value == 0,
              "the duplicate's volume is its own");
        CHECK(IDirectSoundBuffer_Play(duplicate, 0, 0, 0) == DS_OK,
              "the duplicate plays independently");
        CHECK(IDirectSoundBuffer_Release(duplicate) == 0,
              "releasing the duplicate does not take the original's samples with it");
        /* If it had, this would read freed memory. */
        CHECK(IDirectSoundBuffer_Lock(buffer, 0, 64, &ptr1, &bytes1, &ptr2, &bytes2, 0) == DS_OK,
              "the original is still usable afterwards");
        IDirectSoundBuffer_Unlock(buffer, ptr1, bytes1, ptr2, bytes2);
    }

    /* --- QueryInterface tells the truth about what it is not -------------- */
    {
        void *unused = NULL;

        CHECK(IDirectSoundBuffer_QueryInterface(buffer, &IID_IDirectSound3DBuffer,
                                               &unused) == E_NOINTERFACE && !unused,
              "IDirectSound3DBuffer is refused rather than faked");
    }

    if (primary)
        IDirectSoundBuffer_Release(primary);
    IDirectSoundBuffer_Release(buffer);
    IDirectSound_Release(ds);
    SDL_Quit();

    printf(g_failures ? "\nFAILED (%d failure%s)\n" : "\nPASSED (%d failure%s)\n",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
