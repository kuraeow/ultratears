/* The text field the game's software keyboard opens (swkbd.c: naming a horse): one field over the picture for every
 * platform.
 *  - Text: SDL's text input. On a PC the physical keyboard (and the IME); on the Steam Deck SDL opens Steam's on-screen
 *    keyboard itself (SDL_ENABLE_STEAM_SCREEN_KEYBOARD, which Steam sets in Gaming Mode and boot.c sets with SteamDeck=1)
 *    and its keys come back as the same text events.
 *  - Keys: Enter / A or + = OK, Backspace / B = delete (B on an empty field = cancel), Esc = cancel, Y = the on-screen
 *    keyboard again (after it was closed). Buttons by position as everywhere in the port (window_sdl.c: the right face
 *    button is A).
 *  - The game: the thread that called waits here with its virtual core given up, so the other guest threads go on;
 *    its input is held (window_sdl.c) while the field is open and afterwards until every key and button is released,
 *    so the press that closed the field does not reach the game.
 *  - The picture: present.c shows the last game frame again every 33 ms while the field is open (the thread that called
 *    may be the one that draws) and overlay.c copies the panel drawn here (CPU, stb_truetype) over it. Glyphs: Inter
 *    (assets/fonts, SIL OFL 1.1: Latin, Cyrillic, Greek), then the player's Switch system fonts (data/fonts) for
 *    Japanese, Chinese and Korean; without those such characters show as boxes but are still entered right.
 * Costs nothing while closed: nothing is drawn and present.c waits as before. */
#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "host.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

/* Inter compiled in (the build runs from the source root: tools/build_game.py, ninja -C) */
#ifdef _WIN32
#define TE_RODATA ".section .rdata,\"dr\"\n"
#else
#define TE_RODATA ".section .rodata\n"
#endif
__asm__(TE_RODATA ".balign 16\n.globl te_inter_ttf\nte_inter_ttf:\n.incbin \"assets/fonts/Inter-Regular.ttf\"\n"
        ".globl te_inter_ttf_end\nte_inter_ttf_end:\n.byte 0\n.text\n");
extern const unsigned char te_inter_ttf[], te_inter_ttf_end[];

int runtime_core_away(void);           /* threads.c */
void runtime_core_back(int *away);
void vkp_wake(void);                    /* vk/present.c */
const uint8_t *runtime_shared_font_sfnt(int type, size_t *n);   /* shared_font.c */

enum { TE_MAX = 500 };
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static SDL_Window *g_window;
static Uint32 g_event;                  /* the registered SDL event that opens the field (0: no window) */
static int g_state;                     /* 0 idle, 1 asked for, 2 open, 3 answered */
static int g_result;                    /* 1 OK, 0 cancelled */
static uint32_t g_text[TE_MAX];         /* code points */
static int g_n, g_max_units, g_min_units, g_numeric;
static uint32_t g_disabled;
static _Atomic int g_visible;           /* the panel is drawn */
static _Atomic int g_block;             /* 1 open, 2 closed but keys or buttons still down */
static int g_deck;

void text_entry_init(SDL_Window *w) {
    g_window = w;
    g_event = SDL_RegisterEvents(1);
    const char *d = getenv("SteamDeck");
    g_deck = d && !strcmp(d, "1");
}

static int units(uint32_t c) { return c >= 0x10000 ? 2 : 1; }
static int count_units(void) { int u = 0; for (int i = 0; i < g_n; i++) u += units(g_text[i]); return u; }

/* What the applet lets through: no control characters, the disabled keys (bits as the applet's: 1 space, 2 '@',
   3 '%', 4 '/', 5 '\', 6 digits), digits only on a numeric keyboard. */
