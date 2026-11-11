/* nn::hid: one full-key controller (docked) or the handheld controller, fed by the keyboard and game controllers.
 * Ported from MacNativeGames games/totk/runtime/input.c @b75be8c (MIT); state ABI follows libnx HID. Changes: key codes
 * stay MacNativeGames' (Mac virtual key codes) and the window layer translates its own keys into them; the game
 * controller source is runtime_gamepad_read (none until the window / SDL3 layer of milestone 3.2); the diagnostic input
 * script / file commands are not ported. */
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "../runtime/hle.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 40, "Npad full-key ABI");

int runtime_handheld_mode(void);

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char keys[128];
static uint64_t pending;
static uint32_t styles = 1;
static bool enabled, allowed = true;
static int64_t sample;

/* Mac virtual key codes -> Switch buttons (MacNativeGames' layout: J=A, K/Space=B, I=X, U=Y, Z/C=L/R ...). */
static uint64_t button(unsigned code) {
    switch (code) {
    case 38: return 1; case 40: case 49: return 2; case 34: return 4; case 32: return 8; case 6: return 16; case 8: return 32;
    case 12: return 64; case 14: return 128; case 58: return 256; case 56: return 512; case 36: return 1024;
    case 48: case 53: return 2048; case 3: return 4096; case 17: return 8192; case 4: return 16384; case 5: return 32768;
    default: return 0;
    }
}

/* Game controllers: filled by the window layer (SDL3, milestone 3.2). */
static RuntimeGamepadState g_pad;
void runtime_gamepad_set(const RuntimeGamepadState *s) { pthread_mutex_lock(&input_lock); g_pad = *s; pthread_mutex_unlock(&input_lock); }
static void gamepad_read(RuntimeGamepadState *out) { *out = g_pad; }

void runtime_input_key(unsigned code, int down) {
    if (code >= 128) return;
    pthread_mutex_lock(&input_lock);
    if (down && !keys[code]) pending |= button(code);
    keys[code] = (unsigned char)!!down;
    pthread_mutex_unlock(&input_lock);
}
static void mouse_clear(void);   /* the mouse's counts and notches dropped (below) */
void runtime_input_clear(void) {
    pthread_mutex_lock(&input_lock);
    memset(keys, 0, sizeof keys);
    pending = 0;
    memset(&g_pad, 0, sizeof g_pad);
    pthread_mutex_unlock(&input_lock);
    mouse_clear();
}

/* TOTK_INPUT_SCRIPT=<frame>:<key>[*<frames held>],... (development: drive the game without a person, e.g.
   "520:J,700:J*30,900:W*120"). Frames are the graphics frontend's frames; keys are named as on the keyboard
   (A-Z, RETURN, SPACE, TAB, ESCAPE, UP, DOWN, LEFT, RIGHT, LSHIFT, LALT) and go through the same layout as typing;
   MX=<counts> / MY=<counts> move the mouse that many counts a frame for the frames held (e.g. "1500:MX=20*60"). */
static int script_code(const char *k, size_t n) {
    static const struct { const char *name; int code; } named[] = {
        {"RETURN", 36}, {"SPACE", 49}, {"TAB", 48}, {"ESCAPE", 53}, {"UP", 126}, {"DOWN", 125}, {"LEFT", 123},
        {"RIGHT", 124}, {"LSHIFT", 56}, {"LALT", 58}};
    static const int letters[26] = {0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6};
    if (n == 1 && k[0] >= 'A' && k[0] <= 'Z') return letters[k[0] - 'A'];
    for (unsigned i = 0; i < sizeof named / sizeof *named; i++)
        if (strlen(named[i].name) == n && !strncmp(named[i].name, k, n)) return named[i].code;
    return -1;
}
void runtime_input_script_tick(uint64_t frame) {
    static const char *script;
    static int init;
    if (!init) { init = 1; script = getenv("TOTK_INPUT_SCRIPT"); }
    for (const char *p = script; p && *p;) {
        char *end;
        uint64_t at = strtoull(p, &end, 10);
        const char *k = *end == ':' ? end + 1 : end;
        size_t n = strcspn(k, "*,");
        uint64_t hold = k[n] == '*' ? strtoull(k + n + 1, NULL, 10) : 6;
        int code = script_code(k, n);
        if (code >= 0 && frame == at) { fprintf(stderr, "[totk] input script: frame %llu press %.*s\n", (unsigned long long)frame, (int)n, k); runtime_input_key((unsigned)code, 1); }
        if (code >= 0 && frame == at + hold) runtime_input_key((unsigned)code, 0);
        /* MX=<counts> / MY=<counts>: mouse motion, that many counts each frame over the hold (default 6 frames) */
        if (n > 3 && k[0] == 'M' && (k[1] == 'X' || k[1] == 'Y') && k[2] == '=' && frame >= at && frame < at + hold) {
            float v = (float)strtod(k + 3, NULL);
            if (frame == at) fprintf(stderr, "[totk] input script: frame %llu mouse %.*s\n", (unsigned long long)frame, (int)n, k);
            runtime_mouse_motion(k[1] == 'X' ? v : 0, k[1] == 'Y' ? v : 0);
        }
        p = strchr(k, ',');
        if (p) p++;
    }
}

