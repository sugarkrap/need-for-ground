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

/*
 * The action map, which is how this game asks for its keyboard - it never creates a
 * keyboard device directly. Each entry binds one key to one slot in the buffer the
 * game reads, and carries the game's own cookie back to it.
 *
 * 128 is comfortably above the 53 actions NFSU2 registers, and two actions may name
 * the same key (it binds a primary and an alternate for each control), so entries
 * are not unique per key.
 */
#define DINPUT_MAX_ACTIONS 128

/*
 * A semantic's top byte says which class of device can service the action, and for
 * the keyboard and mouse classes its low byte says which object on that device.
 * dinput.h does not document this as a bit field, but it reads straight off the
 * constants:
 *
 *   DIKEYBOARD_ESCAPE     = DIK_ESCAPE | 0x81000400   keyboard, low byte is the DIK
 *   DIMOUSE_XAXIS         = DIMOFS_X   | 0x82000300   mouse, low byte is an offset
 *   DIAXIS_DRIVINGR_STEER = 0x01008a01                genre-relative, needs a table
 *
 * Here rather than in one .c file because both the enumerator and the device use it,
 * and the two disagreeing would be a silently unbound control.
 */
#define SEMANTIC_CLASS(s)  (((s) >> 24) & 0xffu)
#define SEMANTIC_KEYBOARD  0x81u
#define SEMANTIC_MOUSE     0x82u
#define SEMANTIC_JOYSTICK  0x83u

struct dinput_action {
    DWORD offset;         /* where the value sits in the action-mapped state buffer */
    UINT_PTR app_data;    /* uAppData from DIACTION, returned in DIDEVICEOBJECTDATA */
    unsigned char dik;    /* the key that services it */
};

struct dinput_device {
    /* Must be first: this is what the game dereferences as lpVtbl. */
    const IDirectInputDevice8AVtbl *vtbl;
    LONG refs;

    enum dinput_device_kind kind;
    int acquired;
    DWORD state_size;   /* what SetDataFormat asked for */
    HWND window;

    /*
     * Which APIs this device has been used through, traced once each. A shim can
     * be entirely correct and still deliver no input if the game is asking a
     * different way - immediate state versus buffered data versus an action map -
     * and "it was never called" and "it was called and returned nothing" are
     * indistinguishable without this.
     */
    int traced_state;
    int traced_data;

    /* Set by SetActionMap. Zero actions means the raw data format is in use. */
    int action_count;
    struct dinput_action actions[DINPUT_MAX_ACTIONS];

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
const char *nfsu2_dinput_kind_name(enum dinput_device_kind kind);

/* app_data is what DirectInput returns in DIDEVICEOBJECTDATA::uAppData; it is zero
 * for a raw (non-action-mapped) device object. */
void nfsu2_dinput_device_buffer_push_action(struct dinput_device *device, DWORD offset,
                                           DWORD data, UINT_PTR app_data);
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
