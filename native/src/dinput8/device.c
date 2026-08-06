/*
 * device.c - IDirectInputDevice8A over SDL: keyboard, mouse and joysticks.
 *
 * Immediate state (GetDeviceState) is read straight from SDL's own state
 * snapshot, which is what the game's driving code uses every frame. Buffered
 * data (GetDeviceData) needs the *event stream*, and the user32 message pump
 * already consumes it - so message.c hands each SDL event to
 * nfsu2_dinput_notify_sdl_event() (a weak symbol there, so user32 does not
 * depend on this module) and the events are fanned out to whichever devices
 * asked for buffering.
 *
 * The consequence is worth knowing: buffered input only works while something
 * pumps messages. Immediate state works either way, because GetDeviceState
 * pumps SDL itself.
 */
#include "dinput8_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LIVE_DEVICES 8

static struct dinput_device *g_live[MAX_LIVE_DEVICES];

void nfsu2_dinput_register(struct dinput_device *device)
{
    int i;

    for (i = 0; i < MAX_LIVE_DEVICES; i++) {
        if (!g_live[i]) {
            g_live[i] = device;
            return;
        }
    }
    nfsu2_shim_trace("dinput: live-device table full; buffered data will be dropped");
}

void nfsu2_dinput_unregister(struct dinput_device *device)
{
    int i;

    for (i = 0; i < MAX_LIVE_DEVICES; i++) {
        if (g_live[i] == device)
            g_live[i] = NULL;
    }
}

void nfsu2_dinput_device_buffer_push(struct dinput_device *device, DWORD offset, DWORD data)
{
    DIDEVICEOBJECTDATA *entry;

    if (!device->buffer_size || !device->acquired)
        return;
    if (device->buffer_count == DINPUT_BUFFER_CAPACITY) {
        /* DirectInput signals overflow via DI_BUFFEROVERFLOW on the next
         * GetDeviceData; dropping the oldest keeps the most recent input, which
         * is what a dropped-frame situation wants. */
        device->buffer_head = (device->buffer_head + 1) % DINPUT_BUFFER_CAPACITY;
        device->buffer_count--;
    }
    entry = &device->buffer[(device->buffer_head + device->buffer_count) % DINPUT_BUFFER_CAPACITY];
    memset(entry, 0, sizeof(*entry));
    entry->dwOfs = offset;
    entry->dwData = data;
    entry->dwTimeStamp = GetTickCount();
    entry->dwSequence = ++device->sequence;
    device->buffer_count++;
}

/*
 * Called from user32's SDL pump for every event (weak symbol there). Only
 * buffered-mode devices care; immediate state is read from SDL directly.
 */
void nfsu2_dinput_notify_sdl_event(const void *opaque_event)
{
    const SDL_Event *event = opaque_event;
    int i;

    for (i = 0; i < MAX_LIVE_DEVICES; i++) {
        struct dinput_device *device = g_live[i];

        if (!device)
            continue;

        switch (event->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (device->kind == DINPUT_KEYBOARD && !event->key.repeat) {
                unsigned char dik = nfsu2_dik_from_sdl_scancode(event->key.keysym.scancode);
                if (dik)
                    nfsu2_dinput_device_buffer_push(device, dik,
                                                    event->type == SDL_KEYDOWN ? 0x80 : 0x00);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (device->kind == DINPUT_MOUSE) {
                DWORD offset;

                switch (event->button.button) {
                case SDL_BUTTON_LEFT:   offset = DIMOFS_BUTTON0; break;
                case SDL_BUTTON_RIGHT:  offset = DIMOFS_BUTTON1; break;
                case SDL_BUTTON_MIDDLE: offset = DIMOFS_BUTTON2; break;
                default: continue;
                }
                nfsu2_dinput_device_buffer_push(
                    device, offset, event->type == SDL_MOUSEBUTTONDOWN ? 0x80 : 0x00);
            }
            break;
        case SDL_MOUSEWHEEL:
            if (device->kind == DINPUT_MOUSE) {
                /* SDL has no "wheel delta since last query", so it is
                 * accumulated here and drained by GetDeviceState. */
                device->wheel_accumulator += event->wheel.y * 120; /* WHEEL_DELTA */
                nfsu2_dinput_device_buffer_push(device, DIMOFS_Z,
                                                (DWORD)(event->wheel.y * 120));
            }
            break;
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            if (device->kind == DINPUT_JOYSTICK)
                nfsu2_dinput_device_buffer_push(
                    device, (DWORD)(DIJOFS_BUTTON0 + event->jbutton.button),
                    event->type == SDL_JOYBUTTONDOWN ? 0x80 : 0x00);
            break;
        default:
            break;
        }
    }
}

/* --- helpers ------------------------------------------------------------ */

int nfsu2_dinput_ensure_sdl(void)
{
    /* Video is what owns the keyboard and mouse state; joystick is separate and
     * only needed if a stick is asked for. */
    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        nfsu2_shim_trace("dinput: SDL video init failed: %s", SDL_GetError());
        return -1;
    }
    return 0;
}

