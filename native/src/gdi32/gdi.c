/*
 * gdi.c - the gdi32 entry points the game imports.
 *
 * All twelve of them exist for one feature: the 2004 debug text overlay, which
 * builds a font, renders glyphs into a memory bitmap through a compatible DC,
 * and blits the result. None of it reaches the screen in this port - the D3D9
 * renderer owns every pixel and presents each frame - so the drawing calls are
 * swallowed.
 *
 * What is *not* swallowed is the object model: DCs, bitmaps and fonts get real
 * handles with real lifetimes, SelectObject returns the previous object, and
 * DeleteObject rejects a handle that is still selected. That is deliberate -
 * code that leaks or double-frees GDI objects behaves the same here as on
 * Windows, so the port does not quietly diverge on resource handling while the
 * pixels are still going nowhere.
 *
 * If the overlay ever needs to be visible, the honest way is to render this
 * bitmap into a D3D9 texture and draw it, not to grow a software rasteriser
 * here.
 */
#include "../win32/shim_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum gdi_kind {
    GDI_DC = 1,
    GDI_BITMAP,
    GDI_FONT
};

struct gdi_object {
    enum gdi_kind kind;
    int selected_count; /* how many DCs currently hold this object */

    /* GDI_DC */
    struct gdi_object *bitmap;
    struct gdi_object *font;
    COLORREF text_colour;
    COLORREF bk_colour;
    int bk_mode;

    /* GDI_BITMAP */
    int width;
    int height;
    UINT planes;
    UINT bpp;
    void *pixels;
    size_t pixels_size;

    /* GDI_FONT */
    char face_name[32];
    int height_request;
};

static struct gdi_object *object_alloc(enum gdi_kind kind)
{
    struct gdi_object *obj = calloc(1, sizeof(*obj));

    if (!obj) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    obj->kind = kind;
    return obj;
}

static struct gdi_object *object_of(HGDIOBJ handle, enum gdi_kind kind)
{
    struct gdi_object *obj = (struct gdi_object *)handle;

    if (!obj || obj->kind != kind) {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return obj;
}

/* --- device contexts ---------------------------------------------------- */

HDC WINAPI CreateCompatibleDC(HDC source)
{
    struct gdi_object *dc = object_alloc(GDI_DC);

    (void)source;
    if (!dc)
        return NULL;
    dc->text_colour = 0x00ffffff;
    dc->bk_colour = 0x00000000;
    dc->bk_mode = OPAQUE;
    return (HDC)dc;
}

BOOL WINAPI DeleteDC(HDC hdc)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);

    if (!dc)
        return FALSE;
    if (dc->bitmap)
        dc->bitmap->selected_count--;
    if (dc->font)
        dc->font->selected_count--;
    free(dc);
    return TRUE;
}

/* --- bitmaps and fonts -------------------------------------------------- */

HBITMAP WINAPI CreateBitmap(INT width, INT height, UINT planes, UINT bpp, LPCVOID bits)
{
    struct gdi_object *bmp;
    size_t stride, size;

    if (width <= 0 || height <= 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!planes)
        planes = 1;
    if (!bpp)
        bpp = 1;

    /* Rows are DWORD-aligned, as on Windows: code that computes its own stride
     * from the width must agree with what we allocated. */
    stride = (((size_t)width * bpp * planes + 31) / 32) * 4;
    size = stride * (size_t)height;

    bmp = object_alloc(GDI_BITMAP);
    if (!bmp)
        return NULL;
    bmp->width = width;
    bmp->height = height;
    bmp->planes = planes;
    bmp->bpp = bpp;
    bmp->pixels = calloc(1, size);
    bmp->pixels_size = size;
    if (!bmp->pixels) {
        free(bmp);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (bits)
        memcpy(bmp->pixels, bits, size);
    return (HBITMAP)bmp;
}

HFONT WINAPI CreateFontA(INT height, INT width, INT escapement, INT orientation, INT weight,
                         DWORD italic, DWORD underline, DWORD strikeout, DWORD charset,
                         DWORD out_precision, DWORD clip_precision, DWORD quality,
                         DWORD pitch_and_family, LPCSTR face)
{
    struct gdi_object *font = object_alloc(GDI_FONT);

    (void)width; (void)escapement; (void)orientation; (void)weight; (void)italic;
    (void)underline; (void)strikeout; (void)charset; (void)out_precision;
    (void)clip_precision; (void)quality; (void)pitch_and_family;

    if (!font)
        return NULL;
    font->height_request = height;
    snprintf(font->face_name, sizeof(font->face_name), "%s", face ? face : "");
    NFSU2_STUB("CreateFontA (no glyph rasterisation)");
    return (HFONT)font;
}

HGDIOBJ WINAPI SelectObject(HDC hdc, HGDIOBJ object)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);
    struct gdi_object *obj = (struct gdi_object *)object;
    struct gdi_object *previous;

    if (!dc || !obj)
        return NULL;

    switch (obj->kind) {
    case GDI_BITMAP:
        previous = dc->bitmap;
        if (previous)
            previous->selected_count--;
        dc->bitmap = obj;
        obj->selected_count++;
        return (HGDIOBJ)previous;
    case GDI_FONT:
        previous = dc->font;
        if (previous)
            previous->selected_count--;
        dc->font = obj;
        obj->selected_count++;
        return (HGDIOBJ)previous;
    default:
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }
}

