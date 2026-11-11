/* The game's button prompts after the player's controls (window_sdl.c binds_load: pad_layout=xbox puts Switch A on the
 * bottom face button, B on the right, X on the left, Y on top; pad_<action> binds any action to other inputs). Applied
 * to the decompressed UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc at start (host/ui_capture.c totk_ui_build,
 * host/fs.c serves the result), on the player's machine; nothing changes size, so the game's resource size table holds.
 *
 *  - The action prompt (blyt/DoCommand_00.bflyt) is a diamond of the four face buttons with the one to press marked and
 *    its action written beside it. Each Switch button has its group (N_A_00 ... N_Y_00, shown by
 *    DoCommand_00_ButtonPattern): the diamond's base (P_Base?_0n), the other three letters (T_?_0n) and the marked
 *    button (N_Btn?_0n: its key P_Key*, the action's box and words). On the Switch A is on the right, B at the bottom,
 *    X on top, Y on the left. Here each group marks the place of the face button its Switch button is bound to, with
 *    that button's letter in the layout (Xbox: A bottom, B right, X left, Y top); the base takes the look that marks
 *    that place (the game's own two textures, turned), the action's box opens to the left on the left place, and the
 *    other letters go to their places in the layout. A letter whose place is the marked one is hidden; a place whose
 *    letter is the marked button's own (a button bound away from its place) stays without one: the panes' letters come
 *    from the game's messages, one per pane. A Switch button bound to no face button keeps the layout's place.
 *    Every pane is checked against the Switch layout first; anything else leaves the archive as it is.
 *  - The button guide in play (bottom right) draws L and ZL from timg/__Combined.bntx (Nt_KeyTexL_00, Nt_KeyTexZL_00:
 *    R8, 108 x 108, one level, block linear): the key's shape stays, its letters become the bound input's name (LB / LT,
 *    L1 / L2 on the Steam Deck), drawn with Inter (compiled in, host/text_entry.c).
 *  - The symbols in the game's texts (its button font, in the Font archives): each action's symbol becomes that of the button
 *    it is bound to (below, totk_ui_prompts_font).
 * The idea of redrawing the guide's key images is MacNativeGames' (MIT);
 * the diamond and the code are ours. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../../third_party/stb/stb_truetype.h"
#include "../runtime/bufcat.h"

extern const unsigned char te_inter_ttf[];   /* host/text_entry.c */

int totk_sarc_member(const uint8_t *d, size_t n, const char *name, size_t *lo, size_t *hi);   /* host/ui_capture.c */

/* ---- the controls (as window_sdl.c binds_load reads them) */
static int xbox_layout(void) { const char *v = getenv("TOTK_BIND_pad_layout"); return v && !strcmp(v, "xbox"); }
static int on_deck(void) { const char *v = getenv("SteamDeck"); return v && !strcmp(v, "1"); }
/* the first SDL gamepad name bound to the Switch action (pad_<id>), else the layout's default */
static void bound(const char *id, const char *dflt, char *out, size_t cap) {
    char var[48];
    snprintf(var, sizeof var, "TOTK_BIND_pad_%s", id);
    const char *v = getenv(var);
    if (!v || !*v) v = dflt;
    while (*v == ' ') v++;
    size_t n = strcspn(v, ", ");
    if (n >= cap) n = cap - 1;
    memcpy(out, v, n), out[n] = 0;
}
/* places, as the Switch's buttons are: 0 right (A), 1 bottom (B), 2 top (X), 3 left (Y) */
static int place_of(const char *sdl) {   /* SDL names face buttons by place: a bottom, b right, x left, y top */
    return !strcmp(sdl, "b") ? 0 : !strcmp(sdl, "a") ? 1 : !strcmp(sdl, "y") ? 2 : !strcmp(sdl, "x") ? 3 : -1;
}
/* the letter (0 A, 1 B, 2 X, 3 Y) on each place in the layout */
static int letter_at(int place) {
    static const int xbox[4] = {1, 0, 3, 2};   /* right B, bottom A, top Y, left X */
    return xbox_layout() ? xbox[place] : place;
}
static const char *const FACE_ID[4] = {"a", "b", "x", "y"};
static int face_place(int k) {   /* where Switch button k is: its binding's place, else the layout's */
    static const char *const nin[4] = {"b", "a", "y", "x"}, *const xb[4] = {"a", "b", "x", "y"};
    char s[32];
    bound(FACE_ID[k], xbox_layout() ? xb[k] : nin[k], s, sizeof s);
    int p = place_of(s);
    if (p < 0) {
        fprintf(stderr, "[totk] button prompts: %c is bound to %s, not a face button: the diamond marks the layout's place\n", "ABXY"[k], s);
        p = place_of(xbox_layout() ? xb[k] : nin[k]);
    }
    return p;
}
/* an input's name in the layout (the guide's keys) */
static const char *input_label(const char *n) {
    int x = xbox_layout(), deck = on_deck(), p = place_of(n);
    static const char *const L[4] = {"A", "B", "X", "Y"};
    if (p >= 0) return L[letter_at(p)];
    if (!strcmp(n, "leftshoulder")) return deck ? "L1" : x ? "LB" : "L";
    if (!strcmp(n, "rightshoulder")) return deck ? "R1" : x ? "RB" : "R";
    if (!strcmp(n, "lefttrigger")) return deck ? "L2" : x ? "LT" : "ZL";
    if (!strcmp(n, "righttrigger")) return deck ? "R2" : x ? "RT" : "ZR";
    if (!strcmp(n, "back")) return x || deck ? "View" : "-";
    if (!strcmp(n, "start")) return x || deck ? "Menu" : "+";
    if (!strcmp(n, "leftstick")) return "LS";
    if (!strcmp(n, "rightstick")) return "RS";
    if (!strncmp(n, "paddle", 6)) return n[6] == '1' ? "P1" : n[6] == '2' ? "P2" : n[6] == '3' ? "P3" : "P4";
    return NULL;   /* (the d-pad and the rest: the game's key stays) */
}