/* Map an SDL axis reading onto the range the caller configured. */
static LONG scale_axis(const struct dinput_device *device, Sint16 value)
{
    long span = (long)device->axis_max - (long)device->axis_min;
    long shifted = (long)value + 32768; /* 0 .. 65535 */

    return (LONG)(device->axis_min + (shifted * span) / 65535);
}

static DWORD pov_from_hat(Uint8 hat)
{
    /*
     * DirectInput POV is hundredths of a degree clockwise from north, with
     * 0xFFFFFFFF meaning centred - not a bitmask like SDL's hat.
     */
    switch (hat) {
    case SDL_HAT_UP:        return 0;
    case SDL_HAT_RIGHTUP:   return 4500;
    case SDL_HAT_RIGHT:     return 9000;
    case SDL_HAT_RIGHTDOWN: return 13500;
    case SDL_HAT_DOWN:      return 18000;
    case SDL_HAT_LEFTDOWN:  return 22500;
    case SDL_HAT_LEFT:      return 27000;
    case SDL_HAT_LEFTUP:    return 31500;
    default:                return 0xffffffffu;
    }
}

/* --- the vtable --------------------------------------------------------- */

static HRESULT WINAPI device_QueryInterface(IDirectInputDevice8A *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    /* Every DirectInputDevice IID maps to the same object here; the game only
     * ever asks for the one it created. */
    (void)riid;
    *out = self;
    IDirectInputDevice8_AddRef(self);
    return S_OK;
}

static ULONG WINAPI device_AddRef(IDirectInputDevice8A *self)
{
    struct dinput_device *device = (struct dinput_device *)self;

    return (ULONG)++device->refs;
}

static ULONG WINAPI device_Release(IDirectInputDevice8A *self)
{
    struct dinput_device *device = (struct dinput_device *)self;
    LONG remaining = --device->refs;

    if (remaining <= 0) {
        nfsu2_dinput_unregister(device);
        if (device->joystick)
            SDL_JoystickClose(device->joystick);
        free(device);
        return 0;
    }
    return (ULONG)remaining;
}

static HRESULT WINAPI device_GetCapabilities(IDirectInputDevice8A *self, LPDIDEVCAPS caps)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!caps || caps->dwSize < sizeof(DIDEVCAPS))
        return DIERR_INVALIDPARAM;

    memset((char *)caps + sizeof(DWORD), 0, caps->dwSize - sizeof(DWORD));
    caps->dwFlags = DIDC_ATTACHED | DIDC_EMULATED;

    switch (device->kind) {
    case DINPUT_KEYBOARD:
        caps->dwDevType = DI8DEVTYPE_KEYBOARD | (DI8DEVTYPEKEYBOARD_PCENH << 8);
        caps->dwAxes = 0;
        caps->dwButtons = 128;
        break;
    case DINPUT_MOUSE:
        caps->dwDevType = DI8DEVTYPE_MOUSE | (DI8DEVTYPEMOUSE_TRADITIONAL << 8);
        caps->dwAxes = 3;
        caps->dwButtons = 4;
        break;
    case DINPUT_JOYSTICK:
        caps->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        caps->dwAxes = device->joystick ? (DWORD)SDL_JoystickNumAxes(device->joystick) : 2;
        caps->dwButtons = device->joystick ? (DWORD)SDL_JoystickNumButtons(device->joystick) : 2;
        caps->dwPOVs = device->joystick ? (DWORD)SDL_JoystickNumHats(device->joystick) : 0;
        break;
    }
    return DI_OK;
}

