/*
 * mixer.c - one SDL audio device, every DirectSound buffer mixed into it.
 *
 * DirectSound's model is that the hardware mixes: the application fills secondary
 * buffers and the driver sums them into a primary buffer. There is no such hardware
 * here, so this is the mixer, and it runs on SDL's audio callback thread.
 *
 * The play cursor is the load-bearing part, and not because of the sound. A game
 * streams into a looping buffer by asking where the cursor is and writing ahead of
 * it; if the cursor does not advance in real time the game either stalls waiting for
 * room or writes over audio that has not been played. So the cursor is advanced by
 * exactly the number of frames the callback consumed, which makes the audio device
 * the clock - the same thing DirectSound did.
 *
 * Resampling is nearest-neighbour. It is audibly worse than linear interpolation on
 * a pitch-shifted engine loop and it is a deliberate first cut: the interesting
 * question is whether the game's audio logic works at all, and a better kernel is a
 * self-contained change to sample_frame() once it does.
 */
#include "dsound_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* What we ask SDL for. Fixed rather than negotiated, so the conversion from a
 * buffer's own rate is entirely ours and does not vary by machine. SDL converts to
 * whatever the hardware wants underneath. */
#define MIX_RATE      48000
#define MIX_CHANNELS  2
#define MIX_FRAMES    1024

static SDL_AudioDeviceID g_device;
static int g_device_rate;
static int g_device_frames = MIX_FRAMES;
static pthread_mutex_t g_mix_lock = PTHREAD_MUTEX_INITIALIZER;

/* The live buffers. Grown rather than fixed: a racing game has an engine loop, tyre
 * and surface loops, music, and a duplicate per concurrent effect. */
static struct dsound_buffer **g_buffers;
static int g_buffer_count;
static int g_buffer_capacity;

/* A scratch accumulator, so mixing can sum in 32-bit and clamp once. Allocated at
 * start-up because the callback must not allocate. */
static int *g_accumulator;

void nfsu2_dsound_mixer_lock(void)   { pthread_mutex_lock(&g_mix_lock); }
void nfsu2_dsound_mixer_unlock(void) { pthread_mutex_unlock(&g_mix_lock); }
int nfsu2_dsound_mixer_rate(void)    { return g_device_rate ? g_device_rate : MIX_RATE; }

/*
 * How far ahead of the play cursor a caller must not write, in milliseconds.
 *
 * This is what DirectSound's write cursor expresses: the audio the driver has
 * already committed and that writing to would be heard as a glitch. Here the
 * committed region is whatever the next callback will consume, so the honest number
 * is one callback's worth - derived rather than a guess.
 */
int nfsu2_dsound_mixer_commit_ms(void)
{
    int rate = nfsu2_dsound_mixer_rate();

    return (g_device_frames * 1000 + rate - 1) / rate;
}

/* DirectSound attenuation is in hundredths of a decibel, 0 being full scale and
 * DSBVOLUME_MIN (-10000, i.e. -100 dB) being silence. */
static float gain_from_millibels(LONG millibels)
{
    if (millibels <= DSBVOLUME_MIN)
        return 0.0f;
    if (millibels >= 0)
        return 1.0f;
    return powf(10.0f, (float)millibels / 2000.0f);
}

/*
 * Per-channel gains for a buffer. lPan > 0 attenuates the *left* channel by that
 * many hundredths of a dB and leaves the right alone, and the other way for a
 * negative pan - it is an attenuation of one side, not a crossfade.
 */
static void channel_gains(const struct dsound_buffer *buffer, float *left, float *right)
{
    float volume = gain_from_millibels(buffer->volume);

    *left = volume;
    *right = volume;
    if (buffer->pan > 0)
        *left *= gain_from_millibels(-buffer->pan);
    else if (buffer->pan < 0)
        *right *= gain_from_millibels(buffer->pan);
}