/* Whether the prompts differ from the game's: the Xbox layout, or a face button, L or ZL bound elsewhere. */
int totk_ui_prompts_wanted(void) {
    if (xbox_layout()) return 1;
    static const char *const ids[] = {"a", "b", "x", "y", "l", "zl"};
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; i++) {
        char var[48];
        snprintf(var, sizeof var, "TOTK_BIND_pad_%s", ids[i]);
        const char *v = getenv(var);
        if (v && *v) return 1;
    }
    return 0;
}

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
/* pane fields: flags +0x08 (bit 0 shown), origin +0x09, translate +0x2C, rotate +0x38, scale +0x44; picture material
   +0x64; text position +0x5C */
static void get_t(const uint8_t *d, const pane *p, float t[2]) { memcpy(t, d + p->at + 0x2C, 8); }
static void set_t(uint8_t *d, const pane *p, float x, float y) { float t[2] = {x, y}; memcpy(d + p->at + 0x2C, t, 8); }
static int near(const float t[2], float x, float y) { return fabsf(t[0] - x) < 0.5f && fabsf(t[1] - y) < 0.5f; }

/* by place (0 right, 1 bottom, 2 top, 3 left): the marked button, the other letters, the press effects */
static const float BTN[4][2] = {{39, 0}, {0, -39}, {0, 39}, {-39, 0}};
static const float LETTER[4][2] = {{39, 1}, {2, -39}, {1, 39}, {-38, 0}};
static const float RUSH[4][2] = {{129, 6}, {90, -38}, {90, 38}, {-129, 0}};
static const char LET[4] = {'A', 'B', 'X', 'Y'};
static int letter_index(char c) { for (int k = 0; k < 4; k++) if (LET[k] == c) return k; return -1; }

/* the pane's name is <prefix><letter>_0<digit>: the letter's index, else -1 */
static int named(const char *name, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(name, prefix, n)) return -1;
    int k = letter_index(name[n]);
    if (k < 0 || name[n + 1] != '_' || name[n + 2] != '0' || name[n + 3] < '0' || name[n + 3] > '9' || name[n + 4]) return -1;
    return k;
}
static int nope(const char *what, const char *name) {   /* why the diamond is left as it is, into the log */
    fprintf(stderr, "[totk] button prompts: DoCommand_00 is not as the Switch layout (%s%s%s)\n", what, name ? " " : "", name ? name : "");
    return 0;
}

/* letter panes that say another letter (a button bound away from its place): their messages, changed when the game
   reads its language's message archive (totk_ui_prompts_mals) */
enum { MAXMSG = 16 };
static struct { char label[25]; char letter; } msg[MAXMSG];
static int nmsg;

/* how a pane sits: translate, rotate, scale (0x24 bytes from +0x2C), origin and, for a text, its position */
typedef struct { uint8_t srt[0x24], origin, textpos; } sit;
static sit sit_of(const uint8_t *d, const pane *p) {
    sit s;
    memcpy(s.srt, d + p->at + 0x2C, 0x24);
    s.origin = d[p->at + 9];
    s.textpos = !strcmp(p->kind, "txt1") ? d[p->at + 0x5C] : 0;
    return s;
}
static void sit_set(uint8_t *d, const pane *p, const sit *s) {
    memcpy(d + p->at + 0x2C, s->srt, 0x24);
    d[p->at + 9] = s->origin;
    if (!strcmp(p->kind, "txt1")) d[p->at + 0x5C] = s->textpos;
}