static HRESULT WINAPI device_EnumObjects(IDirectInputDevice8A *self,
                                         LPDIENUMDEVICEOBJECTSCALLBACKA callback,
                                         LPVOID ref, DWORD flags)
{
    struct dinput_device *device = (struct dinput_device *)self;
    DIDEVICEOBJECTINSTANCEA object;
    int axes;
    int buttons;
    int i;

    if (!callback)
        return DIERR_INVALIDPARAM;

    /*
     * Games enumerate axes to discover what a stick has before setting ranges
     * per axis, so this has to report the real SDL device rather than a fixed
     * guess. Only axes and buttons are enumerated; nothing here has effects or
     * force feedback.
     */
    axes = (device->kind == DINPUT_JOYSTICK && device->joystick)
               ? SDL_JoystickNumAxes(device->joystick)
               : (device->kind == DINPUT_MOUSE ? 3 : 0);
    buttons = (device->kind == DINPUT_JOYSTICK && device->joystick)
                  ? SDL_JoystickNumButtons(device->joystick)
                  : (device->kind == DINPUT_MOUSE ? 4 : 0);

    if (flags == DIDFT_ALL || (flags & DIDFT_AXIS)) {
        for (i = 0; i < axes; i++) {
            memset(&object, 0, sizeof(object));
            object.dwSize = sizeof(object);
            object.dwOfs = (DWORD)(i * (int)sizeof(LONG));
            object.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
            object.dwFlags = 0;
            snprintf(object.tszName, sizeof(object.tszName), "Axis %d", i);
            if (callback(&object, ref) == DIENUM_STOP)
                return DI_OK;
        }
    }
    if (flags == DIDFT_ALL || (flags & DIDFT_BUTTON)) {
        for (i = 0; i < buttons; i++) {
            memset(&object, 0, sizeof(object));
            object.dwSize = sizeof(object);
            object.dwOfs = (DWORD)(DIJOFS_BUTTON0 + i);
            object.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
            snprintf(object.tszName, sizeof(object.tszName), "Button %d", i);
            if (callback(&object, ref) == DIENUM_STOP)
                return DI_OK;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI device_GetProperty(IDirectInputDevice8A *self, REFGUID prop,
                                         LPDIPROPHEADER header)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!header)
        return DIERR_INVALIDPARAM;

    /* The DIPROP_* "GUIDs" are small integers cast to a pointer
     * (MAKEDIPROP), so they are compared by identity, not contents. */
    if (prop == DIPROP_BUFFERSIZE) {
        ((LPDIPROPDWORD)header)->dwData = device->buffer_size;
        return DI_OK;
    }
    if (prop == DIPROP_RANGE) {
        ((LPDIPROPRANGE)header)->lMin = device->axis_min;
        ((LPDIPROPRANGE)header)->lMax = device->axis_max;
        return DI_OK;
    }
    if (prop == DIPROP_DEADZONE) {
        ((LPDIPROPDWORD)header)->dwData = device->deadzone;
        return DI_OK;
    }
    if (prop == DIPROP_SATURATION) {
        ((LPDIPROPDWORD)header)->dwData = device->saturation;
        return DI_OK;
    }
    nfsu2_shim_trace("dinput GetProperty(%p): unsupported", (const void *)prop);
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_SetProperty(IDirectInputDevice8A *self, REFGUID prop,
                                         LPCDIPROPHEADER header)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!header)
        return DIERR_INVALIDPARAM;

    if (prop == DIPROP_BUFFERSIZE) {
        DWORD requested = ((LPCDIPROPDWORD)header)->dwData;

        if (requested > DINPUT_BUFFER_CAPACITY) {
            nfsu2_shim_trace("dinput: buffer size %lu clamped to %d",
                             (unsigned long)requested, DINPUT_BUFFER_CAPACITY);
            requested = DINPUT_BUFFER_CAPACITY;
        }
        device->buffer_size = requested;
        return DI_OK;
    }
    if (prop == DIPROP_RANGE) {
        device->axis_min = ((LPCDIPROPRANGE)header)->lMin;
        device->axis_max = ((LPCDIPROPRANGE)header)->lMax;
        return DI_OK;
    }
    if (prop == DIPROP_DEADZONE) {
        device->deadzone = ((LPCDIPROPDWORD)header)->dwData;
        return DI_OK;
    }
    if (prop == DIPROP_SATURATION) {
        device->saturation = ((LPCDIPROPDWORD)header)->dwData;
        return DI_OK;
    }
    if (prop == DIPROP_AXISMODE) {
        /* Absolute is all we report; a relative-axis request would need the
         * per-frame deltas the mouse path already keeps, and no caller here
         * asks for it on a stick. */
        if (((LPCDIPROPDWORD)header)->dwData != DIPROPAXISMODE_ABS)
            NFSU2_STUB("dinput DIPROP_AXISMODE(REL)");
        return DI_OK;
    }
    /* Cooperative-level and calibration properties are accepted silently: they
     * describe how the OS should arbitrate a device we have exclusively. */
    return DI_OK;
}

