// Temporal upscaling and anti-aliasing (antialias=fsr3 in ultratears.cfg, TOTK_AA=fsr3): AMD FidelityFX Super
// Resolution 3.1's upscaler (third_party/fsr3, MIT, FidelityFX SDK v1.1.4) in place of the game's own last scaling pass
// (FSR 1 EASU). The SDK's host code is not used: its passes run here, on the replay's command buffer, with the resources
// and constants its ffx_fsr3upscaler.cpp gives them (that file read for behaviour; the dispatch order, the clears and the
// constants follow it).
//
// Where it runs in TotK's frame (1.4.3, measured with TOTK_VK_DRAWTRACE at Lookout Landing, 2560x1440 output, 2128x1200
// scene):
//   the G-buffer: passes of 6 and 8 colour targets at the scene size with the scene depth (D32F); target 5 is the light
//     target (R11G11B10F), the vertex shaders read the scene camera as constant bank c4 (a 0x940-byte block);
//   lighting into the light target, then transparents and particles into it with the scene depth: the scene is "armed";
//   the bloom chain (passes a quarter as wide and less): the scene has "ended";
//   the tonemap with the bloom into a scratch target, the game's FXAA back into the light target;
//   one pass of the output's size, no depth: the game's FSR 1 EASU draws the scene into it (12 texelFetch at scene
//     coordinates: no other texture can stand in for its input), then the HUD is drawn over it.
// Here: the scene's draws (passes with the scene depth, from the G-buffer until the scene ends) are jittered by moving
// their viewport a sub-pixel Halton(2,3) step; at the EASU draw, FSR 3.1 takes the light target (tonemapped and
// FXAA'd) with the scene depth and the camera's motion (tsr_mv.comp: the depth reprojected through this frame's and last
// frame's camera, in double precision), writes the output's size, which is copied into the pass's target, and the EASU
// draw is skipped. The HUD then draws over FSR's picture at the output size, never jittered.
//
// Threads: the translation thread walks a submission in order before the recording threads record its chunks
// (backend_vk.c vkb_mt_submit); the decisions are made in the walk (vkb_tsr_walk_rt, vkb_tsr_walk_draw) and handed to
// the recording threads as events at command positions (vkb_seq): the jitter turned on or off at a render target
// change, FSR at a draw. A chunk starts with the walk's jitter state at its start. On one recording thread
// (TOTK_VK_RECORD_THREADS=1) the walk calls come in the same order, just before the recording.
//
// [temporary] Stage 1 (dev-local/docs/perf/upscaler-eval.md): only the camera's motion (objects that move on their own get
// none yet); FSR reads the picture after the game's FXAA; no texture LOD bias for the lower render size; fp32 shaders
// only (no fp16 permutation for the Steam Deck yet). Each is listed in dev-local/docs/plan.md.
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "vkb.h"
#include "../../third_party/fsr3/fsr3_spv.h"
#include "vkb_tsr_mv_spv.h"
#include "vkb_shader.h"
#include "../runtime/bufcat.h"

void vkb_tsr_obj_log(void);

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

int totk_output_size(uint64_t *packed);   // host/render_res.c

// The game's FSR 1 EASU fragment shader (the scaling draw FSR 3.1 replaces) and its FXAA (after the tonemap, into the
// light target: replaced by a copy of its source, FSR 3.1 anti-aliases instead), by the key's first hex digits.
static const char *const EASU_FS[] = {"ac58fc18eda5"};
static const char *const FXAA_FS[] = {"22a61cb1ec73"};
enum { CAM_BYTES = 0x940, SCENE_MIN_W = 640 };

