/*
 * dinput8.c - DirectInput8Create and IDirectInput8A.
 *
 * INITGUID is defined here so this one translation unit instantiates the
 * GUID_SysKeyboard / GUID_SysMouse / IID_IDirectInput8A constants that
 * DEFINE_GUID otherwise only declares. On Windows they would come from
 * dxguid.lib; there is no such library here.
 */
#define INITGUID

#include "dinput8_internal.h"

#include <stdlib.h>
#include <string.h>

struct dinput8 {
    /* Must be first: the game dereferences this as lpVtbl. */
    const IDirectInput8AVtbl *vtbl;
    LONG refs;
    DWORD version;
};

/*
 * Joystick instance GUIDs are synthesised: a fixed prefix with the SDL joystick
 * index in the last byte, so CreateDevice can map a GUID the game kept from
 * EnumDevices back to a device. Real DirectInput uses per-device GUIDs from the
 * driver, which have no equivalent here.
 */
static const GUID g_joystick_guid_base = {
    0x9a1b2c3d, 0x4e5f, 0x4a6b, { 0x8c, 0x7d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
};

static void joystick_guid(int index, GUID *out)
{
    *out = g_joystick_guid_base;
    out->Data4[7] = (unsigned char)index;
}

static int joystick_index_from_guid(REFGUID guid)
{
    GUID candidate;
    int index;

    for (index = 0; index < 16; index++) {
        joystick_guid(index, &candidate);
        if (memcmp(guid, &candidate, sizeof(GUID)) == 0)
            return index;
    }
    return -1;
}

/* --- IDirectInput8A ----------------------------------------------------- */

static HRESULT WINAPI dinput_QueryInterface(IDirectInput8A *self, REFIID riid, void **out)
{
    if (!out)
        return E_POINTER;
    (void)riid;
    *out = self;
    IDirectInput8_AddRef(self);
    return S_OK;
}

static ULONG WINAPI dinput_AddRef(IDirectInput8A *self)
{
    struct dinput8 *dinput = (struct dinput8 *)self;

    return (ULONG)++dinput->refs;
}

static ULONG WINAPI dinput_Release(IDirectInput8A *self)
{
    struct dinput8 *dinput = (struct dinput8 *)self;
    LONG remaining = --dinput->refs;

    if (remaining <= 0) {
        free(dinput);
        return 0;
    }
    return (ULONG)remaining;
}

static HRESULT WINAPI dinput_CreateDevice(IDirectInput8A *self, REFGUID guid,
                                          LPDIRECTINPUTDEVICE8A *out, LPUNKNOWN outer)
{
    struct dinput_device *device;
    int joystick_index;

    (void)self; (void)outer;
    if (!out || !guid)
        return E_POINTER;
    *out = NULL;

    if (nfsu2_dinput_ensure_sdl() != 0)
        return DIERR_NOTINITIALIZED;

    if (memcmp(guid, &GUID_SysKeyboard, sizeof(GUID)) == 0) {
        device = nfsu2_dinput_device_create(DINPUT_KEYBOARD, -1);
    } else if (memcmp(guid, &GUID_SysMouse, sizeof(GUID)) == 0) {
        device = nfsu2_dinput_device_create(DINPUT_MOUSE, -1);
    } else {
        joystick_index = joystick_index_from_guid(guid);
        if (joystick_index < 0) {
            nfsu2_shim_trace("dinput CreateDevice: unrecognised device GUID");
            return DIERR_DEVICENOTREG;
        }
        device = nfsu2_dinput_device_create(DINPUT_JOYSTICK, joystick_index);
    }

    if (!device)
        return DIERR_OUTOFMEMORY;

    nfsu2_shim_trace("dinput CreateDevice: %s", nfsu2_dinput_kind_name(device->kind));
    *out = (LPDIRECTINPUTDEVICE8A)device;
    return DI_OK;
}

/*
 * The DIDEVICEINSTANCE for one of our devices. Shared by the two enumerators so a
 * device describes itself the same way however it was found - a game that matches
 * the instance GUID from one enumeration against the other would otherwise see two
 * different devices.
 */
static void fill_instance(DIDEVICEINSTANCEA *instance, enum dinput_device_kind kind,
                          int joystick_index)
{
    const char *name;

    memset(instance, 0, sizeof(*instance));
    instance->dwSize = sizeof(*instance);

    switch (kind) {
    case DINPUT_KEYBOARD:
        instance->guidInstance = GUID_SysKeyboard;
        instance->guidProduct = GUID_SysKeyboard;
        instance->dwDevType = DI8DEVTYPE_KEYBOARD | (DI8DEVTYPEKEYBOARD_PCENH << 8);
        name = "Keyboard";
        break;
    case DINPUT_MOUSE:
        instance->guidInstance = GUID_SysMouse;
        instance->guidProduct = GUID_SysMouse;
        instance->dwDevType = DI8DEVTYPE_MOUSE | (DI8DEVTYPEMOUSE_TRADITIONAL << 8);
        name = "Mouse";
        break;
    default:
        joystick_guid(joystick_index, &instance->guidInstance);
        joystick_guid(joystick_index, &instance->guidProduct);
        instance->dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
        name = SDL_JoystickNameForIndex(joystick_index);
        if (!name)
            name = "Joystick";
        break;
    }

    snprintf(instance->tszInstanceName, sizeof(instance->tszInstanceName), "%s", name);
    snprintf(instance->tszProductName, sizeof(instance->tszProductName), "%s", name);
}

static HRESULT WINAPI dinput_EnumDevices(IDirectInput8A *self, DWORD device_type,
                                         LPDIENUMDEVICESCALLBACKA callback, LPVOID ref,
                                         DWORD flags)
{
    DIDEVICEINSTANCEA instance;
    int want_keyboard;
    int want_mouse;
    int want_sticks;
    int count;
    int i;

    (void)self; (void)flags;
    if (!callback)
        return DIERR_INVALIDPARAM;
    if (nfsu2_dinput_ensure_sdl() != 0)
        return DIERR_NOTINITIALIZED;

    want_keyboard = (device_type == DI8DEVCLASS_ALL || device_type == DI8DEVCLASS_KEYBOARD);
    want_mouse = (device_type == DI8DEVCLASS_ALL || device_type == DI8DEVCLASS_POINTER);
    want_sticks = (device_type == DI8DEVCLASS_ALL || device_type == DI8DEVCLASS_GAMECTRL);

    if (want_keyboard) {
        fill_instance(&instance, DINPUT_KEYBOARD, -1);
        if (callback(&instance, ref) == DIENUM_STOP)
            return DI_OK;
    }

    if (want_mouse) {
        fill_instance(&instance, DINPUT_MOUSE, -1);
        if (callback(&instance, ref) == DIENUM_STOP)
            return DI_OK;
    }

    if (want_sticks) {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) != 0) {
            nfsu2_shim_trace("dinput: no joystick subsystem: %s", SDL_GetError());
            return DI_OK; /* zero game controllers is not an error */
        }
        count = SDL_NumJoysticks();
        for (i = 0; i < count && i < 16; i++) {
            fill_instance(&instance, DINPUT_JOYSTICK, i);
            if (callback(&instance, ref) == DIENUM_STOP)
                return DI_OK;
        }
    }
    return DI_OK;
}