static HRESULT WINAPI device_Acquire(IDirectInputDevice8A *self)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (device->acquired)
        return S_FALSE; /* already acquired: what DirectInput returns */
    if (nfsu2_dinput_ensure_sdl() != 0)
        return DIERR_NOTINITIALIZED;

    device->acquired = 1;
    device->buffer_head = 0;
    device->buffer_count = 0;
    return DI_OK;
}

static HRESULT WINAPI device_Unacquire(IDirectInputDevice8A *self)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!device->acquired)
        return DI_NOEFFECT;
    device->acquired = 0;
    return DI_OK;
}

static HRESULT WINAPI device_GetDeviceState(IDirectInputDevice8A *self, DWORD size, LPVOID data)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!data)
        return DIERR_INVALIDPARAM;
    if (!device->acquired)
        return DIERR_NOTACQUIRED;

    /*
     * Pump SDL here so immediate state is current even if the game reads input
     * without running a message loop that frame. SDL_PumpEvents only fills the
     * queue - it does not consume it - so this does not steal events from
     * user32's PeekMessageA.
     */
    SDL_PumpEvents();

    switch (device->kind) {
    case DINPUT_KEYBOARD: {
        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        unsigned char *out = data;
        DWORD i;

        if (size < 256)
            return DIERR_INVALIDPARAM;
        memset(out, 0, size);
        for (i = 0; i < 256; i++) {
            SDL_Scancode scancode = nfsu2_sdl_scancode_from_dik((unsigned char)i);
            /* DirectInput reports "down" as the high bit, not as 1. */
            if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode])
                out[i] = 0x80;
        }
        return DI_OK;
    }
    case DINPUT_MOUSE: {
        DIMOUSESTATE2 state;
        Uint32 buttons;
        int dx = 0, dy = 0;

        if (size < sizeof(DIMOUSESTATE))
            return DIERR_INVALIDPARAM;

        memset(&state, 0, sizeof(state));
        /* Relative since the last call, which is exactly DIMOUSESTATE's
         * contract - hence SDL_GetRelativeMouseState rather than a position. */
        buttons = SDL_GetRelativeMouseState(&dx, &dy);
        state.lX = dx;
        state.lY = dy;
        state.lZ = device->wheel_accumulator;
        device->wheel_accumulator = 0;
        if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))
            state.rgbButtons[0] = 0x80;
        if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))
            state.rgbButtons[1] = 0x80;
        if (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE))
            state.rgbButtons[2] = 0x80;

        /* Copy only what the caller's data format asked for: DIMOUSESTATE (12
         * bytes, 4 buttons) and DIMOUSESTATE2 (20 bytes, 8) share a prefix. */
        memcpy(data, &state, size < sizeof(state) ? size : sizeof(state));
        return DI_OK;
    }
    case DINPUT_JOYSTICK: {
        DIJOYSTATE2 state;
        int axes, buttons, hats;
        int i;

        if (size < sizeof(DIJOYSTATE))
            return DIERR_INVALIDPARAM;
        if (!device->joystick)
            return DIERR_INPUTLOST;

        memset(&state, 0, sizeof(state));
        for (i = 0; i < 4; i++)
            state.rgdwPOV[i] = 0xffffffffu; /* centred */

        axes = SDL_JoystickNumAxes(device->joystick);
        buttons = SDL_JoystickNumButtons(device->joystick);
        hats = SDL_JoystickNumHats(device->joystick);

        for (i = 0; i < axes && i < 8; i++) {
            LONG value = scale_axis(device, SDL_JoystickGetAxis(device->joystick, i));

            /* Axis order matches DIJOYSTATE's layout: X, Y, Z, Rx, Ry, Rz,
             * then the two sliders. */
            switch (i) {
            case 0: state.lX = value; break;
            case 1: state.lY = value; break;
            case 2: state.lZ = value; break;
            case 3: state.lRx = value; break;
            case 4: state.lRy = value; break;
            case 5: state.lRz = value; break;
            default: state.rglSlider[i - 6] = value; break;
            }
        }
        for (i = 0; i < buttons && i < 128; i++) {
            if (SDL_JoystickGetButton(device->joystick, i))
                state.rgbButtons[i] = 0x80;
        }
        for (i = 0; i < hats && i < 4; i++)
            state.rgdwPOV[i] = pov_from_hat(SDL_JoystickGetHat(device->joystick, i));

        memcpy(data, &state, size < sizeof(state) ? size : sizeof(state));
        return DI_OK;
    }
    }
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_GetDeviceData(IDirectInputDevice8A *self, DWORD object_size,
                                           LPDIDEVICEOBJECTDATA out, LPDWORD in_out, DWORD flags)
{
    struct dinput_device *device = (struct dinput_device *)self;
    DWORD wanted;
    DWORD produced = 0;

    if (!in_out)
        return DIERR_INVALIDPARAM;
    if (!device->acquired)
        return DIERR_NOTACQUIRED;
    if (!device->buffer_size) {
        /* DirectInput's own answer when no buffer was configured - and the sign
         * that a caller forgot DIPROP_BUFFERSIZE. */
        return DIERR_NOTBUFFERED;
    }
    if (object_size && object_size != sizeof(DIDEVICEOBJECTDATA))
        return DIERR_INVALIDPARAM;

    wanted = *in_out;
    if (!out) {
        /* Count-only query. */
        *in_out = (DWORD)device->buffer_count;
        return DI_OK;
    }

    while (produced < wanted && device->buffer_count > 0) {
        out[produced++] = device->buffer[device->buffer_head];
        if (!(flags & DIGDD_PEEK)) {
            device->buffer_head = (device->buffer_head + 1) % DINPUT_BUFFER_CAPACITY;
            device->buffer_count--;
        } else {
            break; /* peeking never advances, so one entry is all there is */
        }
    }
    *in_out = produced;
    return DI_OK;
}

