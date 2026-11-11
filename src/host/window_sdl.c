// The window (milestone 3.2): SDL3 on the process's main thread — window, keyboard, game controllers with rumble —
// while the game runs on its own thread (boot.c). The picture goes through src/vk/present.c.
//  - keyboard: SDL scancodes become the Mac virtual key codes the controller emulation of overrides_input.c (from
//    MacNativeGames) expects, so the key layout is theirs (J/K/I/U = A/B/X/Y, WASD + arrows = sticks, ...);
//  - game controllers: buttons by position as on a Switch controller (the right face button is A), sticks with Y up;
//  - rumble: the game's vibration values (hid_feedback.c) drive the controller's low / high frequency motors.
//  - motion: the controller's gyro and accelerometer feed motion_fusion.c (gyro aiming); F11 / Alt+Enter: fullscreen.
//  - the mouse: kept in the window, the cursor hidden (mouse_capture); Esc lets it go.
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED   // main() is boot.c's; SDL only gets SDL_SetMainReady
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "../vk/present.h"
#ifndef _WIN32
#include "icon_png.h"   // build-linux/: tools/build_game.py
#endif

static SDL_Window *window;
static SDL_Gamepad *pad;
static SDL_Mutex *pad_lock;

/* ---- bindings (the launcher's Controls tab writes them into ultratears.cfg; boot.c passes pad_* / key_* lines on as
   TOTK_BIND_pad_* / TOTK_BIND_key_*). Unset: the layouts below, as before.
   - pad_layout=xbox: the face buttons as an Xbox pad (and the Steam Deck) labels them, A the bottom one; default
     (nintendo): by position as on a Switch controller, A the right one.
   - pad_<action>=<SDL gamepad names, comma separated>: a, b, x, y (SDL's names by position: a = the bottom button),
     leftshoulder, rightshoulder, lefttrigger, righttrigger, back, start, leftstick, rightstick, dpup ..., paddle1..4;
     none: unbound. Actions: a b x y l r zl zr plus minus lstick rstick up down left right.
   - key_<action>=<SDL scancode names, comma separated> (SDL_GetScancodeName: "W", "Space", "Left Shift"); none:
     unbound. The same actions and move_up / move_down / move_left / move_right, cam_up / cam_down / cam_left /
     cam_right (the sticks the keys drive); the mouse's buttons too: Mouse Left, Mouse Right, Mouse Middle, Mouse 4,
     Mouse 5. Unset: the PC layout of the keys column below (the launcher's CTL_BUTTONS / CTL_MOVE hold the same); a key
     may do two actions. Escape lets the mouse go (mouse_capture) only when it is bound to none of them. */
typedef struct { const char *id; uint64_t bit; int mac; const char *keys; } bind_action;
static const bind_action ACTIONS[] = {
    {"a", 1, 38, "E"}, {"b", 2, 40, "Left Shift"}, {"x", 4, 34, "Space"}, {"y", 8, 32, "Mouse Left,T"},
    {"lstick", 16, 6, "Left Ctrl"}, {"rstick", 32, 8, "C"}, {"l", 64, 12, "Q"}, {"r", 128, 14, "R"},
    {"zl", 256, 58, "Mouse Right,Z"}, {"zr", 512, 56, "F,X"}, {"plus", 1024, 36, "Escape"}, {"minus", 2048, 48, "Tab,M"},
    {"left", 4096, 3, "1,Mouse 4"}, {"up", 8192, 17, "2"}, {"right", 16384, 4, "3,Mouse 5"}, {"down", 32768, 5, "4"},
    {"move_up", 0, 13, "W"}, {"move_down", 0, 1, "S"}, {"move_left", 0, 0, "A"}, {"move_right", 0, 2, "D"},
    {"cam_up", 0, 126, "Up"}, {"cam_down", 0, 125, "Down"}, {"cam_left", 0, 123, "Left"}, {"cam_right", 0, 124, "Right"}};
