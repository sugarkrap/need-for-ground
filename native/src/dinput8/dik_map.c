/*
 * dik_map.c - DirectInput scancodes (DIK_*) <-> SDL scancodes.
 *
 * This is a *different* mapping from user32/keymap.c's, and both are needed:
 * window messages carry virtual-key codes (VK_*, layout-dependent on Windows),
 * while DirectInput reports raw PS/2 set-1 scancodes (DIK_*, always physical).
 * A game reads WASD as VK_W..VK_D through WM_KEYDOWN and as DIK_W..DIK_D
 * through GetDeviceState, and the two numbering schemes have nothing in common
 * (DIK_W is 0x11, VK_W is 0x57).
 *
 * SDL scancodes are USB HID usages, also physical, so this is a pure
 * position-to-position table with no layout question to answer - which is why
 * it can be exhaustive and exact where the VK table needed a judgement call.
 */
#include "dinput8_internal.h"

struct dik_pair {
    unsigned char dik;
    SDL_Scancode scancode;
};

static const struct dik_pair g_pairs[] = {
    { DIK_ESCAPE,       SDL_SCANCODE_ESCAPE },
    { DIK_1,            SDL_SCANCODE_1 },
    { DIK_2,            SDL_SCANCODE_2 },
    { DIK_3,            SDL_SCANCODE_3 },
    { DIK_4,            SDL_SCANCODE_4 },
    { DIK_5,            SDL_SCANCODE_5 },
    { DIK_6,            SDL_SCANCODE_6 },
    { DIK_7,            SDL_SCANCODE_7 },
    { DIK_8,            SDL_SCANCODE_8 },
    { DIK_9,            SDL_SCANCODE_9 },
    { DIK_0,            SDL_SCANCODE_0 },
    { DIK_MINUS,        SDL_SCANCODE_MINUS },
    { DIK_EQUALS,       SDL_SCANCODE_EQUALS },
    { DIK_BACK,         SDL_SCANCODE_BACKSPACE },
    { DIK_TAB,          SDL_SCANCODE_TAB },
    { DIK_Q,            SDL_SCANCODE_Q },
    { DIK_W,            SDL_SCANCODE_W },
    { DIK_E,            SDL_SCANCODE_E },
    { DIK_R,            SDL_SCANCODE_R },
    { DIK_T,            SDL_SCANCODE_T },
    { DIK_Y,            SDL_SCANCODE_Y },
    { DIK_U,            SDL_SCANCODE_U },
    { DIK_I,            SDL_SCANCODE_I },
    { DIK_O,            SDL_SCANCODE_O },
    { DIK_P,            SDL_SCANCODE_P },
    { DIK_LBRACKET,     SDL_SCANCODE_LEFTBRACKET },
    { DIK_RBRACKET,     SDL_SCANCODE_RIGHTBRACKET },
    { DIK_RETURN,       SDL_SCANCODE_RETURN },
    { DIK_LCONTROL,     SDL_SCANCODE_LCTRL },
    { DIK_A,            SDL_SCANCODE_A },
    { DIK_S,            SDL_SCANCODE_S },
    { DIK_D,            SDL_SCANCODE_D },
    { DIK_F,            SDL_SCANCODE_F },
    { DIK_G,            SDL_SCANCODE_G },
    { DIK_H,            SDL_SCANCODE_H },
    { DIK_J,            SDL_SCANCODE_J },
    { DIK_K,            SDL_SCANCODE_K },
    { DIK_L,            SDL_SCANCODE_L },
    { DIK_SEMICOLON,    SDL_SCANCODE_SEMICOLON },
    { DIK_APOSTROPHE,   SDL_SCANCODE_APOSTROPHE },
    { DIK_GRAVE,        SDL_SCANCODE_GRAVE },
    { DIK_LSHIFT,       SDL_SCANCODE_LSHIFT },
    { DIK_BACKSLASH,    SDL_SCANCODE_BACKSLASH },
    { DIK_Z,            SDL_SCANCODE_Z },
    { DIK_X,            SDL_SCANCODE_X },
    { DIK_C,            SDL_SCANCODE_C },
    { DIK_V,            SDL_SCANCODE_V },
    { DIK_B,            SDL_SCANCODE_B },
    { DIK_N,            SDL_SCANCODE_N },
    { DIK_M,            SDL_SCANCODE_M },
    { DIK_COMMA,        SDL_SCANCODE_COMMA },
    { DIK_PERIOD,       SDL_SCANCODE_PERIOD },
    { DIK_SLASH,        SDL_SCANCODE_SLASH },
    { DIK_RSHIFT,       SDL_SCANCODE_RSHIFT },
    { DIK_MULTIPLY,     SDL_SCANCODE_KP_MULTIPLY },
    { DIK_LMENU,        SDL_SCANCODE_LALT },
    { DIK_SPACE,        SDL_SCANCODE_SPACE },
    { DIK_CAPITAL,      SDL_SCANCODE_CAPSLOCK },
    { DIK_F1,           SDL_SCANCODE_F1 },
    { DIK_F2,           SDL_SCANCODE_F2 },
    { DIK_F3,           SDL_SCANCODE_F3 },
    { DIK_F4,           SDL_SCANCODE_F4 },
    { DIK_F5,           SDL_SCANCODE_F5 },
    { DIK_F6,           SDL_SCANCODE_F6 },
    { DIK_F7,           SDL_SCANCODE_F7 },
    { DIK_F8,           SDL_SCANCODE_F8 },
    { DIK_F9,           SDL_SCANCODE_F9 },
    { DIK_F10,          SDL_SCANCODE_F10 },
    { DIK_NUMLOCK,      SDL_SCANCODE_NUMLOCKCLEAR },
    { DIK_SCROLL,       SDL_SCANCODE_SCROLLLOCK },
    { DIK_NUMPAD7,      SDL_SCANCODE_KP_7 },
    { DIK_NUMPAD8,      SDL_SCANCODE_KP_8 },
    { DIK_NUMPAD9,      SDL_SCANCODE_KP_9 },
    { DIK_SUBTRACT,     SDL_SCANCODE_KP_MINUS },
    { DIK_NUMPAD4,      SDL_SCANCODE_KP_4 },
    { DIK_NUMPAD5,      SDL_SCANCODE_KP_5 },
    { DIK_NUMPAD6,      SDL_SCANCODE_KP_6 },
    { DIK_ADD,          SDL_SCANCODE_KP_PLUS },
    { DIK_NUMPAD1,      SDL_SCANCODE_KP_1 },
    { DIK_NUMPAD2,      SDL_SCANCODE_KP_2 },
    { DIK_NUMPAD3,      SDL_SCANCODE_KP_3 },
    { DIK_NUMPAD0,      SDL_SCANCODE_KP_0 },
    { DIK_DECIMAL,      SDL_SCANCODE_KP_PERIOD },
    /* The extra key on a 102-key (non-US) layout. The DX SDK calls it
     * DIK_OEM_102; Wine's dinput.h does not define that name, so the set-1
     * scancode is spelled out. It matters here - this is a French build. */
    { 0x56,             SDL_SCANCODE_NONUSBACKSLASH },
    { DIK_F11,          SDL_SCANCODE_F11 },
    { DIK_F12,          SDL_SCANCODE_F12 },
    { DIK_NUMPADENTER,  SDL_SCANCODE_KP_ENTER },
    { DIK_RCONTROL,     SDL_SCANCODE_RCTRL },
    { DIK_DIVIDE,       SDL_SCANCODE_KP_DIVIDE },
    { DIK_SYSRQ,        SDL_SCANCODE_PRINTSCREEN },
    { DIK_RMENU,        SDL_SCANCODE_RALT },
    { DIK_PAUSE,        SDL_SCANCODE_PAUSE },
    { DIK_HOME,         SDL_SCANCODE_HOME },
    { DIK_UP,           SDL_SCANCODE_UP },
    { DIK_PRIOR,        SDL_SCANCODE_PAGEUP },
    { DIK_LEFT,         SDL_SCANCODE_LEFT },
    { DIK_RIGHT,        SDL_SCANCODE_RIGHT },
    { DIK_END,          SDL_SCANCODE_END },
    { DIK_DOWN,         SDL_SCANCODE_DOWN },
    { DIK_NEXT,         SDL_SCANCODE_PAGEDOWN },
    { DIK_INSERT,       SDL_SCANCODE_INSERT },
    { DIK_DELETE,       SDL_SCANCODE_DELETE },
    { DIK_LWIN,         SDL_SCANCODE_LGUI },
    { DIK_RWIN,         SDL_SCANCODE_RGUI },
    { DIK_APPS,         SDL_SCANCODE_APPLICATION },
};

#define PAIR_COUNT (sizeof(g_pairs) / sizeof(g_pairs[0]))

static SDL_Scancode g_dik_to_sdl[256];
static unsigned char g_sdl_to_dik[SDL_NUM_SCANCODES];
static int g_built;

static void build_tables(void)
{
    size_t i;

    if (g_built)
        return;
    for (i = 0; i < PAIR_COUNT; i++) {
        g_dik_to_sdl[g_pairs[i].dik] = g_pairs[i].scancode;
        g_sdl_to_dik[g_pairs[i].scancode] = g_pairs[i].dik;
    }
    g_built = 1;
}

SDL_Scancode nfsu2_sdl_scancode_from_dik(unsigned char dik)
{
    build_tables();
    return g_dik_to_sdl[dik];
}

unsigned char nfsu2_dik_from_sdl_scancode(SDL_Scancode scancode)
{
    build_tables();
    if (scancode < 0 || scancode >= SDL_NUM_SCANCODES)
        return 0;
    return g_sdl_to_dik[scancode];
}