// ---------------------------------------------------------------- settings
static int g_mode = -1;   // 0 off (the game's FSR 1), 1 FSR 3.1
static float g_sharp = 0.5f;
static int mode(void) {
    if (g_mode < 0) {
        const char *e = getenv("TOTK_AA");
        g_mode = e && (!strcmp(e, "fsr3") || !strcmp(e, "fsr")) ? 1 : 0;
        const char *s = getenv("TOTK_SHARPNESS");
        int v = s && *s ? atoi(s) : 50;
        g_sharp = (v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
        if (g_mode) VLOG("antialiasing: FSR 3.1 (sharpness %.0f %%)", g_sharp * 100);
    }
    return g_mode;
}
int vkb_tsr_on(void) { return mode() == 1 && vkb_ready(); }

// The texture LOD bias for a scene drawn smaller than the output (AMD's advice for FSR: log2(render / display) - 1),
// from the scale setting at the start (render_res.c: unset is the game's 5/6). TOTK_AA_LODBIAS=<value> overrides.
float vkb_tsr_lod_bias(void) {
    static float bias = 1e9f;
    if (bias > 1e8f) {
        bias = 0;
        if (mode() == 1) {
            const char *s = getenv("TOTK_RENDER_SCALE"), *o = getenv("TOTK_AA_LODBIAS");
            double k = s && *s ? atof(s) / 100.0 : 5.0 / 6.0;
            if (k < 0.25 || k > 4) k = 5.0 / 6.0;
            bias = (float)(log2(k) - 1.0);
            if (bias > 0) bias = 0;
            if (o && *o) bias = (float)atof(o);
            VLOG("antialiasing: texture LOD bias %.2f", bias);
        }
    }
    return bias;
}

// ---------------------------------------------------------------- the walk: where the scene is
enum { PH_IDLE, PH_SCENE, PH_ARMED, PH_ENDED, PH_DONE };
enum { EV_JIT_ON = 1, EV_JIT_OFF, EV_FSR, EV_FXAA };
typedef struct {
    tex_t color, depth;      // the light target and the scene depth (copies: the game copies texture objects by value)
    float m[16];             // this frame's clip space to last frame's, column-major (tsr_mv.comp)
    int have_m, reset;
    float jx, jy;            // the jitter the scene was drawn with, pixels
    float near_, far_, fov;  // the camera, for FSR's depth thresholds
    float dt_ms;
} fsr_job;
typedef struct { uint64_t seq; int kind; float jx, jy; const void *depth_mem; int job, jit; uint32_t frame, sw, sh; } tsr_event;
enum { MAX_EV = 64, MAX_JOB = 4 };
static struct {
    int phase;
    uint32_t sw, sh;               // this frame's scene size
    const void *depth_mem;         // the scene depth's memory (passes drawing with it are the scene's)
    tex_t color, depth;            // the armed light target, the scene depth
    int have_color;
    uint64_t cam_addr;             // this frame's camera block (vertex c4 of the scene's draws)
    double prev_vp[16];
    int prev_ok;
    uint32_t jframe;
    float jx, jy;                  // this frame's jitter
    int jit;                       // this frame is jittered
    int hold;                      // frames left without jitter (a jittered frame FSR did not resolve shook)
    int reset;                     // the next FSR starts a new history
    uint64_t sub;                  // the submission the events are of
    uint64_t frames, missed, holds, easu_unknown, logged_at;
    double last_ms;
    int said_depth, said_begin;
    uint32_t done_w;               // the scene width of the last frame FSR ran on
} W;
// events of the submission being recorded (the walk appends, the recording threads read up to n)
static tsr_event g_ev[MAX_EV];
static _Atomic int g_nev;
static fsr_job g_job[MAX_JOB];
static int g_njob;

static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

static void ev_add(int kind, int job) {
    int n = atomic_load_explicit(&g_nev, memory_order_relaxed);
    if (n >= MAX_EV) return;
    g_ev[n] = (tsr_event){vkb_seq(), kind, W.jx, W.jy, W.depth_mem, job, W.jit, (uint32_t)W.frames, W.sw, W.sh};
    atomic_store_explicit(&g_nev, n + 1, memory_order_release);
}


static int is_light_format(const tex_t *t) {
    VkImageAspectFlags a;
    int astc;
    VkFormat f = vkb_format((int)t->d.format, &a, &astc);
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}
static int is_depth32(const tex_t *t) {
    VkImageAspectFlags a;
    int astc;
    return t && vkb_format((int)t->d.format, &a, &astc) == VK_FORMAT_D32_SFLOAT;
}

static void output_size(uint32_t *w, uint32_t *h) {
    uint64_t p;
    if (totk_output_size(&p)) *w = (uint32_t)p, *h = (uint32_t)(p >> 32);
    else *w = 1920, *h = 1080;
}

// A new frame's scene starts (its first G-buffer pass): the last frame is closed, this one's jitter chosen.
static void frame_begin(void) {
    W.frames++;
    if (W.phase != PH_DONE && W.phase != PH_IDLE) {   // the last scene got no FSR
        W.missed++;
        if (W.jit) W.hold = 60, W.holds++;
        W.reset = 1;
    } else if (W.hold)
        W.hold--;
    W.phase = PH_SCENE;
    W.have_color = 0;
    W.cam_addr = 0;
    uint32_t ow, oh;
    output_size(&ow, &oh);
    double r = W.sw ? (double)ow / W.sw : 1.0;
    uint32_t phases = (uint32_t)(8.0 * r * r);   // FSR's jitter sequence length
    if (phases < 8) phases = 8;
    uint32_t i = W.jframe++ % phases + 1;
    static int nojit = -1;
    if (nojit < 0) { const char *e = getenv("TOTK_AA_JITTER"); nojit = e && *e == '0'; }   // development: no jitter
    W.jit = !W.hold && !nojit;
    W.jx = W.jit ? halton(i, 2) - 0.5f : 0, W.jy = W.jit ? halton(i, 3) - 0.5f : 0;
}

void vkb_tsr_walk_rt(const p_set_rt *rt) {
    if (!vkb_tsr_on()) return;
    const tex_t *c = rt->n > 0 ? rt->color[0] : NULL, *z = rt->depth;
    // the G-buffer: three or more colour targets with a D32F depth of their size
    if (rt->n >= 3 && c && is_depth32(z) && z->d.width == c->d.width && z->d.height == c->d.height && c->d.width >= SCENE_MIN_W) {
        if (W.phase == PH_SCENE && z->mem == W.depth_mem) return;   // the second G-buffer pass of the frame
        // a later G-buffer pass of the same scene (TotK draws 8 targets with the scene depth again after the lighting and
        // some small passes): the scene goes on, jittered again
        if ((W.phase == PH_ARMED || W.phase == PH_ENDED) && z->mem == W.depth_mem && c->d.width == W.sw) {
            if (W.phase == PH_ENDED) ev_add(EV_JIT_ON, -1);
            W.phase = PH_SCENE;
            return;
        }
        if (c->d.width < W.sw && (W.phase == PH_SCENE || W.done_w == W.sw)) return;   // a smaller pass of the same shape
        if (W.phase != PH_DONE && W.phase != PH_IDLE && W.said_begin < 8) {
            W.said_begin++;
            VLOG("antialiasing: a scene starts in phase %d: %d targets %ux%u, depth %p (the scene's %p, %ux%u)", W.phase, rt->n,
                 c->d.width, c->d.height, z->mem, W.depth_mem, W.sw, W.sh);
        }
        W.sw = c->d.width, W.sh = c->d.height, W.depth_mem = z->mem, W.depth = *z;
        frame_begin();
        ev_add(EV_JIT_ON, -1);   // the scene's passes: jittered (when W.jit) and drawn with the object motion target
        return;
    }
    if (W.phase == PH_SCENE || W.phase == PH_ARMED) {
        // armed: one colour target of the scene size in the light format, drawn with the scene depth
        if (rt->n == 1 && c && z && z->mem == W.depth_mem && is_light_format(c) && c->d.width == W.sw && c->d.height == W.sh) {
            W.color = *c, W.have_color = 1, W.phase = PH_ARMED;
            return;
        }
        // ended: the bloom chain, a pass at most a quarter as wide as the scene, after arming
        if (W.phase == PH_ARMED && c && c->d.width * 4 <= W.sw) {
            W.phase = PH_ENDED;
            ev_add(EV_JIT_OFF, -1);
        }
    }
}

static double now_ms(void) { return (double)runtime_now_ns() / 1e6; }

// The inverse of a row-major 4x4 matrix (0: singular): the adjugate over the determinant, both from the 2x2 minors of
// the top two rows (s) and of the bottom two (c).
static int inv4(const double m[16], double o[16]) {
    const double *a = m, *b = m + 4, *c = m + 8, *d = m + 12;   // the rows
    double s0 = a[0] * b[1] - b[0] * a[1], s1 = a[0] * b[2] - b[0] * a[2], s2 = a[0] * b[3] - b[0] * a[3];
    double s3 = a[1] * b[2] - b[1] * a[2], s4 = a[1] * b[3] - b[1] * a[3], s5 = a[2] * b[3] - b[2] * a[3];
    double c0 = c[0] * d[1] - d[0] * c[1], c1 = c[0] * d[2] - d[0] * c[2], c2 = c[0] * d[3] - d[0] * c[3];
    double c3 = c[1] * d[2] - d[1] * c[2], c4 = c[1] * d[3] - d[1] * c[3], c5 = c[2] * d[3] - d[2] * c[3];
    double det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (!(fabs(det) > 1e-300)) return 0;
    double k = 1.0 / det;
    o[0] = (b[1] * c5 - b[2] * c4 + b[3] * c3) * k;
    o[1] = (-a[1] * c5 + a[2] * c4 - a[3] * c3) * k;
    o[2] = (d[1] * s5 - d[2] * s4 + d[3] * s3) * k;
    o[3] = (-c[1] * s5 + c[2] * s4 - c[3] * s3) * k;
    o[4] = (-b[0] * c5 + b[2] * c2 - b[3] * c1) * k;
    o[5] = (a[0] * c5 - a[2] * c2 + a[3] * c1) * k;
    o[6] = (-d[0] * s5 + d[2] * s2 - d[3] * s1) * k;
    o[7] = (c[0] * s5 - c[2] * s2 + c[3] * s1) * k;
    o[8] = (b[0] * c4 - b[1] * c2 + b[3] * c0) * k;
    o[9] = (-a[0] * c4 + a[1] * c2 - a[3] * c0) * k;
    o[10] = (d[0] * s4 - d[1] * s2 + d[3] * s0) * k;
    o[11] = (-c[0] * s4 + c[1] * s2 - c[3] * s0) * k;
    o[12] = (-b[0] * c3 + b[1] * c1 - b[2] * c0) * k;
    o[13] = (a[0] * c3 - a[1] * c1 + a[2] * c0) * k;
    o[14] = (-d[0] * s3 + d[1] * s1 - d[2] * s0) * k;
    o[15] = (c[0] * s3 - c[1] * s1 + c[2] * s0) * k;
    return 1;
}

// The FSR job of this frame: the camera's reprojection, read from the uniforms as of the submit call.
static int make_job(void) {
    if (g_njob >= MAX_JOB) return -1;
    fsr_job *j = &g_job[g_njob];
    memset(j, 0, sizeof *j);
    j->color = W.color, j->depth = W.depth;
    j->jx = W.jit ? W.jx : 0, j->jy = W.jit ? W.jy : 0;
    j->near_ = 0.1f, j->far_ = 10000.0f, j->fov = 1.0f;
    if (W.cam_addr) {
        float blk[44];   // vec4 rows 0-10: view (0-2), view-projection (3-6), projection (7-10)
        vkb_ucap_read(W.cam_addr, blk, sizeof blk);
        double cur[16], ci[16];
        for (int i = 0; i < 16; i++) cur[i] = blk[12 + i];
        if (W.prev_ok && inv4(cur, ci)) {
            double m[16];
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    double v = 0;
                    for (int x = 0; x < 4; x++) v += W.prev_vp[r * 4 + x] * ci[x * 4 + k];
                    m[r * 4 + k] = v;
                }
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) j->m[k * 4 + r] = (float)m[r * 4 + k];   // column-major for GLSL
            j->have_m = 1;
            // TOTK_AA_LOG=<n>[@<present>] (development): n frames' motion at five points, pixels, from that present on
            static int logn = -1, logged;
            static unsigned long long from;
            if (logn < 0) { const char *e = getenv("TOTK_AA_LOG"); logn = e ? atoi(e) : 0; from = e && strchr(e, '@') ? strtoull(strchr(e, '@') + 1, NULL, 10) : 0; }
            if (logged < logn && vkb_frame() >= from && fabs(m[3]) + fabs(m[7]) + fabs(m[0] - 1) + fabs(m[5] - 1) > 1e-5) {
                logged++;
                char line[400];
                int n = 0;
                static const double pts[5][2] = {{0, 0}, {-0.8, 0.8}, {0.8, 0.8}, {-0.8, -0.8}, {0.8, -0.8}};
                for (int p = 0; p < 5; p++) {
                    double v[4] = {pts[p][0], pts[p][1], 0.9, 1}, o[4] = {0};
                    for (int r = 0; r < 4; r++)
                        for (int k = 0; k < 4; k++) o[r] += m[r * 4 + k] * v[k];
                    n = bufcat(line, sizeof line, n, " (%+.1f,%+.1f)", (o[0] / o[3] - v[0]) * 0.5 * W.sw,
                                  -(o[1] / o[3] - v[1]) * 0.5 * W.sh);
                }
                VLOG("antialiasing: motion at centre, top-left, top-right, bottom-left, bottom-right (px):%s; cam %.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f",
                     line, cur[0], cur[1], cur[2], cur[3], cur[12], cur[13], cur[14], cur[15]);
            }
        }
        memcpy(W.prev_vp, cur, sizeof cur);
        W.prev_ok = 1;
        // the projection (rows 7-10, OpenGL form): cot(fov/2) = P[1][1]; near and far from P[2][2], P[2][3]
        const float *P = blk + 28;
        if (P[5] > 0.01f) j->fov = 2.0f * atanf(1.0f / P[5]);
        double a = P[10], b = P[11];
        if (fabs(a - 1) > 1e-9 && fabs(a + 1) > 1e-9) {
            double n = b / (a - 1), f = b / (a + 1);
            n = fabs(n), f = fabs(f);
            if (n > f) { double t = n; n = f; f = t; }
            if (n > 1e-4 && f > n) j->near_ = (float)n, j->far_ = (float)f;
        }
        if (!W.said_depth) {
            W.said_depth = 1;
            VLOG("antialiasing: camera near %.3f far %.1f fov %.1f deg (P22 %g P23 %g)", j->near_, j->far_, j->fov * 57.2958f, P[10], P[11]);
        }
    } else
        W.prev_ok = 0;
    j->reset = W.reset || !j->have_m;
    W.reset = 0;
    double t = now_ms();
    j->dt_ms = W.last_ms > 0 && t - W.last_ms < 100 ? (float)(t - W.last_ms) : 16.7f;
    W.last_ms = t;
    return g_njob++;
}

