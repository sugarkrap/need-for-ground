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

    *out = (LPDIRECTINPUTDEVICE8A)device;
    return DI_OK;
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
        memset(&instance, 0, sizeof(instance));
        instance.dwSize = sizeof(instance);
        instance.guidInstance = GUID_SysKeyboard;
        instance.guidProduct = GUID_SysKeyboard;
        instance.dwDevType = DI8DEVTYPE_KEYBOARD | (DI8DEVTYPEKEYBOARD_PCENH << 8);
        snprintf(instance.tszInstanceName, sizeof(instance.tszInstanceName), "Keyboard");
        snprintf(instance.tszProductName, sizeof(instance.tszProductName), "Keyboard");
        if (callback(&instance, ref) == DIENUM_STOP)
            return DI_OK;
    }

    if (want_mouse) {
        memset(&instance, 0, sizeof(instance));
        instance.dwSize = sizeof(instance);
        instance.guidInstance = GUID_SysMouse;
        instance.guidProduct = GUID_SysMouse;
        instance.dwDevType = DI8DEVTYPE_MOUSE | (DI8DEVTYPEMOUSE_TRADITIONAL << 8);
        snprintf(instance.tszInstanceName, sizeof(instance.tszInstanceName), "Mouse");
        snprintf(instance.tszProductName, sizeof(instance.tszProductName), "Mouse");
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
            const char *name = SDL_JoystickNameForIndex(i);

            memset(&instance, 0, sizeof(instance));
            instance.dwSize = sizeof(instance);
            joystick_guid(i, &instance.guidInstance);
            joystick_guid(i, &instance.guidProduct);
            instance.dwDevType = DI8DEVTYPE_JOYSTICK | (DI8DEVTYPEJOYSTICK_STANDARD << 8);
            snprintf(instance.tszInstanceName, sizeof(instance.tszInstanceName), "%s",
                     name ? name : "Joystick");
            snprintf(instance.tszProductName, sizeof(instance.tszProductName), "%s",
                     name ? name : "Joystick");
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

static HRESULT WINAPI dinput_EnumDevicesBySemantics(IDirectInput8A *self, LPCSTR user,
                                                    LPDIACTIONFORMATA format,
                                                    LPDIENUMDEVICESBYSEMANTICSCBA callback,
                                                    LPVOID ref, DWORD flags)
{
    (void)self; (void)user; (void)format; (void)callback; (void)ref; (void)flags;
    /* Action-mapping (the "semantics" API) is not implemented; see
     * device_SetActionMap. Games that use it fall back to explicit binding. */
    NFSU2_STUB("DirectInput8 EnumDevicesBySemantics");
    return DIERR_UNSUPPORTED;
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
