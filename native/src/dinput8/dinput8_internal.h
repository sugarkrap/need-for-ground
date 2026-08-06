/*
 * dinput8_internal.h - shared internals of the SDL2-backed DirectInput 8 shim.
 *
 * Note the ABI contrast with d3d9_native.h: DXVK forced D3D9's COM vtables to
 * cdecl because DXVK was compiled that way. Here *we* implement the interfaces,
 * so there is nothing to accommodate - the vtables are ordinary WINAPI
 * (stdcall on i386), matching what Wine's dinput.h declares and what the game's
 * call sites expect. Nothing in this directory should touch the convention
 * macros.
 */
#ifndef NFSU2_DINPUT8_INTERNAL_H
#define NFSU2_DINPUT8_INTERNAL_H

#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <dinput.h>

#include <SDL2/SDL.h>

enum dinput_device_kind {
    DINPUT_KEYBOARD = 1,
    DINPUT_MOUSE,
    DINPUT_JOYSTICK
};

#define DINPUT_BUFFER_CAPACITY 128

struct dinput_device {
    /* Must be first: this is what the game dereferences as lpVtbl. */
    const IDirectInputDevice8AVtbl *vtbl;
    LONG refs;

    enum dinput_device_kind kind;
    int acquired;
    DWORD state_size;   /* what SetDataFormat asked for */
    HWND window;

    /* Joystick */
    SDL_Joystick *joystick;
    int joystick_index;
    LONG axis_min;
    LONG axis_max;
    DWORD deadzone;
    DWORD saturation;

    /* Wheel movement has no "since last call" query in SDL, so it is
     * accumulated from the event stream (see nfsu2_dinput_notify_sdl_event). */
    int wheel_accumulator;

    /* Buffered mode (GetDeviceData). Zero dwBufferSize means immediate-only,
     * which is DirectInput's default. */
    DWORD buffer_size;
    DIDEVICEOBJECTDATA buffer[DINPUT_BUFFER_CAPACITY];
    int buffer_head;
    int buffer_count;
    DWORD sequence;
};

/* device.c */
struct dinput_device *nfsu2_dinput_device_create(enum dinput_device_kind kind, int joystick_index);
void nfsu2_dinput_device_buffer_push(struct dinput_device *device, DWORD offset, DWORD data);

/* Registry of live devices, so the SDL event hook can fan events out to them. */
void nfsu2_dinput_register(struct dinput_device *device);
void nfsu2_dinput_unregister(struct dinput_device *device);

/* dik_map.c: DIK_* scancode <-> SDL_Scancode. */
SDL_Scancode nfsu2_sdl_scancode_from_dik(unsigned char dik);
unsigned char nfsu2_dik_from_sdl_scancode(SDL_Scancode scancode);

/* Lazily bring up the SDL subsystems this needs. 0 on success. */
int nfsu2_dinput_ensure_sdl(void);

#endif /* NFSU2_DINPUT8_INTERNAL_H */