int vkb_tsr_fs_easu(const char *key) {
    if (!key) return 0;
    for (size_t i = 0; i < sizeof EASU_FS / sizeof *EASU_FS; i++)
        if (!strncmp(key, EASU_FS[i], strlen(EASU_FS[i]))) return 1;
    return 0;
}
int vkb_tsr_fs_fxaa(const char *key) { return key && !strncmp(key, FXAA_FS[0], strlen(FXAA_FS[0])); }

// A draw in the walk: the scene's camera, and the EASU draw where FSR goes.
static void arena_keep(int bank, uint64_t addr, uint64_t size);
void vkb_tsr_walk_draw(const p_set_rt *rt, const p_bind_buf *vubo, const char *fs_key) {
    if (!vkb_tsr_on() || W.phase == PH_IDLE || W.phase == PH_DONE) return;
    if (W.phase == PH_SCENE && rt->depth && rt->depth->mem == W.depth_mem) {
        uint64_t c4_addr = vubo[1].addr, c4_size = vubo[1].size;
        if (c4_size == CAM_BYTES && c4_addr && !W.cam_addr) W.cam_addr = c4_addr;
        for (int bank = 4; bank <= 7; bank++)   // this frame's blocks of the camera and the objects, for the next frame
            if (vubo[bank - 3].addr && vubo[bank - 3].size) arena_keep(bank, vubo[bank - 3].addr, vubo[bank - 3].size);
        return;
    }
    // the game's FXAA into the light target, after the scene: a copy of its source instead (vkb_draw.c fxaa_copy)
    if (W.phase == PH_ENDED && W.have_color && rt->n == 1 && rt->color[0] && rt->color[0]->mem == W.color.mem && fs_key &&
        vkb_tsr_fs_fxaa(fs_key)) {
        ev_add(EV_FXAA, -1);
        return;
    }
    // The output pass: no depth when the scene is scaled; with the scene at the output's size (scale=100) the game's
    // EASU draws back into the light target with the scene depth bound - known by its shader then. Missed, no frame was
    // ever done, and the half-size passes with a D32F depth started "scenes" of their own: the object motion target was
    // made again at each size change, every frame, until the driver failed (2026-11-08, L27).
    if ((W.phase == PH_ENDED || W.phase == PH_ARMED) && W.have_color && rt->n == 1 && rt->color[0] &&
        (!rt->depth || vkb_tsr_fs_easu(fs_key))) {
        uint32_t ow, oh;
        output_size(&ow, &oh);
        const tex_t *c = rt->color[0];
        if (c->d.width != ow || c->d.height != oh || ow < W.sw) return;
        if (!vkb_tsr_fs_easu(fs_key)) {
            if (fs_key && W.easu_unknown++ < 4) VLOG("antialiasing: the output pass draws with fs %.12s, not the game's EASU", fs_key);
            return;
        }
        int j = make_job();
        if (j >= 0) ev_add(EV_FSR, j);
        W.phase = PH_DONE;
        W.done_w = W.sw;
        if (W.frames >= W.logged_at + 1800 || W.frames == 120) {
            W.logged_at = W.frames;
            VLOG("antialiasing: FSR 3.1 %ux%u -> %ux%u, %llu frames, %llu without FSR, %llu jitter holds", W.sw, W.sh, ow, oh,
                 (unsigned long long)W.frames, (unsigned long long)W.missed, (unsigned long long)W.holds);
            vkb_tsr_obj_log();
        }
    }
}

// ---------------------------------------------------------------- recording: the jitter
// T.on: the scene's passes (jittered when T.jit, and drawn with the object motion target); T.frame: whose blocks
static __thread struct { int ev; float jx, jy; const void *mem; int on, jit; uint32_t frame, sw, sh; } T;
typedef struct { float jx, jy; const void *mem; int on, jit; uint32_t frame, sw, sh; } tsr_chunk_state;
_Static_assert(sizeof(tsr_chunk_state) <= 48, "the chunk keeps 48 bytes of it (backend_vk.c mt_chunk.tsr)");
size_t vkb_tsr_state_size(void) { return sizeof(tsr_chunk_state); }
void vkb_tsr_state_save(void *dst) {   // the walk, at a chunk's start
    tsr_chunk_state s = {W.jx, W.jy, W.depth_mem, W.phase == PH_SCENE || W.phase == PH_ARMED, W.jit, (uint32_t)W.frames, W.sw, W.sh};
    memcpy(dst, &s, sizeof s);
}
void vkb_tsr_sub_begin(void) {   // a new submission: its positions start again (the translation thread)
    atomic_store_explicit(&g_nev, 0, memory_order_relaxed);
    g_njob = 0;
    T.ev = 0;
}
void vkb_tsr_rec_begin(const void *state) {   // a recording thread starts a chunk at the current position (NULL: one thread)
    T.ev = 0;
    int n = atomic_load_explicit(&g_nev, memory_order_acquire);
    while (T.ev < n && g_ev[T.ev].seq < vkb_seq()) T.ev++;
    if (state) {
        tsr_chunk_state s;
        memcpy(&s, state, sizeof s);
        T.jx = s.jx, T.jy = s.jy, T.mem = s.mem, T.on = s.on, T.jit = s.jit, T.frame = s.frame, T.sw = s.sw, T.sh = s.sh;
    }
}
static const tsr_event *ev_here(int kind) {   // an event of this kind at the current position
    int n = atomic_load_explicit(&g_nev, memory_order_acquire);
    uint64_t seq = vkb_seq();
    while (T.ev < n && g_ev[T.ev].seq < seq) T.ev++;
    for (int i = T.ev; i < n && g_ev[i].seq == seq; i++)
        if (g_ev[i].kind == kind) return &g_ev[i];
    return NULL;
}
static void obj_clear(uint32_t w, uint32_t h);
static void obj_prepare(uint32_t w, uint32_t h, int new_only);
VkImageView vkb_tsr_obj_view(void);
void vkb_tsr_rec_rt(void) {   // after a render target change
    if (!atomic_load_explicit(&g_nev, memory_order_relaxed)) return;
    const tsr_event *e = ev_here(EV_JIT_ON);
    if (e) {
        T.on = 1, T.jit = e->jit, T.jx = e->jx, T.jy = e->jy, T.mem = e->depth_mem, T.sw = e->sw, T.sh = e->sh;
        if (e->frame != T.frame) T.frame = e->frame, obj_clear(e->sw, e->sh);   // a new frame: no object moved yet
    }
    if (ev_here(EV_JIT_OFF)) T.on = 0;
}
int vkb_tsr_jitter(const p_set_rt *rt, float *jx, float *jy) {   // the viewport offset of a draw into these targets
    if (!T.on || !T.jit || !rt->depth || rt->depth->mem != T.mem) return 0;
    *jx = T.jx, *jy = T.jy;
    return 1;
}