static HRESULT WINAPI dinput_GetDeviceStatus(IDirectInput8A *self, REFGUID guid)
{
    (void)self;
    if (!guid)
        return E_POINTER;
    if (memcmp(guid, &GUID_SysKeyboard, sizeof(GUID)) == 0 ||
        memcmp(guid, &GUID_SysMouse, sizeof(GUID)) == 0)
        return DI_OK;
    if (joystick_index_from_guid(guid) >= 0 &&
        joystick_index_from_guid(guid) < SDL_NumJoysticks())
        return DI_OK;
    return DI_NOTATTACHED;
}

static HRESULT WINAPI dinput_RunControlPanel(IDirectInput8A *self, HWND owner, DWORD flags)
{
    (void)self; (void)owner; (void)flags;
    NFSU2_STUB("DirectInput8 RunControlPanel");
    return DI_OK;
}

static HRESULT WINAPI dinput_Initialize(IDirectInput8A *self, HINSTANCE instance, DWORD version)
{
    struct dinput8 *dinput = (struct dinput8 *)self;

    (void)instance;
    dinput->version = version;
    return DI_OK;
}

static HRESULT WINAPI dinput_FindDevice(IDirectInput8A *self, REFGUID guid, LPCSTR name,
                                        LPGUID out)
{
    (void)self; (void)guid; (void)name;
    if (!out)
        return E_POINTER;
    NFSU2_STUB("DirectInput8 FindDevice");
    return DIERR_DEVICENOTREG;
}