// scancode / mouse button -> the Mac key codes overrides_input.c reads (up to two actions), -1 none
static int8_t g_keymap[SDL_SCANCODE_COUNT][2];
static int8_t g_mousemap[6][2];   // SDL_BUTTON_LEFT (1) .. SDL_BUTTON_X2 (5)
static int mouse_from_name(const char *t) {
    static const char *const names[] = {NULL, "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse 4", "Mouse 5"};
    for (int b = 1; b < 6; b++) if (!SDL_strcasecmp(t, names[b])) return b;
    return 0;
}
static void map_add(int8_t m[2], int code) {
    if (m[0] == code || m[1] == code) return;
    if (m[0] < 0) m[0] = (int8_t)code;
    else if (m[1] < 0) m[1] = (int8_t)code;
}
static uint64_t g_padbits[SDL_GAMEPAD_BUTTON_COUNT];   // SDL button -> Switch buttons
static uint64_t g_trigbits[2];                     // the left / right trigger -> Switch buttons
static int g_xbox_layout;

static const char *bind_value(const char *kind, const char *id) {
    char var[64];
    snprintf(var, sizeof var, "TOTK_BIND_%s_%s", kind, id);
    const char *v = getenv(var);
    return v && *v ? v : NULL;
}
static void binds_load(void) {
    memset(g_keymap, -1, sizeof g_keymap);
    memset(g_mousemap, -1, sizeof g_mousemap);
    const char *layout = getenv("TOTK_BIND_pad_layout");
    g_xbox_layout = layout && !SDL_strcasecmp(layout, "xbox");
    static const struct { SDL_GamepadButton b; uint64_t bit; } common[] = {
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, 16}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 32}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 64},
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 128}, {SDL_GAMEPAD_BUTTON_START, 1024}, {SDL_GAMEPAD_BUTTON_BACK, 2048},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 4096}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 8192}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 16384},
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 32768}};
    memset(g_padbits, 0, sizeof g_padbits);
    for (unsigned i = 0; i < sizeof common / sizeof *common; i++) g_padbits[common[i].b] = common[i].bit;
    if (g_xbox_layout) {   // as labelled: A bottom, B right, X left, Y top
        g_padbits[SDL_GAMEPAD_BUTTON_SOUTH] = 1, g_padbits[SDL_GAMEPAD_BUTTON_EAST] = 2;
        g_padbits[SDL_GAMEPAD_BUTTON_WEST] = 4, g_padbits[SDL_GAMEPAD_BUTTON_NORTH] = 8;
    } else {               // by position, as a Switch controller: A right, B bottom, X top, Y left
        g_padbits[SDL_GAMEPAD_BUTTON_EAST] = 1, g_padbits[SDL_GAMEPAD_BUTTON_SOUTH] = 2;
        g_padbits[SDL_GAMEPAD_BUTTON_NORTH] = 4, g_padbits[SDL_GAMEPAD_BUTTON_WEST] = 8;
    }
    g_trigbits[0] = 256, g_trigbits[1] = 512;
    int changed = 0;
    for (unsigned a = 0; a < sizeof ACTIONS / sizeof *ACTIONS; a++) {
        const bind_action *A = &ACTIONS[a];
        const char *v;
        if (A->bit && (v = bind_value("pad", A->id))) {   // its buttons: these and no others
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) g_padbits[b] &= ~A->bit;
            g_trigbits[0] &= ~A->bit, g_trigbits[1] &= ~A->bit;
            char list[256];
            snprintf(list, sizeof list, "%s", v);
            for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
                while (*t == ' ') t++;
                SDL_GamepadButton b = SDL_GetGamepadButtonFromString(t);
                SDL_GamepadAxis ax = SDL_GetGamepadAxisFromString(t);
                if (b != SDL_GAMEPAD_BUTTON_INVALID) g_padbits[b] |= A->bit;
                else if (ax == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) g_trigbits[0] |= A->bit;
                else if (ax == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) g_trigbits[1] |= A->bit;
                else if (SDL_strcasecmp(t, "none")) fprintf(stderr, "[totk] pad_%s: no button \"%s\"\n", A->id, t);
            }
            changed++;
        }
        v = bind_value("key", A->id);   // its keys and mouse buttons: these, else the defaults
        if (v) changed++;
        char list[256];
        snprintf(list, sizeof list, "%s", v ? v : A->keys);
        for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) {
            while (*t == ' ') t++;
            int mb = mouse_from_name(t);
            SDL_Scancode s = mb ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(t);
            if (mb) map_add(g_mousemap[mb], A->mac);
            else if (s != SDL_SCANCODE_UNKNOWN) map_add(g_keymap[s], A->mac);
            else if (SDL_strcasecmp(t, "none")) fprintf(stderr, "[totk] key_%s: no key \"%s\"\n", A->id, t);
        }
    }
    fprintf(stderr, "[totk] controls: %s layout%s, %d action(s) bound in ultratears.cfg\n", g_xbox_layout ? "Xbox" : "Nintendo",
            g_xbox_layout ? " (A the bottom button)" : " (A the right button)", changed);
}
// The Switch buttons an SDL button stands for (text_entry.c: OK is whatever is A)
uint64_t window_pad_bits(int button) { return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT ? g_padbits[button] : 0; }

