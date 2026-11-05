/* The game's button prompts for an Xbox / Steam Deck layout (pad_layout=xbox, window_sdl.c binds_load: Switch A is the
 * bottom face button, B the right one, X the left one, Y the top one). Applied to the decompressed
 * UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc at start (host/ui_capture.c totk_ui_build, host/fs.c serves the
 * result), on the player's machine; nothing changes size, so the game's resource size table holds.
 *
 *  - The action prompt (blyt/DoCommand_00.bflyt) is a diamond of the four face buttons with the one to press marked and
 *    its action written beside it. Each button has its group (N_A_00 ... N_Y_00, shown by DoCommand_00_ButtonPattern):
 *    the diamond's base (P_Base?_0n), the other three letters (T_?_0n) and the marked button (N_Btn?_0n: its key, the
 *    action's box and words). On the Switch A is on the right, B at the bottom, X on top, Y on the left. Here each
 *    button goes where it is on an Xbox pad: A and B, X and Y trade places, the base takes the other's look (texture,
 *    turn, mirror), and the action's box and words of X and Y trade sides (Y's open to the left: on an Xbox pad X is
 *    the left one). Every pane is checked against the Switch layout first; anything else leaves the archive as it is.
 *  - The button guide in play (bottom right) draws L and ZL from timg/__Combined.bntx (Nt_KeyTexL_00, Nt_KeyTexZL_00:
 *    R8, 108 x 108, one level, block linear): the key's shape stays, its letters become LB / LT (L1 / L2 on the Steam
 *    Deck), drawn with Inter (compiled in, host/text_entry.c).
 * The prompts drawn in the game's button font (L, R, ZL, ZR, +, - in texts) are not changed here.
 * The idea of redrawing the guide's key images is MacNativeGames' / totk-pc-recomp's (MIT, tools/button_icons.py);
 * the diamond and the code are ours. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"

extern const unsigned char te_inter_ttf[];   /* host/text_entry.c */

int totk_sarc_member(const uint8_t *d, size_t n, const char *name, size_t *lo, size_t *hi);   /* host/ui_capture.c */

/* ---- the layout's panes */
enum { MAXP = 1024 };
typedef struct { size_t at; char kind[5]; char name[25]; } pane;
static pane P[MAXP];
static int NP;

static int panes(const uint8_t *d, size_t lo, size_t hi) {
    NP = 0;
    if (hi - lo < 0x14 || memcmp(d + lo, "FLYT", 4)) return 0;
    uint16_t hdr;
    memcpy(&hdr, d + lo + 6, 2);
    size_t pos = lo + hdr;
    while (pos + 8 <= hi) {
        uint32_t size;
        memcpy(&size, d + pos + 4, 4);
        if (size < 8 || pos + size > hi) return 0;
        if (!memcmp(d + pos, "pan1", 4) || !memcmp(d + pos, "pic1", 4) || !memcmp(d + pos, "txt1", 4) ||
            !memcmp(d + pos, "wnd1", 4) || !memcmp(d + pos, "prt1", 4) || !memcmp(d + pos, "bnd1", 4)) {
            if (size < 0x54 || NP == MAXP) return 0;   /* (a pane's own fields; a picture's material and a text's
                                                          position after them: checked below) */
            if ((!memcmp(d + pos, "pic1", 4) && size < 0x68) || (!memcmp(d + pos, "txt1", 4) && size < 0x60)) return 0;
            pane *p = &P[NP++];
            p->at = pos;
            memcpy(p->kind, d + pos, 4), p->kind[4] = 0;
            memcpy(p->name, d + pos + 0x0C, 24), p->name[24] = 0;
        }
        pos += size;
    }
    return NP;
}
static pane *find(const char *name) {
    for (int i = 0; i < NP; i++) if (!strcmp(P[i].name, name)) return &P[i];
    return NULL;
}
/* pane fields: origin +0x09, translate +0x2C, rotate +0x38, scale +0x44; picture material +0x64; text position +0x5C */
static void get_t(const uint8_t *d, const pane *p, float t[2]) { memcpy(t, d + p->at + 0x2C, 8); }
static void set_t(uint8_t *d, const pane *p, float x, float y) { float t[2] = {x, y}; memcpy(d + p->at + 0x2C, t, 8); }
static int near(const float t[2], float x, float y) { return fabsf(t[0] - x) < 0.5f && fabsf(t[1] - y) < 0.5f; }

