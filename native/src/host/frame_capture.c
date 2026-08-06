/*
 * frame_capture.c - see frame_capture.h.
 *
 * The PNG writer emits stored (uncompressed) deflate blocks so there is no zlib
 * dependency. The files are large, but this is a diagnostic, and a diagnostic
 * that needs a new dependency tends not to get used when it is needed.
 */
#include "frame_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal PNG writer: one IDAT of uncompressed deflate "stored" blocks, so no
 * zlib dependency. Not small output, but this is a diagnostic, and a diagnostic
 * that needs a new dependency tends not to get used.
 */
static unsigned long crc32_of(const unsigned char *data, size_t length, unsigned long crc)
{
    static unsigned long table[256];
    size_t i;

    if (!table[1]) {
        unsigned long c;
        int n, k;
        for (n = 0; n < 256; n++) {
            c = (unsigned long)n;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    }
    crc ^= 0xffffffffUL;
    for (i = 0; i < length; i++)
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffUL;
}

static void put32(unsigned char *out, unsigned long value)
{
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int write_chunk(FILE *f, const char *type, const unsigned char *data, size_t length)
{
    unsigned char header[8];
    unsigned char crc[4];
    unsigned long value;

    put32(header, (unsigned long)length);
    memcpy(header + 4, type, 4);
    if (fwrite(header, 1, 8, f) != 8)
        return -1;
    if (length && fwrite(data, 1, length, f) != length)
        return -1;
    value = crc32_of((const unsigned char *)type, 4, 0);
    if (length)
        value = crc32_of(data, length, value);
    put32(crc, value);
    return fwrite(crc, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png(const char *path, const unsigned char *pixels, int width, int height,
                     int pitch)
{
    static const unsigned char signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    unsigned char ihdr[13];
    unsigned char *raw;
    unsigned char *stream;
    size_t raw_size = (size_t)(width * 3 + 1) * (size_t)height;
    size_t stream_size;
    size_t offset = 0;
    size_t written = 0;
    unsigned long adler_a = 1, adler_b = 0;
    FILE *f;
    int y, x;
    int result = -1;

    raw = malloc(raw_size);
    if (!raw)
        return -1;

    /* D3DFMT_X8R8G8B8 is BGRX in memory; PNG wants RGB. */
    for (y = 0; y < height; y++) {
        unsigned char *row = raw + (size_t)y * (size_t)(width * 3 + 1);
        const unsigned char *source = pixels + (size_t)y * (size_t)pitch;

        row[0] = 0; /* filter: none */
        for (x = 0; x < width; x++) {
            row[1 + x * 3 + 0] = source[x * 4 + 2];
            row[1 + x * 3 + 1] = source[x * 4 + 1];
            row[1 + x * 3 + 2] = source[x * 4 + 0];
        }
    }

    for (offset = 0; offset < raw_size; offset++) {
        adler_a = (adler_a + raw[offset]) % 65521;
        adler_b = (adler_b + adler_a) % 65521;
    }

    /* zlib header + stored deflate blocks (max 65535 bytes each) + adler32. */
    stream_size = 2 + ((raw_size + 65534) / 65535) * 5 + raw_size + 4;
    stream = malloc(stream_size);
    if (!stream) {
        free(raw);
        return -1;
    }
    stream[written++] = 0x78;
    stream[written++] = 0x01;
    offset = 0;
    while (offset < raw_size) {
        size_t block = raw_size - offset > 65535 ? 65535 : raw_size - offset;
        int final = (offset + block >= raw_size);

        stream[written++] = (unsigned char)(final ? 1 : 0);
        stream[written++] = (unsigned char)(block & 0xff);
        stream[written++] = (unsigned char)(block >> 8);
        stream[written++] = (unsigned char)(~block & 0xff);
        stream[written++] = (unsigned char)((~block >> 8) & 0xff);
        memcpy(stream + written, raw + offset, block);
        written += block;
        offset += block;
    }
    put32(stream + written, (adler_b << 16) | adler_a);
    written += 4;

    f = fopen(path, "wb");
    if (f) {
        put32(ihdr, (unsigned long)width);
        put32(ihdr + 4, (unsigned long)height);
        ihdr[8] = 8;  /* bit depth */
        ihdr[9] = 2;  /* colour type: truecolour */
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        if (fwrite(signature, 1, 8, f) == 8 &&
            write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
            write_chunk(f, "IDAT", stream, written) == 0 &&
            write_chunk(f, "IEND", NULL, 0) == 0)
            result = 0;
        fclose(f);
    }
    free(stream);
    free(raw);
    return result;
}


/*
 * Both entry points do the same dance: get the backbuffer, copy it into a
 * lockable system-memory surface, and read that. GetRenderTargetData is the only
 * way round - a backbuffer in the default pool cannot be locked.
 */
static int with_backbuffer(IDirect3DDevice9 *device,
                           int (*use)(const unsigned char *pixels, int width, int height,
                                      int pitch, void *context),
                           void *context)
{
    IDirect3DSurface9 *back = NULL;
    IDirect3DSurface9 *system = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT locked;
    int result = -1;

    if (!device)
        return -1;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back)))
        return -1;
    if (FAILED(IDirect3DSurface9_GetDesc(back, &desc)))
        goto out;
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(device, desc.Width, desc.Height,
                                                            desc.Format, D3DPOOL_SYSTEMMEM,
                                                            &system, NULL)))
        goto out;
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(device, back, system)))
        goto out;
    if (FAILED(IDirect3DSurface9_LockRect(system, &locked, NULL, D3DLOCK_READONLY)))
        goto out;

    result = use((const unsigned char *)locked.pBits, (int)desc.Width, (int)desc.Height,
                 locked.Pitch, context);
    IDirect3DSurface9_UnlockRect(system);

out:
    if (system)
        IDirect3DSurface9_Release(system);
    if (back)
        IDirect3DSurface9_Release(back);
    return result;
}

struct pixel_request {
    int x;
    int y;
    unsigned char *out;
};

static int read_pixel(const unsigned char *pixels, int width, int height, int pitch,
                      void *context)
{
    struct pixel_request *request = context;

    if (request->x < 0 || request->x >= width || request->y < 0 || request->y >= height)
        return -1;
    memcpy(request->out, pixels + (size_t)request->y * (size_t)pitch + (size_t)request->x * 4, 4);
    return 0;
}

static int save_png(const unsigned char *pixels, int width, int height, int pitch,
                    void *context)
{
    return write_png((const char *)context, pixels, width, height, pitch);
}

int nfsu2_capture_pixel(IDirect3DDevice9 *device, int x, int y, unsigned char *pixel)
{
    struct pixel_request request = { x, y, pixel };

    return with_backbuffer(device, read_pixel, &request);
}

int nfsu2_capture_png(IDirect3DDevice9 *device, const char *path)
{
    return with_backbuffer(device, save_png, (void *)path);
}