/*
 * Report what the game is asking to bind, once. This exists because the previous
 * comment here - "games that use it fall back to explicit binding" - was wrong
 * about this one, and there was no way to tell from a run: the game asks for its
 * keyboard *only* through action mapping, never creates a keyboard device, and so a
 * stubbed enumerator produced a game with no keyboard and no diagnostic.
 *
 * The semantic layout is not documented as a bit field, but it reads straight off
 * the constants in dinput.h:
 *
 *   DIKEYBOARD_ESCAPE     = DIK_ESCAPE | 0x81000400   keyboard, low byte is the DIK
 *   DIMOUSE_XAXIS         = DIMOFS_X   | 0x82000300   mouse, low byte is an offset
 *   DIJOFS-based          = 0x83......                joystick
 *   DIAXIS_DRIVINGR_STEER = 0x01008a01                genre-relative
 *
 * so the top byte says which class of device can service the action, and anything
 * below 0x80 there is a genre semantic that has to go through the genre's default
 * mapping instead. Which of those this game uses decides how much work the rest is,
 * so it is printed rather than assumed. (It is all keyboard, as it turns out.)
 *
 * The macros live in dinput8_internal.h, shared with the device that binds them.
 */
static const char *semantic_class_name(DWORD semantic)
{
    switch (SEMANTIC_CLASS(semantic)) {
    case SEMANTIC_KEYBOARD: return "keyboard";
    case SEMANTIC_MOUSE:    return "mouse";
    case SEMANTIC_JOYSTICK: return "joystick";
    default:                return "genre";
    }
}

static void dump_action_format(const DIACTIONFORMATA *format)
{
    static int dumped;
    DWORD i;

    if (dumped || !nfsu2_shim_trace_enabled())
        return;
    dumped = 1;

    nfsu2_shim_trace("dinput action map \"%s\": %lu action(s), genre 0x%08lx, "
                     "dwDataSize=%lu, dwBufferSize=%lu, axis range %ld..%ld",
                     format->tszActionMap, (unsigned long)format->dwNumActions,
                     (unsigned long)format->dwGenre, (unsigned long)format->dwDataSize,
                     (unsigned long)format->dwBufferSize,
                     (long)format->lAxisMin, (long)format->lAxisMax);
    if (!format->rgoAction)
        return;
    for (i = 0; i < format->dwNumActions; i++) {
        const DIACTIONA *action = &format->rgoAction[i];

        nfsu2_shim_trace("  action %2lu: semantic 0x%08lx (%s, low 0x%02lx) "
                         "flags 0x%lx appdata 0x%lx  \"%s\"",
                         (unsigned long)i, (unsigned long)action->dwSemantic,
                         semantic_class_name(action->dwSemantic),
                         (unsigned long)(action->dwSemantic & 0xffu),
                         (unsigned long)action->dwFlags,
                         (unsigned long)action->uAppData,
                         (action->dwFlags & DIA_APPMAPPED) || !action->lptszActionName
                             ? "" : action->lptszActionName);
    }
}

/*
 * Offer the game a device for every class its action format mentions.
 *
 * The devices handed to the callback are DirectInput's, not the caller's: an
 * application AddRefs the ones it keeps. We hold our reference rather than
 * releasing after the callback, because real DirectInput caches these objects and a
 * game that relies on that would be left with a pointer to freed memory. A handful
 * of device objects for the life of the process is the cheaper mistake.
 */