/* where each face button and its letter are on the Switch (A right, B bottom, X top, Y left) */
static const char LET[4] = {'A', 'B', 'X', 'Y'};
static const float BTN[4][2] = {{39, 0}, {0, -39}, {0, 39}, {-39, 0}};
static const float LETTER[4][2] = {{39, 1}, {2, -39}, {1, 39}, {-38, 0}};
static const float RUSH[4][2] = {{129, 6}, {90, -38}, {90, 38}, {-129, 0}};
static int partner(int k) { return k ^ 1; }   /* A <-> B, X <-> Y */
static int letter_index(char c) { for (int k = 0; k < 4; k++) if (LET[k] == c) return k; return -1; }

/* the pane's name is <prefix><letter><suffix> with suffix "_0<digit>": the letter's index, else -1 */
static int named(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n)) return -1;
    int k = letter_index(name[n]);
    if (k < 0 || name[n + 1] != '_' || name[n + 2] != '0' || name[n + 3] < '0' || name[n + 3] > '9' || name[n + 4]) return -1;
    return k;
}
/* two panes trade where they are and how they face: translate, rotate, scale, origin (and a text's position) */
static void trade(uint8_t *d, const pane *a, const pane *b) {
    uint8_t t[0x24], o;
    memcpy(t, d + a->at + 0x2C, 0x24); memcpy(d + a->at + 0x2C, d + b->at + 0x2C, 0x24); memcpy(d + b->at + 0x2C, t, 0x24);
    o = d[a->at + 9]; d[a->at + 9] = d[b->at + 9]; d[b->at + 9] = o;
    if (!strcmp(a->kind, "txt1") && !strcmp(b->kind, "txt1")) { o = d[a->at + 0x5C]; d[a->at + 0x5C] = d[b->at + 0x5C]; d[b->at + 0x5C] = o; }
}

static int nope(const char *what, const char *name) {   /* why the diamond is left as it is, into the log */
    fprintf(stderr, "[totk] button prompts: DoCommand_00 is not as the Switch layout (%s%s%s)\n", what, name ? " " : "", name ? name : "");
    return 0;
}

static int diamond(uint8_t *blarc, size_t n) {
    size_t lo, hi;
    if (!totk_sarc_member(blarc, n, "blyt/DoCommand_00.bflyt", &lo, &hi) || !panes(blarc, lo, hi)) return nope("no layout", NULL);
    uint8_t *d = blarc;
    /* the X / Y pairs whose sides trade (each a box and words that open to the right for X, to the left for Y) */
    static const char *const sides[][2] = {
        {"P_CaptureX_00", "P_CaptureY_00"}, {"W_BaseX_00", "W_BaseY_00"}, {"T_GuideX_00", "T_GuideY_00"},
        {"T_GuideX_TW", "T_GuideY_TW"}, {"N_CaptureX_00", "N_CaptureY_00"}, {"T_CaptureGuideX_00", "T_CaptureGuideY_00"},
        {"T_CaptureGuideX_TW", "T_CaptureGuideY_TW"}, {"P_CaptureKeyX_00", "P_CaptureKeyY_00"}};
    /* 1. every pane as the Switch layout has it, before anything changes */
    struct { pane *p; int k; int what; } todo[256];
    int nt = 0, bases = 0;
    pane *base[4] = {0};
    uint8_t look[4][0x24];   /* rotate, scale (from +0x38) are kept with the material below */
    uint16_t mat[4];
    for (int i = 0; i < NP && nt < 256; i++) {
        pane *p = &P[i];
        float t[2];
        get_t(d, p, t);
        int k;
        if (!strcmp(p->kind, "txt1") && !strncmp(p->name, "T_", 2) && (k = named(p->name, "T_")) >= 0) {
            if (!near(t, LETTER[k][0], LETTER[k][1])) return nope("a letter elsewhere:", p->name);
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 0;
        } else if (!strcmp(p->kind, "pan1") && ((k = named(p->name, "N_Btn")) >= 0 || (k = named(p->name, "N_CompanionBtn")) >= 0)) {
            if (!near(t, BTN[k][0], BTN[k][1])) return nope("a button elsewhere:", p->name);
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 1;
        } else if (!strcmp(p->kind, "pic1") && (k = named(p->name, "P_Base")) >= 0) {
            if (!strcmp(p->name + 7, "_00")) {
                base[k] = p;
                memcpy(look[k], d + p->at + 0x38, 0x14);
                memcpy(&mat[k], d + p->at + 0x64, 2);
                bases++;
            }
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 2;
        } else if (!strcmp(p->kind, "pic1") && !strncmp(p->name, "P_Capture", 9) && (k = letter_index(p->name[9])) >= 0 &&
                   p->name[10] == '_' && p->name[11] == '0' && p->name[12] >= '1' && p->name[12] <= '3' && !p->name[13]) {
            if (!near(t, RUSH[k][0], RUSH[k][1]) && !(k == 0 && near(t, 29, 6))) return nope("an effect elsewhere:", p->name);   /* (A's hold: below) */
            if (near(t, RUSH[k][0], RUSH[k][1])) todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 3;
        }
    }
    pane *hold = find("N_LoopA_03");
    float ht[2] = {0, 0};
    if (hold) get_t(d, hold, ht);
    if (bases != 4) return nope("not four diamond bases", NULL);
    if (nt < 20) return nope("too few panes", NULL);
    if (!hold || !near(ht, 100, 0)) return nope("the hold effect elsewhere", NULL);
    for (unsigned s = 0; s < sizeof sides / sizeof *sides; s++)
        if (!find(sides[s][0]) || !find(sides[s][1])) return nope("missing", find(sides[s][0]) ? sides[s][1] : sides[s][0]);
    /* 2. where they go on an Xbox pad */
    for (int i = 0; i < nt; i++) {
        pane *p = todo[i].p;
        int q = partner(todo[i].k);
        switch (todo[i].what) {
        case 0: set_t(d, p, LETTER[q][0], LETTER[q][1]); break;
        case 1: set_t(d, p, BTN[q][0], BTN[q][1]); break;
        case 2: memcpy(d + p->at + 0x38, look[q], 0x14); memcpy(d + p->at + 0x64, &mat[q], 2); break;
        case 3: set_t(d, p, RUSH[q][0], RUSH[q][1]); break;
        }
    }
    set_t(d, hold, RUSH[1][0] - 29, RUSH[1][1] - 6);   /* A's hold effect, (29, 6) in it: where B's rush is */
    for (unsigned s = 0; s < sizeof sides / sizeof *sides; s++) trade(d, find(sides[s][0]), find(sides[s][1]));
    return nt;
}