/* The mouse (window_sdl.c while it holds the mouse): counts and wheel notches gathered for the camera hooks
   (mouse_direct.c takes the counts at each camera step, so a turn follows the mouse whatever the frame rate) and for the
   ability wheel and the D-pad strips below. */
static int64_t g_mouse[2];     /* counts not yet taken */
static int32_t g_wheel;        /* wheel notches not yet taken, + away from the player */
static float g_mouse_rest[3];  /* the window thread's fractions of a count / notch */
static uint64_t g_buttons_last;
static int g_selector;
void runtime_mouse_motion(float dx, float dy) {
    float x = g_mouse_rest[0] + dx, y = g_mouse_rest[1] + dy;
    int64_t ix = (int64_t)x, iy = (int64_t)y;
    g_mouse_rest[0] = x - (float)ix, g_mouse_rest[1] = y - (float)iy;
    if (ix) __atomic_add_fetch(&g_mouse[0], ix, __ATOMIC_RELAXED);
    if (iy) __atomic_add_fetch(&g_mouse[1], iy, __ATOMIC_RELAXED);
}
void runtime_mouse_wheel(float notches) {
    float w = g_mouse_rest[2] + notches;
    int32_t n = (int32_t)w;
    g_mouse_rest[2] = w - (float)n;
    if (n) __atomic_add_fetch(&g_wheel, n, __ATOMIC_RELAXED);
}
static int32_t clamp32(int64_t v) { return v < INT32_MIN ? INT32_MIN : v > INT32_MAX ? INT32_MAX : (int32_t)v; }
void runtime_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = clamp32(__atomic_exchange_n(&g_mouse[0], 0, __ATOMIC_RELAXED));
    *dy = clamp32(__atomic_exchange_n(&g_mouse[1], 0, __ATOMIC_RELAXED));
}
uint64_t runtime_input_buttons_last(void) { return __atomic_load_n(&g_buttons_last, __ATOMIC_RELAXED); }
static void mouse_clear(void) {
    int32_t dx, dy;
    runtime_mouse_take(&dx, &dy);
    __atomic_store_n(&g_wheel, 0, __ATOMIC_RELAXED);
}
int runtime_mouse_selector_active(void) { return __atomic_load_n(&g_selector, __ATOMIC_RELAXED) != 0; }

/* The mouse at a controller read (input_lock held), after the keys and the pad:
   - L held (the ability wheel): the mouse moves a virtual cursor on the right stick, which stays where it was put;
   - D-pad up, left or right held (the strips of materials, shields, weapons and bows): horizontal motion steps the strip,
     and so does the wheel, a notch a step (away from the player to the left);
   - else the camera hooks take the counts (mouse_direct.c); counts no camera takes (a menu, a cutscene: no camera step
     with input on) are dropped, so they do not turn the camera at once when it comes back;
   - a mouse shake while Ultrahand holds an object swings the right stick with it (mouse_direct.c totk_mouse_shake).
   The right stick takes the larger of the pad's and the mouse's deflection, as the keys do. */