// ---------------------------------------------------------------- FSR 3.1 on the GPU
enum {
    R_MV, R_DIL_MV, R_DIL_DEPTH, R_RECON, R_FP16X1, R_LUMA1, R_LUMA2, R_SPD, R_FAR1, R_SHADE, R_REACT, R_LOCKS,
    R_ACC1, R_ACC2, R_LH1, R_LH2, R_UP1, R_UP2, R_ATOMIC, R_INFO, R_ZERO, R_EXPO, R_OUT, R_COUNT
};
typedef vkb_gimg gimg;
enum { NSETS = 2, NPASS = FSR3_NPASS + 1 };   // + the motion pass
static struct {
    int tried, ok;
    VkDescriptorSetLayout dsl[NPASS];
    VkPipelineLayout pl[NPASS];
    VkShaderEXT sh[NPASS];
    VkDescriptorPool pool;
    VkDescriptorSet set[VKB_SLOTS][NSETS][NPASS];
    int set_next[VKB_SLOTS];
    uint64_t set_sub[VKB_SLOTS];
    VkSampler point, linear;
    gimg r[R_COUNT];
    uint32_t rw, rh, uw, uh;   // the sizes the images are made for
    int first;                  // the next run is the first on these images
    uint32_t frame;             // resource frame index (odd / even)
    float prev_jx, prev_jy;
    int32_t prev_rw, prev_rh, prev_uw, prev_uh;
    float phase_count, frame_index;
    pthread_mutex_t mu;
} F = {.mu = PTHREAD_MUTEX_INITIALIZER};

static uint32_t memtype(uint32_t bits) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return UINT32_MAX;
}

static int img_make(gimg *g, VkFormat f, uint32_t w, uint32_t h, uint32_t levels) {
    memset(g, 0, sizeof *g);
    g->f = f, g->w = w ? w : 1, g->h = h ? h : 1, g->levels = levels;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {g->w, g->h, 1}, .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.vkCreateImage(vkc.device, &ii, NULL, &g->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vk.vkGetImageMemoryRequirements(vkc.device, g->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits)};
    vkb_mtag = MT_IMAGE;
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &g->mem) != VK_SUCCESS) return 0;
    if (vk.vkBindImageMemory(vkc.device, g->img, g->mem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g->img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    if (vk.vkCreateImageView(vkc.device, &vi, NULL, &g->view) != VK_SUCCESS) return 0;
    for (uint32_t l = 0; l < levels && l < 6; l++) {
        vi.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, l, 1, 0, 1};
        if (vk.vkCreateImageView(vkc.device, &vi, NULL, &g->mip[l]) != VK_SUCCESS) return 0;
    }
    return 1;
}
static void img_free(gimg *g) {
    if (!g->img) return;
    for (int l = 0; l < 6; l++)
        if (g->mip[l]) vkb_destroy_later(g->mip[l], VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkb_destroy_later(g->view, g->img, g->mem);
    memset(g, 0, sizeof *g);
}
int vkb_gimg_make(gimg *g, VkFormat f, uint32_t w, uint32_t h, uint32_t levels) { return img_make(g, f, w, h, levels); }
void vkb_gimg_free(gimg *g) { img_free(g); }

static const fsr3_pass_code *g_passes = fsr3_passes;   // fsr3_passes_h where the GPU does half maths (pipelines)

static int pipelines(void) {
    {   // the SDK's 16-bit permutation where the GPU takes it (shaderFloat16, shaderInt16): faster on AMD (the Steam Deck)
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f2);
        const char *e = getenv("TOTK_AA_FP16");
        int half = f12.shaderFloat16 && f2.features.shaderInt16 && !(e && *e == '0');
        g_passes = half ? fsr3_passes_h : fsr3_passes;
        VLOG("antialiasing: FSR 3.1 with %s maths", half ? "16-bit" : "32-bit");
    }
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0}, {VK_DESCRIPTOR_TYPE_SAMPLER, 0}};
    for (int p = 0; p < NPASS; p++) {
        VkDescriptorSetLayoutBinding b[24];
        uint32_t nb = 0;
        const uint32_t *code;
        size_t size;
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
        if (p < FSR3_NPASS) {
            const fsr3_pass_code *pc = &g_passes[p];
            for (uint32_t i = 0; i < pc->nb && nb < 24; i++) {
                static const VkDescriptorType kt[] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER};
                b[nb++] = (VkDescriptorSetLayoutBinding){pc->b[i].binding, kt[pc->b[i].kind], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
                ps[pc->b[i].kind].descriptorCount += VKB_SLOTS * NSETS;
            }
            code = pc->code, size = pc->size;
        } else {
            b[nb++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            b[nb++] = (VkDescriptorSetLayoutBinding){1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            b[nb++] = (VkDescriptorSetLayoutBinding){2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            ps[0].descriptorCount += 2 * VKB_SLOTS * NSETS, ps[1].descriptorCount += VKB_SLOTS * NSETS;
            code = tsr_mv_spv, size = sizeof tsr_mv_spv;
        }
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b};
        if (vk.vkCreateDescriptorSetLayout(vkc.device, &dci, NULL, &F.dsl[p]) != VK_SUCCESS) return 0;
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &F.dsl[p],
                                         .pushConstantRangeCount = p == FSR3_NPASS, .pPushConstantRanges = &pr};
        if (vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &F.pl[p]) != VK_SUCCESS) return 0;
        VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                    .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = size, .pCode = code, .pName = "main",
                                    .setLayoutCount = 1, .pSetLayouts = &F.dsl[p], .pushConstantRangeCount = p == FSR3_NPASS,
                                    .pPushConstantRanges = &pr};
        if (vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, &F.sh[p]) != VK_SUCCESS) {
            VLOG("antialiasing: the %s pass could not be made", p < FSR3_NPASS ? g_passes[p].name : "motion");
            return 0;
        }
    }
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = VKB_SLOTS * NSETS * NPASS,
                                     .poolSizeCount = 4, .pPoolSizes = ps};
    if (vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &F.pool) != VK_SUCCESS) return 0;
    for (int s = 0; s < VKB_SLOTS; s++)
        for (int k = 0; k < NSETS; k++) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = F.pool,
                                              .descriptorSetCount = NPASS, .pSetLayouts = F.dsl};
            if (vk.vkAllocateDescriptorSets(vkc.device, &ai, F.set[s][k]) != VK_SUCCESS) return 0;
        }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .maxLod = VK_LOD_CLAMP_NONE};
    if (vk.vkCreateSampler(vkc.device, &si, NULL, &F.point) != VK_SUCCESS) return 0;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR, si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    return vk.vkCreateSampler(vkc.device, &si, NULL, &F.linear) == VK_SUCCESS;
}