static int diamond(uint8_t *blarc, size_t n) {
    size_t lo, hi;
    nmsg = 0;
    if (!totk_sarc_member(blarc, n, "blyt/DoCommand_00.bflyt", &lo, &hi) || !panes(blarc, lo, hi)) return nope("no layout", NULL);
    uint8_t *d = blarc;
    /* the action's box and words: X's open to the right, Y's to the left (the templates); per group the panes that turn
       when its button goes to the other side (A's second button and its stop state included) */
    static const char *const side[][2] = {
        {"P_CaptureX_00", "P_CaptureY_00"}, {"W_BaseX_00", "W_BaseY_00"}, {"T_GuideX_00", "T_GuideY_00"},
        {"T_GuideX_TW", "T_GuideY_TW"}, {"N_CaptureX_00", "N_CaptureY_00"}, {"T_CaptureGuideX_00", "T_CaptureGuideY_00"},
        {"T_CaptureGuideX_TW", "T_CaptureGuideY_TW"}, {"P_CaptureKeyX_00", "P_CaptureKeyY_00"}};
    enum { NSIDE = sizeof side / sizeof *side };
    static const char *const group_side[4][NSIDE + 4] = {
        {"P_CaptureA_00", "W_BaseA_00", "T_GuideA_00", "T_GuideA_TW", "N_CaptureA_00", "T_CaptureGuideA_00",
         "T_CaptureGuideA_TW", "P_CaptureKeyA_00", "P_CaptureA_04", "W_BaseA_01", "T_GuideA_01", "T_GuideA_TW_00"},
        {"P_CaptureB_00", "W_BaseB_00", "T_GuideB_00", "T_GuideB_TW", "N_CaptureB_00", "T_CaptureGuideB_00",
         "T_CaptureGuideB_TW", "P_CaptureKeyB_00"},
        {"P_CaptureX_00", "W_BaseX_00", "T_GuideX_00", "T_GuideX_TW", "N_CaptureX_00", "T_CaptureGuideX_00",
         "T_CaptureGuideX_TW", "P_CaptureKeyX_00"},
        {"P_CaptureY_00", "W_BaseY_00", "T_GuideY_00", "T_GuideY_TW", "N_CaptureY_00", "T_CaptureGuideY_00",
         "T_CaptureGuideY_TW", "P_CaptureKeyY_00"}};
    /* the A group's extra ones map to these templates: P_CaptureA_04 like P_Capture?_00, W_BaseA_01 like W_Base?_00, ... */
    static const int extra_tpl[4] = {0, 1, 2, 3};
    /* 1. every pane as the Switch layout has it, before anything changes */
    struct { pane *p; int k; int what; } todo[256];
    int nt = 0, bases = 0;
    uint8_t look[4][0x14];   /* rotate, scale (from +0x38) */
    uint16_t mat[4], keymat[4];
    int keys = 0;
    for (int i = 0; i < NP && nt < 256; i++) {
        pane *p = &P[i];
        float t[2];
        get_t(d, p, t);
        int k;
        if (!strcmp(p->kind, "txt1") && (k = named(p->name, "T_")) >= 0) {
            if (!near(t, LETTER[k][0], LETTER[k][1])) return nope("a letter elsewhere:", p->name);
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 0;
        } else if (!strcmp(p->kind, "pan1") && ((k = named(p->name, "N_Btn")) >= 0 || (k = named(p->name, "N_CompanionBtn")) >= 0)) {
            if (!near(t, BTN[k][0], BTN[k][1])) return nope("a button elsewhere:", p->name);
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 1;
        } else if (!strcmp(p->kind, "pic1") && (k = named(p->name, "P_Base")) >= 0) {
            if (!strcmp(p->name + 7, "_00")) {
                memcpy(look[k], d + p->at + 0x38, 0x14);
                memcpy(&mat[k], d + p->at + 0x64, 2);
                bases++;
            }
            todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 2;
        } else if (!strcmp(p->kind, "pic1") && !strncmp(p->name, "P_Capture", 9) && (k = letter_index(p->name[9])) >= 0 &&
                   p->name[10] == '_' && p->name[11] == '0' && p->name[12] >= '1' && p->name[12] <= '3' && !p->name[13]) {
            if (!near(t, RUSH[k][0], RUSH[k][1]) && !(k == 0 && near(t, 29, 6))) return nope("an effect elsewhere:", p->name);
            if (near(t, RUSH[k][0], RUSH[k][1])) todo[nt].p = p, todo[nt].k = k, todo[nt++].what = 3;
        } else if (!strcmp(p->kind, "pic1") && !strncmp(p->name, "P_Key", 5) && p->name[6] == '_' && p->name[7] == '0' && !p->name[9]) {
            /* the marked key's picture: P_KeyA_0n (A), P_KeyB_00, P_KeyX_00, P_Key0_00 (Y's) */
            int kk = p->name[5] == '0' ? 3 : letter_index(p->name[5]);
            if (kk < 0) continue;
            if (!strcmp(p->name + 6, "_00")) memcpy(&keymat[kk], d + p->at + 0x64, 2), keys |= 1 << kk;
            todo[nt].p = p, todo[nt].k = kk, todo[nt++].what = 4;
        }
    }
    pane *hold = find("N_LoopA_03");
    float ht[2] = {0, 0};
    if (hold) get_t(d, hold, ht);
    if (bases != 4) return nope("not four diamond bases", NULL);
    if (keys != 15) return nope("not four keys", NULL);
    if (nt < 20) return nope("too few panes", NULL);
    if (!hold || !near(ht, 100, 0)) return nope("the hold effect elsewhere", NULL);
    sit tpl[2][NSIDE];   /* [0] opens to the right (X's), [1] to the left (Y's) */
    for (int s = 0; s < NSIDE; s++) {
        pane *a = find(side[s][0]), *b = find(side[s][1]);
        if (!a || !b) return nope("missing", a ? side[s][1] : side[s][0]);
        tpl[0][s] = sit_of(d, a), tpl[1][s] = sit_of(d, b);
    }
    /* 2. where each group's button is now */
    int place[4], changed = 0;
    for (int k = 0; k < 4; k++) changed |= (place[k] = face_place(k)) != k || letter_at(place[k]) != k;
    if (!changed) return 0;   /* the game's own */
    for (int i = 0; i < nt; i++) {
        pane *p = todo[i].p;
        int k = todo[i].k;
        switch (todo[i].what) {   /* (the letters, what 0: below, by the group they are in) */
        case 1: set_t(d, p, BTN[place[k]][0], BTN[place[k]][1]); break;
        case 2: memcpy(d + p->at + 0x38, look[place[k]], 0x14); memcpy(d + p->at + 0x64, &mat[place[k]], 2); break;
        case 3: set_t(d, p, RUSH[place[k]][0], RUSH[place[k]][1]); break;
        case 4: memcpy(d + p->at + 0x64, &keymat[letter_at(place[k])], 2); break;   /* the letter on that place */
        default: break;
        }
    }
    /* the other letters: each to its place in the layout; hidden where its place is the group's marked one. A letter
       pane belongs to the group whose pane block it is in: the T_<L>_<n> panes of group G follow G's N_Loop?_00 */
    int group = -1;
    for (int i = 0; i < NP; i++) {
        pane *p = &P[i];
        int k;
        if (!strcmp(p->kind, "pan1") && (k = named(p->name, "N_Loop")) >= 0) group = k;
        else if (!strcmp(p->kind, "pan1") && (k = named(p->name, "N_Stop")) >= 0) group = k;
        if (strcmp(p->kind, "txt1") || (k = named(p->name, "T_")) < 0 || group < 0) continue;
        int to = -1, own = -1;
        for (int q = 0; q < 4; q++) {
            if (letter_at(q) == k) to = q;
            if (letter_at(q) == group) own = q;   /* where the group's own letter is on the pad */
        }
        if (to == place[group]) {
            /* under the marked button: this pane goes where the group's own letter is on the pad (a button bound away
               from its place leaves that place empty otherwise) and says that letter (host/fs.c: the game's messages,
               totk_ui_prompts_message) */
            if (own >= 0 && own != place[group] && nmsg < MAXMSG) {
                set_t(d, p, LETTER[own][0], LETTER[own][1]);
                snprintf(msg[nmsg].label, sizeof msg[nmsg].label, "%s", p->name);
                msg[nmsg++].letter = LET[group];
            } else
                d[p->at + 8] &= (uint8_t)~1;   /* hidden */
        } else
            set_t(d, p, LETTER[to][0], LETTER[to][1]);
    }
    set_t(d, hold, RUSH[place[0]][0] - 29, RUSH[place[0]][1] - 6);   /* A's hold effect, (29, 6) in it */
    /* the action's box opens to the left on the left place, else to the right */
    for (int k = 0; k < 4; k++) {
        int left = place[k] == 3;
        if (left == (k == 3)) continue;   /* as on the Switch */
        for (int s = 0; s < NSIDE + 4 && group_side[k][s]; s++) {
            pane *p = find(group_side[k][s]);
            if (p) sit_set(d, p, &tpl[left][s < NSIDE ? s : extra_tpl[s - NSIDE]]);
        }
    }
    static const char *const where[4] = {"right", "bottom", "top", "left"};
    fprintf(stderr, "[totk] button prompts: A %s (%c), B %s (%c), X %s (%c), Y %s (%c)\n", where[place[0]], LET[letter_at(place[0])],
            where[place[1]], LET[letter_at(place[1])], where[place[2]], LET[letter_at(place[2])], where[place[3]],
            LET[letter_at(place[3])]);
    return 1;
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
   label cut out in black, centred on the key, as tall as the game's letters (about 38 % of the image; smaller for a
   long name), bold. */
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
        float room = (x1 - x0) * 0.8f;   /* a long name ("Menu") smaller, within the key */
        if (adv_total * scale > room) cap *= room / (adv_total * scale), scale = room / adv_total;
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