// Switch npad button bits (libnx HidNpadButton): A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down
static int32_t stick(Sint16 v, int invert) {
    int32_t x = invert ? -(int32_t)v : v;
    return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
}
/* The stick dead zone (GitHub #39: with an Xbox One pad Link walked right on his own): on the Switch the HID service
   takes a resting stick's offset off before the game sees it, SDL gives the raw position. A radial dead zone of
   TOTK_STICK_DEADZONE percent of the travel (default 10, 0..50) reads as centred, and the rest is scaled so the stick
   still goes from 0 to full beyond it, without a jump at its edge. Each stick its own (the launcher's Sticks and gyro
   page: stick_deadzone_left= / _right= in ultratears.cfg): TOTK_STICK_DEADZONE_LEFT / _RIGHT over the shared one. */
static void dead_zone(int32_t *x, int32_t *y, int side) {
    static double dzs[2] = {-1, -1};
    if (dzs[side] < 0) {
        const char *name = side ? "TOTK_STICK_DEADZONE_RIGHT" : "TOTK_STICK_DEADZONE_LEFT", *e = getenv(name);
        if (!e || !*e) name = "TOTK_STICK_DEADZONE", e = getenv(name);
        double v = e && *e ? atof(e) : 10;
        dzs[side] = (v >= 0 && v <= 50 ? v : 10) / 100 * 32767;
        if (e && *e) fprintf(stderr, "[totk] %s stick dead zone %.0f %% (%s)\n", side ? "right" : "left", dzs[side] * 100 / 32767, name);
    }
    double dz = dzs[side];
    double r = sqrt((double)*x * *x + (double)*y * *y);
    if (r <= dz) { *x = *y = 0; return; }
    double k = r >= 32767 ? 32767 / r : (r - dz) / (32767 - dz) * 32767 / r;
    *x = (int32_t)(*x * k);
    *y = (int32_t)(*y * k);
}
int text_entry_blocks_input(void);   // text_entry.c: the software keyboard's field holds the game's input
static int pad_down(void) {   // any button or trigger of the controller pressed
    if (!pad) return 0;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) return 1;
    return SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384 || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384;
}
static void pad_update(void) {
    RuntimeGamepadState s = {0};
    if (pad && text_entry_blocks_input()) s.connected = 1;   // held: connected, nothing pressed
    else if (pad) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)   // through the bindings (binds_load)
            if (g_padbits[b] && SDL_GetGamepadButton(pad, (SDL_GamepadButton)b)) s.buttons |= g_padbits[b];
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) s.buttons |= g_trigbits[0];
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) s.buttons |= g_trigbits[1];
        s.lx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0);
        s.ly = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), 1);
        s.rx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), 0);
        s.ry = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), 1);
        dead_zone(&s.lx, &s.ly, 0);
        dead_zone(&s.rx, &s.ry, 1);
        s.connected = 1;
    }
    runtime_gamepad_set(&s);
}