static int images(uint32_t rw, uint32_t rh, uint32_t uw, uint32_t uh) {
    if (F.rw == rw && F.rh == rh && F.uw == uw && F.uh == uh && F.r[R_OUT].img) return 1;
    for (int i = 0; i < R_COUNT; i++) img_free(&F.r[i]);
    uint32_t hw = rw / 2, hh = rh / 2, m = hw > hh ? hw : hh, levels = 1;
    while ((m >>= 1) && levels < 13) levels++;
    static const struct { int id; VkFormat f; int size; } T_[] = {   // size: 0 render, 1 half render, 2 upscale, 3 one texel
        {R_MV, VK_FORMAT_R16G16_SFLOAT, 0}, {R_DIL_MV, VK_FORMAT_R16G16_SFLOAT, 0}, {R_DIL_DEPTH, VK_FORMAT_R32_SFLOAT, 0},
        {R_RECON, VK_FORMAT_R32_UINT, 0}, {R_FP16X1, VK_FORMAT_R16_SFLOAT, 0}, {R_LUMA1, VK_FORMAT_R16_SFLOAT, 0},
        {R_LUMA2, VK_FORMAT_R16_SFLOAT, 0}, {R_SPD, VK_FORMAT_R16G16_SFLOAT, 1}, {R_FAR1, VK_FORMAT_R16_SFLOAT, 1},
        {R_SHADE, VK_FORMAT_R8_UNORM, 1}, {R_REACT, VK_FORMAT_R8G8B8A8_UNORM, 0}, {R_LOCKS, VK_FORMAT_R8_UNORM, 2},
        {R_ACC1, VK_FORMAT_R8_UNORM, 0}, {R_ACC2, VK_FORMAT_R8_UNORM, 0}, {R_LH1, VK_FORMAT_R8G8B8A8_UNORM, 0},
        {R_LH2, VK_FORMAT_R8G8B8A8_UNORM, 0}, {R_UP1, VK_FORMAT_R16G16B16A16_SFLOAT, 2}, {R_UP2, VK_FORMAT_R16G16B16A16_SFLOAT, 2},
        {R_ATOMIC, VK_FORMAT_R32_UINT, 3}, {R_INFO, VK_FORMAT_R32G32B32A32_SFLOAT, 3}, {R_ZERO, VK_FORMAT_R8_UNORM, 3},
        {R_EXPO, VK_FORMAT_R32G32_SFLOAT, 3}, {R_OUT, VK_FORMAT_R16G16B16A16_SFLOAT, 2}};
    for (size_t i = 0; i < sizeof T_ / sizeof *T_; i++) {
        uint32_t w = T_[i].size == 0 ? rw : T_[i].size == 1 ? hw : T_[i].size == 2 ? uw : 1;
        uint32_t h = T_[i].size == 0 ? rh : T_[i].size == 1 ? hh : T_[i].size == 2 ? uh : 1;
        if (!img_make(&F.r[T_[i].id], T_[i].f, w, h, T_[i].id == R_SPD ? levels : 1)) {
            VLOG("antialiasing: FSR 3.1's images (%ux%u -> %ux%u) could not be made", rw, rh, uw, uh);
            return 0;
        }
    }
    F.rw = rw, F.rh = rh, F.uw = uw, F.uh = uh, F.first = 1;
    VLOG("antialiasing: FSR 3.1 images made for %ux%u -> %ux%u", rw, rh, uw, uh);
    return 1;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = dst, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &di);
}
static void to_general(VkCommandBuffer cb) {   // the images' first use: undefined to GENERAL (kept so)
    VkImageMemoryBarrier2 ib[R_COUNT];
    for (int i = 0; i < R_COUNT; i++)
        ib[i] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = F.r[i].img,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = R_COUNT, .pImageMemoryBarriers = ib};
    vk.vkCmdPipelineBarrier2(cb, &di);
}
static void clear(VkCommandBuffer cb, int id, float a, float b, float c, float d) {
    VkClearColorValue v;
    if (F.r[id].f == VK_FORMAT_R32_UINT) v.uint32[0] = (uint32_t)a, v.uint32[1] = v.uint32[2] = v.uint32[3] = 0;
    else v.float32[0] = a, v.float32[1] = b, v.float32[2] = c, v.float32[3] = d;
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vk.vkCmdClearColorImage(cb, F.r[id].img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
}

typedef struct {   // cbFSR3UPSCALER (std140, as the SDK's Fsr3UpscalerConstants)
    int32_t renderSize[2], previousFrameRenderSize[2], upscaleSize[2], previousFrameUpscaleSize[2], maxRenderSize[2], maxUpscaleSize[2];
    float deviceToViewDepth[4], jitterOffset[2], previousFrameJitterOffset[2], motionVectorScale[2], downscaleFactor[2];
    float motionVectorJitterCancellation[2], tanHalfFOV, jitterPhaseCount;
    float deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex;
    float velocityFactor, reactivenessScale, shadingChangeScale, accumulationAddedPerFrame, minDisocclusionAccumulation;
} fsr3_consts;

typedef struct { VkImageView color, depth; int odd; VkDescriptorBufferInfo cb[3]; } bind_ctx;   // cb: main, SPD, RCAS
static int resource_of(const char *n, const bind_ctx *x, VkImageView *view) {   // the SDK's binding names to our images
    int odd = x->odd;
    static const struct { const char *name; int id; } fixed[] = {
        {"r_input_motion_vectors", R_MV}, {"rw_dilated_motion_vectors", R_DIL_MV}, {"r_dilated_motion_vectors", R_DIL_MV},
        {"rw_dilated_depth", R_DIL_DEPTH}, {"r_dilated_depth", R_DIL_DEPTH},
        {"rw_reconstructed_previous_nearest_depth", R_RECON}, {"r_reconstructed_previous_nearest_depth", R_RECON},
        {"rw_farthest_depth", R_FP16X1}, {"r_farthest_depth", R_FP16X1}, {"rw_luma_instability", R_FP16X1},
        {"r_luma_instability", R_FP16X1}, {"rw_spd_global_atomic", R_ATOMIC}, {"rw_frame_info", R_INFO}, {"r_frame_info", R_INFO},
        {"r_spd_mips", R_SPD}, {"rw_farthest_depth_mip1", R_FAR1}, {"r_farthest_depth_mip1", R_FAR1},
        {"r_input_exposure", R_EXPO}, {"r_reactive_mask", R_ZERO}, {"r_transparency_and_composition_mask", R_ZERO},
        {"r_lanczos_lut", R_ZERO}, {"rw_shading_change", R_SHADE}, {"r_shading_change", R_SHADE},
        {"rw_dilated_reactive_masks", R_REACT}, {"r_dilated_reactive_masks", R_REACT}, {"rw_new_locks", R_LOCKS},
        {"rw_upscaled_output", R_OUT}};
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (!strcmp(n, fixed[i].name)) { *view = F.r[fixed[i].id].view; return 1; }
    if (!strncmp(n, "rw_spd_mip", 10)) { *view = F.r[R_SPD].mip[n[10] - '0']; return 1; }
    int id = -1;
    if (!strcmp(n, "r_input_color_jittered")) { *view = x->color; return 1; }
    if (!strcmp(n, "r_input_depth")) { *view = x->depth; return 1; }
    if (!strcmp(n, "rw_current_luma") || !strcmp(n, "r_current_luma")) id = odd ? R_LUMA2 : R_LUMA1;
    else if (!strcmp(n, "r_previous_luma")) id = odd ? R_LUMA1 : R_LUMA2;
    else if (!strcmp(n, "r_accumulation")) id = odd ? R_ACC2 : R_ACC1;
    else if (!strcmp(n, "rw_accumulation")) id = odd ? R_ACC1 : R_ACC2;
    else if (!strcmp(n, "r_internal_upscaled_color")) id = odd ? R_UP2 : R_UP1;
    else if (!strcmp(n, "rw_internal_upscaled_color") || !strcmp(n, "r_rcas_input")) id = odd ? R_UP1 : R_UP2;
    else if (!strcmp(n, "r_luma_history")) id = odd ? R_LH2 : R_LH1;
    else if (!strcmp(n, "rw_luma_history")) id = odd ? R_LH1 : R_LH2;
    if (id < 0) return 0;
    *view = F.r[id].view;
    return 1;
}

static void dispatch(VkCommandBuffer cb, VkDescriptorSet set, int p, const bind_ctx *x, uint32_t gx, uint32_t gy) {
    const fsr3_pass_code *pc = &g_passes[p];
    VkWriteDescriptorSet w[24];
    VkDescriptorImageInfo ii[24];
    VkDescriptorBufferInfo bi[24];
    uint32_t n = 0;
    for (uint32_t i = 0; i < pc->nb; i++) {
        const fsr3_binding *b = &pc->b[i];
        w[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b->binding, .descriptorCount = 1};
        if (b->kind == FSR3_CB) {
            int k = !strcmp(b->name, "cbSPD") ? 1 : !strcmp(b->name, "cbRCAS") ? 2 : 0;
            bi[n] = x->cb[k];
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w[n].pBufferInfo = &bi[n];
        } else if (b->kind == FSR3_SAMPLER) {
            ii[n] = (VkDescriptorImageInfo){!strcmp(b->name, "s_PointClamp") ? F.point : F.linear, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, w[n].pImageInfo = &ii[n];
        } else {
            VkImageView v = VK_NULL_HANDLE;
            if (!resource_of(b->name, x, &v) || !v) {
                VLOG("antialiasing: FSR 3.1 pass %s: no image for %s", pc->name, b->name);
                v = F.r[R_ZERO].view;
            }
            ii[n] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
            w[n].descriptorType = b->kind == FSR3_SRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[n].pImageInfo = &ii[n];
        }
        n++;
    }
    vk.vkUpdateDescriptorSets(vkc.device, n, w, 0, NULL);
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
    vk.vkCmdBindShadersEXT(cb, 1, &st, &F.sh[p]);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.pl[p], 0, 1, &set, 0, NULL);
    vk.vkCmdDispatch(cb, gx, gy, 1);
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

static VkDescriptorBufferInfo upload(const void *data, size_t n) {
    VkBuffer buf;
    VkDeviceSize off;
    void *p = vkb_ring_alloc(256, vkc.props.limits.minUniformBufferOffsetAlignment, &buf, &off);
    memset(p, 0, 256);
    memcpy(p, data, n);
    return (VkDescriptorBufferInfo){buf, off, 256};
}

// FSR 3.1 for one frame on the current command buffer (outside a pass): into the target of the pass being drawn.
static int run(const fsr_job *j, vkb_image *target) {
    pthread_mutex_lock(&F.mu);
    int ok = 0;
    if (!F.tried) {
        F.tried = 1;
        F.ok = pipelines();
        if (!F.ok) VLOG("antialiasing: FSR 3.1 could not be set up: the game's own scaling stays");
    }
    if (!F.ok) goto out;
    vkb_image *ci = vkb_image_of(&j->color), *zi = vkb_image_of(&j->depth);
    if (!ci || !zi || !target) goto out;
    {   // the copy into the pass's target is a blit: its format must take one
        VkFormatProperties fp;
        vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, target->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            VLOG("antialiasing: the output target's format %d takes no blit: FSR 3.1 off for this run", (int)target->format);
            F.ok = 0;
            goto out;
        }
    }
    uint32_t rw = ci->width, rh = ci->height, uw = target->width, uh = target->height;
    if (!images(rw, rh, uw, uh)) { F.ok = 0; goto out; }
    VkCommandBuffer cb = vkb_cmd();
    VkImageView cv = vkb_view(ci, NULL, 0, VK_IMAGE_VIEW_TYPE_2D), zv = vkb_view(zi, NULL, 0, VK_IMAGE_VIEW_TYPE_2D);
    if (!cv || !zv) goto out;
    vkb_image_use(cb, ci, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    vkb_image_use(cb, zi, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    int slot = vkb_slot();
    if (F.set_sub[slot] != mtl_last_fence()) F.set_sub[slot] = mtl_last_fence(), F.set_next[slot] = 0;
    if (F.set_next[slot] >= NSETS) goto out;   // a third FSR in one submission: not drawn
    VkDescriptorSet *sets = F.set[slot][F.set_next[slot]++];
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    int reset = j->reset || F.first;
    if (F.first) {
        to_general(cb);
        for (int i = 0; i < R_COUNT; i++) clear(cb, i, 0, 0, 0, 0);
    }
    int odd = F.frame & 1;
    if (reset) {
        clear(cb, odd ? R_ACC2 : R_ACC1, 0, 0, 0, 0);
        clear(cb, R_INFO, -1, 1, 0, 0);
    }
    {   // the far plane: depth 1.0, stored as its bits (the shader takes atomic minima of asuint(depth))
        float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, 4);
        clear(cb, R_RECON, (float)bits, 0, 0, 0);
    }
    clear(cb, R_ATOMIC, 0, 0, 0, 0);
    clear(cb, R_SPD, 0, 0, 0, 0);
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    // the camera's motion vectors
    {
        obj_prepare(rw, rh, 1);   // the object motion target, cleared when it is new (no objects drawn into it then)
        VkDescriptorImageInfo di = {VK_NULL_HANDLE, zv, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, F.r[R_MV].view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo bi2 = {VK_NULL_HANDLE, vkb_tsr_obj_view(), VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[3] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[FSR3_NPASS], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &di},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[FSR3_NPASS], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[FSR3_NPASS], .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &bi2}};
        if (!bi2.imageView) { VLOG("antialiasing: no object motion target: FSR 3.1 off for this run"); goto out; }
        vk.vkUpdateDescriptorSets(vkc.device, 3, w, 0, NULL);
        struct { float m[16], size[2], jit[2]; } pcs;
        if (j->have_m) memcpy(pcs.m, j->m, sizeof pcs.m);
        else for (int i = 0; i < 16; i++) pcs.m[i] = i % 5 == 0;   // no last frame: zero motion
        pcs.size[0] = (float)rw, pcs.size[1] = (float)rh, pcs.jit[0] = j->jx, pcs.jit[1] = j->jy;
        VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
        vk.vkCmdBindShadersEXT(cb, 1, &st, &F.sh[FSR3_NPASS]);
        vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.pl[FSR3_NPASS], 0, 1, &sets[FSR3_NPASS], 0, NULL);
        vk.vkCmdPushConstants(cb, F.pl[FSR3_NPASS], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcs, &pcs);
        vk.vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
        barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    // the constants (ffx_fsr3upscaler.cpp fsr3upscalerDispatch)
    fsr3_consts k;
    memset(&k, 0, sizeof k);
    k.renderSize[0] = (int32_t)rw, k.renderSize[1] = (int32_t)rh;
    k.previousFrameRenderSize[0] = F.prev_rw ? F.prev_rw : (int32_t)rw, k.previousFrameRenderSize[1] = F.prev_rh ? F.prev_rh : (int32_t)rh;
    k.upscaleSize[0] = (int32_t)uw, k.upscaleSize[1] = (int32_t)uh;
    k.previousFrameUpscaleSize[0] = F.prev_uw ? F.prev_uw : (int32_t)uw, k.previousFrameUpscaleSize[1] = F.prev_uh ? F.prev_uh : (int32_t)uh;
    k.maxRenderSize[0] = (int32_t)rw, k.maxRenderSize[1] = (int32_t)rh, k.maxUpscaleSize[0] = (int32_t)uw, k.maxUpscaleSize[1] = (int32_t)uh;
    {   // device depth to view depth: not inverted, not infinite (setupDeviceDepthToViewSpaceDepthParams); TotK draws
        // with LEQUAL and the depth range 0..1 (logged once by vkb_draw.c)
        float fmin = j->near_, fmax = j->far_;
        float q = fmax / (fmin - fmax);
        k.deviceToViewDepth[0] = -1.0f * q;
        k.deviceToViewDepth[1] = q * fmin;
        float aspect = (float)rw / (float)rh, cot = cosf(0.5f * j->fov) / sinf(0.5f * j->fov);
        k.deviceToViewDepth[2] = 1.0f / (cot / aspect), k.deviceToViewDepth[3] = 1.0f / cot;
        float hfov = 2.0f * atanf(tanf(j->fov / 2) * aspect);
        k.tanHalfFOV = tanf(hfov * 0.5f);
    }
    k.jitterOffset[0] = j->jx, k.jitterOffset[1] = j->jy;
    k.previousFrameJitterOffset[0] = F.prev_jx, k.previousFrameJitterOffset[1] = F.prev_jy;
    k.motionVectorScale[0] = 1.0f / (float)rw, k.motionVectorScale[1] = 1.0f / (float)rh;   // vectors in render pixels
    k.downscaleFactor[0] = (float)rw / (float)uw, k.downscaleFactor[1] = (float)rh / (float)uh;
    {
        int phases = (int)(8.0f * powf((float)uw / (float)rw, 2.0f));
        if (reset || F.phase_count == 0) F.phase_count = (float)phases;
        else if (phases > F.phase_count) F.phase_count++;
        else if (phases < F.phase_count) F.phase_count--;
        k.jitterPhaseCount = F.phase_count;
    }
    k.deltaTime = j->dt_ms / 1000.0f;
    if (k.deltaTime > 1) k.deltaTime = 1;
    k.deltaPreExposure = 1.0f;
    k.viewSpaceToMetersFactor = 1.0f;
    F.frame_index = reset ? 0 : F.frame_index + 1;
    k.frameIndex = F.frame_index;
    k.velocityFactor = 1.0f, k.reactivenessScale = 1.0f, k.shadingChangeScale = 1.0f;
    k.accumulationAddedPerFrame = 1.0f / 3.0f, k.minDisocclusionAccumulation = -1.0f / 3.0f;
    struct { uint32_t mips, numWorkGroups, workGroupOffset[2], renderSize[2]; } spd;
    uint32_t gx = (rw - 1) / 64 + 1, gy = (rh - 1) / 64 + 1, res = rw > rh ? rw : rh;
    spd.workGroupOffset[0] = spd.workGroupOffset[1] = 0;
    spd.numWorkGroups = gx * gy;
    spd.mips = (uint32_t)fminf(floorf(log2f((float)res)), 12.0f);
    spd.renderSize[0] = rw, spd.renderSize[1] = rh;
    struct { uint32_t c[4]; } rcas;
    {   // FsrRcasCon with the SDK's remap of sharpness 0..1 to stops 2..0
        float s = exp2f(-((-2.0f * g_sharp) + 2.0f));
        memcpy(&rcas.c[0], &s, 4);
        uint16_t h;   // half of s, packed twice
        {
            uint32_t b;
            memcpy(&b, &s, 4);
            uint32_t sign = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, man = b & 0x7fffff;
            h = e < 113 ? (uint16_t)sign : e > 142 ? (uint16_t)(sign | 0x7c00) : (uint16_t)(sign | ((e - 112) << 10) | (man >> 13));
        }
        rcas.c[1] = (uint32_t)h | (uint32_t)h << 16, rcas.c[2] = rcas.c[3] = 0;
    }
    bind_ctx x = {cv, zv, odd, {upload(&k, sizeof k), upload(&spd, sizeof spd), upload(&rcas, sizeof rcas)}};
    uint32_t sx = (rw + 7) / 8, sy = (rh + 7) / 8, dx = (uw + 7) / 8, dy = (uh + 7) / 8;
    uint32_t chx = ((uint32_t)(rw * 0.5f) + 7) / 8, chy = ((uint32_t)(rh * 0.5f) + 7) / 8;
    int sharpen = g_sharp > 0.0f;
    dispatch(cb, sets[FSR3_P_PREPARE_INPUTS], FSR3_P_PREPARE_INPUTS, &x, sx, sy);
    dispatch(cb, sets[FSR3_P_LUMA_PYRAMID], FSR3_P_LUMA_PYRAMID, &x, gx, gy);
    dispatch(cb, sets[FSR3_P_SHADING_CHANGE_PYRAMID], FSR3_P_SHADING_CHANGE_PYRAMID, &x, gx, gy);
    dispatch(cb, sets[FSR3_P_SHADING_CHANGE], FSR3_P_SHADING_CHANGE, &x, chx, chy);
    dispatch(cb, sets[FSR3_P_PREPARE_REACTIVITY], FSR3_P_PREPARE_REACTIVITY, &x, sx, sy);
    dispatch(cb, sets[FSR3_P_LUMA_INSTABILITY], FSR3_P_LUMA_INSTABILITY, &x, sx, sy);
    dispatch(cb, sets[sharpen ? FSR3_P_ACCUMULATE_SHARPEN : FSR3_P_ACCUMULATE], sharpen ? FSR3_P_ACCUMULATE_SHARPEN : FSR3_P_ACCUMULATE, &x, dx, dy);
    if (sharpen) dispatch(cb, sets[FSR3_P_RCAS], FSR3_P_RCAS, &x, (uw + 15) / 16, (uh + 15) / 16);
    // the picture into the pass's target (the HUD draws over it)
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    vkb_image_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    static int mvdebug = -1;   // TOTK_AA_MVDEBUG=1 (development): the motion vectors in place of the picture
    if (mvdebug < 0) { const char *e = getenv("TOTK_AA_MVDEBUG"); mvdebug = e && *e == '1'; }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)uw, (int32_t)uh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)uw, (int32_t)uh, 1}}};
    if (mvdebug) bl.srcOffsets[1] = (VkOffset3D){(int32_t)rw, (int32_t)rh, 1};
    vk.vkCmdBlitImage(cb, F.r[mvdebug ? R_MV : R_OUT].img, VK_IMAGE_LAYOUT_GENERAL, target->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                      VK_FILTER_NEAREST);
    vkb_image_written(target);
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    F.prev_jx = j->jx, F.prev_jy = j->jy;
    F.prev_rw = (int32_t)rw, F.prev_rh = (int32_t)rh, F.prev_uw = (int32_t)uw, F.prev_uh = (int32_t)uh;
    F.frame = (F.frame + 1) % 16;
    F.first = 0;
    ok = 1;