static void mouse_route(NpadState *s) {
    static int was;
    static float cursor[2];
    static TotkMouseRow row;
    static int32_t wheel_dir;
    static uint64_t wheel_until;
    uint64_t b = s->buttons, now = runtime_now_ns();
    int on = totk_mouse_enabled();
    int sel = !on ? 0 : (b & 64) ? 1 : (b & 8192) ? 2 : (b & 4096) ? 3 : (b & 16384) ? 4 : 0;
    __atomic_store_n(&g_selector, sel, __ATOMIC_RELAXED);
    int32_t dx, dy, out[2] = {0, 0};
    int32_t notches = __atomic_exchange_n(&g_wheel, 0, __ATOMIC_RELAXED);
    if (sel != was) cursor[0] = cursor[1] = 0, row = (TotkMouseRow){0}, wheel_dir = 0, wheel_until = 0;
    if (sel) {
        runtime_mouse_take(&dx, &dy);
        if (sel == 1) totk_mouse_radial(dx, dy, cursor, out);
        else {
            totk_mouse_row(dx, now, &row, out);
            /* the wheel: a 32 ms full-tilt pulse a notch with 32 ms of neutral after it, the mouse's step timing */
            if (!out[0]) {
                if (wheel_dir && now < wheel_until) out[0] = wheel_dir * 32767;
                else if (wheel_dir) wheel_dir = 0, wheel_until = now + 32000000ull;
                else if (notches && now >= wheel_until) {
                    wheel_dir = notches > 0 ? -1 : 1;
                    wheel_until = now + 32000000ull;
                    out[0] = wheel_dir * 32767;
                }
            }
        }
    } else if (was || !totk_mouse_camera_active()) {
        runtime_mouse_take(&dx, &dy);   /* a strip let go, or no camera takes the mouse now: dropped */
    }
    was = sel;
    int shake = on ? totk_mouse_shake() : 0;
    if (shake) out[0] = shake * 32767, out[1] = 0;
    if (out[0] || out[1]) {
        int64_t m2 = (int64_t)out[0] * out[0] + (int64_t)out[1] * out[1], p2 = (int64_t)s->rx * s->rx + (int64_t)s->ry * s->ry;
        if (shake || m2 > p2) s->rx = out[0], s->ry = out[1];
    }
}

static void initialize(void) {
    pthread_mutex_lock(&input_lock); enabled = true; pthread_mutex_unlock(&input_lock);
    fprintf(stderr, "[totk] keyboard / game controller input initialized\n");
}
/* Docked: one full-key controller as player 1 (id 0, style bit 0). Handheld (TOTK_OPERATION_MODE=handheld): the same
   input is the handheld controller (id 0x20, style bit 1) and player 1 is not connected. */