static int allowed(uint32_t c) {
    int digit = c >= '0' && c <= '9';
    if (c < 0x20 || c == 0x7f || (c >= 0xd800 && c < 0xe000) || c > 0x10ffff) return 0;
    if (g_numeric && !digit) return 0;
    if ((g_disabled & 1u << 1 && c == ' ') || (g_disabled & 1u << 2 && c == '@') || (g_disabled & 1u << 3 && c == '%') ||
        (g_disabled & 1u << 4 && c == '/') || (g_disabled & 1u << 5 && c == '\\') || (g_disabled & 1u << 6 && digit)) return 0;
    return 1;
}
static void append(uint32_t c) {   /* mu held */
    if (allowed(c) && g_n < TE_MAX && count_units() + units(c) <= g_max_units) g_text[g_n++] = c;
}

/* ---- the game's side (swkbd.c) ---- */

/* Shows the field with the initial text and waits for the answer. 1: OK (the text in out, *nout UTF-16 units),
   0: cancelled, -1: no window (TOTK_HEADLESS): the caller answers by itself. */
int text_entry_run(const uint16_t *init, size_t ninit, int max_units, int min_units, uint32_t disabled, int numeric,
                   uint16_t *out, size_t cap, size_t *nout) {
    *nout = 0;
    if (!g_window || !g_event) return -1;
    pthread_mutex_lock(&mu);
    while (g_state) pthread_cond_wait(&cv, &mu);   /* one field at a time, as the applet */
    g_max_units = max_units > 0 && max_units <= TE_MAX ? max_units : TE_MAX;
    g_min_units = min_units > 0 ? min_units : 1;
    g_disabled = disabled;
    g_numeric = numeric;
    g_n = 0;
    for (size_t i = 0; i < ninit; i++) {
        uint32_t c = init[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < ninit && init[i + 1] >= 0xdc00 && init[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (init[++i] - 0xdc00);
        append(c);
    }
    g_state = 1;
    pthread_mutex_unlock(&mu);
    SDL_Event e;
    SDL_zero(e);
    e.type = g_event;
    SDL_PushEvent(&e);   /* the window thread opens it (SDL's text input belongs there) */
    int away = runtime_core_away();
    pthread_mutex_lock(&mu);
    while (g_state != 3) pthread_cond_wait(&cv, &mu);
    int ok = g_result;
    size_t n = 0;
    if (ok)
        for (int i = 0; i < g_n; i++) {
            uint32_t c = g_text[i];
            if (c >= 0x10000) {
                if (n + 2 > cap) break;
                out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
                out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
            } else {
                if (n + 1 > cap) break;
                out[n++] = (uint16_t)c;
            }
        }
    g_state = 0;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    runtime_core_back(&away);
    *nout = n;
    return ok;
}

/* ---- the panel's layout (pixels of the window's picture), shared by the drawing and the on-screen keyboard's area ---- */
typedef struct { int fs, x0, y0, w, h, fx, fy, fw, fh, hint_fs, hint_y; } layout_t;
static layout_t layout(int sw, int sh) {
    layout_t L;
    L.fs = sh / 14;
    if (L.fs < 20) L.fs = 20;
    if (L.fs > 100) L.fs = 100;
    L.hint_fs = L.fs * 9 / 20;
    if (L.hint_fs < 12) L.hint_fs = 12;
    L.w = L.fs * 16;
    if (L.w > sw - 2 * L.fs / 2) L.w = sw - 2 * L.fs / 2;
    int pad = L.fs / 3;
    L.fh = L.fs * 3 / 2;
    L.h = pad + L.fh + pad / 2 + L.hint_fs * 3 / 2 + pad / 2;
    L.x0 = (sw - L.w) / 2;
    L.y0 = sh / 5;
    L.fx = L.x0 + pad;
    L.fy = L.y0 + pad;
    L.fw = L.w - 2 * pad;
    L.hint_y = L.fy + L.fh + pad / 2 + L.hint_fs;   /* its baseline */
    return L;
}

/* ---- the window thread ---- */
static void screen_keyboard(int on) {
    if (!on) { SDL_StopTextInput(g_window); return; }
    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
    SDL_GetWindowSize(g_window, &ww, &wh);
    if (pw > 0 && ph > 0 && ww > 0 && wh > 0) {   /* the field, so the on-screen keyboard leaves it in view */
        layout_t L = layout(pw, ph);
        SDL_Rect r = {L.fx * ww / pw, L.fy * wh / ph, L.fw * ww / pw, L.fh * wh / ph};
        SDL_SetTextInputArea(g_window, &r, 0);
    }
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetNumberProperty(p, SDL_PROP_TEXTINPUT_TYPE_NUMBER, g_numeric ? SDL_TEXTINPUT_TYPE_NUMBER : SDL_TEXTINPUT_TYPE_TEXT_NAME);
    SDL_SetBooleanProperty(p, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, false);
    SDL_StartTextInputWithProperties(g_window, p);
    SDL_DestroyProperties(p);
}
static void finish(int ok) {
    pthread_mutex_lock(&mu);
    if (g_state != 2) { pthread_mutex_unlock(&mu); return; }
    if (ok && count_units() < g_min_units) { pthread_mutex_unlock(&mu); return; }   /* OK on too short a text: nothing */
    g_result = ok;
    g_state = 3;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    screen_keyboard(0);
    atomic_store(&g_visible, 0);
    atomic_store(&g_block, 2);
    fprintf(stderr, "[totk] text field: %s\n", ok ? "confirmed" : "cancelled");
}
static void backspace(int cancel_when_empty) {
    pthread_mutex_lock(&mu);
    int empty = g_n == 0;
    if (!empty) g_n--;
    pthread_mutex_unlock(&mu);
    if (empty && cancel_when_empty) finish(0);
}

/* Every SDL event first: 1 when it was the field's (the window loop then gives it nothing else). */
int text_entry_event(const SDL_Event *e) {
    if (g_event && e->type == g_event) {
        pthread_mutex_lock(&mu);
        int ask = g_state == 1;
        if (ask) g_state = 2;
        pthread_mutex_unlock(&mu);
        if (ask) {
            runtime_input_clear();
            atomic_store(&g_block, 1);
            atomic_store(&g_visible, 1);
            screen_keyboard(1);
            vkp_wake();
            fprintf(stderr, "[totk] text field open (%s)\n", g_deck ? "Steam's on-screen keyboard" : "keyboard");
        }
        return 1;
    }
    if (atomic_load(&g_block) != 1) return 0;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT: {
        const unsigned char *s = (const unsigned char *)e->text.text;
        pthread_mutex_lock(&mu);
        while (s && *s) {
            uint32_t c; int k;
            if (*s < 0x80) c = *s, k = 0;
            else if ((*s & 0xe0) == 0xc0) c = *s & 0x1f, k = 1;
            else if ((*s & 0xf0) == 0xe0) c = *s & 0x0f, k = 2;
            else if ((*s & 0xf8) == 0xf0) c = *s & 0x07, k = 3;
            else { s++; continue; }
            s++;
            for (int i = 0; i < k && *s; i++, s++) c = c << 6 | (*s & 0x3f);
            append(c);
        }
        pthread_mutex_unlock(&mu);
        return 1;
    }
    case SDL_EVENT_KEY_DOWN:
        switch (e->key.scancode) {
        case SDL_SCANCODE_BACKSPACE: backspace(0); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: if (!e->key.repeat) finish(1); break;
        case SDL_SCANCODE_ESCAPE: if (!e->key.repeat) finish(0); break;
        default: break;
        }
        return 1;
    case SDL_EVENT_KEY_UP: return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {   /* by the Switch button it is (window_sdl.c: the layout and bindings) */
        uint64_t window_pad_bits(int button);
        uint64_t bits = window_pad_bits(e->gbutton.button);
        if (bits & (1 | 1024)) finish(1);                                  /* A, + */
        else if (bits & 2) backspace(1);                                   /* B */
        else if (bits & 8) { screen_keyboard(0); screen_keyboard(1); }    /* Y: the keyboard again */
        return 0;   /* (window_sdl.c still updates the controller: the game gets it held) */
    }
    default: return 0;
    }
}

/* The game's input is held: the field is open, or keys / buttons pressed for it are still down. */
int text_entry_blocks_input(void) { return atomic_load(&g_block) != 0; }

/* After the window loop's events: lets the game's input go once every key and button is up. */
void text_entry_tick(int pad_down) {
    if (atomic_load(&g_block) != 2 || pad_down) return;
    int n = 0;
    const bool *keys = SDL_GetKeyboardState(&n);
    for (int i = 0; i < n; i++) if (keys[i]) return;
    atomic_store(&g_block, 0);
}

/* ---- the present thread ---- */
int text_entry_visible(void) { return atomic_load(&g_visible); }

typedef struct { stbtt_fontinfo info; int ok; float scale; } face_t;
enum { FACES = 7 };
static face_t g_face[FACES];   /* Inter, then the system fonts 0..5 */
static int g_faces_fs;

static void faces(int fs) {
    static int loaded;
    if (!loaded) {
        loaded = 1;
        g_face[0].ok = stbtt_InitFont(&g_face[0].info, te_inter_ttf, stbtt_GetFontOffsetForIndex(te_inter_ttf, 0));
        static const int order[FACES - 1] = {0, 1, 2, 3, 4, 5};
        for (int i = 0; i < FACES - 1; i++) {
            size_t n = 0;
            const uint8_t *p = runtime_shared_font_sfnt(order[i], &n);
            g_face[i + 1].ok = p && stbtt_InitFont(&g_face[i + 1].info, p, stbtt_GetFontOffsetForIndex(p, 0));
        }
    }
    if (g_faces_fs != fs) {
        g_faces_fs = fs;
        for (int i = 0; i < FACES; i++) if (g_face[i].ok) g_face[i].scale = stbtt_ScaleForPixelHeight(&g_face[i].info, (float)fs);
    }
}
static face_t *face_for(uint32_t c, int *glyph) {
    for (int i = 0; i < FACES; i++)
        if (g_face[i].ok && (*glyph = stbtt_FindGlyphIndex(&g_face[i].info, (int)c))) return &g_face[i];
    *glyph = 0;
    return g_face[0].ok ? &g_face[0] : NULL;   /* Inter's missing-glyph box */
}

typedef struct { uint8_t *px; int w, h, bgr; } canvas_t;
static void fill(canvas_t *cv_, int x, int y, int w, int h, const uint8_t rgb[3]) {
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= cv_->h) continue;
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= cv_->w) continue;
            uint8_t *q = cv_->px + ((size_t)j * cv_->w + i) * 4;
            q[0] = cv_->bgr ? rgb[2] : rgb[0]; q[1] = rgb[1]; q[2] = cv_->bgr ? rgb[0] : rgb[2]; q[3] = 0xff;
        }
    }
}
/* Draws (or with cv_ NULL measures) the text at pen x on the baseline; glyphs outside [clip0, clip1) are cut. */
static int text(canvas_t *cv_, const uint32_t *s, int n, int fs, int x, int base, int clip0, int clip1, const uint8_t rgb[3]) {
    static uint8_t bm[256 * 256];
    faces(fs);
    int pen = x;
    for (int k = 0; k < n; k++) {
        int g;
        face_t *f = face_for(s[k], &g);
        if (!f) continue;
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetGlyphHMetrics(&f->info, g, &adv, &lsb);
        if (cv_) {
            stbtt_GetGlyphBitmapBox(&f->info, g, f->scale, f->scale, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0 && gw <= 256 && gh <= 256) {
                stbtt_MakeGlyphBitmap(&f->info, bm, gw, gh, gw, f->scale, f->scale, g);
                for (int j = 0; j < gh; j++) {
                    int py = base + y0 + j;
                    if (py < 0 || py >= cv_->h) continue;
                    for (int i = 0; i < gw; i++) {
                        int pxx = pen + x0 + i, a = bm[j * gw + i];
                        if (!a || pxx < clip0 || pxx >= clip1 || pxx < 0 || pxx >= cv_->w) continue;
                        uint8_t *q = cv_->px + ((size_t)py * cv_->w + pxx) * 4;
                        uint8_t c[3] = {cv_->bgr ? rgb[2] : rgb[0], rgb[1], cv_->bgr ? rgb[0] : rgb[2]};
                        for (int ch = 0; ch < 3; ch++) q[ch] = (uint8_t)((q[ch] * (255 - a) + c[ch] * a) / 255);
                    }
                }
            }
        }
        pen += (int)(adv * f->scale + 0.5f);
    }
    return pen - x;
}