out:
    pthread_mutex_unlock(&F.mu);
    return ok;
}

// A draw being recorded: 1 when it is the EASU draw FSR replaced (FSR ran; the draw is skipped).
int vkb_tsr_rec_draw(const p_set_rt *rt) {   // 1: FSR ran in place of this draw; 2: the FXAA draw (vkb_draw.c copies)
    if (!atomic_load_explicit(&g_nev, memory_order_relaxed)) return 0;
    if (ev_here(EV_FXAA)) return 2;
    const tsr_event *e = ev_here(EV_FSR);
    if (!e || e->job < 0 || rt->n < 1 || !rt->color[0]) return 0;
    vkb_image *target = vkb_image_of(rt->color[0]);
    vkb_pass_end_r(PE_OTHER);
    int ok = run(&g_job[e->job], target);
    vkb_rec_begin();   // our shaders and sets were bound: the replay binds everything again
    if (!ok && !F.ok && F.tried) g_mode = 0;   // FSR 3.1 cannot run here: no more jitter either
    return ok;
}

// ---------------------------------------------------------------- per-object motion
// The camera's motion is right for what holds still; an object that moves on its own (Link, creatures, what they carry,
// moving props) gets its own: right after its draw in a G-buffer pass it is drawn again (vkb_draw.c obj_motion_draw)
// with its vertex shader's last-frame position variant (made by the translator, Transforms/PreviousPosition.cs, bound
// with last frame's blocks of the banks the position reads) and tsr_obj.frag, into the object motion target this file
// adds to the scene's G-buffer passes at a target slot they leave free (backend_vk.c pass_begin). Only objects whose
// blocks changed are drawn again.
// Last frame's blocks: the walk keeps a copy of every block of banks c4..c7 the scene's draws bind, as of the submit
// call, in an arena per frame (four in turn: the recording of a frame reads the one of the frame before while the
// walk fills the next). A block of this frame is found last frame at the same address (an object the game did not
// write again) or one step away: the game writes an object's blocks into two copies that take turns, the second right
// after the first, a step of the block's size rounded up to 256 bytes apart.
// Both neighbours there (copies of adjacent objects interleave): the one more like this frame's.
// TOTK_AA_OBJMV=0: the camera's motion only.
enum { ARENAS = 4, BLOCK_MAX = 16384, ATAB = 1 << 14 };
typedef struct { uint64_t addr; uint32_t off, size; uint8_t bank, used; } ablk;
typedef struct { uint64_t frame; ablk tab[ATAB]; uint32_t n; uint8_t *data; size_t used, cap; } arena;
static arena *g_arena[ARENAS];
static _Atomic uint64_t g_obj_draws, g_obj_still, g_obj_noprev;
static uint64_t g_kept_blocks, g_kept_bytes;