static int guide_keys(uint8_t *blarc, size_t n) {
    char l[32], zl[32];
    bound("l", "leftshoulder", l, sizeof l);
    bound("zl", "lefttrigger", zl, sizeof zl);
    const char *ll = input_label(l), *zll = input_label(zl);
    int do_l = ll && strcmp(ll, "L"), do_zl = zll && strcmp(zll, "ZL");   /* (the game's own: nothing to draw) */
    if (!do_l && !do_zl) return 0;
    size_t lo, hi, dl, dz;
    unsigned w, h, bh, w2, h2, bh2;
    if (!totk_sarc_member(blarc, n, "timg/__Combined.bntx", &lo, &hi) || memcmp(blarc + lo, "BNTX", 4)) return 0;
    uint8_t *b = blarc + lo;
    size_t bn = hi - lo;
    if (!texture(b, bn, "Nt_KeyTexL_00", &dl, &w, &h, &bh) || !texture(b, bn, "Nt_KeyTexZL_00", &dz, &w2, &h2, &bh2)) return 0;
    if (do_l) relabel(b, dl, w, h, bh, ll);
    if (do_zl) relabel(b, dz, w2, h2, bh2, zll);
    fprintf(stderr, "[totk] button prompts: the guide's keys %s / %s\n", do_l ? ll : "L", do_zl ? zll : "ZL");
    return 1;
}

