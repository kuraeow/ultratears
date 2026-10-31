// The window (milestone 3.2): SDL3 on the process's main thread — window, keyboard, game controllers with rumble —
// while the game runs on its own thread (boot.c). The picture goes through src/vk/present.c.
//  - keyboard: SDL scancodes become the Mac virtual key codes the controller emulation of overrides_input.c (from
//    MacNativeGames) expects, so the key layout is theirs (J/K/I/U = A/B/X/Y, WASD + arrows = sticks, ...);
//  - game controllers: buttons by position as on a Switch controller (the right face button is A), sticks with Y up;
//  - rumble: the game's vibration values (hid_feedback.c) drive the controller's low / high frequency motors.
//  - motion: the controller's gyro and accelerometer feed motion_fusion.c (gyro aiming); F11 / Alt+Enter: fullscreen.
#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED   // main() is boot.c's; SDL only gets SDL_SetMainReady
#include <SDL3/SDL_main.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"
#include "../vk/present.h"

static SDL_Window *window;
static SDL_Gamepad *pad;
static SDL_Mutex *pad_lock;

static int mac_key(SDL_Scancode s) {
    switch (s) {
    case SDL_SCANCODE_A: return 0; case SDL_SCANCODE_S: return 1; case SDL_SCANCODE_D: return 2;
    case SDL_SCANCODE_F: return 3; case SDL_SCANCODE_H: return 4; case SDL_SCANCODE_G: return 5;
    case SDL_SCANCODE_Z: return 6; case SDL_SCANCODE_X: return 7; case SDL_SCANCODE_C: return 8;
    case SDL_SCANCODE_V: return 9; case SDL_SCANCODE_B: return 11; case SDL_SCANCODE_Q: return 12;
    case SDL_SCANCODE_W: return 13; case SDL_SCANCODE_E: return 14; case SDL_SCANCODE_R: return 15;
    case SDL_SCANCODE_Y: return 16; case SDL_SCANCODE_T: return 17; case SDL_SCANCODE_O: return 31;
    case SDL_SCANCODE_U: return 32; case SDL_SCANCODE_I: return 34; case SDL_SCANCODE_P: return 35;
    case SDL_SCANCODE_L: return 37; case SDL_SCANCODE_J: return 38; case SDL_SCANCODE_K: return 40;
    case SDL_SCANCODE_N: return 45; case SDL_SCANCODE_M: return 46; case SDL_SCANCODE_RETURN: return 36;
    case SDL_SCANCODE_TAB: return 48; case SDL_SCANCODE_SPACE: return 49; case SDL_SCANCODE_ESCAPE: return 53;
    case SDL_SCANCODE_LSHIFT: return 56; case SDL_SCANCODE_RSHIFT: return 60; case SDL_SCANCODE_LALT: return 58;
    case SDL_SCANCODE_RALT: return 61; case SDL_SCANCODE_LCTRL: return 59; case SDL_SCANCODE_RCTRL: return 62;
    case SDL_SCANCODE_LEFT: return 123; case SDL_SCANCODE_RIGHT: return 124; case SDL_SCANCODE_DOWN: return 125;
    case SDL_SCANCODE_UP: return 126;
    default: return -1;
    }
}

// Switch npad button bits (libnx HidNpadButton): A B X Y StickL StickR L R ZL ZR Plus Minus Left Up Right Down
static int32_t stick(Sint16 v, int invert) {
    int32_t x = invert ? -(int32_t)v : v;
    return x > 32767 ? 32767 : x < -32767 ? -32767 : x;
}
static void pad_update(void) {
    RuntimeGamepadState s = {0};
    if (pad) {
        static const struct { SDL_GamepadButton b; uint64_t bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_EAST, 1}, {SDL_GAMEPAD_BUTTON_SOUTH, 2}, {SDL_GAMEPAD_BUTTON_NORTH, 4},
            {SDL_GAMEPAD_BUTTON_WEST, 8}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, 16}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 32},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 64}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 128},
            {SDL_GAMEPAD_BUTTON_START, 1024}, {SDL_GAMEPAD_BUTTON_BACK, 2048}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 4096},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, 8192}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 16384},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 32768}};
        for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GetGamepadButton(pad, map[i].b)) s.buttons |= map[i].bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) s.buttons |= 256;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) s.buttons |= 512;
        s.lx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), 0);
        s.ly = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY), 1);
        s.rx = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), 0);
        s.ry = stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY), 1);
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
    window = SDL_CreateWindow("UltraTears", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) { fprintf(stderr, "[totk] window: %s\n", SDL_GetError()); return 0; }
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
    if (!vkp_init(window))
        runtime_fail("Vulkan initialization failed (Vulkan 1.3 with VK_EXT_shader_object is required)");
    return 1;
}

// Motion: the controller's accelerometer and gyro (SDL_EVENT_GAMEPAD_SENSOR_UPDATE) go to motion_fusion.c.
static void pad_attached(void) {   // pad_lock held; pad may be NULL
    if (!pad) { runtime_motion_live(0); return; }
    fprintf(stderr, "[totk] game controller: %s\n", SDL_GetGamepadName(pad));
    if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
        SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) && SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
        const char *axes = getenv("TOTK_GYRO_AXES");
        if (runtime_motion_axes(axes) < 0) fprintf(stderr, "[totk] TOTK_GYRO_AXES=%s is not like \"-x,y,-z\": ignored\n", axes);
        runtime_motion_live(1);
        fprintf(stderr, "[totk] controller motion sensors on (%.0f Hz gyro)\n", SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO));
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
        runtime_motion_feed(acc, gyro, runtime_now_ns());
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
                switch (e.type) {
                case SDL_EVENT_QUIT:
                    fprintf(stderr, "[totk] window closed\n");
                    _Exit(0);
                case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                        (e.key.scancode == SDL_SCANCODE_F11 ||
                         (e.key.scancode == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                        toggle_fullscreen();
                        break;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat && e.key.scancode == SDL_SCANCODE_F9) {
                        void vkb_dump_request(void);   // gfx/vkb.h: TOTK_DUMP_KEY (development)
                        vkb_dump_request();
                        break;
                    }
                    int k = mac_key(e.key.scancode);
                    if (k >= 0 && !e.key.repeat) runtime_input_key((unsigned)k, e.type == SDL_EVENT_KEY_DOWN);
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
        title_update();
    }
}
