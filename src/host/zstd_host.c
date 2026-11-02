// The game's zstd decoder on the host's libzstd. Most RomFS files of TotK 1.4.3 are zstd frames, many with a dictionary
// from Pack/ZsDic.pack.zs; the decoder is BSD zstd linked into main. Seven call sites of ZSTD_decompressMultiFrame
// (main+0x3a1f0) and ZSTD_decompress_usingDDict (main+0x3a1e0) come here instead (tools/lifter/patches.py, zstd):
// lifted, the decoder ran its literal and match copies as chains of small guest functions; here the same frames decode
// in native code, several times faster (loading stalls). After totk-pc-recomp runtime/ov_zstd.c (MIT, Copyright (c)
// 2026 Valiantsin Dzerakh and contributors), which found the sites and checked the results against the guest decoder.
//
// - Results match the guest's: libzstd decodes the same format, and every call it cannot take or answers with an error
//   goes to the guest decoder (which writes its own error code: a frame libzstd rejects may be one the game accepts).
// - Input inside the output buffer (decoding in place) stays in the guest: how the writes overtake the reads there is
//   not something libzstd promises to do as the game's version does.
// - A guest ZSTD_DDict (zstd's struct: dictBuffer, dictContent, dictSize first) becomes a host ZSTD_DDict made from a
//   copy of its content, kept by guest address, content address, size and a fingerprint of the content.
// - Inputs of 64 KiB or more give the virtual core up while they decode, so other guest threads run on it.
// TOTK_ZSTD_NATIVE=0: the guest decoder everywhere.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "host.h"

int runtime_core_away(void);
void runtime_core_back(int *away);

#define GUEST_MULTIFRAME 0x80003a1f0ull    // 1.4.3 main: ZSTD_decompressMultiFrame
#define GUEST_USING_DDICT 0x80003a1e0ull   // ZSTD_decompress_usingDDict
#define AWAY_BYTES (64u << 10)
#define MAX_DICT (64u << 20)
#define ADDRESS_LIMIT (1ull << 40)         // above any guest mapping: a field that does not look like an address
#define NDICT 32

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

int totk_zstd_native(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_ZSTD_NATIVE");
        on = !(e && *e == '0');
        fprintf(stderr, "[zstd] the game's zstd frames decode %s (TOTK_ZSTD_NATIVE)\n", on ? "on the host" : "in the guest");
    }
    return on;
}

typedef struct { uint64_t guest, content, size, print; ZSTD_DDict *dd; } Dict;
static Dict dicts[NDICT];
static int ndicts;
static pthread_rwlock_t dict_lock = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic uint64_t s_calls, s_in, s_out, s_guest, s_inplace;

// Size, both ends and a sample of the middle: what tells a new dictionary at a reused address from the old one.
static uint64_t fingerprint(const uint8_t *p, uint64_t n) {
    uint64_t h = n * 0x9E3779B97F4A7C15ull;
    uint64_t at[3] = {0, n / 2, n > 64 ? n - 64 : 0};
    for (int k = 0; k < 3; k++)
        for (uint64_t i = at[k]; i < at[k] + 64 && i < n; i += 8) {
            uint64_t v = 0;
            memcpy(&v, p + i, n - i < 8 ? n - i : 8);
            h = (h ^ v) * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 29;
        }
    return h;
}

// The host dictionary for a guest ZSTD_DDict or raw dictionary bytes (guest == 0); NULL when its fields look wrong.
// Kept for good (another thread may be decoding with one); a full table sends further dictionaries to the guest.
static ZSTD_DDict *dict_for(uint64_t guest, uint64_t content, uint64_t size) {
    if (!content || !size || size > MAX_DICT || content >= ADDRESS_LIMIT || content + size > ADDRESS_LIMIT) return NULL;
    uint64_t print = fingerprint(gp(content), size);
    ZSTD_DDict *dd = NULL;
    pthread_rwlock_rdlock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print) dd = dicts[i].dd;
    pthread_rwlock_unlock(&dict_lock);
    if (dd) return dd;
    ZSTD_DDict *made = ZSTD_createDDict(gp(content), (size_t)size);   // a copy: the guest may free or reuse its buffer
    if (!made) return NULL;
    int slot = -1;
    pthread_rwlock_wrlock(&dict_lock);
    for (int i = 0; i < ndicts && !dd; i++)   // another thread may have made it meanwhile
        if (dicts[i].guest == guest && dicts[i].content == content && dicts[i].size == size && dicts[i].print == print) dd = dicts[i].dd;
    if (!dd && ndicts < NDICT) slot = ndicts, dicts[ndicts++] = (Dict){guest, content, size, print, made}, dd = made;
    pthread_rwlock_unlock(&dict_lock);
    if (dd != made) {
        ZSTD_freeDDict(made);
        if (!dd) fprintf(stderr, "[zstd] more than %d dictionaries: further ones decode in the guest\n", NDICT);
        return dd;
    }
    fprintf(stderr, "[zstd] host dictionary %d for %s %#llx: %llu bytes, id %u\n", slot, guest ? "DDict" : "raw dictionary",
            (unsigned long long)(guest ? guest : content), (unsigned long long)size, ZSTD_getDictID_fromDDict(dd));
    return dd;
}