/* The prompts after the player's controls into the decompressed layout archive. 1 when anything changed; each part
   that does not find the archive as the Switch's leaves it. */
int totk_ui_prompts_apply(uint8_t *blarc, size_t n) {
    int a = diamond(blarc, n), b = guide_keys(blarc, n);
    return a || b;
}

/* ---- the letters' messages: LayoutMsg/DoCommand_00.msbt in the language's Mals/<lang>.Product.<n>.sarc (MSBT:
   a 0x20-byte header, then sections of a 16-byte header and their data, each padded to 16; LBL1 hashes the labels to
   their message index, TXT2 holds UTF-16 texts). Each label of msg[] gets its letter in place of its one letter: the
   same size, so the archive keeps its size. */
static int msbt_letters(uint8_t *m, size_t n) {
    if (n < 0x20 || memcmp(m, "MsgStdBn", 8)) return 0;
    size_t lbl = 0, txt = 0, pos = 0x20;
    uint32_t lsize = 0, tsize = 0;
    while (pos + 16 <= n) {
        uint32_t size;
        memcpy(&size, m + pos + 4, 4);
        if (pos + 16 + size > n) return 0;
        if (!memcmp(m + pos, "LBL1", 4)) lbl = pos + 16, lsize = size;
        if (!memcmp(m + pos, "TXT2", 4)) txt = pos + 16, tsize = size;
        pos += 16 + ((size + 15) & ~15u);
    }
    if (!lbl || !txt || lsize < 4 || tsize < 4) return 0;
    uint32_t slots, count;
    memcpy(&slots, m + lbl, 4);
    memcpy(&count, m + txt, 4);
    int done = 0;
    for (uint32_t s = 0; s < slots && 4 + 8 * (size_t)s + 8 <= lsize; s++) {
        uint32_t nl, off;
        memcpy(&nl, m + lbl + 4 + 8 * s, 4);
        memcpy(&off, m + lbl + 4 + 8 * s + 4, 4);
        size_t o = lbl + off;
        for (uint32_t j = 0; j < nl; j++) {
            if (o + 1 > lbl + lsize) return done;
            unsigned len = m[o];
            if (o + 1 + len + 4 > lbl + lsize) return done;
            uint32_t index;
            memcpy(&index, m + o + 1 + len, 4);
            for (int k = 0; k < nmsg; k++) {
                if (strlen(msg[k].label) != len || memcmp(m + o + 1, msg[k].label, len) || index >= count) continue;
                uint32_t at;
                memcpy(&at, m + txt + 4 + 4 * index, 4);
                uint8_t *t = m + txt + at;
                /* exactly one capital letter, then the end: else (another language's own wording) left as it is */
                if (txt + at + 4 > txt + tsize || t[1] || t[0] < 'A' || t[0] > 'Z' || t[2] || t[3]) continue;
                t[0] = (uint8_t)msg[k].letter;
                done++;
            }
            o += 1 + len + 4;
        }
    }
    return done;
}
static int mals_edit(uint8_t *sarc, size_t n, void *ctx) {
    (void)ctx;
    size_t lo, hi;
    if (!totk_sarc_member(sarc, n, "LayoutMsg/DoCommand_00.msbt", &lo, &hi)) return 0;
    int done = msbt_letters(sarc + lo, hi - lo);
    fprintf(stderr, "[totk] button prompts: %d of %d diamond letters say the pad's letter\n", done, nmsg);
    return done > 0;
}
int totk_zs_edit(const void *in, size_t n, const void *dicts_z, size_t dn, int (*edit)(uint8_t *raw, size_t n, void *ctx),
                 void *ctx, void **out, size_t *out_n);   /* host/ui_capture.c */
/* host/fs.c, when the game opens Mals/<lang>...sarc.zs: the archive with the letters changed, 0 when there is nothing
   to change (the layout's prompts are the game's, or no letter moved). */
int totk_ui_prompts_mals(const void *in, size_t n, const void *dicts_z, size_t dn, void **out, size_t *out_n) {
    *out = NULL;
    return nmsg ? totk_zs_edit(in, n, dicts_z, dn, mals_edit, NULL, out, out_n) : 0;
}