/*
 * One frame from a buffer, as a signed 16-bit stereo pair. The only formats
 * DirectSound secondary buffers ever carried are 8-bit unsigned and 16-bit signed
 * PCM, mono or stereo; nfsu2_dsound_format_supported refuses anything else at
 * creation time, so there is no fallback needed here.
 */
static void sample_frame(const struct dsound_buffer *buffer, DWORD frame,
                         int *out_left, int *out_right)
{
    const WAVEFORMATEX *format = &buffer->format;
    const unsigned char *base = buffer->samples->data + (size_t)frame * format->nBlockAlign;
    int left;
    int right;

    if (format->wBitsPerSample == 8) {
        /* 8-bit PCM is unsigned with 128 as silence. */
        left = ((int)base[0] - 128) << 8;
        right = (format->nChannels >= 2) ? ((int)base[1] - 128) << 8 : left;
    } else {
        const short *samples = (const short *)base;

        left = samples[0];
        right = (format->nChannels >= 2) ? samples[1] : left;
    }
    *out_left = left;
    *out_right = right;
}

/* Mix one buffer into the accumulator. Caller holds the mixer lock. */
static void mix_buffer(struct dsound_buffer *buffer, int frames)
{
    DWORD total = nfsu2_dsound_buffer_frames(buffer);
    double rate;
    double step;
    float left_gain;
    float right_gain;
    int i;

    if (!buffer->playing || buffer->primary || !total)
        return;

    rate = buffer->frequency ? (double)buffer->frequency
                             : (double)buffer->format.nSamplesPerSec;
    step = rate / (double)nfsu2_dsound_mixer_rate();
    channel_gains(buffer, &left_gain, &right_gain);

    for (i = 0; i < frames; i++) {
        int left;
        int right;
        DWORD frame = (DWORD)buffer->play_cursor;

        if (frame >= total)
            frame = total - 1;
        sample_frame(buffer, frame, &left, &right);
        g_accumulator[i * 2 + 0] += (int)((float)left * left_gain);
        g_accumulator[i * 2 + 1] += (int)((float)right * right_gain);
        buffer->mixed_frames++;

        buffer->play_cursor += step;
        if (buffer->play_cursor < (double)total)
            continue;
        if (buffer->looping) {
            buffer->play_cursor = fmod(buffer->play_cursor, (double)total);
            continue;
        }
        /* A one-shot that has run out. DirectSound leaves the cursor at the start
         * and clears DSBSTATUS_PLAYING; it does not report an error anywhere. */
        buffer->playing = 0;
        buffer->play_cursor = 0.0;
        break;
    }
}

/*
 * The peak level leaving the mixer, reported periodically when
 * NFSU2_DSOUND_TRACE_LEVEL=1.
 *
 * This is the difference between "audio is playing" and "audio is audible", and
 * without it the two are indistinguishable from a log: a buffer can be playing,
 * looping and advancing its cursor exactly as it should while containing nothing but
 * silence, because the game's own mixer is muted or has written nothing into it.
 * Off by default, and the only I/O the callback ever does.
 */
static int g_peak;
static unsigned long g_reported_frames;
static int g_trace_level = -1;

static void report_level(int frames)
{
    if (g_trace_level < 0) {
        const char *value = getenv("NFSU2_DSOUND_TRACE_LEVEL");

        g_trace_level = (value && *value && *value != '0') ? 1 : 0;
    }
    if (!g_trace_level)
        return;

    g_reported_frames += (unsigned long)frames;
    if (g_reported_frames < (unsigned long)nfsu2_dsound_mixer_rate() * 2)
        return;
    nfsu2_shim_trace("dsound: peak output over the last 2 s: %d of 32767 (%s)",
                     g_peak, g_peak ? "audible" : "SILENT");
    g_peak = 0;
    g_reported_frames = 0;
}