// libzstd's result for the call; *ok 0 when the inputs cannot be taken.
static size_t host_decode(uint64_t dst, uint64_t cap, uint64_t src, uint64_t size, uint64_t dict, uint64_t dict_size,
                          uint64_t ddict, int *ok) {
    static __thread ZSTD_DCtx *dctx;
    *ok = 0;
    if ((cap && !dst) || dst + cap > ADDRESS_LIMIT || !src || src + size > ADDRESS_LIMIT) return 0;
    ZSTD_DDict *dd = NULL;
    if (ddict) {
        if (ddict >= ADDRESS_LIMIT - 24) return 0;
        uint64_t f[3];   // ZSTD_DDict: dictBuffer, dictContent, dictSize
        memcpy(f, gp(ddict), sizeof f);
        if (!(dd = dict_for(ddict, f[1], f[2]))) return 0;
    } else if (dict && dict_size) {
        if (!(dd = dict_for(0, dict, dict_size))) return 0;
    }
    if (!dctx && !(dctx = ZSTD_createDCtx())) return 0;
    *ok = 1;
    int away = size >= AWAY_BYTES ? runtime_core_away() : 0;
    size_t r = dd ? ZSTD_decompress_usingDDict(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size, dd)
                  : ZSTD_decompressDCtx(dctx, gp(dst), (size_t)cap, gp(src), (size_t)size);
    runtime_core_back(&away);
    return r;
}

// At the call: x0 dctx, x1 dst, x2 capacity, x3 src, x4 size, then (dict, dict size, ddict) as the entry has them; x30
// the return address. guest: the guest decoder for what the host does not take.
static void decode(Cpu *c, uint64_t dict, uint64_t dict_size, uint64_t ddict, uint64_t guest) {
    uint64_t dst = c->x[1], cap = c->x[2], src = c->x[3], size = c->x[4];
    int ok = 0;
    size_t r = 0;
    if (cap && src < dst + cap && dst < src + size) s_inplace++;   // in place: the guest's
    else r = host_decode(dst, cap, src, size, dict, dict_size, ddict, &ok);
    if (!ok || ZSTD_isError(r)) {
        uint64_t n = ++s_guest;
        if (ok && n <= 8) fprintf(stderr, "[zstd] %llu bytes at %#llx go to the guest decoder: %s\n", (unsigned long long)size,
                                  (unsigned long long)src, ZSTD_getErrorName(r));
        GuestFn g = dispatch_lookup(guest);
        if (!g) runtime_fail("zstd: no guest decoder");
        g(c);   // the registers are still the call's
        return;
    }
    c->x[0] = r;
    uint64_t n = ++s_calls;
    s_in += size; s_out += r;
    if (n == 1 || !(n & 0x3fff))
        fprintf(stderr, "[zstd] %llu calls on the host, %.1f MiB in, %.1f MiB out; %llu to the guest decoder (%llu in place)\n",
                (unsigned long long)n, s_in / 1048576.0, s_out / 1048576.0, (unsigned long long)s_guest, (unsigned long long)s_inplace);
}

void totk_zstd_multiframe(Cpu *c) { decode(c, c->x[5], c->x[6], c->x[7], GUEST_MULTIFRAME); }
void totk_zstd_using_ddict(Cpu *c) { decode(c, 0, 0, c->x[5], GUEST_USING_DDICT); }