/* ---- the button guide's L and ZL keys */
static size_t gob_addr(unsigned x, unsigned y, unsigned width, unsigned bh) {   /* Tegra block linear, 1 byte a pixel */
    size_t wg = (width + 63) / 64;
    return (size_t)(y / (8 * bh)) * 512 * bh * wg + (size_t)(x / 64) * 512 * bh + (size_t)((y % (8 * bh)) / 8) * 512 +
           ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

/* The texture named name (BRTI, the name without its "^d"): its pixels' offset, width, height, block height. */
static int texture(const uint8_t *b, size_t n, const char *name, size_t *data, unsigned *w, unsigned *h, unsigned *bh) {
    size_t want = strlen(name);
    for (size_t i = 0; i + 0x78 <= n; i++) {
        if (memcmp(b + i, "BRTI", 4)) continue;
        uint64_t np, mp, dp;
        uint16_t nl, mips;
        uint32_t fmt, ww, hh, layout, size;
        memcpy(&np, b + i + 0x60, 8);
        if (np + 2 > n) continue;
        memcpy(&nl, b + np, 2);
        if (np + 2 + nl > n || nl < want || memcmp(b + np + 2, name, want) || (nl > want && b[np + 2 + want] != '^')) continue;
        memcpy(&mips, b + i + 0x16, 2); memcpy(&fmt, b + i + 0x1C, 4); memcpy(&ww, b + i + 0x24, 4); memcpy(&hh, b + i + 0x28, 4);
        memcpy(&layout, b + i + 0x34, 4); memcpy(&size, b + i + 0x50, 4); memcpy(&mp, b + i + 0x70, 8);
        if (mips != 1 || fmt != 0x201 || ww != 108 || hh != 108 || mp + 8 > n) return 0;
        memcpy(&dp, b + mp, 8);
        *bh = 1u << (layout & 7);
        if (dp + size > n || gob_addr(ww - 1, hh - 1, ww, *bh) >= size) return 0;
        *data = (size_t)dp, *w = ww, *h = hh;
        return 1;
    }
    return 0;
}

/* The key's letters replaced: inside the key everything white again (row by row between its edges), then the new
   label cut out in black, centred on the key, as tall as the game's letters (about 38 % of the image), bold. */
static void relabel(uint8_t *b, size_t data, unsigned w, unsigned h, unsigned bh, const char *label) {
    uint8_t *px = malloc((size_t)w * h);
    if (!px) return;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) px[y * w + x] = b[data + gob_addr(x, y, w, bh)];
    unsigned x0 = w, x1 = 0, y0 = h, y1 = 0;
    for (unsigned y = 0; y < h; y++) {
        int first = -1, last = -1;
        for (unsigned x = 0; x < w; x++) if (px[y * w + x] > 200) { if (first < 0) first = (int)x; last = (int)x; }
        if (first < 0) continue;
        for (int x = first; x <= last; x++) px[y * w + x] = 255;
        if ((unsigned)first < x0) x0 = (unsigned)first;
        if ((unsigned)last > x1) x1 = (unsigned)last;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    stbtt_fontinfo f;
    if (x1 > x0 && y1 > y0 && stbtt_InitFont(&f, te_inter_ttf, stbtt_GetFontOffsetForIndex(te_inter_ttf, 0))) {
        float cap = h * 0.38f, scale = stbtt_ScaleForPixelHeight(&f, cap / 0.73f);   /* Inter's capitals: 0.73 em */
        int adv_total = 0, lsb;
        for (const char *c = label; *c; c++) { int adv; stbtt_GetCodepointHMetrics(&f, *c, &adv, &lsb); adv_total += adv; }
        float tw = adv_total * scale, pen = (x0 + x1 + 1) / 2.0f - tw / 2.0f, base = (y0 + y1 + 1) / 2.0f + cap / 2.0f;
        static uint8_t g[128 * 128];
        for (const char *c = label; *c; c++) {
            int adv, gx0, gy0, gx1, gy1;
            stbtt_GetCodepointHMetrics(&f, *c, &adv, &lsb);
            stbtt_GetCodepointBitmapBox(&f, *c, scale, scale, &gx0, &gy0, &gx1, &gy1);
            int gw = gx1 - gx0, gh = gy1 - gy0;
            if (gw > 0 && gh > 0 && gw <= 128 && gh <= 128) {
                stbtt_MakeCodepointBitmap(&f, g, gw, gh, gw, scale, scale, *c);
                for (int dd = 0; dd < 6; dd++)   /* drawn 3 x 2 times a pixel apart: as bold as the game's letters */
                    for (int j = 0, dx = dd % 3, dy = dd / 3; j < gh; j++)
                        for (int i = 0; i < gw; i++) {
                            int x = (int)(pen + 0.5f) + gx0 + i + dx - 1, y = (int)(base + 0.5f) + gy0 + j + dy;
                            if (x < 0 || y < 0 || x >= (int)w || y >= (int)h) continue;
                            int v = px[y * w + x] - g[j * gw + i];
                            px[y * w + x] = (uint8_t)(v < 0 ? 0 : v);
                        }
            }
            pen += adv * scale;
        }
    }
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) b[data + gob_addr(x, y, w, bh)] = px[y * w + x];
    free(px);
}

