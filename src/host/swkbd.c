/* nn::swkbd::ShowKeyboard: text entry for the game.
 *
 * On the Switch the call starts the keyboard library applet, which takes the screen until the player confirms. TotK
 * calls it in one place: naming a horse at a stable (UserName preset, at most 9 characters, '@' '%' '\' disabled).
 * Without this the SDK went on to nn::applet::StartLibraryAppletImmediately, which has no host side, and the game
 * crashed there (GitHub #58, Woodland Stable).
 *
 * The player types into a field over the picture (text_entry.c: the physical keyboard on a PC, Steam's on-screen
 * keyboard on the Steam Deck), which starts with the game's initial text, else a default name in the game's language.
 * TOTK_SWKBD_TEXT=<text> answers without the field (scripted runs); without a window (TOTK_HEADLESS) the starting
 * text is the answer.
 *
 * The ShowKeyboardArg layout (KeyboardConfig, then the work buffer {pointer, size}) follows the SDK's own setters, as
 * MacNativeGames' swkbd_host.c (MIT) reads them; the code is ours. SDK 20.5 (TotK
 * 1.4.3) grew KeyboardConfig to 0x518 bytes; the fields used here below +0x3d8 did not move. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "../runtime/hle.h"

enum {
    KB_TYPE = 0x00, KB_DISABLED = 0x1c, KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0, KB_UTF8 = 0x3bd, KB_INIT_OFF = 0x3c0,
    KB_INIT_LEN = 0x3c4, ARG_WORK = 0x518, ARG_WORK_SIZE = 0x520,
    KB_TYPE_NUMPAD = 1, KB_LIMIT = 500, RESULT_CANCELED = 0x29f,
};

uint64_t runtime_game_language(void);   /* overrides_oe.c: the Switch language code, packed into 8 bytes */
int text_entry_run(const uint16_t *init, size_t ninit, int max_units, int min_units, uint32_t disabled, int numeric,
                   uint16_t *out, size_t cap, size_t *nout);   /* text_entry.c */

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* UTF-8 -> UTF-16, at most cap units; a character that does not fit whole is dropped, bad bytes become U+FFFD. */
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        uint32_t c; int k;
        if (*p < 0x80) c = *p, k = 0;
        else if ((*p & 0xe0) == 0xc0) c = *p & 0x1f, k = 1;
        else if ((*p & 0xf0) == 0xe0) c = *p & 0x0f, k = 2;
        else if ((*p & 0xf8) == 0xf0) c = *p & 0x07, k = 3;
        else c = 0xfffd, k = 0;
        p++;
        for (int i = 0; i < k; i++, p++) {
            if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; }
            c = c << 6 | (*p & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 > cap) break;
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10));
            out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            if (n + 1 > cap) break;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* UTF-16 -> NUL-terminated UTF-8 in cap bytes, cut at a character boundary. */
static void utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        char b[4]; size_t k;
        if (c < 0x80) b[0] = (char)c, k = 1;
        else if (c < 0x800) b[0] = (char)(0xc0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3f)), k = 2;
        else if (c < 0x10000) b[0] = (char)(0xe0 | c >> 12), b[1] = (char)(0x80 | ((c >> 6) & 0x3f)), b[2] = (char)(0x80 | (c & 0x3f)), k = 3;
        else b[0] = (char)(0xf0 | c >> 18), b[1] = (char)(0x80 | ((c >> 12) & 0x3f)), b[2] = (char)(0x80 | ((c >> 6) & 0x3f)), b[3] = (char)(0x80 | (c & 0x3f)), k = 4;
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
}

/* Characters the applet would refuse: control characters, and the disabled keys (bits as the applet's: 1 space, 2 '@',
   3 '%', 4 '/', 5 '\', 6 digits); a numeric keyboard takes digits only. Surrogate pairs are kept whole or dropped. */
static size_t clean(uint16_t *t, size_t n, size_t max, uint32_t disabled, int numeric) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = t[i];
        int pair = c >= 0xd800 && c < 0xdc00 && i + 1 < n;
        if (!pair) {
            int digit = c >= '0' && c <= '9';
            if (c < 0x20 || c == 0x7f || (numeric && !digit)) continue;
            if ((disabled & 1u << 1 && c == ' ') || (disabled & 1u << 2 && c == '@') || (disabled & 1u << 3 && c == '%') ||
                (disabled & 1u << 4 && c == '/') || (disabled & 1u << 5 && c == '\\') || (disabled & 1u << 6 && digit)) continue;
        }
        size_t k = pair ? 2 : 1;
        if (o + k > max) break;
        t[o++] = c;
        if (pair) t[o++] = t[++i];
    }
    return o;
}