static HRESULT WINAPI dinput_EnumDevicesBySemantics(IDirectInput8A *self, LPCSTR user,
                                                    LPDIACTIONFORMATA format,
                                                    LPDIENUMDEVICESBYSEMANTICSCBA callback,
                                                    LPVOID ref, DWORD flags)
{
    static struct dinput_device *cached[3];
    enum dinput_device_kind order[3] = { DINPUT_KEYBOARD, DINPUT_MOUSE, DINPUT_JOYSTICK };
    DIDEVICEINSTANCEA instance;
    int sticks = 0;
    int i;

    (void)self; (void)user; (void)flags;
    if (!callback || !format)
        return DIERR_INVALIDPARAM;
    if (format->dwSize != sizeof(*format) || format->dwActionSize != sizeof(DIACTIONA)) {
        nfsu2_shim_trace("dinput EnumDevicesBySemantics: unexpected structure sizes "
                         "(dwSize=%lu, dwActionSize=%lu)",
                         (unsigned long)format->dwSize, (unsigned long)format->dwActionSize);
        return DIERR_INVALIDPARAM;
    }
    if (nfsu2_dinput_ensure_sdl() != 0)
        return DIERR_NOTINITIALIZED;

    dump_action_format(format);

    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) == 0)
        sticks = SDL_NumJoysticks();

    /*
     * Keyboard first, then mouse, then the first stick. DirectInput enumerates
     * "most suitable first" and a racing game would rank a wheel highest, but a
     * game that binds only what it is offered first must not lose its keyboard -
     * and every caller is free to keep all of them.
     */
    for (i = 0; i < 3; i++) {
        enum dinput_device_kind kind = order[i];
        DWORD remaining;

        if (kind == DINPUT_JOYSTICK && sticks <= 0)
            continue;
        if (!cached[i]) {
            cached[i] = nfsu2_dinput_device_create(kind, kind == DINPUT_JOYSTICK ? 0 : -1);
            if (!cached[i])
                continue;
        }

        fill_instance(&instance, kind, 0);
        /* How many more will follow this one, which is what dwRemaining means. */
        remaining = (kind == DINPUT_KEYBOARD) ? (sticks > 0 ? 2 : 1)
                  : (kind == DINPUT_MOUSE)    ? (sticks > 0 ? 1 : 0)
                                              : 0;
        nfsu2_shim_trace("dinput EnumDevicesBySemantics: offering %s",
                         nfsu2_dinput_kind_name(kind));
        if (callback(&instance, (LPDIRECTINPUTDEVICE8A)cached[i],
                     DIEDBS_MAPPEDPRI1, remaining, ref) == DIENUM_STOP)
            break;
    }
    return DI_OK;
}

static HRESULT WINAPI dinput_ConfigureDevices(IDirectInput8A *self,
                                              LPDICONFIGUREDEVICESCALLBACK callback,
                                              LPDICONFIGUREDEVICESPARAMSA params, DWORD flags,
                                              LPVOID ref)
{
    (void)self; (void)callback; (void)params; (void)flags; (void)ref;
    NFSU2_STUB("DirectInput8 ConfigureDevices");
    return DIERR_UNSUPPORTED;
}

static const IDirectInput8AVtbl g_dinput_vtbl = {
    dinput_QueryInterface,
    dinput_AddRef,
    dinput_Release,
    dinput_CreateDevice,
    dinput_EnumDevices,
    dinput_GetDeviceStatus,
    dinput_RunControlPanel,
    dinput_Initialize,
    dinput_FindDevice,
    dinput_EnumDevicesBySemantics,
    dinput_ConfigureDevices,
};

HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, REFIID riid,
                                 LPVOID *out, LPUNKNOWN outer)
{
    struct dinput8 *dinput;

    (void)instance; (void)outer;
    if (!out)
        return E_POINTER;
    *out = NULL;

    /*
     * Only the ANSI interface is implemented. The game is an ANSI application
     * (it imports the *A entry points throughout), so a request for
     * IID_IDirectInput8W is a caller bug rather than something to accommodate -
     * and a wrong-interface object would fail later and less clearly.
     */
    if (riid && memcmp(riid, &IID_IDirectInput8W, sizeof(GUID)) == 0) {
        nfsu2_shim_trace("DirectInput8Create: IID_IDirectInput8W is not implemented");
        return DIERR_NOINTERFACE;
    }

    if (nfsu2_dinput_ensure_sdl() != 0)
        return DIERR_NOTINITIALIZED;

    dinput = calloc(1, sizeof(*dinput));
    if (!dinput)
        return DIERR_OUTOFMEMORY;
    dinput->vtbl = &g_dinput_vtbl;
    dinput->refs = 1;
    dinput->version = version;

    *out = dinput;
    return DI_OK;
}