static int guide_keys(uint8_t *blarc, size_t n, int deck) {
    size_t lo, hi, dl, dz;
    unsigned w, h, bh, w2, h2, bh2;
    if (!totk_sarc_member(blarc, n, "timg/__Combined.bntx", &lo, &hi) || memcmp(blarc + lo, "BNTX", 4)) return 0;
    uint8_t *b = blarc + lo;
    size_t bn = hi - lo;
    if (!texture(b, bn, "Nt_KeyTexL_00", &dl, &w, &h, &bh) || !texture(b, bn, "Nt_KeyTexZL_00", &dz, &w2, &h2, &bh2)) return 0;
    relabel(b, dl, w, h, bh, deck ? "L1" : "LB");
    relabel(b, dz, w2, h2, bh2, deck ? "L2" : "LT");
    return 1;
}

/* The prompts for the Xbox / Steam Deck layout into the decompressed layout archive. deck: the guide's keys named as the
   Steam Deck's (L1, L2). 1 when anything changed; each part that does not find the archive as the Switch's leaves it. */
int totk_ui_prompts_apply(uint8_t *blarc, size_t n, int deck) {
    int a = diamond(blarc, n), b = guide_keys(blarc, n, deck);
    fprintf(stderr, "[totk] button prompts for the Xbox layout: the action diamond %s, the guide's keys %s\n",
            a ? "rearranged (A bottom, B right, X left, Y top)" : "not as expected: left as it is",
            b ? (deck ? "L1 / L2" : "LB / LT") : "not as expected: left as they are");
    return a || b;
}