// hid_feedback.c: the game's vibration value for one side (0 left, 1 right). A Switch controller has a low and a high
// frequency actuator per side; an SDL controller has a low and a high frequency motor, so each takes the stronger side.
// The game sends values every frame; the 250 ms duration stops the motors if it stops sending.
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)freq_low; (void)freq_high;
    static float low[2], high[2];
    if (side > 1 || !pad_lock) return;
    SDL_LockMutex(pad_lock);
    low[side] = amp_low < 0 ? 0 : amp_low > 1 ? 1 : amp_low;
    high[side] = amp_high < 0 ? 0 : amp_high > 1 ? 1 : amp_high;
    float l = low[0] > low[1] ? low[0] : low[1], h = high[0] > high[1] ? high[0] : high[1];
    if (pad) SDL_RumbleGamepad(pad, (Uint16)(l * 65535), (Uint16)(h * 65535), 250);
    SDL_UnlockMutex(pad_lock);
}

int window_init(void) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "480");   // 10 ms device buffer at 48 kHz
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");        // the exe's icon resource (assets/icon.ico via build/ultratears.rc) for the window
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "[totk] SDL: %s\n", SDL_GetError());
        return 0;
    }
    pad_lock = SDL_CreateMutex();
    binds_load();
    window = SDL_CreateWindow("UltraTears", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { fprintf(stderr, "[totk] window: %s\n", SDL_GetError()); return 0; }
#ifndef _WIN32
    {   // the port's icon (assets/icon.ico's 256 px PNG, build/icon_png.h); on Windows the exe's resource gives it
        SDL_Surface *icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(ultrs_icon_png, sizeof ultrs_icon_png), true);
        if (icon) SDL_SetWindowIcon(window, icon), SDL_DestroySurface(icon);
    }
#endif
    // window= in ultratears.cfg (TOTK_WINDOW): window (default), borderless (the desktop's size, no mode change) or
    // fullscreen (exclusive, at the desktop's resolution and refresh rate); F11 / Alt+Enter still switch
    const char *mode = getenv("TOTK_WINDOW");
    if (mode && (!SDL_strcmp(mode, "borderless") || !SDL_strcmp(mode, "fullscreen"))) {
        const SDL_DisplayMode *desk = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window));
        if (!SDL_strcmp(mode, "fullscreen") && desk)
            SDL_SetWindowFullscreenMode(window, desk);
        SDL_SetWindowFullscreen(window, true);
        fprintf(stderr, "[totk] window: %s\n", mode);
    }
    SDL_SetWindowRelativeMouseMode(window, true);   // the mouse is the game window's from the start (mouse_capture)
    void text_entry_init(SDL_Window *w);   // text_entry.c: the software keyboard's field
    text_entry_init(window);
    if (!vkp_init(window))
        runtime_fail("Vulkan initialization failed (Vulkan 1.3 with VK_EXT_shader_object is required)");
    return 1;
}