static int objmv_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_AA_OBJMV"); v = !(e && *e == '0'); }
    return v && vkb_tsr_on();
}

static uint32_t ahash(int bank, uint64_t addr) {
    return (uint32_t)((((addr >> 4) ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull) >> 50);
}

static void arena_keep(int bank, uint64_t addr, uint64_t size) {   // the walk: a block this frame's scene binds
    if (!objmv_on()) return;
    arena **pa = &g_arena[W.frames % ARENAS];
    if (!*pa && !(*pa = calloc(1, sizeof **pa))) return;
    arena *a = *pa;
    if (a->frame != W.frames) a->frame = W.frames, a->n = 0, a->used = 0, memset(a->tab, 0, sizeof a->tab);
    if (a->n >= ATAB / 2) return;
    uint32_t n = size > BLOCK_MAX ? BLOCK_MAX : (uint32_t)size;
    for (uint32_t h = ahash(bank, addr);; h = (h + 1) & (ATAB - 1)) {
        ablk *b = &a->tab[h];
        if (b->used && b->addr == addr && b->bank == bank) return;   // kept already
        if (b->used) continue;
        if (a->used + n > a->cap) {
            size_t cap = a->cap ? a->cap * 2 : (size_t)1 << 20;
            while (cap < a->used + n) cap *= 2;
            uint8_t *d = realloc(a->data, cap);
            if (!d) return;
            a->data = d, a->cap = cap;
        }
        vkb_ucap_read(addr, a->data + a->used, n);
        *b = (ablk){addr, (uint32_t)a->used, n, (uint8_t)bank, 1};
        a->used += n, a->n++;
        g_kept_blocks++, g_kept_bytes += n;
        return;
    }
}

static const ablk *arena_find(const arena *a, int bank, uint64_t addr) {
    for (uint32_t h = ahash(bank, addr);; h = (h + 1) & (ATAB - 1)) {
        const ablk *b = &a->tab[h];
        if (!b->used) return NULL;
        if (b->addr == addr && b->bank == bank) return b;
    }
}

// Last frame's copy of a block this draw binds (cur: its bytes now, n of them): NULL when there is none (a new object).
const uint8_t *vkb_tsr_prev_block(int bank, uint64_t addr, const uint8_t *cur, uint32_t n) {
    if (!T.frame) return NULL;
    const arena *a = g_arena[(T.frame - 1) % ARENAS];
    if (!a || a->frame != T.frame - 1) return NULL;
    const ablk *self = arena_find(a, bank, addr);
    if (self && self->size >= n) return a->data + self->off;
    uint64_t step = ((uint64_t)n + 255) & ~255ull;
    const ablk *lo = addr >= step ? arena_find(a, bank, addr - step) : NULL, *hi = arena_find(a, bank, addr + step);
    if (lo && lo->size < n) lo = NULL;
    if (hi && hi->size < n) hi = NULL;
    if (lo && hi) {   // the one with more of its first 16 words equal to this frame's
        int el = 0, eh = 0;
        for (uint32_t w = 0; w < 16 && w * 4 + 4 <= n; w++) {
            el += !memcmp(a->data + lo->off + w * 4, cur + w * 4, 4);
            eh += !memcmp(a->data + hi->off + w * 4, cur + w * 4, 4);
        }
        return a->data + (eh > el ? hi : lo)->off;
    }
    return lo ? a->data + lo->off : hi ? a->data + hi->off : NULL;
}

void vkb_tsr_obj_count(int drawn, int found_prev) {   // statistics (the log line)
    atomic_fetch_add_explicit(drawn ? &g_obj_draws : found_prev ? &g_obj_still : &g_obj_noprev, 1, memory_order_relaxed);
}

// The object motion target: RGBA32F at the scene size, GENERAL; cleared at each frame's first scene pass.
static struct { VkImage img; VkDeviceMemory mem; VkImageView view; uint32_t w, h; pthread_mutex_t mu; VkShaderEXT fs[32][8]; } O = {
    .mu = PTHREAD_MUTEX_INITIALIZER};
static uint32_t memtype(uint32_t bits);
static int obj_image(uint32_t w, uint32_t h) {   // O.mu held; 2: made now (UNDEFINED layout)
    if (O.img && O.w == w && O.h == h) return 1;
    if (O.img) vkb_destroy_later(O.view, O.img, O.mem), O.img = VK_NULL_HANDLE, O.view = VK_NULL_HANDLE, O.w = O.h = 0;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32G32B32A32_SFLOAT,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.vkCreateImage(vkc.device, &ii, NULL, &O.img) != VK_SUCCESS) return O.img = VK_NULL_HANDLE, 0;
    VkMemoryRequirements mr;
    vk.vkGetImageMemoryRequirements(vkc.device, O.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits)};
    vkb_mtag = MT_IMAGE;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = O.img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R32G32B32A32_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &O.mem) != VK_SUCCESS ||
        vk.vkBindImageMemory(vkc.device, O.img, O.mem, 0) != VK_SUCCESS || vk.vkCreateImageView(vkc.device, &vi, NULL, &O.view) != VK_SUCCESS) {
        VLOG("antialiasing: the object motion target could not be made");
        O.view = VK_NULL_HANDLE;
        return 0;
    }
    O.w = w, O.h = h;
    VLOG("antialiasing: object motion target made for %ux%u", w, h);
    return 2;
}
static void obj_clear(uint32_t w, uint32_t h) { if (objmv_on()) obj_prepare(w, h, 0); }
// The target cleared (recording, outside a pass): at a frame's first scene pass, and once when made for FSR's motion
// pass (always: its texelFetch reads it, objects or not; new is 1 then: only a new image is cleared).
static void obj_prepare(uint32_t w, uint32_t h, int new_only) {
    if (!w || !h) return;
    pthread_mutex_lock(&O.mu);
    int made = obj_image(w, h);
    pthread_mutex_unlock(&O.mu);
    if (!made || (new_only && made != 2)) return;
    vkb_pass_end_r(PE_OTHER);
    VkCommandBuffer cb = vkb_cmd();
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = made == 2 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = O.img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vk.vkCmdPipelineBarrier2(cb, &di);
    VkClearColorValue z = {{0, 0, 0, 0}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cb, O.img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vk.vkCmdPipelineBarrier2(cb, &di);
}

// A pass being begun: the target slot of the object motion target in it, and its view (VK_NULL_HANDLE: none). The
// scene's G-buffer passes (three or more targets with the scene depth) of a frame whose target is cleared.
VkImageView vkb_tsr_obj_target(const p_set_rt *rt, int *slot) {
    *slot = -1;
    if (!T.on || !objmv_on() || rt->n < 3 || !rt->depth || rt->depth->mem != T.mem || !rt->color[0] ||
        rt->color[0]->d.width != T.sw || rt->color[0]->d.height != T.sh || O.w != T.sw || O.h != T.sh || !O.view)
        return VK_NULL_HANDLE;
    for (int i = 7; i >= 0 && *slot < 0; i--)
        if (i >= rt->n || !rt->color[i]) *slot = i;
    return *slot >= 0 ? O.view : VK_NULL_HANDLE;
}

// The fragment shader of an object motion draw: tsr_obj.frag with its input at the variant's output location and its
// output at the pass's free slot (the template's Location decorations 31 and 7 patched).
VkShaderEXT vkb_tsr_obj_fs(int in_loc, int out_slot) {
    if (in_loc < 0 || in_loc >= 32 || out_slot < 0 || out_slot >= 8) return VK_NULL_HANDLE;
    pthread_mutex_lock(&O.mu);
    VkShaderEXT s = O.fs[in_loc][out_slot];
    if (!s) {
        uint32_t code[sizeof tsr_obj_spv / 4];
        memcpy(code, tsr_obj_spv, sizeof code);
        for (size_t i = 5; i < sizeof code / 4;) {
            uint32_t op = code[i] & 0xffff, wc = code[i] >> 16;
            if (!wc) break;
            if (op == 71 && wc == 4 && code[i + 2] == 30) code[i + 3] = code[i + 3] == 31 ? (uint32_t)in_loc : (uint32_t)out_slot;
            i += wc;
        }
        s = O.fs[in_loc][out_slot] = vkb_shader_own_fragment(code, sizeof code);
    }
    pthread_mutex_unlock(&O.mu);
    return s;
}

VkImageView vkb_tsr_obj_view(void) { return O.view; }

void vkb_tsr_obj_log(void) {   // once in a while (vkb_tsr_walk_draw's line)
    VLOG("antialiasing: object motion: %llu draws again, %llu held still, %llu without last frame's blocks; kept %llu blocks, %.1f MiB",
         (unsigned long long)g_obj_draws, (unsigned long long)g_obj_still, (unsigned long long)g_obj_noprev,
         (unsigned long long)g_kept_blocks, g_kept_bytes / 1048576.0);
}