static void SDLCALL mixer_callback(void *userdata, Uint8 *stream, int bytes)
{
    int frames = bytes / (MIX_CHANNELS * (int)sizeof(short));
    short *out = (short *)stream;
    int i;

    (void)userdata;
    memset(stream, 0, (size_t)bytes);
    if (!g_accumulator)
        return;
    memset(g_accumulator, 0, (size_t)frames * MIX_CHANNELS * sizeof(*g_accumulator));

    pthread_mutex_lock(&g_mix_lock);
    for (i = 0; i < g_buffer_count; i++)
        mix_buffer(g_buffers[i], frames);
    pthread_mutex_unlock(&g_mix_lock);

    for (i = 0; i < frames * MIX_CHANNELS; i++) {
        int value = g_accumulator[i];

        /* Clamp rather than wrap: summing many buffers at full scale overflows, and
         * wrapping turns that into a loud click instead of quiet distortion. */
        if (value > 32767)
            value = 32767;
        else if (value < -32768)
            value = -32768;
        out[i] = (short)value;
        if (value < 0)
            value = -value;
        if (value > g_peak)
            g_peak = value;
    }
    report_level(frames);
}

int nfsu2_dsound_mixer_start(void)
{
    SDL_AudioSpec want;
    SDL_AudioSpec got;

    if (g_device)
        return 0;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        nfsu2_shim_trace("dsound: no audio subsystem: %s", SDL_GetError());
        return -1;
    }

    memset(&want, 0, sizeof(want));
    want.freq = MIX_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = MIX_CHANNELS;
    want.samples = MIX_FRAMES;
    want.callback = mixer_callback;

    /* No allowed changes: the mixer converts from each buffer's format to exactly
     * this one, and SDL converts from here to the hardware. */
    g_device = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!g_device) {
        nfsu2_shim_trace("dsound: cannot open an audio device: %s", SDL_GetError());
        return -1;
    }
    g_device_rate = got.freq;
    g_device_frames = got.samples ? got.samples : MIX_FRAMES;

    g_accumulator = calloc((size_t)got.samples * MIX_CHANNELS, sizeof(*g_accumulator));
    if (!g_accumulator) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
        return -1;
    }

    SDL_PauseAudioDevice(g_device, 0);
    nfsu2_shim_trace("dsound: mixing at %d Hz, %d channel(s), %d-frame buffer",
                     got.freq, got.channels, got.samples);
    return 0;
}

void nfsu2_dsound_mixer_stop(void)
{
    if (!g_device)
        return;
    SDL_CloseAudioDevice(g_device);
    g_device = 0;
    /* After the device is closed the callback cannot be running, so this is the
     * one place the accumulator and the list can be released safely. */
    free(g_accumulator);
    g_accumulator = NULL;
    free(g_buffers);
    g_buffers = NULL;
    g_buffer_count = 0;
    g_buffer_capacity = 0;
}

void nfsu2_dsound_mixer_add(struct dsound_buffer *buffer)
{
    pthread_mutex_lock(&g_mix_lock);
    if (g_buffer_count == g_buffer_capacity) {
        int capacity = g_buffer_capacity ? g_buffer_capacity * 2 : 32;
        struct dsound_buffer **grown = realloc(g_buffers, (size_t)capacity * sizeof(*grown));

        if (!grown) {
            pthread_mutex_unlock(&g_mix_lock);
            nfsu2_shim_trace("dsound: out of memory for the buffer list; %p will be "
                             "silent", (void *)buffer);
            return;
        }
        g_buffers = grown;
        g_buffer_capacity = capacity;
    }
    g_buffers[g_buffer_count++] = buffer;
    pthread_mutex_unlock(&g_mix_lock);
}

void nfsu2_dsound_mixer_remove(struct dsound_buffer *buffer)
{
    int i;

    pthread_mutex_lock(&g_mix_lock);
    for (i = 0; i < g_buffer_count; i++) {
        if (g_buffers[i] != buffer)
            continue;
        g_buffers[i] = g_buffers[--g_buffer_count];
        break;
    }
    pthread_mutex_unlock(&g_mix_lock);
}