// Motion: the controller's accelerometer and gyro (SDL_EVENT_GAMEPAD_SENSOR_UPDATE) go to motion_fusion.c.
int deck_imu_start(void);   // deck_imu.c
static void pad_attached(void) {   // pad_lock held; pad may be NULL
    if (!pad) { runtime_motion_live(0); return; }
    fprintf(stderr, "[totk] game controller: %s\n", SDL_GetGamepadName(pad));
    {   // which device it is: a Steam Input virtual gamepad (Steam handle) gives no motion; the Deck's own controller,
        // opened by SDL's HIDAPI driver (Steam Input off for the game), has a gyro
        const char *path = SDL_GetGamepadPath(pad);
        fprintf(stderr, "[totk]   type %d, vendor %04x product %04x, %s, path %s\n", (int)SDL_GetGamepadType(pad),
                SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad),
                SDL_GetGamepadSteamHandle(pad) ? "a Steam Input virtual gamepad" : "not through Steam Input", path ? path : "-");
    }
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
        const char *axes = getenv("TOTK_GYRO_AXES");
        if (runtime_motion_axes(axes) < 0) fprintf(stderr, "[totk] TOTK_GYRO_AXES=%s is not like \"-x,y,-z\": ignored\n", axes);
        runtime_motion_live(1);
        fprintf(stderr, "[totk] controller motion sensors on (%.0f Hz gyro)\n", SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO));
    } else if (deck_imu_start()) {   // a Steam Input gamepad on the Steam Deck: the Deck's own motion (deck_imu.c)
        runtime_motion_live(1);
    } else {
        runtime_motion_live(0);
        fprintf(stderr, "[totk] controller has no motion sensors: no gyro aiming\n");
    }
}
// SDL's controller frame: x right, y out of the face, z toward the player (the bottom edge); acceleration in m/s^2 as
// the force holding it up (face up reads y = +9.8). The Switch frame (runtime_motion_feed): x right, y toward the top
// edge, z out of the face, acceleration as gravity (face up reads z = -1 G). (x, -z, y) is a rotation, so the gyro
// keeps its sense; the acceleration also changes sign.
static void pad_sensor(const SDL_GamepadSensorEvent *e) {
    static double acc[3] = {0, 0, -1};
    const float *d = e->data;
    if (e->sensor == SDL_SENSOR_ACCEL) {
        acc[0] = -d[0] / SDL_STANDARD_GRAVITY;
        acc[1] = d[2] / SDL_STANDARD_GRAVITY;
        acc[2] = -d[1] / SDL_STANDARD_GRAVITY;
    } else if (e->sensor == SDL_SENSOR_GYRO) {
        double gyro[3] = {d[0], -d[2], d[1]};
        runtime_motion_feed(acc, gyro, runtime_now_ns(), e->sensor_timestamp);
    }
}

static void open_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (!pad) {
        pad = SDL_OpenGamepad(id);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}
static void close_pad(SDL_JoystickID id) {
    SDL_LockMutex(pad_lock);
    if (pad && SDL_GetGamepadID(pad) == id) {
        SDL_CloseGamepad(pad);
        pad = NULL;
        int n = 0;   // another one still connected takes over
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        if (ids && n) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        pad_attached();
    }
    SDL_UnlockMutex(pad_lock);
    pad_update();
}

// The refresh rate of the monitor the window is on (frame limit "display", frame_rate.c); 0 when not known.
float window_refresh_hz(void) {
    if (!window) return 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return m ? m->refresh_rate : 0;
}

// The mouse is the window's while the game has the focus, as in other PC games: the cursor hidden and kept in the
// window (SDL's relative mode), taken at the start and by a click into the window, let go by Esc and whenever another
// window gets the focus (Alt+Tab, the Windows key; SDL gives it back with the focus). While it is held its raw motion and
// wheel go to overrides_input.c (the camera: mouse_direct.c); the game's own HID mouse stays one that does not move.
static void mouse_capture(bool on) {
    if (SDL_GetWindowRelativeMouseMode(window) == on) return;
    SDL_SetWindowRelativeMouseMode(window, on);
}

static void toggle_fullscreen(void) {
    bool on = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, !on);   // borderless desktop fullscreen (no display mode change)
}

