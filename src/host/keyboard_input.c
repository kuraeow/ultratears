/* Configurable physical keyboard/mouse buttons -> the existing guest controller ABI.
 * Both the native defaults and launcher descriptions use keyboard_bindings.def. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "host.h"
#include "keyboard_input.h"

typedef struct { const char *id; unsigned defaults[2], keys[2], legacy; int active; } Binding;
#define INPUT_BINDING(id, group, sw, description, primary, secondary, legacy) \
    {#id, {primary, secondary}, {primary, secondary}, legacy, 0},
static Binding bindings[] = {
#include "keyboard_bindings.def"
};
#undef INPUT_BINDING
_Static_assert(SDL_SCANCODE_COUNT == 512, "input config reserves codes 512..514 for mouse buttons");
static unsigned char held[INPUT_CODE_COUNT];
static int mouse_mode;

static int valid_key(unsigned k) {
    return k < INPUT_CODE_COUNT && k != SDL_SCANCODE_F8 && k != SDL_SCANCODE_F11;
}

void keyboard_input_init(void) {
    unsigned char used[INPUT_CODE_COUNT] = {0};
    const char *mode = getenv("TOTK_CAMERA_MODE");
    mouse_mode = mode && !strcmp(mode, "mouse");
    float sensitivity = 1;
    const char *s = getenv("TOTK_MOUSE_SENSITIVITY");
    if (s) {
        char *end; float value = strtof(s, &end);
        if (end != s && !*end && isfinite(value) && value >= 0.1f && value <= 5.0f) sensitivity = value;
    }
    const char *invert = getenv("TOTK_MOUSE_INVERT_Y");
    runtime_input_mouse_config(mouse_mode, sensitivity, invert && !strcmp(invert, "1"));
    mouse_camera_init_from_env();
    MouseCameraConfig mcfg;
    mouse_camera_get_config(&mcfg);
    memset(held, 0, sizeof held);
    for (unsigned i = 0; i < sizeof bindings / sizeof *bindings; i++) {
        Binding *b = &bindings[i]; char name[64];
        b->active = 0;
        memcpy(b->keys, b->defaults, sizeof b->keys);
        snprintf(name, sizeof name, "TOTK_KEY_%s", b->id);
        const char *value = getenv(name);
        if (value) {
            unsigned a, c; char tail;
            if (sscanf(value, "%u,%u%c", &a, &c, &tail) == 2 && valid_key(a) && valid_key(c)) {
                b->keys[0] = a; b->keys[1] = c;
            } else fprintf(stderr, "[totk-input] invalid %s=%s; using defaults\n", name, value);
        }
        for (unsigned j = 0; j < 2; j++) {
            unsigned code = b->keys[j];
            if (code && used[code]) {
                fprintf(stderr, "[totk-input] duplicate key %u on %s; binding cleared\n", code, b->id);
                b->keys[j] = 0;
            } else if (code) used[code] = 1;
        }
    }
    fprintf(stderr, "[totk-input] custom keyboard mapping loaded; camera=%s, backend=%s, look=%.4f deg/count, aim=%.2fx, scope=%.2fx, smooth=%dms, compat-sens=%.2f, invert-y=%d\n",
            mouse_mode ? "mouse (F8 releases/captures cursor)" : "keyboard",
            mcfg.backend == MOUSE_BACKEND_LEGACY_STICK ? "legacy_stick" : "native",
            mcfg.look_deg_per_count, mcfg.aim_multiplier, mcfg.scope_multiplier,
            mcfg.smooth_ms, sensitivity, invert && !strcmp(invert, "1"));
}

int keyboard_input_mouse_mode(void) { return mouse_mode; }

void keyboard_input_key(unsigned scancode, int down) {
    if (scancode >= INPUT_CODE_COUNT || !scancode) return;
    held[scancode] = !!down;
    for (unsigned i = 0; i < sizeof bindings / sizeof *bindings; i++) {
        Binding *b = &bindings[i];
        if (mouse_mode && !strncmp(b->id, "LOOK_", 5)) continue;
        int active = (b->keys[0] && held[b->keys[0]]) || (b->keys[1] && held[b->keys[1]]);
        if (active != b->active) { b->active = active; runtime_input_key(b->legacy, active); }
    }
}

void keyboard_input_mouse_button(unsigned button, int down) {
    unsigned code = button == SDL_BUTTON_LEFT ? INPUT_MOUSE_LEFT :
                    button == SDL_BUTTON_RIGHT ? INPUT_MOUSE_RIGHT :
                    button == SDL_BUTTON_MIDDLE ? INPUT_MOUSE_MIDDLE : 0;
    if (code) keyboard_input_key(code, down);
}

void keyboard_input_release_mouse_buttons(void) {
    for (unsigned code = INPUT_MOUSE_LEFT; code < INPUT_CODE_COUNT; code++) keyboard_input_key(code, 0);
}

void keyboard_input_clear(void) {
    memset(held, 0, sizeof held);
    for (unsigned i = 0; i < sizeof bindings / sizeof *bindings; i++) bindings[i].active = 0;
    mouse_camera_reset(MOUSE_RESET_FOCUS_LOST);
    runtime_input_clear();
}
