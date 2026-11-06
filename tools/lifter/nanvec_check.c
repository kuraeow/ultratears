// Check of simd_fp.h's vector NaN fix (g_simd_nanvec, TOTK_FP_NANVEC): for every op it covers, both precisions,
// FPCR.DN and FPCR.FZ on and off, vslow's vector path must give the same bits as its lane-by-lane scalar path, over the
// special values (signalling and quiet NaNs of both signs and payloads, infinities, zeros, denormals, normals) in every
// lane combination and over random bit patterns. Build and run (MSYS2 CLANG64 clang):
//   clang -O2 -march=x86-64-v3 -Isrc/runtime tools/lifter/nanvec_check.c -o nanvec_check.exe && ./nanvec_check.exe
// Exit code 0 and "0 differences": the two paths agree.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "cpu.h"
#include "simd.h"

volatile int g_fnprof;
int g_simd_nanvec = 1;

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static const uint32_t SPEC_S[] = {0x7fc00000, 0xffc00000, 0x7f800001, 0xff800001, 0x7fa00000, 0x7fc12345, 0xff812345,
                                  0x7f800000, 0xff800000, 0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x3f800000,
                                  0xbf800000, 0x7f7fffff, 0x00800000, 0x40490fdb};
static const uint64_t SPEC_D[] = {0x7ff8000000000000, 0xfff8000000000000, 0x7ff0000000000001, 0xfff0000000000001,
                                  0x7ff4000000000000, 0x7ff8000000012345, 0xfff0000000012345, 0x7ff0000000000000,
                                  0xfff0000000000000, 0, 0x8000000000000000, 1, 0x800fffffffffffff, 0x3ff0000000000000,
                                  0xbff0000000000000, 0x7fefffffffffffff, 0x0010000000000000, 0x400921fb54442d18};
enum { NS = sizeof SPEC_S / sizeof SPEC_S[0], ND = sizeof SPEC_D / sizeof SPEC_D[0] };
static const int OPS[] = {FOP_ADD, FOP_SUB, FOP_MUL, FOP_NMUL, FOP_DIV, FOP_ABD, FOP_SQRT};
static const char *OPN[] = {"ADD", "SUB", "MUL", "NMUL", "DIV", "ABD", "SQRT"};

static uint64_t diffs, checks;

static void one_s(Cpu *c, int k, const uint32_t ua[4], const uint32_t ub[4]) {
    vf32 a, b, d = {0};
    memcpy(&a, ua, 16); memcpy(&b, ub, 16);
    g_simd_nanvec = 0; vf32 r0 = vslow_s(c, OPS[k], a, b, d);
    g_simd_nanvec = 1; vf32 r1 = vslow_s(c, OPS[k], a, b, d);
    checks++;
    if (memcmp(&r0, &r1, 16)) {
        if (diffs++ < 20) {
            uint32_t x[4], y[4]; memcpy(x, &r0, 16); memcpy(y, &r1, 16);
            printf("s %s fpcr %#llx a %08x b %08x: scalar %08x vector %08x (lane 0)\n", OPN[k], (unsigned long long)c->fpcr,
                   ua[0], ub[0], x[0], y[0]);
        }
    }
}
static void one_d(Cpu *c, int k, const uint64_t ua[2], const uint64_t ub[2]) {
    vf64 a, b, d = {0};
    memcpy(&a, ua, 16); memcpy(&b, ub, 16);
    g_simd_nanvec = 0; vf64 r0 = vslow_d(c, OPS[k], a, b, d);
    g_simd_nanvec = 1; vf64 r1 = vslow_d(c, OPS[k], a, b, d);
    checks++;
    if (memcmp(&r0, &r1, 16) && diffs++ < 20) {
        uint64_t x[2], y[2]; memcpy(x, &r0, 16); memcpy(y, &r1, 16);
        printf("d %s fpcr %#llx a %016llx b %016llx: scalar %016llx vector %016llx (lane 0)\n", OPN[k],
               (unsigned long long)c->fpcr, (unsigned long long)ua[0], (unsigned long long)ub[0],
               (unsigned long long)x[0], (unsigned long long)y[0]);
    }
}

int main(void) {
    static Cpu cpu;
    Cpu *c = &cpu;
    const uint64_t modes[] = {0, FPCR_DN, FPCR_FZ, FPCR_DN | FPCR_FZ};
    for (int m = 0; m < 4; m++) {
        c->fpcr = modes[m];
        // the host's FZ follows FPCR.FZ (rt_fpcr_write): MXCSR FTZ and DAZ
        _mm_setcsr((_mm_getcsr() & ~0x8040u) | ((modes[m] & FPCR_FZ) ? 0x8040u : 0));
        for (int k = 0; k < 7; k++) {
            for (int i = 0; i < NS; i++)
                for (int j = 0; j < NS; j++) {   // every pair in lane 0; the other lanes: shifted pairs and a normal
                    uint32_t a[4] = {SPEC_S[i], SPEC_S[(i + 1) % NS], 0x3f800000, SPEC_S[j]};
                    uint32_t b[4] = {SPEC_S[j], SPEC_S[(j + 3) % NS], SPEC_S[i], 0x40000000};
                    one_s(c, k, a, b);
                }
            for (int i = 0; i < ND; i++)
                for (int j = 0; j < ND; j++) {
                    uint64_t a[2] = {SPEC_D[i], SPEC_D[j]}, b[2] = {SPEC_D[j], SPEC_D[(i + 5) % ND]};
                    one_d(c, k, a, b);
                }
            for (int n = 0; n < 200000; n++) {   // random bits, every fourth lane forced to a NaN of random payload
                uint32_t a[4], b[4];
                for (int l = 0; l < 4; l++) {
                    a[l] = (uint32_t)rnd(); b[l] = (uint32_t)rnd();
                    if (rnd() % 4 == 0) a[l] |= 0x7f800000 | 1;
                    if (rnd() % 4 == 0) b[l] |= 0x7f800000 | 1;
                }
                one_s(c, k, a, b);
                uint64_t ad[2], bd[2];
                for (int l = 0; l < 2; l++) {
                    ad[l] = rnd(); bd[l] = rnd();
                    if (rnd() % 4 == 0) ad[l] |= 0x7ff0000000000001ull;
                    if (rnd() % 4 == 0) bd[l] |= 0x7ff0000000000001ull;
                }
                one_d(c, k, ad, bd);
            }
        }
    }
    printf("%llu checks, %llu differences\n", (unsigned long long)checks, (unsigned long long)diffs);
    return diffs != 0;
}