/* A default name in the game's language (at most 9 characters, none of them disabled for a horse's name). */
static const char *default_name(void) {
    uint64_t packed = runtime_game_language();
    char code[9] = {0};
    memcpy(code, &packed, 8);
    static const struct { const char *code, *name; } names[] = {
        {"ja", "\xe3\x82\xa6\xe3\x83\x9e"},            /* ウマ */
        {"de", "Pferd"}, {"fr", "Cheval"}, {"es", "Caballo"}, {"it", "Cavallo"}, {"nl", "Paard"}, {"pt", "Cavalo"},
        {"ru", "\xd0\x9b\xd0\xbe\xd1\x88\xd0\xb0\xd0\xb4\xd0\xba\xd0\xb0"},   /* Лошадка */
        {"ko", "\xeb\xa7\x90"},                        /* 말 */
        {"zh-Hant", "\xe9\xa6\xac"},                   /* 馬 */
        {"zh", "\xe9\xa9\xac"},                        /* 马 */
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strncmp(code, names[i].code, strlen(names[i].code))) return names[i].name;
    return "Horse";
}

/* Result ShowKeyboard(String *out, const ShowKeyboardArg &arg); String = {pointer, byte size}. */
static uint32_t show_keyboard(uint64_t *str, const uint8_t *arg) {
    if (!str || !arg) runtime_fail("nn::swkbd::ShowKeyboard: null argument");
    int32_t max_len = (int32_t)rd32(arg + KB_MAX_LEN), min_len = (int32_t)rd32(arg + KB_MIN_LEN);
    if (max_len <= 0 || max_len > KB_LIMIT) max_len = KB_LIMIT;
    if (min_len < 0) min_len = 0;
    uint32_t disabled = rd32(arg + KB_DISABLED);
    int numeric = rd32(arg + KB_TYPE) == KB_TYPE_NUMPAD, utf8 = arg[KB_UTF8] != 0;

    uint16_t u[KB_LIMIT + 1];
    size_t n = 0;
    const char *from = "the game's initial text";
    uint64_t work = rd64(arg + ARG_WORK), work_size = rd64(arg + ARG_WORK_SIZE);
    int32_t init_off = (int32_t)rd32(arg + KB_INIT_OFF), init_len = (int32_t)rd32(arg + KB_INIT_LEN);
    if (init_len > 0 && work && init_off >= 0 && (uint64_t)init_off + (uint64_t)init_len * 2 <= work_size) {
        const uint16_t *t = (const uint16_t *)(uintptr_t)(work + (uint64_t)init_off);
        while (n < (size_t)init_len && n < KB_LIMIT && t[n]) { u[n] = t[n]; n++; }
    }
    const char *e = getenv("TOTK_SWKBD_TEXT");   /* an answer without the field (scripted runs) */
    int forced = e && *e;
    if (forced) { from = "TOTK_SWKBD_TEXT"; n = utf8_to_utf16(e, u, KB_LIMIT); }
    else if (!n) { from = "the default name"; n = utf8_to_utf16(default_name(), u, KB_LIMIT); }
    n = clean(u, n, (size_t)max_len, disabled, numeric);
    int confirmed = n > 0 && (int)n >= min_len;
    if (!forced) {   /* the field over the picture (text_entry.c), starting with that text; none without a window */
        uint16_t typed[KB_LIMIT + 1];
        size_t got = 0;
        int r = text_entry_run(u, n, max_len, min_len, disabled, numeric, typed, KB_LIMIT, &got);
        if (r >= 0) {
            from = "the text field";
            memcpy(u, typed, got * 2);
            n = clean(u, got, (size_t)max_len, disabled, numeric);
            confirmed = r == 1 && n > 0 && (int)n >= min_len;
        }
    }
    if (!confirmed) n = 0;

    uint64_t buf = str[0], size = str[1];
    if (buf && size) {
        if (utf8) utf16_to_utf8(u, n, (char *)(uintptr_t)buf, size);
        else {
            size_t units = size / 2;   /* one kept for the terminator */
            if (units) {
                if (n > units - 1) { n = units - 1; if (n && u[n - 1] >= 0xd800 && u[n - 1] < 0xdc00) n--; }
                memcpy((void *)(uintptr_t)buf, u, n * 2);
                ((uint16_t *)(uintptr_t)buf)[n] = 0;
            }
        }
    }
    char shown[KB_LIMIT * 3 + 1];
    utf16_to_utf8(u, n, shown, sizeof shown);
    fprintf(stderr, "[totk] software keyboard: %d-%d characters, %s out: \"%s\" from %s%s\n", min_len, max_len,
            utf8 ? "UTF-8" : "UTF-16", shown, from, confirmed ? "" : " (cancelled: nothing usable)");
    return confirmed ? 0 : RESULT_CANCELED;
}

HLE_NATIVE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", show_keyboard, 'w')
