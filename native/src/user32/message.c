/*
 * message.c - the message queue and SDL-event -> WM_* translation.
 *
 * One queue per thread, because PostThreadMessageA exists and the game's
 * loader thread uses it. The queue that matters is the one belonging to the
 * thread that pumps SDL, which SDL requires to be the thread that initialised
 * video - i.e. the game's main thread.
 */
#include "user32_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QUEUE_CAPACITY 256
#define MAX_QUEUES 16

struct msg_queue {
    DWORD thread_id;
    int in_use;
    MSG items[QUEUE_CAPACITY];
    int head;
    int count;
    int quit_posted;
    int quit_code;
    pthread_mutex_t lock;
};

static struct msg_queue g_queues[MAX_QUEUES];
static pthread_mutex_t g_queues_lock = PTHREAD_MUTEX_INITIALIZER;

static struct msg_queue *queue_for_thread(DWORD thread_id, int create)
{
    struct msg_queue *found = NULL;
    int i;

    pthread_mutex_lock(&g_queues_lock);
    for (i = 0; i < MAX_QUEUES; i++) {
        if (g_queues[i].in_use && g_queues[i].thread_id == thread_id) {
            found = &g_queues[i];
            break;
        }
    }
    if (!found && create) {
        for (i = 0; i < MAX_QUEUES; i++) {
            if (!g_queues[i].in_use) {
                g_queues[i].in_use = 1;
                g_queues[i].thread_id = thread_id;
                g_queues[i].head = 0;
                g_queues[i].count = 0;
                g_queues[i].quit_posted = 0;
                pthread_mutex_init(&g_queues[i].lock, NULL);
                found = &g_queues[i];
                break;
            }
        }
        if (!found)
            nfsu2_shim_trace("message queues exhausted (%d threads)", MAX_QUEUES);
    }
    pthread_mutex_unlock(&g_queues_lock);
    return found;
}

static struct msg_queue *current_queue(void)
{
    return queue_for_thread(GetCurrentThreadId(), 1);
}

static BOOL queue_push(struct msg_queue *q, const MSG *msg)
{
    BOOL ok = TRUE;

    pthread_mutex_lock(&q->lock);
    if (q->count == QUEUE_CAPACITY) {
        /* Dropping the newest keeps the oldest input in order, which is the
         * lesser evil for a queue that only overflows if nothing is pumping. */
        nfsu2_shim_trace("message queue full, dropping message 0x%04x", msg->message);
        ok = FALSE;
    } else {
        q->items[(q->head + q->count) % QUEUE_CAPACITY] = *msg;
        q->count++;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

/* Take the first message matching the filters. Returns 0 if none. */
static int queue_take(struct msg_queue *q, MSG *out, HWND hwnd,
                      UINT filter_min, UINT filter_max, int remove)
{
    int found = 0;
    int i;

    pthread_mutex_lock(&q->lock);
    for (i = 0; i < q->count; i++) {
        MSG *candidate = &q->items[(q->head + i) % QUEUE_CAPACITY];

        if (hwnd && candidate->hwnd != hwnd)
            continue;
        if (filter_max != 0 &&
            (candidate->message < filter_min || candidate->message > filter_max))
            continue;

        *out = *candidate;
        found = 1;
        if (remove) {
            /* Close the gap; the queue is short and this is not a hot path
             * unless a filter is in use. */
            int j;
            for (j = i; j > 0; j--) {
                int dst = (q->head + j) % QUEUE_CAPACITY;
                int src = (q->head + j - 1) % QUEUE_CAPACITY;
                q->items[dst] = q->items[src];
            }
            q->head = (q->head + 1) % QUEUE_CAPACITY;
            q->count--;
        }
        break;
    }
    pthread_mutex_unlock(&q->lock);
    return found;
}

void nfsu2_msg_drop_window(HWND hwnd)
{
    struct msg_queue *q = current_queue();
    MSG kept[QUEUE_CAPACITY];
    int kept_count = 0;
    int i;

    if (!q)
        return;
    pthread_mutex_lock(&q->lock);
    for (i = 0; i < q->count; i++) {
        MSG *m = &q->items[(q->head + i) % QUEUE_CAPACITY];
        if (m->hwnd != hwnd)
            kept[kept_count++] = *m;
    }
    for (i = 0; i < kept_count; i++)
        q->items[i] = kept[i];
    q->head = 0;
    q->count = kept_count;
    pthread_mutex_unlock(&q->lock);
}

BOOL nfsu2_msg_post(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct msg_queue *q = current_queue();
    MSG msg;

    if (!q) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = message;
    msg.wParam = wparam;
    msg.lParam = lparam;
    msg.time = GetTickCount();
    return queue_push(q, &msg);
}

/* --- SDL -> WM_* translation ------------------------------------------- */

WPARAM nfsu2_mouse_key_flags(void)
{
    Uint32 buttons = SDL_GetMouseState(NULL, NULL);
    SDL_Keymod mods = SDL_GetModState();
    WPARAM flags = 0;

    if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))
        flags |= MK_LBUTTON;
    if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))
        flags |= MK_RBUTTON;
    if (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE))
        flags |= MK_MBUTTON;
    if (mods & KMOD_SHIFT)
        flags |= MK_SHIFT;
    if (mods & KMOD_CTRL)
        flags |= MK_CONTROL;
    return flags;
}

