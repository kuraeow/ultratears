/* The game's UI layouts for an output of another shape than 16:9 (16:10 as the Steam Deck's 1280x800, 21:9, 32:9), edited
 * in the decompressed UI/LayoutArchive/Common.Product.140.Nin_NX_NVN.blarc that host/ui_capture.c builds for host/fs.c.
 * A C port of totk-pc-recomp's tools/ultrawide_layout.py (MIT, Copyright (c) 2026 Valiantsin Dzerakh), adapted from
 * MacNativeGames games/totk/scripts/ultrawide_layout.py @ 933b6a0 (MIT, Copyright (c) 2026 MacNativeGames contributors).
 * The pane list and the distances are those the community TOTK-Ultrawide mod for emulators found (only the values,
 * no code).
 *
 * The game lays its UI out on a 1920x1080 grid mapped onto the canvas: on 21:9 every layout comes out 34% too wide, on
 * 16:10 11% too tall. Each layout's root pane is scaled back to 16:9 on the stretched axis, backgrounds that must cover
 * the screen are stretched back, and the HUD moves out to the corners (wider) or to the top and bottom edges
 * (narrower). Every value is checked against the one the game ships (the edit's value at k = 1) before it is written:
 * another file stops the whole edit and the game keeps its own layouts. The camera's side is the aspect code patch
 * (tools/lifter/patches.py, host/render_res.c totk_aspect). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rd16(const uint8_t *p, size_t n, size_t at, uint16_t *v) { if (at + 2 > n) return 0; memcpy(v, p + at, 2); return 1; }
static int rd32(const uint8_t *p, size_t n, size_t at, uint32_t *v) { if (at + 4 > n) return 0; memcpy(v, p + at, 4); return 1; }
static float getf(const uint8_t *p, size_t at) { float f; memcpy(&f, p + at, 4); return f; }
static void putf(uint8_t *p, size_t at, float f) { memcpy(p + at, &f, 4); }

static uint8_t *B;   // the archive being edited
static size_t BN;
static char why[256];   // the first mismatch, for the log

// ---- the SARC: members by name
typedef struct { char name[96]; size_t lo, hi; } Member;
static Member *members;
static int nmembers;

static int sarc_index(void) {
    uint16_t header, count;
    uint32_t start;
    if (BN < 0x14 || memcmp(B, "SARC", 4) || !rd16(B, BN, 4, &header) || !rd32(B, BN, 12, &start) || start > BN ||
        header + 12 > BN || memcmp(B + header, "SFAT", 4) || !rd16(B, BN, header + 6, &count))
        return 0;
    size_t names = header + 12 + (size_t)count * 16;
    if (names + 8 > BN || memcmp(B + names, "SFNT", 4)) return 0;
    names += 8;
    members = calloc(count ? count : 1, sizeof *members);
    if (!members) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t attr, a, b;
        size_t node = header + 12 + (size_t)i * 16;
        if (!rd32(B, BN, node + 4, &attr) || !rd32(B, BN, node + 8, &a) || !rd32(B, BN, node + 12, &b)) return 0;
        size_t at = names + (size_t)(attr & 0xffffff) * 4;
        if (at >= start || a > b || (size_t)start + b > BN) return 0;
        size_t len = strnlen((const char *)B + at, start - at);
        if (len >= sizeof members[0].name) continue;
        memcpy(members[nmembers].name, B + at, len);
        members[nmembers].lo = start + a, members[nmembers].hi = start + b;
        nmembers++;
    }
    return 1;
}

static const Member *member(const char *name) {
    for (int i = 0; i < nmembers; i++) if (!strcmp(members[i].name, name)) return &members[i];
    return NULL;
}

// ---- BFLYT panes: name at +12 (24 bytes), translate x / y at +44 / +48, scale x / y at +68 / +72
enum { F_X = 44, F_Y = 48, F_SX = 68, F_SY = 72 };
static const char PANE_MAGICS[][5] = {"pan1", "pic1", "txt1", "wnd1", "bnd1", "prt1", "ali1", "scr1"};

// The first pane of that name in blyt/<layout>.bflyt: its section offset, 0 if absent (why says so).
static size_t pane(const char *layout, const char *name) {
    char m[96];
    snprintf(m, sizeof m, "blyt/%s.bflyt", layout);
    const Member *f = member(m);
    uint16_t head, count;
    if (!f || f->hi - f->lo < 0x14 || memcmp(B + f->lo, "FLYT", 4) || !rd16(B, f->hi, f->lo + 6, &head) ||
        !rd16(B, f->hi, f->lo + 0x10, &count)) {
        snprintf(why, sizeof why, "%s: no layout", layout);
        return 0;
    }
    size_t pos = f->lo + head;
    for (unsigned s = 0; s < count; s++) {
        uint32_t size;
        if (!rd32(B, f->hi, pos + 4, &size) || size < 8 || pos + size > f->hi) break;
        for (size_t k = 0; k < sizeof PANE_MAGICS / sizeof *PANE_MAGICS; k++)
            if (!memcmp(B + pos, PANE_MAGICS[k], 4) && pos + 76 <= f->hi && !strncmp((const char *)B + pos + 12, name, 24))
                return pos;
        pos += size;
    }
    snprintf(why, sizeof why, "%s: no pane %s", layout, name);
    return 0;
}

// ---- the edits: (layout, pane, field, value, the value the game ships); OFF moves a pane out of sight (not checked)
#define OFF 10000.0f
typedef struct { const char *layout, *pane; int field; float value, shipped; } Edit;
static Edit *E;
static int ne, cap;
static void add(const char *layout, const char *pane_, int field, float value, float shipped) {
    if (ne == cap) {
        cap = cap ? cap * 2 : 256;
        E = realloc(E, (size_t)cap * sizeof *E);
    }
    E[ne++] = (Edit){layout, pane_, field, value, shipped};
}
// value as a function of k, the shipped one being its value at k = 1
#define ADD(layout, pane_, field, expr) do { float k = kk; float v_ = (expr); k = 1.0f; add(layout, pane_, field, v_, (expr)); } while (0)

static const char *const KEEP_ROOT[] = {"AppMenuBG_00", "PauseMenuBG_00", "ShopMenuParasailBG_00", "ShortCutBG_00",
                                        "TitleMenuBG_00", "EventFade_00", "EventFadeWipe_00", "CameraShutter_00",
                                        "PaAppPictureBookDetail_00", "AppMenuOverlay_00"};
static const char *const SYSTEM_MENUS[] = {"SystemLoadList_00", "SystemOption_00", "SystemSaveLoad_00"};

// A canvas wider than 16:9: kk = 16/9 over the aspect (the root's scale x); w = how far a 1920-wide layout's edge moves out.
static void wide_edits(float kk) {
#define W (960 * (1 / k - 1))
#define WIDE (1 / k)
    ADD("AppAlbum_00", "N_FadeInOut_00", F_SX, k); ADD("AppAlbum_00", "N_Open_01", F_SX, WIDE);
    static const char *const blur[] = {"N_BlurInOut_00", "N_BlurInOut_01", "N_BlurInOut_02", "N_BlurInOut_03"};
    for (int i = 0; i < 4; i++) ADD("AppMap_00", blur[i], F_SX, WIDE);
    ADD("AppMap_00", "N_MiniMap_00", F_X, -270 * (1 - k));
    static const char *const ovl[] = {"N_FadeInOut_00", "N_FadeInOut_01", "N_Icon_00", "N_TitleOne_01"};
    for (int i = 0; i < 4; i++) ADD("AppMenuOverlay_00", ovl[i], F_SX, k);
    ADD("AppOpenDemoWindow_00", "P_BG_00", F_SX, WIDE); ADD("AutoBuilder_00", "N_BG_00", F_SX, WIDE);
    ADD("CameraPointer_00", "N_InOutScope_00", F_SX, WIDE); ADD("CameraPointer_00", "N_InOutScope_00", F_SY, WIDE);
    ADD("CameraPointer_00", "N_Type_01", F_SX, k); ADD("CameraPointer_00", "Pa_CameraFocus_00", F_SX, WIDE);
    ADD("CameraPointer_00", "S_Scissor_00", F_SX, WIDE); ADD("CameraPointer_00", "W_Shadow_01", F_SX, WIDE);
    ADD("CameraSystemWindow_00", "N_AlbumCon_00", F_SX, k); ADD("CameraSystemWindow_00", "N_InOut_00", F_SX, WIDE);
    ADD("CameraSystemWindow_00", "Pa_BtnL_00", F_SX, k); ADD("CameraSystemWindow_00", "Pa_BtnR_00", F_SX, k);
    ADD("CameraSystemWindow_00", "T_Text_00", F_SX, k);
    add("ChallengeLog_00", "C_CaptureUp_00", F_X, OFF, OFF); add("ChallengeLog_00", "C_CaptureUp_01", F_X, OFF, OFF);
    ADD("GameOver_00", "P_BG_00", F_SX, WIDE);
    ADD("LoadingFade_00", "N_Base_00", F_SX, WIDE); ADD("LoadingFade_00", "N_Logo_00", F_X, W);
    ADD("LoadingFade_00", "N_Status_00", F_X, W); ADD("LoadingFade_00", "N_Tips_00", F_X, -W);
    ADD("LoadingFade_00", "P_BG_00", F_SX, WIDE); ADD("LoadingFade_00", "P_SideBGDeco_00", F_SX, k);
    static const char *const load[] = {"Pa_IconPlayer_00", "Pa_LargeDungeonMap_00", "Pa_LargeDungeonMap_01", "Pa_Map_00", "Pa_Map_01"};
    for (int i = 0; i < 5; i++) ADD("LoadingFade_00", load[i], F_SX, k);
    ADD("MessageLog_01", "P_Capture_00", F_SX, WIDE); ADD("OwnedHorseList_00", "N_BG_00", F_SX, WIDE);
    add("PaCommonBtnTitle_00", "B_Hit_00", F_X, OFF, OFF); add("PaCommonBtnTitle_00", "N_Cursor_00", F_X, OFF, OFF);
    ADD("PaLargeDungeonMap_00", "P_BG_00", F_SX, WIDE); add("PaMessageBtn_00", "B_Hit_00", F_X, OFF, OFF);
    ADD("PaRecipeCardList_00", "P_BG_00", F_SX, WIDE);
    ADD("SaveLoadIcon_00", "RootPane", F_X, -960 * (1 - k));
    ADD("ShopMenu_00", "N_BG_00", F_SX, WIDE); ADD("StaffRoll_00", "N_BG_00", F_SX, WIDE);
    // menus framed by two side decorations, a header and a background: the frame stays at the screen's edges
    static const char *const framed[] = {"PauseMenuOverlay_00", "SystemLoadList_00", "SystemOption_00", "SystemSaveLoad_00"};
    for (int i = 0; i < 4; i++) {
        const char *l = framed[i];
        ADD(l, "P_DecoL_00", F_SX, k); ADD(l, "P_DecoL_00", F_X, -(675 + W) * k);
        ADD(l, "P_DecoR_00", F_SX, k); ADD(l, "P_DecoR_00", F_X, (737 + W) * k);
        if (i == 0) {
            ADD(l, "P_FooterBase_00", F_SX, WIDE); ADD(l, "S_DecoScissor_00", F_SX, WIDE);
        } else {
            ADD(l, "A_Title_00", F_SX, k); ADD(l, "N_Header_00", F_SX, WIDE); ADD(l, "N_SystemBG_00", F_SX, WIDE);
        }
    }
    ADD("SystemActionGuide_00", "A_Title_00", F_SX, k); ADD("SystemActionGuide_00", "N_Header_00", F_SX, WIDE);
    ADD("SystemActionGuide_00", "N_SystemBG_00", F_SX, WIDE);
    ADD("SystemActionGuide_00", "P_DecoL_01", F_SX, k); ADD("SystemActionGuide_00", "P_DecoL_01", F_X, -(675 + W) * k);
    ADD("SystemActionGuide_00", "P_DecoL_02", F_SX, k); ADD("SystemActionGuide_00", "P_DecoL_02", F_X, (675 + W) * k);
    // the HUD in the screen's corners instead of the middle 16:9
    static const char *const right[] = {"EnvironmentInfo_00", "EventSkip_00", "InstantTipsAppMenu_00", "InstantTipsWithPict_00",
                                        "InstantTips_00", "NumDisplaySecond_00", "NumDisplay_00", "PickUpWin_00"};
    for (int i = 0; i < 8; i++) ADD(right[i], "RootPane", F_X, 960 * (1 - k));
    ADD("Location_00", "N_Small_00", F_X, -(388 + W));
    ADD("PlayerStatus3D_00", "N_ExtraStaminaTarget_01", F_X, -(745 + W));
    ADD("PlayerStatus_00", "N_DevilDemoTargetC_00", F_X, -(284 + W));
    ADD("PlayerStatus_00", "N_DevilDemoTargetLT_00", F_X, -(576 + W));
    ADD("PlayerStatus_00", "N_HeartGaugeTargetLT_00", F_X, -(576 + W));
    ADD("PlayerStatus_00", "N_HeartPos_00", F_X, -W);
    ADD("PlayerStatus_00", "Pa_CompanionLife_00", F_X, -(840 + W));
#undef W
#undef WIDE
}

// A canvas narrower than 16:9: kk = the aspect over 16/9 (the root's scale y); h = how far a 1080-high layout's edge moves.
static void tall_edits(float kk) {
#define H (540 * (1 / k - 1))
#define TALL (1 / k)
    ADD("AppAlbum_00", "N_FadeInOut_00", F_SY, k); ADD("AppAlbum_00", "N_Open_01", F_SY, TALL);
    static const char *const ovl[] = {"N_FadeInOut_00", "N_FadeInOut_01", "N_Icon_00", "N_TitleOne_01"};
    for (int i = 0; i < 4; i++) ADD("AppMenuOverlay_00", ovl[i], F_SY, k);
    ADD("CameraPointer_00", "N_Type_01", F_SY, k); ADD("CameraSystemWindow_00", "N_FirstOut_00", F_SY, k);
    static const char *const load[] = {"Pa_IconPlayer_00", "Pa_LargeDungeonMap_00", "Pa_LargeDungeonMap_01", "Pa_Map_00", "Pa_Map_01"};
    for (int i = 0; i < 5; i++) ADD("LoadingFade_00", load[i], F_SY, k);
    static const char *const blur[] = {"N_BlurInOut_00", "N_BlurInOut_01", "N_BlurInOut_02", "N_BlurInOut_03"};
    for (int i = 0; i < 4; i++) ADD("AppMap_00", blur[i], F_SY, TALL);
    static const char *const bg[] = {"AppOpenDemoWindow_00", "GameOver_00", "LoadingFade_00", "PaLargeDungeonMap_00", "PaRecipeCardList_00"};
    for (int i = 0; i < 5; i++) ADD(bg[i], "P_BG_00", F_SY, TALL);
    ADD("LoadingFade_00", "N_Base_00", F_SY, TALL);
    static const char *const nbg[] = {"AutoBuilder_00", "OwnedHorseList_00", "ShopMenu_00", "StaffRoll_00"};
    for (int i = 0; i < 4; i++) ADD(nbg[i], "N_BG_00", F_SY, TALL);
    static const char *const cam[] = {"N_InOutScope_00", "Pa_CameraFocus_00", "S_Scissor_00", "W_Shadow_01"};
    for (int i = 0; i < 4; i++) ADD("CameraPointer_00", cam[i], F_SY, TALL);
    ADD("CameraSystemWindow_00", "N_InOut_00", F_SY, TALL); ADD("MessageLog_01", "P_Capture_00", F_SY, TALL);
    ADD("SystemActionGuide_00", "N_SystemBG_00", F_SY, TALL);
    for (int i = 0; i < 3; i++) ADD(SYSTEM_MENUS[i], "N_SystemBG_00", F_SY, TALL);
    // the HUD at the screen's top and bottom edges instead of the middle 16:9
    static const char *const up[] = {"AppAlbum_00", "AppPictureBook_00", "ChallengeLog_00", "Challenge_00", "InstantTipsWithPict_00",
                                     "InstantTips_00", "MiniGameTimer_00", "NumDisplaySecond_00", "NumDisplay_00", "PickUpWin_00",
                                     "PlayerStatus_00", "Pouch_00", "ShopMenu_00", "SystemActionGuide_00", "SystemLoadList_00",
                                     "SystemOption_00", "SystemSaveLoad_00"};
    for (size_t i = 0; i < sizeof up / sizeof *up; i++) ADD(up[i], "RootPane", F_Y, 540 * (1 - k));
    static const char *const down[] = {"CtrlGuide_00", "DoCommand_00", "EnvironmentInfo_00", "HorseGauge_00", "MessageDialog_00",
                                       "MessageLog_01", "PauseMenuOverlay_00", "SaveLoadIcon_00", "TreasureFullShortCut_00"};
    for (size_t i = 0; i < sizeof down / sizeof *down; i++) ADD(down[i], "RootPane", F_Y, -540 * (1 - k));
    ADD("AppAlbum_00", "N_PicLoad_01", F_Y, -540 * (1 - k)); ADD("AppMap_00", "N_AllMapTime_00", F_Y, -H);
    ADD("AppMenuOverlay_00", "A_Align_00", F_Y, -(505 + H)); ADD("AppMenuOverlay_00", "Pa_GuideLeft_00", F_Y, -(505 + H));
    ADD("AppMenuOverlay_00", "N_TitleTab_00", F_Y, 491 + H);
    ADD("LoadingFade_00", "N_Logo_00", F_Y, -H); ADD("LoadingFade_00", "N_Sage_00", F_Y, -477 - 2 * H);
    ADD("LoadingFade_00", "N_Status_00", F_Y, H); ADD("LoadingFade_00", "N_Tips_00", F_Y, -(360 + H));
    ADD("Location_00", "N_Large_00", F_Y, 270 + H); ADD("Location_00", "N_Small_00", F_Y, -(395 + H));
    ADD("MessageLog_01", "P_Capture_00", F_Y, H);
    ADD("PaExtraStaminaGauge_00", "N_BasePattern_00", F_Y, H); ADD("PaExtraStaminaGauge_00", "N_GaugeMainPattern_00", F_Y, H);
    ADD("Pouch_00", "N_Info_00", F_Y, -H); ADD("Pouch_00", "Pa_ExtraStamina_00", F_Y, -H);
    ADD("ShopMenu_00", "A_AlignGuide_00", F_Y, -(477 + H));
    ADD("SystemActionGuide_00", "N_SystemBG_00", F_Y, -H);
    for (int i = 0; i < 3; i++) ADD(SYSTEM_MENUS[i], "N_SystemBG_00", F_Y, -H);
    ADD("SystemLoadList_00", "Pa_Guide_00", F_Y, -(474 + H)); ADD("SystemOption_00", "A_Guide_00", F_Y, -(474 + H));
#undef H
#undef TALL
}

// ---- BFLAN hermite keys (frame, value, slope: 12 bytes) of one pane, tag (FLPA pane SRT, FLTS texture SRT) and target
enum { FLPA_Y = 1, FLTS_SY = 4 };
// The key offsets of the only matching curve into keys[]; the number of curves found (-1 malformed).
static int anim_curve(const char *anim, const char *pane_, const char *tag, int target, size_t *keys, int *nkeys, int max) {
    char m[96];
    snprintf(m, sizeof m, "anim/%s.bflan", anim);
    const Member *f = member(m);
    uint16_t head, sections;
    if (!f || f->hi - f->lo < 0x14 || memcmp(B + f->lo, "FLAN", 4) || !rd16(B, f->hi, f->lo + 6, &head) ||
        !rd16(B, f->hi, f->lo + 0x10, &sections))
        return -1;
    size_t pos = f->lo + head, hi = f->hi;
    int curves = 0;
    for (unsigned s = 0; s < sections; s++) {
        uint32_t size;
        if (!rd32(B, hi, pos + 4, &size) || size < 8 || pos + size > hi) return -1;
        if (!memcmp(B + pos, "pai1", 4)) {
            uint16_t count;
            uint32_t table;
            if (!rd16(B, hi, pos + 14, &count) || !rd32(B, hi, pos + 16, &table)) return -1;
            for (unsigned e = 0; e < count; e++) {
                uint32_t eo_rel;
                if (!rd32(B, hi, pos + table + 4 * e, &eo_rel)) return -1;
                size_t eo = pos + eo_rel;
                if (eo + 32 > hi || strncmp((const char *)B + eo, pane_, 28)) continue;
                for (unsigned t = 0; t < B[eo + 28]; t++) {
                    uint32_t to_rel;
                    if (!rd32(B, hi, eo + 32 + 4 * t, &to_rel)) return -1;
                    size_t to = eo + to_rel;
                    if (to + 8 > hi || memcmp(B + to, tag, 4)) continue;
                    for (unsigned g = 0; g < B[to + 4]; g++) {
                        uint32_t go_rel, ko_rel;
                        uint16_t n;
                        if (!rd32(B, hi, to + 8 + 4 * g, &go_rel)) return -1;
                        size_t go = to + go_rel;
                        if (go + 12 > hi || B[go + 1] != target) continue;
                        if (B[go + 2] != 2 || !rd16(B, hi, go + 4, &n) || !rd32(B, hi, go + 8, &ko_rel)) return -1;
                        if (++curves > 1 || n > max || go + ko_rel + 12u * n > hi) return -1;
                        for (unsigned i = 0; i < n; i++) keys[i] = go + ko_rel + 12 * i;
                        *nkeys = n;
                    }
                }
            }
        }
        pos += size;
    }
    return curves;
}
static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }

// Edits B (the decompressed archive) for a w x h canvas of another shape than 16:9. 1 on success; 0 leaves it unusable
// (the caller keeps the game's file) with the reason logged.
int totk_ui_aspect_apply(uint8_t *blarc, size_t n, unsigned w, unsigned h) {
    B = blarc, BN = n, why[0] = 0, ne = 0, nmembers = 0;
    free(members);
    members = NULL;
    int ok = 0;
    size_t *roots = NULL;
    float aspect = (float)w / (float)h, base = 16.0f / 9.0f;
    int wide = aspect > base;
    float k = wide ? base / aspect : aspect / base;
    int root = wide ? F_SX : F_SY;
    if (k >= 0.999f || !sarc_index()) {
        snprintf(why, sizeof why, "not a layout archive");
        goto done;
    }
    // every layout's root pane scaled back to 16:9 on the stretched axis, but for the full-screen ones (written with the
    // rest, once everything is checked)
    roots = calloc((size_t)nmembers + 1, sizeof *roots);
    int nroots = 0;
    if (!roots) goto done;
    for (int i = 0; i < nmembers; i++) {
        const char *m = members[i].name;
        size_t len = strlen(m);
        if (strncmp(m, "blyt/", 5) || len < 12 || strcmp(m + len - 6, ".bflyt")) continue;
        char layout[96];
        memcpy(layout, m + 5, len - 11);
        layout[len - 11] = 0;
        int keep = 0;
        for (size_t j = 0; j < sizeof KEEP_ROOT / sizeof *KEEP_ROOT; j++) keep |= !strcmp(layout, KEEP_ROOT[j]);
        if (keep) continue;
        size_t at = pane(layout, "RootPane");
        if (!at) goto done;
        if (!near(getf(B, at + root), 1.0f)) {
            snprintf(why, sizeof why, "%s RootPane scale %.3f, expected 1", layout, getf(B, at + root));
            goto done;
        }
        roots[nroots++] = at + root;
    }
    for (size_t j = 0; j < sizeof KEEP_ROOT / sizeof *KEEP_ROOT; j++) {
        char m[96];
        snprintf(m, sizeof m, "blyt/%s.bflyt", KEEP_ROOT[j]);
        if (!member(m)) { snprintf(why, sizeof why, "no layout %s", KEEP_ROOT[j]); goto done; }
    }
    if (wide) wide_edits(k); else tall_edits(k);
    // checked first, all of them, then written: a mismatch leaves the archive as the game ships it
    for (int i = 0; i < ne; i++) {
        size_t at = pane(E[i].layout, E[i].pane);
        if (!at) goto done;
        if (E[i].shipped != OFF && !near(getf(B, at + E[i].field), E[i].shipped)) {
            snprintf(why, sizeof why, "%s %s +%d: %.3f, expected %.3f", E[i].layout, E[i].pane, E[i].field,
                     getf(B, at + E[i].field), E[i].shipped);
            goto done;
        }
    }
    size_t keys[8];
    int nk;
    if (wide) {
        // the dragon's tears map icon zooms its picture to 1.08 and 1.28; on the stretched axis by 1 / k more
        const Member *dt = member("anim/PaMapIconDragonTears_00_Zoom.bflan");
        if (!dt || dt->hi - dt->lo < 304 || !near(getf(B, dt->lo + 288), 1.08f) || !near(getf(B, dt->lo + 300), 1.28f)) {
            snprintf(why, sizeof why, "PaMapIconDragonTears_00_Zoom: not the shipped keys");
            goto done;
        }
        // the minimap's slide-in keys end at x 765, the right edge of a 16:9 layout
        const Member *mm = member("anim/AppMap_00_MiniMap.bflan");
        static const unsigned mo[] = {872, 880, 1628, 1636, 3264, 3272, 3468, 3476};
        for (int i = 0; i < 8; i++)
            if (!mm || mm->hi - mm->lo < mo[i] + 4 || !near(getf(B, mm->lo + mo[i]), 765.0f)) {
                snprintf(why, sizeof why, "AppMap_00_MiniMap +%u: not 765", mo[i]);
                goto done;
            }
        for (int i = 0; i < nroots; i++) putf(B, roots[i], k);
        for (int i = 0; i < ne; i++) putf(B, pane(E[i].layout, E[i].pane) + E[i].field, E[i].value);
        putf(B, dt->lo + 288, 1.08f / k), putf(B, dt->lo + 300, 1.28f / k);
        for (int i = 0; i < 8; i++) putf(B, mm->lo + mo[i], 765.0f + 960 * (1 / k - 1));
    } else {
        float hh = 540 * (1 / k - 1);
        // the minimap's four cut panes slide in on straight lines from 0 at frame 0 to y -360, the bottom of a 16:9 layout
        static const char *const cut[] = {"N_Cut_00", "N_Cut_01", "N_Cut_02", "N_Cut_03"};
        size_t ck[4][2];
        for (int i = 0; i < 4; i++) {
            if (anim_curve("AppMap_00_MiniMap", cut[i], "FLPA", FLPA_Y, keys, &nk, 8) != 1 || nk != 2 ||
                !(getf(B, keys[0]) == 0 && getf(B, keys[0] + 4) == 0 && near(getf(B, keys[0] + 8), -360) &&
                  getf(B, keys[1]) == 1 && near(getf(B, keys[1] + 4), -360) && getf(B, keys[1] + 8) == 0)) {
                snprintf(why, sizeof why, "AppMap_00_MiniMap %s: not a line from 0 to y -360", cut[i]);
                goto done;
            }
            ck[i][0] = keys[0], ck[i][1] = keys[1];
        }
        // the large place name of a discovery holds its height (285, then 260) in flat keys: to the top edge
        size_t lk[8];
        int nl;
        if (anim_curve("Location_00_IsDiscovery", "N_Large_00", "FLPA", FLPA_Y, lk, &nl, 8) != 1 || nl != 3 ||
            !near(getf(B, lk[0] + 4), 285) || !near(getf(B, lk[1] + 4), 285) || !near(getf(B, lk[2] + 4), 260)) {
            snprintf(why, sizeof why, "Location_00_IsDiscovery N_Large_00: not the shipped keys");
            goto done;
        }
        // the dragon's tears map icon zooms its picture to 1.08 and 1.28: on the stretched axis by 1 / k more
        size_t dk[8];
        int nd;
        if (anim_curve("PaMapIconDragonTears_00_Zoom", "P_Geoglyph_00", "FLTS", FLTS_SY, dk, &nd, 8) != 1 || nd != 2 ||
            !near(getf(B, dk[0] + 4), 1.08f) || !near(getf(B, dk[1] + 4), 1.28f)) {
            snprintf(why, sizeof why, "PaMapIconDragonTears_00_Zoom P_Geoglyph_00: not the shipped keys");
            goto done;
        }
        for (int i = 0; i < nroots; i++) putf(B, roots[i], k);
        for (int i = 0; i < ne; i++) putf(B, pane(E[i].layout, E[i].pane) + E[i].field, E[i].value);
        for (int i = 0; i < 4; i++) putf(B, ck[i][0] + 8, -(360 + hh)), putf(B, ck[i][1] + 4, -(360 + hh));
        for (int i = 0; i < 3; i++) putf(B, lk[i] + 4, getf(B, lk[i] + 4) + hh);
        for (int i = 0; i < 2; i++) putf(B, dk[i] + 4, getf(B, dk[i] + 4) / k);
    }
    ok = 1;
done:
    free(roots);
    if (!ok) fprintf(stderr, "[totk] UI for %ux%u: the layout archive is not as expected (%s); the UI stays 16:9-shaped\n", w, h, why);
    free(members);
    members = NULL;
    free(E);
    E = NULL;
    ne = cap = 0;
    return ok;
}
