/*
 * data_formats.c - the standard DIDATAFORMAT constants.
 *
 * c_dfDIKeyboard, c_dfDIMouse, c_dfDIMouse2, c_dfDIJoystick and c_dfDIJoystick2
 * are *exported data*, not functions: on Windows a game links them out of
 * dinput8.lib and passes them straight to SetDataFormat. There is no such
 * library here, so they have to be defined.
 *
 * The object arrays are filled by a constructor rather than written out as
 * static initialisers - the keyboard alone has 256 entries, and a hand-written
 * table that large is a liability rather than documentation. The DIDATAFORMAT
 * headers themselves are const, as their declarations require; only the arrays
 * they point at are mutable, which is what makes the constructor legal.
 *
 * Our SetDataFormat only reads dwDataSize (enough to tell DIMOUSESTATE from
 * DIMOUSESTATE2, and DIJOYSTATE from DIJOYSTATE2 - see device.c), but the object
 * lists are filled in properly anyway, because a caller is entitled to walk them
 * and finding dwNumObjs entries of zeroes would be worse than useless.
 */
#include "dinput8_internal.h"

#include <string.h>

/* Instantiated in dinput8.c, which defines INITGUID. */
extern const GUID GUID_Key;
extern const GUID GUID_XAxis;
extern const GUID GUID_YAxis;
extern const GUID GUID_ZAxis;
extern const GUID GUID_RxAxis;
extern const GUID GUID_RyAxis;
extern const GUID GUID_RzAxis;
extern const GUID GUID_Slider;
extern const GUID GUID_Button;
extern const GUID GUID_POV;

#define KEYBOARD_OBJECTS 256
#define MOUSE_OBJECTS    7   /* 3 axes + 4 buttons */
#define MOUSE2_OBJECTS   11  /* 3 axes + 8 buttons */
#define JOYSTICK_OBJECTS 44  /* 6 axes + 2 sliders + 4 POVs + 32 buttons */
#define JOYSTICK2_OBJECTS 44 /* the prefix we model; DIJOYSTATE2 has more */

static DIOBJECTDATAFORMAT g_keyboard_objects[KEYBOARD_OBJECTS];
static DIOBJECTDATAFORMAT g_mouse_objects[MOUSE_OBJECTS];
static DIOBJECTDATAFORMAT g_mouse2_objects[MOUSE2_OBJECTS];
static DIOBJECTDATAFORMAT g_joystick_objects[JOYSTICK_OBJECTS];
static DIOBJECTDATAFORMAT g_joystick2_objects[JOYSTICK2_OBJECTS];

const DIDATAFORMAT c_dfDIKeyboard = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    KEYBOARD_OBJECTS, KEYBOARD_OBJECTS, g_keyboard_objects
};

const DIDATAFORMAT c_dfDIMouse = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE), MOUSE_OBJECTS, g_mouse_objects
};

const DIDATAFORMAT c_dfDIMouse2 = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE2), MOUSE2_OBJECTS, g_mouse2_objects
};

const DIDATAFORMAT c_dfDIJoystick = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
    sizeof(DIJOYSTATE), JOYSTICK_OBJECTS, g_joystick_objects
};

const DIDATAFORMAT c_dfDIJoystick2 = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS,
    sizeof(DIJOYSTATE2), JOYSTICK2_OBJECTS, g_joystick2_objects
};

static void set_object(DIOBJECTDATAFORMAT *object, const GUID *guid, DWORD offset, DWORD type)
{
    object->pguid = guid;
    object->dwOfs = offset;
    object->dwType = type;
    object->dwFlags = 0;
}

static void __attribute__((constructor)) build_data_formats(void)
{
    int i;

    for (i = 0; i < KEYBOARD_OBJECTS; i++)
        set_object(&g_keyboard_objects[i], &GUID_Key, (DWORD)i,
                   DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL);

    /* Mouse: lX, lY, lZ then the buttons, matching DIMOUSESTATE's layout. */
    set_object(&g_mouse_objects[0], &GUID_XAxis, DIMOFS_X, DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(0));
    set_object(&g_mouse_objects[1], &GUID_YAxis, DIMOFS_Y, DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(1));
    set_object(&g_mouse_objects[2], &GUID_ZAxis, DIMOFS_Z,
               DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(2) | DIDFT_OPTIONAL);
    for (i = 0; i < 4; i++)
        set_object(&g_mouse_objects[3 + i], &GUID_Button, (DWORD)(DIMOFS_BUTTON0 + i),
                   DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL);

    memcpy(g_mouse2_objects, g_mouse_objects, sizeof(g_mouse_objects));
    for (i = 4; i < 8; i++)
        set_object(&g_mouse2_objects[3 + i], &GUID_Button, (DWORD)(DIMOFS_BUTTON0 + i),
                   DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL);

    /* Joystick: the DIJOYSTATE field order, which device.c fills to match. */
    set_object(&g_joystick_objects[0], &GUID_XAxis, DIJOFS_X,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0) | DIDFT_OPTIONAL);
    set_object(&g_joystick_objects[1], &GUID_YAxis, DIJOFS_Y,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1) | DIDFT_OPTIONAL);
    set_object(&g_joystick_objects[2], &GUID_ZAxis, DIJOFS_Z,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2) | DIDFT_OPTIONAL);
    set_object(&g_joystick_objects[3], &GUID_RxAxis, DIJOFS_RX,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3) | DIDFT_OPTIONAL);
    set_object(&g_joystick_objects[4], &GUID_RyAxis, DIJOFS_RY,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4) | DIDFT_OPTIONAL);
    set_object(&g_joystick_objects[5], &GUID_RzAxis, DIJOFS_RZ,
               DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(5) | DIDFT_OPTIONAL);
    for (i = 0; i < 2; i++)
        set_object(&g_joystick_objects[6 + i], &GUID_Slider, (DWORD)DIJOFS_SLIDER(i),
                   DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(6 + i) | DIDFT_OPTIONAL);
    for (i = 0; i < 4; i++)
        set_object(&g_joystick_objects[8 + i], &GUID_POV, (DWORD)DIJOFS_POV(i),
                   DIDFT_POV | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL);
    for (i = 0; i < 32; i++)
        set_object(&g_joystick_objects[12 + i], &GUID_Button, (DWORD)DIJOFS_BUTTON(i),
                   DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i) | DIDFT_OPTIONAL);

    memcpy(g_joystick2_objects, g_joystick_objects, sizeof(g_joystick_objects));
}