static LPARAM mouse_lparam(int x, int y)
{
    return (LPARAM)(((y & 0xffff) << 16) | (x & 0xffff));
}

static void post_to_window(SDL_Window *sdl, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (!sdl)
        return;
    nfsu2_msg_post(nfsu2_hwnd_from_sdl(sdl), message, wparam, lparam);
}

static void post_to_focus(UINT message, WPARAM wparam, LPARAM lparam)
{
    post_to_window(SDL_GetKeyboardFocus(), message, wparam, lparam);
}

static void translate_key(const SDL_KeyboardEvent *key, int down)
{
    WPARAM vk = nfsu2_vk_from_scancode(key->keysym.scancode);
    LPARAM lparam;

    if (!vk) {
        nfsu2_shim_trace("unmapped SDL scancode %d", key->keysym.scancode);
        return;
    }
    /*
     * lParam layout: repeat count (0-15), scan code (16-23), extended (24),
     * previous state (30), transition (31). Games read the repeat and
     * transition bits; the rest is filled in for completeness.
     */
    lparam = 1;
    lparam |= (LPARAM)(key->keysym.scancode & 0xff) << 16;
    if (key->repeat)
        lparam |= 1L << 30;
    if (!down)
        lparam |= (1L << 30) | (1L << 31);

    post_to_window(SDL_GetWindowFromID(key->windowID),
                   down ? WM_KEYDOWN : WM_KEYUP, vk, lparam);
}

static void translate_text(const SDL_TextInputEvent *text)
{
    const unsigned char *p = (const unsigned char *)text->text;

    /*
     * WM_CHAR carries one ANSI character. SDL gives UTF-8; pass ASCII through
     * and fold two-byte sequences down to Latin-1 where they fit, which covers
     * the accented characters a French keyboard produces. Anything wider is
     * dropped rather than mangled - this game's text input is limited to
     * profile names.
     */
    while (*p) {
        unsigned int cp;

        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xe0) == 0xc0 && p[1]) {
            cp = (unsigned int)((*p & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else {
            nfsu2_shim_trace("WM_CHAR: dropping non-Latin-1 input");
            return;
        }
        if (cp > 0xff)
            continue;
        post_to_window(SDL_GetWindowFromID(text->windowID), WM_CHAR, cp, 1);
    }
}

static void translate_window_event(const SDL_WindowEvent *ev)
{
    SDL_Window *sdl = SDL_GetWindowFromID(ev->windowID);

    switch (ev->event) {
    case SDL_WINDOWEVENT_CLOSE:
        post_to_window(sdl, WM_CLOSE, 0, 0);
        break;
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        post_to_window(sdl, WM_ACTIVATEAPP, TRUE, 0);
        post_to_window(sdl, WM_ACTIVATE, WA_ACTIVE, 0);
        post_to_window(sdl, WM_SETFOCUS, 0, 0);
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        post_to_window(sdl, WM_KILLFOCUS, 0, 0);
        post_to_window(sdl, WM_ACTIVATE, WA_INACTIVE, 0);
        post_to_window(sdl, WM_ACTIVATEAPP, FALSE, 0);
        break;
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        post_to_window(sdl, WM_SIZE, SIZE_RESTORED, mouse_lparam(ev->data1, ev->data2));
        break;
    case SDL_WINDOWEVENT_MOVED:
        post_to_window(sdl, WM_MOVE, 0, mouse_lparam(ev->data1, ev->data2));
        break;
    case SDL_WINDOWEVENT_MINIMIZED:
        post_to_window(sdl, WM_SIZE, SIZE_MINIMIZED, 0);
        break;
    case SDL_WINDOWEVENT_RESTORED:
    case SDL_WINDOWEVENT_MAXIMIZED: {
        int w = 0, h = 0;
        if (sdl)
            SDL_GetWindowSize(sdl, &w, &h);
        post_to_window(sdl, WM_SIZE,
                       ev->event == SDL_WINDOWEVENT_MAXIMIZED ? SIZE_MAXIMIZED : SIZE_RESTORED,
                       mouse_lparam(w, h));
        break;
    }
    case SDL_WINDOWEVENT_EXPOSED:
        post_to_window(sdl, WM_PAINT, 0, 0);
        break;
    default:
        break;
    }
}

static void translate_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_QUIT:
        /* Windows has no process-level quit event: the window gets WM_CLOSE and
         * the app decides. Route it the same way. */
        post_to_focus(WM_CLOSE, 0, 0);
        break;
    case SDL_WINDOWEVENT:
        translate_window_event(&ev->window);
        break;
    case SDL_KEYDOWN:
        translate_key(&ev->key, 1);
        break;
    case SDL_KEYUP:
        translate_key(&ev->key, 0);
        break;
    case SDL_TEXTINPUT:
        translate_text(&ev->text);
        break;
    case SDL_MOUSEMOTION:
        post_to_window(SDL_GetWindowFromID(ev->motion.windowID), WM_MOUSEMOVE,
                       nfsu2_mouse_key_flags(), mouse_lparam(ev->motion.x, ev->motion.y));
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int down = ev->type == SDL_MOUSEBUTTONDOWN;
        UINT message;

        switch (ev->button.button) {
        case SDL_BUTTON_LEFT:   message = down ? WM_LBUTTONDOWN : WM_LBUTTONUP; break;
        case SDL_BUTTON_RIGHT:  message = down ? WM_RBUTTONDOWN : WM_RBUTTONUP; break;
        case SDL_BUTTON_MIDDLE: message = down ? WM_MBUTTONDOWN : WM_MBUTTONUP; break;
        default: return;
        }
        post_to_window(SDL_GetWindowFromID(ev->button.windowID), message,
                       nfsu2_mouse_key_flags(), mouse_lparam(ev->button.x, ev->button.y));
        break;
    }
    case SDL_MOUSEWHEEL: {
        int x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        post_to_window(SDL_GetWindowFromID(ev->wheel.windowID), WM_MOUSEWHEEL,
                       (WPARAM)((ev->wheel.y * WHEEL_DELTA) << 16) | nfsu2_mouse_key_flags(),
                       mouse_lparam(x, y));
        break;
    }
    default:
        break;
    }
}