static HRESULT WINAPI device_SetDataFormat(IDirectInputDevice8A *self, LPCDIDATAFORMAT format)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!format || format->dwSize != sizeof(DIDATAFORMAT))
        return DIERR_INVALIDPARAM;

    /*
     * The format's object list is not honoured: we always report the standard
     * layout for the device kind (c_dfDIKeyboard / c_dfDIMouse2 / c_dfDIJoystick2),
     * which is what every caller passes. What is kept is dwDataSize, so
     * GetDeviceState can tell DIMOUSESTATE from DIMOUSESTATE2 and DIJOYSTATE
     * from DIJOYSTATE2.
     */
    device->state_size = format->dwDataSize;
    return DI_OK;
}

static HRESULT WINAPI device_SetEventNotification(IDirectInputDevice8A *self, HANDLE event)
{
    (void)self;
    if (event)
        NFSU2_STUB("dinput SetEventNotification (no event signalling)");
    return DI_OK;
}

static HRESULT WINAPI device_SetCooperativeLevel(IDirectInputDevice8A *self, HWND window,
                                                 DWORD flags)
{
    struct dinput_device *device = (struct dinput_device *)self;

    device->window = window;
    /*
     * Exclusive/foreground arbitration is the window manager's business here.
     * The one thing worth honouring is exclusive mouse access, which is what
     * "capture the pointer and hide it" means to the player.
     */
    if (device->kind == DINPUT_MOUSE && (flags & DISCL_EXCLUSIVE)) {
        if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0)
            nfsu2_shim_trace("dinput: relative mouse mode failed: %s", SDL_GetError());
    }
    return DI_OK;
}

static HRESULT WINAPI device_GetObjectInfo(IDirectInputDevice8A *self,
                                           LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how)
{
    struct dinput_device *device = (struct dinput_device *)self;

    (void)how;
    if (!info || info->dwSize < sizeof(DIDEVICEOBJECTINSTANCEA))
        return DIERR_INVALIDPARAM;

    memset((char *)info + sizeof(DWORD), 0, info->dwSize - sizeof(DWORD));
    info->dwOfs = obj;
    if (device->kind == DINPUT_KEYBOARD) {
        SDL_Scancode scancode = nfsu2_sdl_scancode_from_dik((unsigned char)obj);
        info->dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE((int)obj);
        snprintf(info->tszName, sizeof(info->tszName), "%s",
                 scancode != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(scancode) : "Key");
    } else {
        info->dwType = DIDFT_PSHBUTTON;
        snprintf(info->tszName, sizeof(info->tszName), "Object %lu", (unsigned long)obj);
    }
    return DI_OK;
}