/* ---- the button font: the symbols the game writes in its texts ("press [A]") come from scft/External.bfotf in each
   Font/Font*.Nin_NX_NVN.bfarc (SARC). A .bfotf is an OpenType (CFF) font behind an 8-byte header, every big-endian
   32-bit word XORed with one key (bytes 4..8 are the font's size XORed with it). Each button's symbol is a glyph at a
   private-use code (U+E040 A ...); an action bound to another button gets that button's glyph in its place. The glyphs
   only change places (their charstrings in the CFF CharStrings INDEX and their hmtx metrics), so the font, the archive
   and the game's resource size table keep their sizes. The symbols stay the Switch's: the layout's letters (the Xbox
   layout's A is the Switch's A symbol), L / R / ZL / ZR for the shoulders, + / - for Menu / View. */
static const struct { const char *id; unsigned cp; const char *nin, *xbox; } SYM[] = {
    {"a", 0xE040, "b", "a"}, {"b", 0xE041, "a", "b"}, {"x", 0xE042, "y", "x"}, {"y", 0xE043, "x", "y"},
    {"l", 0xE044, "leftshoulder", "leftshoulder"}, {"r", 0xE045, "rightshoulder", "rightshoulder"},
    {"zl", 0xE046, "lefttrigger", "lefttrigger"}, {"zr", 0xE047, "righttrigger", "righttrigger"},
    {"plus", 0xE04B, "start", "start"}, {"minus", 0xE04C, "back", "back"},
    {"lstick", 0xE056, "leftstick", "leftstick"}, {"rstick", 0xE057, "rightstick", "rightstick"},
    {"up", 0xE060, "dpup", "dpup"}, {"down", 0xE061, "dpdown", "dpdown"}, {"left", 0xE062, "dpleft", "dpleft"},
    {"right", 0xE063, "dpright", "dpright"}};
enum { NSYM = sizeof SYM / sizeof *SYM };
static int sym_from[NSYM];   /* the glyph each action's place shows: SYM index */
static int sym_state;        /* 0 not worked out, 1 the game's own, 2 changed */

/* Each action shows the symbol of the button it is bound to (its first input); the result is always a permutation:
   an action bound to nothing with a symbol, or to a button another action has too, keeps a symbol no one else shows. */
