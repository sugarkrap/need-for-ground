/*
 * frame_capture.h - read rendered pixels back out of a D3D9 device.
 *
 * Exists because a frame counter is not evidence that anything is on screen. It
 * separates "we rendered nothing" from "we rendered correctly and the compositor
 * is not showing it" - a distinction that cost real time on this port (see
 * NOTES.md on the black window), and one no amount of staring at a window can
 * settle.
 */
#ifndef NFSU2_FRAME_CAPTURE_H
#define NFSU2_FRAME_CAPTURE_H

#include <nfsu2/d3d9_native.h>

/* Read one pixel of the backbuffer. `pixel` receives B, G, R, X. 0 on success. */
int nfsu2_capture_pixel(IDirect3DDevice9 *device, int x, int y, unsigned char *pixel);

/* Write the whole backbuffer to `path` as a PNG. 0 on success. */
int nfsu2_capture_png(IDirect3DDevice9 *device, const char *path);

#endif /* NFSU2_FRAME_CAPTURE_H */