static uint32_t pad_id(void) { return runtime_handheld_mode() ? 0x20u : 0u; }
static uint32_t pad_style(void) { return runtime_handheld_mode() ? 2u : 1u; }
static void supported_ids(const uint32_t *ids, uint64_t n) {
    if (n > 10 || (!ids && n)) runtime_fail("invalid controller ids");
    pthread_mutex_lock(&input_lock);
    allowed = false;
    for (uint64_t i = 0; i < n; i++) if (ids[i] == pad_id()) allowed = true;
    pthread_mutex_unlock(&input_lock);
}
static void supported_styles(uint32_t value) { pthread_mutex_lock(&input_lock); styles = value; pthread_mutex_unlock(&input_lock); }
static uint32_t style(const uint32_t *id) {
    if (!id) runtime_fail("controller id pointer");
    pthread_mutex_lock(&input_lock);
    uint32_t v = enabled && allowed && *id == pad_id() ? (styles & pad_style()) : 0;
    pthread_mutex_unlock(&input_lock);
    return v;
}
static int pad_states(NpadState *out, int max, const uint32_t *id, uint32_t want_style) {
    if (max <= 0) return 0;
    if (!out || !id) runtime_fail("controller output pointer");
    pthread_mutex_lock(&input_lock);
    NpadState state = {.sampling = ++sample};
    if (enabled && allowed && *id == pad_id() && want_style == pad_style() && (styles & want_style)) {
        state.attributes = 1;
        for (unsigned i = 0; i < 128; i++) if (keys[i]) state.buttons |= button(i);
        state.buttons |= pending;
        pending = 0;
        state.lx = 32767 * ((int)keys[2] - (int)keys[0]);       /* D / A */
        state.ly = 32767 * ((int)keys[13] - (int)keys[1]);      /* W / S */
        state.rx = 32767 * ((int)keys[124] - (int)keys[123]);   /* arrow right / left */
        state.ry = 32767 * ((int)keys[126] - (int)keys[125]);   /* arrow up / down */
        RuntimeGamepadState pad;
        gamepad_read(&pad);
        state.buttons |= pad.buttons;
        if (abs(pad.lx) > abs(state.lx)) state.lx = pad.lx;
        if (abs(pad.ly) > abs(state.ly)) state.ly = pad.ly;
        if (abs(pad.rx) > abs(state.rx)) state.rx = pad.rx;
        if (abs(pad.ry) > abs(state.ry)) state.ry = pad.ry;
        mouse_route(&state);
        __atomic_store_n(&g_buttons_last, state.buttons, __ATOMIC_RELAXED);
    }
    *out = state;
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static int full_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 1); }
static int handheld_states(NpadState *out, int max, const uint32_t *id) { return pad_states(out, max, id, 2); }
static int disconnected_states(NpadState *out, int max, const uint32_t *id) {
    (void)id;
    if (max <= 0) return 0;
    if (!out) runtime_fail("controller output pointer");
    pthread_mutex_lock(&input_lock);
    *out = (NpadState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static void hold_type(int type) { if (type < 0 || type > 1) runtime_fail("invalid joy hold type"); }
static void disconnect(const uint32_t *id) {
    if (!id) runtime_fail("disconnect id");
    if (*id == pad_id()) {   /* only the pad in use: in handheld mode TOTK disconnects player 1 */
        pthread_mutex_lock(&input_lock); allowed = false; pthread_mutex_unlock(&input_lock);
        runtime_input_clear();
    }
}

/* Mouse and keyboard devices: the keyboard drives the controller, so the game sees no mouse and a keyboard with no keys. */
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
_Static_assert(sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "mouse/keyboard ABI");
static bool mouse_ready, keyboard_ready;
static void initialize_mouse(void) { pthread_mutex_lock(&input_lock); mouse_ready = true; pthread_mutex_unlock(&input_lock); }
static void initialize_keyboard(void) { pthread_mutex_lock(&input_lock); keyboard_ready = true; pthread_mutex_unlock(&input_lock); }
static int mouse_states(MouseState *out, int max) {
    if (max <= 0) return 0;
    if (!out) runtime_fail("mouse output pointer");
    pthread_mutex_lock(&input_lock);
    if (!mouse_ready) runtime_fail("mouse used before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
    return 1;
}
static void keyboard_state(KeyboardState *out) {
    if (!out) runtime_fail("keyboard output pointer");
    pthread_mutex_lock(&input_lock);
    if (!keyboard_ready) runtime_fail("keyboard used before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    pthread_mutex_unlock(&input_lock);
}
/* Controller support applet: exactly one controller here, so it returns at once (one player, on the controller in use). */
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");
static uint32_t show_controller_support(ControllerSupportResult *out, const void *arg) {
    (void)arg;
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    return 0;
}

HLE_NATIVE("_ZN2nn3hid14InitializeNpadEv", initialize, 'v')
HLE_NATIVE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", supported_ids, 'v')
HLE_NATIVE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", supported_styles, 'v')
HLE_NATIVE("_ZN2nn3hid15GetNpadStyleSetERKj", style, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", full_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", disconnected_states, 'w')
HLE_NATIVE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", handheld_states, 'w')
HLE_NATIVE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", hold_type, 'v')
HLE_NATIVE("_ZN2nn3hid14DisconnectNpadERKj", disconnect, 'v')
HLE_NATIVE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", show_controller_support, 'w')
HLE_NATIVE("_ZN2nn3hid15InitializeMouseEv", initialize_mouse, 'v')
HLE_NATIVE("_ZN2nn3hid18InitializeKeyboardEv", initialize_keyboard, 'v')
HLE_NATIVE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", mouse_states, 'w')
HLE_NATIVE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", keyboard_state, 'v')