/* The panel into px (cap bytes, rows of *w pixels), its place on the screen in *x0 / *y0. 0: nothing to draw. */
int text_entry_panel(uint8_t *px, size_t cap, uint32_t sw, uint32_t sh, int bgr, uint32_t *x0, uint32_t *y0, uint32_t *w,
                     uint32_t *h) {
    if (!atomic_load(&g_visible) || sw < 64 || sh < 64) return 0;
    layout_t L = layout((int)sw, (int)sh);
    if (L.w <= 0 || L.h <= 0 || (size_t)L.w * L.h * 4 > cap || L.x0 < 0 || L.y0 + L.h > (int)sh) return 0;
    uint32_t s[TE_MAX];
    pthread_mutex_lock(&mu);
    int n = g_n;
    memcpy(s, g_text, sizeof *s * (size_t)n);
    pthread_mutex_unlock(&mu);
    canvas_t c = {px, L.w, L.h, bgr};
    static const uint8_t bg[3] = {22, 24, 28}, edge[3] = {196, 170, 104}, field[3] = {8, 9, 11}, white[3] = {242, 242, 242},
                         grey[3] = {160, 160, 160};
    int b = L.fs / 24 > 1 ? L.fs / 24 : 1;
    fill(&c, 0, 0, L.w, L.h, edge);
    fill(&c, b, b, L.w - 2 * b, L.h - 2 * b, bg);
    int fx = L.fx - L.x0, fy = L.fy - L.y0;
    fill(&c, fx, fy, L.fw, L.fh, field);
    /* the text, its end kept in view; the caret after it, blinking */
    int inner0 = fx + L.fs / 4, inner1 = fx + L.fw - L.fs / 4;
    int width = text(NULL, s, n, L.fs, 0, 0, 0, 0, white);
    int start = width > inner1 - inner0 - L.fs / 4 ? inner1 - L.fs / 4 - width : inner0;
    int base = fy + L.fh / 2 + L.fs * 7 / 20;
    text(&c, s, n, L.fs, start, base, inner0, inner1, white);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (ts.tv_nsec < 500000000)
        fill(&c, start + width + L.fs / 12, fy + L.fh / 5, L.fs / 14 > 1 ? L.fs / 14 : 2, L.fh * 3 / 5, white);
    /* the keys */
    const char *hint = g_deck ? "A  OK      B  delete      Y  keyboard" : "Enter / A  OK      Backspace / B  delete      Esc  cancel";
    uint32_t hs[96];
    int hn = 0;
    for (const char *p = hint; *p && hn < 96; p++) hs[hn++] = (unsigned char)*p;
    int hw = text(NULL, hs, hn, L.hint_fs, 0, 0, 0, 0, grey);
    text(&c, hs, hn, L.hint_fs, (L.w - hw) / 2, L.hint_y - L.y0, 0, L.w, grey);
    *x0 = (uint32_t)L.x0; *y0 = (uint32_t)L.y0; *w = (uint32_t)L.w; *h = (uint32_t)L.h;
    return 1;
}