BOOL WINAPI DeleteObject(HGDIOBJ object)
{
    struct gdi_object *obj = (struct gdi_object *)object;

    if (!obj) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (obj->kind == GDI_DC) {
        /* Windows tolerates this; be equally forgiving rather than leaking. */
        return DeleteDC((HDC)object);
    }
    if (obj->kind != GDI_BITMAP && obj->kind != GDI_FONT) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (obj->selected_count > 0) {
        /* Same refusal as Windows: deleting a selected object would leave a DC
         * pointing at freed memory. */
        nfsu2_shim_trace("DeleteObject: object still selected into a DC");
        return FALSE;
    }
    free(obj->pixels);
    free(obj);
    return TRUE;
}

/* --- drawing (swallowed) ------------------------------------------------ */

BOOL WINAPI BitBlt(HDC dst, INT x, INT y, INT width, INT height,
                   HDC src, INT src_x, INT src_y, DWORD rop)
{
    struct gdi_object *dst_dc = object_of((HGDIOBJ)dst, GDI_DC);

    (void)x; (void)y; (void)width; (void)height; (void)src; (void)src_x; (void)src_y;

    if (!dst_dc)
        return FALSE;
    /* WHITENESS/BLACKNESS have no source and are the two the overlay uses to
     * clear; honouring them keeps a subsequent GetPixel self-consistent. */
    if (dst_dc->bitmap && dst_dc->bitmap->pixels &&
        (rop == WHITENESS || rop == BLACKNESS)) {
        memset(dst_dc->bitmap->pixels, rop == WHITENESS ? 0xff : 0x00,
               dst_dc->bitmap->pixels_size);
        return TRUE;
    }
    NFSU2_STUB("BitBlt (no rasterisation)");
    return TRUE;
}

BOOL WINAPI ExtTextOutA(HDC hdc, INT x, INT y, UINT options, const RECT *rect,
                        LPCSTR text, UINT count, const INT *dx)
{
    (void)x; (void)y; (void)options; (void)rect; (void)dx;

    if (!object_of((HGDIOBJ)hdc, GDI_DC))
        return FALSE;
    if (nfsu2_shim_trace_enabled() && text && count)
        nfsu2_shim_trace("ExtTextOutA: \"%.*s\"", (int)count, text);
    return TRUE;
}

COLORREF WINAPI GetPixel(HDC hdc, INT x, INT y)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);

    (void)x; (void)y;
    if (!dc || !dc->bitmap)
        return CLR_INVALID;
    /*
     * Nothing was ever rasterised, so any colour we returned would be a lie.
     * CLR_INVALID is the documented "no pixel here" answer and lets a caller's
     * error path run instead of it acting on invented data.
     */
    NFSU2_STUB("GetPixel (nothing is rasterised)");
    return CLR_INVALID;
}

/* --- DC attributes ----------------------------------------------------- */

COLORREF WINAPI SetTextColor(HDC hdc, COLORREF colour)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);
    COLORREF previous;

    if (!dc)
        return CLR_INVALID;
    previous = dc->text_colour;
    dc->text_colour = colour;
    return previous;
}

COLORREF WINAPI SetBkColor(HDC hdc, COLORREF colour)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);
    COLORREF previous;

    if (!dc)
        return CLR_INVALID;
    previous = dc->bk_colour;
    dc->bk_colour = colour;
    return previous;
}

INT WINAPI SetBkMode(HDC hdc, INT mode)
{
    struct gdi_object *dc = object_of((HGDIOBJ)hdc, GDI_DC);
    INT previous;

    if (!dc)
        return 0;
    previous = dc->bk_mode;
    dc->bk_mode = mode;
    return previous;
}