static HRESULT WINAPI device_GetDeviceInfo(IDirectInputDevice8A *self, LPDIDEVICEINSTANCEA info)
{
    struct dinput_device *device = (struct dinput_device *)self;
    const char *name = "Unknown";

    if (!info || info->dwSize < sizeof(DIDEVICEINSTANCEA))
        return DIERR_INVALIDPARAM;

    memset((char *)info + sizeof(DWORD), 0, info->dwSize - sizeof(DWORD));
    switch (device->kind) {
    case DINPUT_KEYBOARD:
        info->guidInstance = GUID_SysKeyboard;
        info->guidProduct = GUID_SysKeyboard;
        info->dwDevType = DI8DEVTYPE_KEYBOARD | (DI8DEVTYPEKEYBOARD_PCENH << 8);
        name = "Keyboard";
        break;
    case DINPUT_MOUSE:
        info->guidInstance = GUID_SysMouse;
        info->guidProduct = GUID_SysMouse;
        info->dwDevType = DI8DEVTYPE_MOUSE | (DI8DEVTYPEMOUSE_TRADITIONAL << 8);
        name = "Mouse";
        break;
    case DINPUT_JOYSTICK:
        info->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        if (device->joystick && SDL_JoystickName(device->joystick))
            name = SDL_JoystickName(device->joystick);
        else
            name = "Joystick";
        break;
    }
    snprintf(info->tszInstanceName, sizeof(info->tszInstanceName), "%s", name);
    snprintf(info->tszProductName, sizeof(info->tszProductName), "%s", name);
    return DI_OK;
}

static HRESULT WINAPI device_RunControlPanel(IDirectInputDevice8A *self, HWND owner, DWORD flags)
{
    (void)self; (void)owner; (void)flags;
    NFSU2_STUB("dinput RunControlPanel");
    return DI_OK;
}

static HRESULT WINAPI device_Initialize(IDirectInputDevice8A *self, HINSTANCE instance,
                                        DWORD version, REFGUID guid)
{
    (void)self; (void)instance; (void)version; (void)guid;
    return DI_OK;
}

static HRESULT WINAPI device_Poll(IDirectInputDevice8A *self)
{
    struct dinput_device *device = (struct dinput_device *)self;

    if (!device->acquired)
        return DIERR_NOTACQUIRED;
    /* Polled devices need their state refreshed before GetDeviceState; for SDL
     * that is the event pump plus an explicit joystick update. */
    SDL_PumpEvents();
    if (device->kind == DINPUT_JOYSTICK)
        SDL_JoystickUpdate();
    return DI_OK;
}

/* --- force feedback and the rest: not supported ------------------------- */

