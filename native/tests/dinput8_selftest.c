/*
 * dinput8_selftest.c - DirectInput 8 over SDL.
 *
 * Needs a display (exits 77 = meson SKIP otherwise). Keyboard state is checked
 * by pushing synthetic SDL key events and reading them back both ways
 * DirectInput offers: immediate state via GetDeviceState, and the buffered
 * stream via GetDeviceData. Those are two entirely separate paths in the shim -
 * immediate state comes from SDL's snapshot, buffered data from the event hook
 * in user32's message pump - so both need covering.
 */
#include <nfsu2/win32_compat.h>
#include <nfsu2/win32_shim.h>

#include <ddraw.h>
#include <dinput.h>

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

static int g_enum_keyboards;
static int g_enum_mice;
static int g_enum_sticks;

static BOOL CALLBACK enum_devices_cb(LPCDIDEVICEINSTANCEA instance, LPVOID ref)
{
    (void)ref;
    switch (GET_DIDEVICE_TYPE(instance->dwDevType)) {
    case DI8DEVTYPE_KEYBOARD: g_enum_keyboards++; break;
    case DI8DEVTYPE_MOUSE:    g_enum_mice++; break;
    case DI8DEVTYPE_JOYSTICK:
    case DI8DEVTYPE_GAMEPAD:  g_enum_sticks++; break;
    default: break;
    }
    return DIENUM_CONTINUE;
}

static void push_key(SDL_Scancode scancode, Uint32 type)
{
    SDL_Event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.key.type = type;
    ev.key.timestamp = SDL_GetTicks();
    ev.key.state = (type == SDL_KEYDOWN) ? SDL_PRESSED : SDL_RELEASED;
    ev.key.keysym.scancode = scancode;
    ev.key.keysym.sym = SDL_GetKeyFromScancode(scancode);
    SDL_PushEvent(&ev);
}

/* Run the user32 pump, which is what feeds DirectInput's buffered stream. */
static void pump(void)
{
    MSG msg;
    int guard = 0;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && ++guard < 256)
        ;
}