// Game frame rate and shown frame rate in the title, twice a second.
static void title_update(void) {
    static uint64_t t0, game0, shown0;
    uint64_t now = runtime_now_ns(), game = vkp_frames(), shown = vkp_presented();
    if (!t0) { t0 = now; game0 = game; shown0 = shown; return; }
    if (now - t0 < 500000000) return;
    double s = (now - t0) / 1e9;
    char title[128];
    snprintf(title, sizeof title, "UltraTears  |  game %.1f fps  |  shown %.1f fps", (game - game0) / s, (shown - shown0) / s);
    SDL_SetWindowTitle(window, title);
    t0 = now; game0 = game; shown0 = shown;
}

_Noreturn void window_loop(void) {
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                int text_entry_event(const SDL_Event *e);   // text_entry.c: the software keyboard's field first
                if (text_entry_event(&e)) continue;
                if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && text_entry_blocks_input() &&
                    e.key.scancode != SDL_SCANCODE_F11 && e.key.scancode != SDL_SCANCODE_F10) continue;
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    fprintf(stderr, "[totk] window closed\n");
                    runtime_profiler_stop();
                    { void pgo_flush(void); pgo_flush(); }   // the PGO training build only (src/host/pgo.c)
                    _Exit(0);
                case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                        (e.key.scancode == SDL_SCANCODE_F11 ||
                         (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        toggle_fullscreen();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE &&
                        g_keymap[SDL_SCANCODE_ESCAPE][0] < 0) {
                        mouse_capture(false);   // Esc lets the mouse go, unless bound to one of the game's buttons
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F10) {
                        void vk_overlay_toggle(void);   // vk/overlay.c: the frame counter
                        vk_overlay_toggle();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F9) {
                        void vkb_dump_request(void);   // gfx/vkb.h: TOTK_DUMP_KEY (development)
                        vkb_dump_request();
                        break;
                    }
                    if (e.key.scancode < SDL_SCANCODE_COUNT && !e.key.repeat)   // (binds_load)
                        for (int i = 0; i < 2; i++)
                            if (g_keymap[e.key.scancode][i] >= 0) runtime_input_key((unsigned)g_keymap[e.key.scancode][i], e.type == SDL_EVENT_KEY_DOWN);
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (!SDL_GetWindowRelativeMouseMode(window)) {   // a click into the window takes the mouse again (and does nothing else)
                        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_capture(true);
                        break;
                    }
                    if (e.button.button < 6 && !(e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && text_entry_blocks_input()))
                        for (int i = 0; i < 2; i++)
                            if (g_mousemap[e.button.button][i] >= 0) runtime_input_key((unsigned)g_mousemap[e.button.button][i], e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    break;
                case SDL_EVENT_MOUSE_MOTION:   // relative mode: raw counts, +y down
                    if (SDL_GetWindowRelativeMouseMode(window) && !text_entry_blocks_input())
                        runtime_mouse_motion(e.motion.xrel, e.motion.yrel);
                    break;
                case SDL_EVENT_MOUSE_WHEEL: {   // + away from the player
                    float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
                    if (SDL_GetWindowRelativeMouseMode(window) && !text_entry_blocks_input()) runtime_mouse_wheel(y);
                    break;
                }
                case SDL_EVENT_WINDOW_FOCUS_LOST: runtime_input_clear(); break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: vkp_resized(); break;
                case SDL_EVENT_GAMEPAD_ADDED: open_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: close_pad(e.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION: case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    pad_update();
                    break;
                case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: pad_sensor(&e.gsensor); break;
                default: break;
                }
            } while (SDL_PollEvent(&e));
        }
        {   // the game's input back once every key and button pressed for the field is up
            void text_entry_tick(int pad_down);
            int held = text_entry_blocks_input();
            SDL_LockMutex(pad_lock);
            text_entry_tick(pad_down());
            SDL_UnlockMutex(pad_lock);
            if (held && !text_entry_blocks_input()) pad_update();
        }
        title_update();
    }
}