static int sym_plan(void) {
    if (sym_state) return sym_state == 2;
    int used[NSYM] = {0}, x = xbox_layout(), odd = 0;
    for (int k = 0; k < NSYM; k++) {
        char s[32];
        bound(SYM[k].id, x ? SYM[k].xbox : SYM[k].nin, s, sizeof s);
        sym_from[k] = -1;
        for (int j = 0; j < NSYM; j++)
            if (!strcmp(s, x ? SYM[j].xbox : SYM[j].nin) && !used[j]) sym_from[k] = j, used[j] = 1;
        if (sym_from[k] < 0) odd = 1;
    }
    for (int k = 0; k < NSYM; k++) {
        if (sym_from[k] >= 0) continue;
        int j = used[k] ? -1 : k;
        for (int q = 0; j < 0 && q < NSYM; q++) if (!used[q]) j = q;
        sym_from[k] = j, used[j] = 1;
    }
    int changed = 0;
    for (int k = 0; k < NSYM; k++) changed |= sym_from[k] != k;
    if (odd)
        fprintf(stderr, "[totk] button font: an action is bound to no button with a symbol, or to one another action has "
                        "too; it keeps a symbol no other action shows\n");
    sym_state = changed ? 2 : 1;
    return changed;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

/* the glyph of cp from a format 4 subtable of the cmap at [c, c + cn); 0 none */
static unsigned cmap_glyph(const uint8_t *f, size_t c, size_t cn, unsigned cp) {
    if (cn < 4) return 0;
    unsigned nt = be16(f + c + 2);
    for (unsigned t = 0; t < nt && 4 + 8 * (size_t)t + 8 <= cn; t++) {
        size_t s = c + be32(f + c + 4 + 8 * t + 4);
        if (s + 14 > c + cn || be16(f + s) != 4) continue;
        size_t len = be16(f + s + 2), segs = be16(f + s + 6) / 2;
        if (s + len > c + cn || 16 + 8 * segs > len) continue;
        size_t ends = s + 14, starts = ends + 2 * segs + 2, deltas = starts + 2 * segs, ranges = deltas + 2 * segs;
        for (size_t i = 0; i < segs; i++) {
            unsigned end = be16(f + ends + 2 * i), start = be16(f + starts + 2 * i);
            if (cp > end || cp < start) continue;
            unsigned delta = be16(f + deltas + 2 * i), ro = be16(f + ranges + 2 * i);
            if (!ro) return (cp + delta) & 0xFFFF;
            size_t g = ranges + 2 * i + ro + 2 * (cp - start);
            if (g + 2 > s + len) return 0;
            unsigned v = be16(f + g);
            return v ? (v + delta) & 0xFFFF : 0;
        }
    }
    return 0;
}

/* a CFF INDEX at p: its count, offset size and where its data start (offsets are 1-based from there); 0 malformed */
static int cff_index(const uint8_t *f, size_t lo, size_t hi, size_t p, unsigned *count, unsigned *osz, size_t *data, size_t *end) {
    if (p + 2 > hi || p < lo) return 0;
    *count = be16(f + p);
    if (!*count) { *osz = 0, *data = *end = p + 2; return 1; }
    if (p + 3 > hi) return 0;
    *osz = f[p + 2];
    if (*osz < 1 || *osz > 4 || p + 3 + (size_t)(*count + 1) * *osz > hi) return 0;
    *data = p + 3 + (size_t)(*count + 1) * *osz - 1;
    size_t last = 0;
    for (unsigned b = 0; b < *osz; b++) last = last << 8 | f[p + 3 + (size_t)*count * *osz + b];
    *end = *data + last;
    return *end <= hi;
}
static size_t cff_off(const uint8_t *f, size_t p, unsigned osz, unsigned i) {   /* offset i of the INDEX at p */
    size_t v = 0;
    for (unsigned b = 0; b < osz; b++) v = v << 8 | f[p + 3 + (size_t)i * osz + b];
    return v;
}
/* the Top DICT at [a, b): its CharStrings offset (operator 17); 0 if absent or the font is CID-keyed (ROS, 12 30:
   glyphs with their own private dicts, not swapped here) */
static size_t cff_charstrings(const uint8_t *f, size_t a, size_t b) {
    long st[48];
    int ns = 0;
    size_t cs = 0;
    for (size_t p = a; p < b;) {
        unsigned v = f[p];
        if (v <= 21) {
            unsigned op = v;
            p++;
            if (v == 12) { if (p >= b) return 0; op = 1200 + f[p++]; }
            if (op == 1230) return 0;
            if (op == 17 && ns == 1) cs = (size_t)st[0];
            ns = 0;
            continue;
        }
        long x;
        if (v == 28) { if (p + 3 > b) return 0; x = (int16_t)be16(f + p + 1), p += 3; }
        else if (v == 29) { if (p + 5 > b) return 0; x = (int32_t)be32(f + p + 1), p += 5; }
        else if (v == 30) {   /* a real: its nibbles up to the end one */
            for (p++; p < b && (f[p] & 15) != 15 && (f[p] >> 4) != 15; p++) {}
            p++, x = 0;
        } else if (v >= 32 && v <= 246) x = (long)v - 139, p++;
        else if (v >= 247 && v <= 250) { if (p + 2 > b) return 0; x = (long)(v - 247) * 256 + f[p + 1] + 108, p += 2; }
        else if (v >= 251 && v <= 254) { if (p + 2 > b) return 0; x = -(long)(v - 251) * 256 - f[p + 1] - 108, p += 2; }
        else return 0;
        if (ns < 48) st[ns++] = x;
    }
    return cs;
}
static uint32_t table_sum(const uint8_t *f, size_t at, size_t len) {
    uint32_t s = 0;
    for (size_t i = 0; i < len; i += 4) {
        uint8_t w[4] = {0};
        memcpy(w, f + at + i, len - i < 4 ? len - i : 4);
        s += be32(w);
    }
    return s;
}

/* The OpenType font f (n bytes): the symbols' glyphs to their actions' places. 1 when done. */
static int otf_swap(uint8_t *f, size_t n) {
    if (n < 12 || memcmp(f, "OTTO", 4)) return 0;
    unsigned nt = be16(f + 4);
    size_t cff = 0, cffn = 0, cmap = 0, cmapn = 0, hmtx = 0, hmtxn = 0, hhea = 0, maxp = 0, head = 0, dir[4] = {0};
    if (12 + 16 * (size_t)nt > n) return 0;
    for (unsigned t = 0; t < nt; t++) {
        const uint8_t *e = f + 12 + 16 * t;
        size_t at = be32(e + 8), len = be32(e + 12);
        if (at + len > n) return 0;
        if (!memcmp(e, "CFF ", 4)) cff = at, cffn = len, dir[0] = 12 + 16 * t;
        else if (!memcmp(e, "cmap", 4)) cmap = at, cmapn = len;
        else if (!memcmp(e, "hmtx", 4)) hmtx = at, hmtxn = len, dir[1] = 12 + 16 * t;
        else if (!memcmp(e, "hhea", 4) && len >= 36) hhea = at;
        else if (!memcmp(e, "maxp", 4) && len >= 6) maxp = at;
        else if (!memcmp(e, "head", 4) && len >= 12) head = at;
    }
    if (!cff || !cmap || !hmtx || !hhea || !maxp || !head) return 0;
    unsigned glyphs = be16(f + maxp + 4), nh = be16(f + hhea + 34), gid[NSYM];
    if (nh > glyphs || (size_t)nh * 4 > hmtxn) return 0;
    for (int k = 0; k < NSYM; k++) {   /* every symbol a glyph with its own advance (not one of the shared-width tail) */
        gid[k] = cmap_glyph(f, cmap, cmapn, SYM[k].cp);
        if (!gid[k] || gid[k] >= nh) return 0;
    }
    /* the CFF: header, Name INDEX, Top DICT INDEX (its first dict) -> the CharStrings INDEX */
    size_t lo = cff, hi = cff + cffn, d0, e0, d1, e1, d2, e2;
    unsigned c0, o0, c1, o1, c2, o2;
    if (cffn < 4 || !cff_index(f, lo, hi, lo + f[lo + 2], &c0, &o0, &d0, &e0) || !cff_index(f, lo, hi, e0, &c1, &o1, &d1, &e1) || c1 < 1)
        return 0;
    size_t cs = cff_charstrings(f, d1 + cff_off(f, e0, o1, 0), d1 + cff_off(f, e0, o1, 1));
    if (!cs || !cff_index(f, lo, hi, lo + cs, &c2, &o2, &d2, &e2) || c2 != glyphs) return 0;
    size_t csp = lo + cs, body = d2 + 1, total = e2 - body;
    /* the charstrings and metrics as they are, then each symbol's place gets its source's */
    uint8_t *old = malloc(total), met[NSYM][4];
    size_t *off = malloc(sizeof *off * (glyphs + 1));
    if (!old || !off) { free(old); free(off); return 0; }
    memcpy(old, f + body, total);
    for (unsigned g = 0; g <= glyphs; g++) off[g] = cff_off(f, csp, o2, g) - 1;
    for (int k = 0; k < NSYM; k++) memcpy(met[k], f + hmtx + 4 * gid[k], 4);
    size_t pos = 0;
    for (unsigned g = 0; g < glyphs; g++) {
        unsigned from = g;
        for (int k = 0; k < NSYM; k++) if (gid[k] == g) from = gid[sym_from[k]];
        size_t len = off[from + 1] - off[from];
        if (off[from + 1] < off[from] || off[from + 1] > total || pos + len > total) { free(old); free(off); return 0; }
        memcpy(f + body + pos, old + off[from], len);
        pos += len;
        size_t v = pos + 1;
        for (int b = (int)o2 - 1; b >= 0; b--, v >>= 8) f[csp + 3 + (size_t)(g + 1) * o2 + b] = (uint8_t)v;
    }
    for (int k = 0; k < NSYM; k++) memcpy(f + hmtx + 4 * gid[k], met[sym_from[k]], 4);
    free(old); free(off);
    /* the two tables' checksums and the font's (head.checkSumAdjustment) */
    wbe32(f + dir[0] + 4, table_sum(f, cff, cffn));
    wbe32(f + dir[1] + 4, table_sum(f, hmtx, hmtxn));
    wbe32(f + head + 8, 0);
    wbe32(f + head + 8, 0xB1B0AFBAu - table_sum(f, 0, n));
    return 1;
}

/* The archive's scft/External.bfotf decoded, its glyphs swapped, encoded again. */
static int font_edit(uint8_t *sarc, size_t n, void *ctx) {
    (void)ctx;
    size_t lo, hi;
    if (!totk_sarc_member(sarc, n, "scft/External.bfotf", &lo, &hi) || hi - lo < 16 || (hi - lo) % 4) return 0;
    uint8_t *b = sarc + lo;
    size_t fn = hi - lo - 8;
    uint32_t key = be32(b + 4) ^ (uint32_t)fn;
    if ((be32(b) ^ key) != 0x7f9a0218u) return 0;   /* (the magic, as host/shared_font.c reads system fonts) */
    for (size_t i = 8; i < hi - lo; i += 4) wbe32(b + i, be32(b + i) ^ key);
    int ok = otf_swap(b + 8, fn);
    for (size_t i = 8; i < hi - lo; i += 4) wbe32(b + i, be32(b + i) ^ key);
    if (!ok) fprintf(stderr, "[totk] button font: External.bfotf is not as expected; the game's symbols stay\n");
    return ok;
}
/* host/fs.c, when the game opens Font/Font*.bfarc.zs: the archive with the button symbols after the controls, 0 when
   they are the game's own (nothing rebound) or the font is not as expected. */
int totk_ui_prompts_font(const void *in, size_t n, const void *dicts_z, size_t dn, void **out, size_t *out_n) {
    *out = NULL;
    if (!sym_plan()) return 0;
    int ok = totk_zs_edit(in, n, dicts_z, dn, font_edit, NULL, out, out_n);
    if (ok) {
        char line[512];
        int at = 0;
        for (int k = 0; k < NSYM && at < (int)sizeof line - 32; k++)
            if (sym_from[k] != k) at = bufcat(line, sizeof line, at, " %s=%s", SYM[k].id, SYM[sym_from[k]].id);
        fprintf(stderr, "[totk] button font: symbols after the controls:%s\n", line);
    }
    return ok;
}
