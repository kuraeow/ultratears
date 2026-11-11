// Milestone 3.1: the NVN front end (MacNativeGames hle_nvn.c) without a GPU. Every command buffer is "executed" at
// once by doing nothing, fences complete immediately, presents only count. Plus the stand-ins for MacNativeGames'
// diagnostics (life log, timeline), MetalFX, virtual pools (not ported yet: stop with their name) and host glue.
// The Vulkan replayer replaces the mtl_* part (milestone 3.2/3.3).
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_obj.h"
#include "nvn/totk_vpool.h"

// ---------------- mtl_*: execution ----------------
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 1; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_pin_refs(void (*mark)(const void *)) { (void)mark; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 60; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
void mtl_note_world(void) {}
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }

// ---------------- mtl_*: statistics (all zero) ----------------
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *passes) { *load = *store = *passes = 0; }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
void mtl_pool_res_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = *draws = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_upload_wait_stats(uint64_t *n, uint64_t *ns) { *n = *ns = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }

// ---------------- MacNativeGames diagnostics: off ----------------
int totk_life_on(void) { return 0; }
void totk_life_log(const char *fmt, ...) { (void)fmt; }
void totk_life_reg(int id, const tex_t *t) { (void)id; (void)t; }
uint64_t totk_life_reg_serial(int id) { (void)id; return 0; }
int totk_life_match(const tex_t *t) { (void)t; return 0; }
void totk_life_cb_mark(const void *cb) { (void)cb; }
int totk_life_cb_take(const void *cb) { (void)cb; return 0; }
void totk_life_handle_add(uint64_t h) { (void)h; }
int totk_life_handle_has(uint64_t h) { (void)h; return 0; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
void runtime_watch_set(uintptr_t addr, size_t len) { (void)addr; (void)len; }
_Atomic int runtime_survey_pending;   // the world survey is not ported: never pending
void runtime_survey_tick(uint64_t frame) { (void)frame; }
const int32_t runtime_survey_sync[2] = {0, -1};

// ---------------- MetalFX (no equivalent here: off) ----------------
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }

// ---------------- host glue (MacNativeGames d2r_gfx/host, runtime/loader.c) ----------------
uint64_t g_t0_ns;
uint64_t g_draw_skip[16];
__thread sigjmp_buf *runtime_fault_guard;
uint64_t now_ns(void) { return runtime_now_ns(); }
uint64_t guest_ns(void) { return runtime_now_ns(); }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
uintptr_t runtime_frame_controller_return_pc(void) { return 0; }
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    for (int i = 0; i < g_nmodules; i++)
        if (pc >= g_modules[i].base && pc < g_modules[i].base + g_modules[i].size) {
            *name = g_modules[i].name; *off = pc - g_modules[i].base; return 1;
        }
    return 0;
}
void runtime_describe_address(uint64_t a, char *out, size_t n) {
    uint64_t off = 0;
    const char *s = loader_symbol_at(a, &off);
    if (s) snprintf(out, n, "%s+%#llx", s, (unsigned long long)off);
    else snprintf(out, n, "%#llx", (unsigned long long)a);
}
void nvn_unimpl_entry(void) { runtime_fail("NVN: an unimplemented entry was reached"); }
void nvn_prof_entry(void) { runtime_fail("NVN: the profiling entry is not ported"); }