void nfsu2_msg_pump_sdl(void)
{
    SDL_Event ev;

    if (!SDL_WasInit(SDL_INIT_VIDEO))
        return;
    while (SDL_PollEvent(&ev))
        translate_event(&ev);
}

/* --- the Win32 entry points -------------------------------------------- */

BOOL WINAPI PeekMessageA(LPMSG msg, HWND hwnd, UINT filter_min, UINT filter_max, UINT flags)
{
    struct msg_queue *q = current_queue();

    if (!msg || !q) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    nfsu2_msg_pump_sdl();

    if (queue_take(q, msg, hwnd, filter_min, filter_max, (flags & PM_REMOVE) != 0))
        return TRUE;

    /* WM_QUIT is synthesised after the queue drains, as on Windows. */
    if (q->quit_posted) {
        memset(msg, 0, sizeof(*msg));
        msg->message = WM_QUIT;
        msg->wParam = (WPARAM)q->quit_code;
        if (flags & PM_REMOVE)
            q->quit_posted = 0;
        return TRUE;
    }
    return FALSE;
}

BOOL WINAPI GetMessageA(LPMSG msg, HWND hwnd, UINT filter_min, UINT filter_max)
{
    if (!msg) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (;;) {
        if (PeekMessageA(msg, hwnd, filter_min, filter_max, PM_REMOVE))
            return msg->message == WM_QUIT ? FALSE : TRUE;
        /* Block in SDL rather than spinning; the timeout keeps us responsive to
         * messages posted by other threads, which SDL knows nothing about. */
        SDL_WaitEventTimeout(NULL, 10);
    }
}

BOOL WINAPI TranslateMessage(const MSG *msg)
{
    /*
     * Nothing to do: WM_CHAR is generated from SDL_TEXTINPUT during the pump,
     * because that is where the platform does keyboard-layout and dead-key
     * composition. Returning TRUE for key messages keeps callers that check
     * the result behaving as they would on Windows.
     */
    if (!msg)
        return FALSE;
    return (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP) ? TRUE : FALSE;
}

LRESULT WINAPI DispatchMessageA(const MSG *msg)
{
    struct nfsu2_window *state;

    if (!msg) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!msg->hwnd)
        return 0; /* thread message: no window to route it to */

    state = nfsu2_window_state(msg->hwnd);
    if (!state || state->destroyed)
        return 0;

    return state->wndproc(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

BOOL WINAPI PostMessageA(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    return nfsu2_msg_post(hwnd, message, wparam, lparam);
}

BOOL WINAPI PostThreadMessageA(DWORD thread_id, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct msg_queue *q = queue_for_thread(thread_id, 1);
    MSG msg;

    if (!q) {
        SetLastError(ERROR_INVALID_THREAD_ID);
        return FALSE;
    }
    memset(&msg, 0, sizeof(msg));
    msg.message = message;
    msg.wParam = wparam;
    msg.lParam = lparam;
    msg.time = GetTickCount();
    return queue_push(q, &msg);
}

VOID WINAPI PostQuitMessage(int exit_code)
{
    struct msg_queue *q = current_queue();

    if (!q)
        return;
    q->quit_code = exit_code;
    q->quit_posted = 1;
}

LRESULT WINAPI DefWindowProcA(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)wparam; (void)lparam;

    switch (message) {
    case WM_CLOSE:
        /* What DefWindowProc does on Windows, and what makes the standard
         * "WM_CLOSE -> WM_DESTROY -> PostQuitMessage" chain work. */
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        if (BeginPaint(hwnd, &ps))
            EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; /* claim it: the renderer owns every pixel */
    case WM_SETCURSOR:
    case WM_NCHITTEST:
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_SIZE:
    case WM_MOVE:
        return 0;
    default:
        return 0;
    }
}