int main(void)
{
    IDirectInput8A *dinput = NULL;
    IDirectInputDevice8A *keyboard = NULL;
    IDirectInputDevice8A *mouse = NULL;
    DIPROPDWORD buffer_size;
    DIDEVCAPS caps;
    DIDEVICEINSTANCEA info;
    DIDEVICEOBJECTDATA events[16];
    DWORD event_count;
    unsigned char keys[256];
    DIMOUSESTATE mouse_state;
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SKIP - no usable display (%s)\n", SDL_GetError());
        return 77;
    }

    if (nfsu2_win32_init(NULL) != 0) {
        printf("FAIL - shim init\n");
        return 1;
    }

    /* --- the factory ---------------------------------------------------- */
    hr = DirectInput8Create(NULL, DIRECTINPUT_VERSION, &IID_IDirectInput8A,
                            (void **)&dinput, NULL);
    CHECK(hr == DI_OK && dinput != NULL, "DirectInput8Create(IID_IDirectInput8A)");
    if (!dinput) {
        printf("\nFAILED (cannot continue)\n");
        return 1;
    }

    {
        void *unicode = NULL;
        CHECK(DirectInput8Create(NULL, DIRECTINPUT_VERSION, &IID_IDirectInput8W,
                                 &unicode, NULL) == DIERR_NOINTERFACE && unicode == NULL,
              "the W interface is refused rather than half-implemented");
    }

    CHECK(IDirectInput8_EnumDevices(dinput, DI8DEVCLASS_ALL, enum_devices_cb, NULL,
                                    DIEDFL_ATTACHEDONLY) == DI_OK,
          "EnumDevices(DI8DEVCLASS_ALL)");
    CHECK(g_enum_keyboards == 1 && g_enum_mice == 1,
          "enumeration reports one keyboard and one mouse (%d joystick(s) attached)",
          g_enum_sticks);
    CHECK(IDirectInput8_GetDeviceStatus(dinput, &GUID_SysKeyboard) == DI_OK,
          "GetDeviceStatus(GUID_SysKeyboard)");

    /* --- keyboard ------------------------------------------------------- */
    hr = IDirectInput8_CreateDevice(dinput, &GUID_SysKeyboard, &keyboard, NULL);
    CHECK(hr == DI_OK && keyboard != NULL, "CreateDevice(GUID_SysKeyboard)");

    CHECK(IDirectInputDevice8_SetDataFormat(keyboard, &c_dfDIKeyboard) == DI_OK,
          "SetDataFormat(c_dfDIKeyboard)");
    CHECK(IDirectInputDevice8_SetCooperativeLevel(keyboard, NULL,
                                                  DISCL_FOREGROUND | DISCL_NONEXCLUSIVE) == DI_OK,
          "SetCooperativeLevel");

    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    CHECK(IDirectInputDevice8_GetCapabilities(keyboard, &caps) == DI_OK &&
              GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD &&
              (caps.dwFlags & DIDC_ATTACHED),
          "GetCapabilities reports an attached keyboard");

    memset(&info, 0, sizeof(info));
    info.dwSize = sizeof(info);
    CHECK(IDirectInputDevice8_GetDeviceInfo(keyboard, &info) == DI_OK &&
              memcmp(&info.guidInstance, &GUID_SysKeyboard, sizeof(GUID)) == 0,
          "GetDeviceInfo returns GUID_SysKeyboard");

    /* State reads must fail before Acquire, as DirectInput requires. */
    CHECK(IDirectInputDevice8_GetDeviceState(keyboard, sizeof(keys), keys) == DIERR_NOTACQUIRED,
          "GetDeviceState before Acquire returns DIERR_NOTACQUIRED");
    CHECK(IDirectInputDevice8_Acquire(keyboard) == DI_OK, "Acquire");
    CHECK(IDirectInputDevice8_Acquire(keyboard) == S_FALSE,
          "a second Acquire returns S_FALSE, not an error");

    memset(keys, 0xcc, sizeof(keys));
    CHECK(IDirectInputDevice8_GetDeviceState(keyboard, sizeof(keys), keys) == DI_OK,
          "GetDeviceState(256 bytes)");
    CHECK(keys[0] == 0 && keys[DIK_ESCAPE] == 0,
          "no key reads as pressed with nothing held");
    CHECK(IDirectInputDevice8_GetDeviceState(keyboard, 16, keys) == DIERR_INVALIDPARAM,
          "a too-small keyboard buffer is rejected");

    /* --- buffered mode -------------------------------------------------- */
    CHECK(IDirectInputDevice8_GetDeviceData(keyboard, sizeof(DIDEVICEOBJECTDATA), events,
                                            &event_count, 0) == (HRESULT)DIERR_NOTBUFFERED,
          "GetDeviceData without DIPROP_BUFFERSIZE returns DIERR_NOTBUFFERED");

    memset(&buffer_size, 0, sizeof(buffer_size));
    buffer_size.diph.dwSize = sizeof(DIPROPDWORD);
    buffer_size.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    buffer_size.diph.dwHow = DIPH_DEVICE;
    buffer_size.dwData = 32;
    CHECK(IDirectInputDevice8_SetProperty(keyboard, DIPROP_BUFFERSIZE, &buffer_size.diph) == DI_OK,
          "SetProperty(DIPROP_BUFFERSIZE, 32)");
    buffer_size.dwData = 0;
    CHECK(IDirectInputDevice8_GetProperty(keyboard, DIPROP_BUFFERSIZE, &buffer_size.diph) ==
              DI_OK && buffer_size.dwData == 32,
          "GetProperty reads the buffer size back");

    /* Re-acquire so the buffer starts clean, then feed it real events. */
    IDirectInputDevice8_Unacquire(keyboard);
    IDirectInputDevice8_Acquire(keyboard);

    push_key(SDL_SCANCODE_W, SDL_KEYDOWN);
    push_key(SDL_SCANCODE_W, SDL_KEYUP);
    push_key(SDL_SCANCODE_LSHIFT, SDL_KEYDOWN);
    pump();

    /*
     * Filtered to the keys this test pushed, rather than asserting an exact
     * count. The buffer is fed from the real SDL event stream, so a keypress
     * from whoever is at the machine (or a compositor-synthesised one) lands in
     * it too - and a test that fails when someone touches the keyboard is worse
     * than no test. What matters is that our three events arrive, in order,
     * with the right DIK offsets and press/release data.
     */
    event_count = 16;
    hr = IDirectInputDevice8_GetDeviceData(keyboard, sizeof(DIDEVICEOBJECTDATA), events,
                                           &event_count, 0);
    CHECK(hr == DI_OK, "GetDeviceData succeeded (%lu event(s) buffered)",
          (unsigned long)event_count);
    {
        DWORD expected_ofs[3] = { DIK_W, DIK_W, DIK_LSHIFT };
        DWORD expected_data[3] = { 0x80, 0x00, 0x80 };
        DWORD previous_sequence = 0;
        int matched = 0;
        int ordered = 1;
        DWORD i;

        for (i = 0; i < event_count && matched < 3; i++) {
            if (events[i].dwOfs != expected_ofs[matched] ||
                events[i].dwData != expected_data[matched])
                continue; /* someone else's keystroke */
            if (events[i].dwSequence <= previous_sequence)
                ordered = 0;
            previous_sequence = events[i].dwSequence;
            matched++;
        }
        CHECK(matched == 3,
              "the pushed sequence (DIK_W down, DIK_W up, DIK_LSHIFT down) arrived");
        CHECK(ordered, "sequence numbers increase across those events");
    }

    /* A non-peek read consumes: our keys must not still be there. */
    event_count = 16;
    IDirectInputDevice8_GetDeviceData(keyboard, sizeof(DIDEVICEOBJECTDATA), events,
                                      &event_count, 0);
    {
        DWORD i;
        int ours = 0;

        for (i = 0; i < event_count; i++) {
            if (events[i].dwOfs == DIK_W || events[i].dwOfs == DIK_LSHIFT)
                ours++;
        }
        CHECK(ours == 0, "the buffer no longer holds the events we already read");
    }

    /* DIK <-> scancode agreement through the public surface: pressing a physical
     * key must show up at the DIK offset the game will look at. */
    push_key(SDL_SCANCODE_ESCAPE, SDL_KEYDOWN);
    push_key(SDL_SCANCODE_ESCAPE, SDL_KEYUP);
    pump();
    event_count = 16;
    IDirectInputDevice8_GetDeviceData(keyboard, sizeof(DIDEVICEOBJECTDATA), events,
                                      &event_count, 0);
    {
        DWORD i;
        int down = 0, up = 0;

        for (i = 0; i < event_count; i++) {
            if (events[i].dwOfs != DIK_ESCAPE)
                continue;
            if (events[i].dwData == 0x80)
                down++;
            else
                up++;
        }
        CHECK(down == 1 && up == 1,
              "SDL escape maps to DIK_ESCAPE (0x%02x) for both press and release",
              DIK_ESCAPE);
    }

    /* --- mouse ---------------------------------------------------------- */
    hr = IDirectInput8_CreateDevice(dinput, &GUID_SysMouse, &mouse, NULL);
    CHECK(hr == DI_OK && mouse != NULL, "CreateDevice(GUID_SysMouse)");
    CHECK(IDirectInputDevice8_SetDataFormat(mouse, &c_dfDIMouse) == DI_OK,
          "SetDataFormat(c_dfDIMouse)");
    CHECK(IDirectInputDevice8_Acquire(mouse) == DI_OK, "Acquire(mouse)");

    memset(&mouse_state, 0xcc, sizeof(mouse_state));
    CHECK(IDirectInputDevice8_GetDeviceState(mouse, sizeof(mouse_state), &mouse_state) == DI_OK,
          "GetDeviceState(DIMOUSESTATE)");
    CHECK(mouse_state.rgbButtons[0] == 0 || mouse_state.rgbButtons[0] == 0x80,
          "mouse buttons read as 0 or 0x80, never anything else");

    memset(&caps, 0, sizeof(caps));
    caps.dwSize = sizeof(caps);
    CHECK(IDirectInputDevice8_GetCapabilities(mouse, &caps) == DI_OK && caps.dwAxes == 3,
          "the mouse reports three axes");

    /* --- refcounts and teardown ----------------------------------------- */
    CHECK(IDirectInputDevice8_AddRef(keyboard) == 2 &&
              IDirectInputDevice8_Release(keyboard) == 1,
          "AddRef/Release track the refcount");

    CHECK(IDirectInputDevice8_Unacquire(mouse) == DI_OK, "Unacquire");
    CHECK(IDirectInputDevice8_Unacquire(mouse) == DI_NOEFFECT,
          "a second Unacquire returns DI_NOEFFECT");

    CHECK(IDirectInputDevice8_Release(mouse) == 0, "releasing the mouse frees it");
    CHECK(IDirectInputDevice8_Release(keyboard) == 0, "releasing the keyboard frees it");
    CHECK(IDirectInput8_Release(dinput) == 0, "releasing the factory frees it");

    /* ddraw, which the game probes before committing to D3D9. */
    {
        LPDIRECTDRAW ddraw = (LPDIRECTDRAW)1;
        CHECK(DirectDrawCreate(NULL, &ddraw, NULL) == DDERR_NODIRECTDRAWSUPPORT &&
                  ddraw == NULL,
              "DirectDrawCreate fails cleanly and clears the out pointer");
    }

    nfsu2_win32_shutdown();
    printf("\n%s (%d failure%s, %d-bit)\n", g_failures ? "FAILED" : "PASSED",
           g_failures, g_failures == 1 ? "" : "s", (int)(sizeof(void *) * 8));
    return g_failures ? 1 : 0;
}