static HRESULT WINAPI device_CreateEffect(IDirectInputDevice8A *self, REFGUID guid,
                                          LPCDIEFFECT effect, LPDIRECTINPUTEFFECT *out,
                                          LPUNKNOWN outer)
{
    (void)self; (void)guid; (void)effect; (void)outer;
    /*
     * Force feedback is genuinely not implemented (SDL's haptic API could back
     * it later). Failing rather than returning a non-functional effect object
     * means the game's own "no force feedback available" path runs.
     */
    NFSU2_STUB("dinput CreateEffect (no force feedback)");
    if (out)
        *out = NULL;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_EnumEffects(IDirectInputDevice8A *self,
                                         LPDIENUMEFFECTSCALLBACKA callback, LPVOID ref,
                                         DWORD type)
{
    (void)self; (void)callback; (void)ref; (void)type;
    /* No effects to enumerate: succeed with an empty enumeration. */
    return DI_OK;
}

static HRESULT WINAPI device_GetEffectInfo(IDirectInputDevice8A *self, LPDIEFFECTINFOA info,
                                           REFGUID guid)
{
    (void)self; (void)info; (void)guid;
    return DIERR_DEVICENOTREG;
}

static HRESULT WINAPI device_GetForceFeedbackState(IDirectInputDevice8A *self, LPDWORD out)
{
    (void)self;
    if (out)
        *out = 0;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_SendForceFeedbackCommand(IDirectInputDevice8A *self, DWORD flags)
{
    (void)self; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_EnumCreatedEffectObjects(IDirectInputDevice8A *self,
                                                      LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb,
                                                      LPVOID ref, DWORD flags)
{
    (void)self; (void)cb; (void)ref; (void)flags;
    return DI_OK;
}

static HRESULT WINAPI device_Escape(IDirectInputDevice8A *self, LPDIEFFESCAPE escape)
{
    (void)self; (void)escape;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_SendDeviceData(IDirectInputDevice8A *self, DWORD object_size,
                                            LPCDIDEVICEOBJECTDATA data, LPDWORD in_out,
                                            DWORD flags)
{
    (void)self; (void)object_size; (void)data; (void)flags;
    if (in_out)
        *in_out = 0;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_EnumEffectsInFile(IDirectInputDevice8A *self, LPCSTR file,
                                               LPDIENUMEFFECTSINFILECALLBACK cb, LPVOID ref,
                                               DWORD flags)
{
    (void)self; (void)file; (void)cb; (void)ref; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_WriteEffectToFile(IDirectInputDevice8A *self, LPCSTR file,
                                               DWORD entries, LPDIFILEEFFECT effects,
                                               DWORD flags)
{
    (void)self; (void)file; (void)entries; (void)effects; (void)flags;
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_BuildActionMap(IDirectInputDevice8A *self, LPDIACTIONFORMATA format,
                                            LPCSTR user, DWORD flags)
{
    (void)self; (void)format; (void)user; (void)flags;
    NFSU2_STUB("dinput BuildActionMap");
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_SetActionMap(IDirectInputDevice8A *self, LPDIACTIONFORMATA format,
                                          LPCSTR user, DWORD flags)
{
    (void)self; (void)format; (void)user; (void)flags;
    NFSU2_STUB("dinput SetActionMap");
    return DIERR_UNSUPPORTED;
}

static HRESULT WINAPI device_GetImageInfo(IDirectInputDevice8A *self,
                                          LPDIDEVICEIMAGEINFOHEADERA header)
{
    (void)self; (void)header;
    return DIERR_UNSUPPORTED;
}

static const IDirectInputDevice8AVtbl g_device_vtbl = {
    device_QueryInterface,
    device_AddRef,
    device_Release,
    device_GetCapabilities,
    device_EnumObjects,
    device_GetProperty,
    device_SetProperty,
    device_Acquire,
    device_Unacquire,
    device_GetDeviceState,
    device_GetDeviceData,
    device_SetDataFormat,
    device_SetEventNotification,
    device_SetCooperativeLevel,
    device_GetObjectInfo,
    device_GetDeviceInfo,
    device_RunControlPanel,
    device_Initialize,
    device_CreateEffect,
    device_EnumEffects,
    device_GetEffectInfo,
    device_GetForceFeedbackState,
    device_SendForceFeedbackCommand,
    device_EnumCreatedEffectObjects,
    device_Escape,
    device_Poll,
    device_SendDeviceData,
    device_EnumEffectsInFile,
    device_WriteEffectToFile,
    device_BuildActionMap,
    device_SetActionMap,
    device_GetImageInfo,
};

struct dinput_device *nfsu2_dinput_device_create(enum dinput_device_kind kind, int joystick_index)
{
    struct dinput_device *device = calloc(1, sizeof(*device));

    if (!device)
        return NULL;

    device->vtbl = &g_device_vtbl;
    device->refs = 1;
    device->kind = kind;
    device->joystick_index = joystick_index;
    /* DirectInput's default axis range when nothing sets DIPROP_RANGE. */
    device->axis_min = 0;
    device->axis_max = 65535;

    if (kind == DINPUT_JOYSTICK) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
            nfsu2_shim_trace("dinput: SDL joystick init failed: %s", SDL_GetError());
            free(device);
            return NULL;
        }
        device->joystick = SDL_JoystickOpen(joystick_index);
        if (!device->joystick) {
            nfsu2_shim_trace("dinput: cannot open joystick %d: %s", joystick_index,
                             SDL_GetError());
            free(device);
            return NULL;
        }
    }

    nfsu2_dinput_register(device);
    return device;
}
