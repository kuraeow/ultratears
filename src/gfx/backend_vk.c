// The NVN replayer on Vulkan (milestone 3.3): the mtl_* execution layer that MacNativeGames implements on Metal
// (nvn_mtl.m), written anew for Vulkan against the same interface (nvn_mtl.h, nvn_obj.h). Step 1: memory pools,
// submissions and their completion; the recorded command stream is walked, and the commands with CPU-visible effects
// (events, counters, buffer copies and updates) run; drawing comes in the next steps.
//  - memory pools: the game's memory is imported as VkBuffers (VK_EXT_external_memory_host), no copies; a GPU address
//    is the CPU address, and mtl_addr finds the buffer and offset for it;
//  - a submission is one command buffer; its fence is the value it signals on one timeline semaphore;
//  - visible effects of completion (event values, counter records) are written by the retire thread after the GPU has
//    finished the submission, in submission order, and only then is the fence complete for mtl_wait (as on Metal).
// Without a usable Vulkan device everything completes at once (the null device's behaviour, events still written).
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_dirty.h"
#include "nvn/nvn_obj.h"
#include "../vk/present.h"
#include "vkb.h"
#include "vkb_shader.h"
#include "vkb_audit.h"
#include "vkb_lock.h"
#include "nvn/totk_timeline.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static _Atomic int g_ok = -1;   // -1 not initialised, 0 no Vulkan (null behaviour), 1 ready
static VkSemaphore g_timeline;
static uint64_t g_fence;                 // last submitted (submit thread; read under g_mu by others)
static uint64_t g_done;                  // last retired (under g_mu)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

void mtl_finish(void);
static void pass_end(void);
static void pass_end_why(int why);
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static uint64_t gpu_ticks(void) { return runtime_now_ns() * 12 / 625; }   // the Switch's 19.2 MHz counter

// ---------------- memory pools ----------------
// sh..defer_pass: the pool's copy in device memory (below, "copies of the pools in device memory")
typedef struct { uintptr_t lo, hi, base; VkBuffer buf; VkDeviceMemory mem; const void *key; VkDeviceSize bytes;
                 VkBuffer sbuf; VkDeviceSize soff; uint32_t sh; uint8_t no_shadow, filled; uint64_t stamp, sync_sub, used_sub, defer_pass, wb_sub; } pool_ent;
static pool_ent *g_pools;
static int g_npools, g_capools;
static vkb_rw g_pool_mu;   // recording threads look pools up for every draw (read); changes write
static uint64_t g_pool_bytes;

#ifdef _WIN32
void vkb_rw_read(vkb_rw *l, int kind) {
    if (TryAcquireSRWLockShared((PSRWLOCK)l)) return;
    uint64_t t0 = runtime_now_ns();
    AcquireSRWLockShared((PSRWLOCK)l);
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}
void vkb_rw_read_end(vkb_rw *l) { ReleaseSRWLockShared((PSRWLOCK)l); }
void vkb_rw_write(vkb_rw *l, int kind) {
    if (TryAcquireSRWLockExclusive((PSRWLOCK)l)) return;
    uint64_t t0 = runtime_now_ns();
    AcquireSRWLockExclusive((PSRWLOCK)l);
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}
void vkb_rw_write_end(vkb_rw *l) { ReleaseSRWLockExclusive((PSRWLOCK)l); }
#else
#include <pthread.h>
static pthread_rwlock_t *get_rw(vkb_rw *l) {
    if (!l->opaque) {
        pthread_rwlock_t *rw = (pthread_rwlock_t *)calloc(1, sizeof(pthread_rwlock_t));
        pthread_rwlock_init(rw, NULL);
        l->opaque = rw;
    }
    return (pthread_rwlock_t *)l->opaque;
}
void vkb_rw_read(vkb_rw *l, int kind) { (void)kind; pthread_rwlock_rdlock(get_rw(l)); }
void vkb_rw_read_end(vkb_rw *l) { pthread_rwlock_unlock(get_rw(l)); }
void vkb_rw_write(vkb_rw *l, int kind) { (void)kind; pthread_rwlock_wrlock(get_rw(l)); }
void vkb_rw_write_end(vkb_rw *l) { pthread_rwlock_unlock(get_rw(l)); }
#endif

// ---- per recording thread (vkb_mt.c design, implemented below in "several recording threads")
static __thread int t_worker;          // vkb_worker(): 0 the translation thread (and any other), 1.. the recording threads
static __thread int t_recording;       // recording a chunk (or the prologue) of a submission
static __thread VkCommandBuffer t_cb;  // the command buffer it records (VK_NULL_HANDLE: the submission's own)
static __thread uint64_t t_seq;        // the position of the command being replayed
int vkb_worker(void) { return t_worker; }
int vkb_mt_recording(void) { return t_recording; }
uint64_t vkb_seq(void) { return t_seq; }
void vkb_seq_set(uint64_t seq) { t_seq = seq; }
void vkb_cmd_set(VkCommandBuffer cb) { t_cb = cb; }

static void wb_forget(VkBuffer host);
static uint32_t memory_type(uint32_t bits) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++) if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

// Commands recorded per frame (the present log line every 300 presents): barriers with their image layout changes and
// render passes begun. Counted by wrapping the two functions once the device exists.
static PFN_vkCmdPipelineBarrier2 g_real_barrier;
static PFN_vkCmdBeginRendering g_real_begin;
static vkb_cnt g_n_barriers, g_n_img_barriers, g_n_passes;
static uint64_t g_parts;
_Atomic uint64_t vkb_lock_wait_ns[LK_COUNT], vkb_lock_waits[LK_COUNT], vkb_step_ns[PK_COUNT], vkb_step_n[PK_COUNT];
static void VKAPI_CALL count_barrier(VkCommandBuffer cb, const VkDependencyInfo *d) {
    vkb_cnt_add(g_n_barriers, 1); vkb_cnt_add(g_n_img_barriers, d->imageMemoryBarrierCount);
    g_real_barrier(cb, d);
}
static void VKAPI_CALL count_begin(VkCommandBuffer cb, const VkRenderingInfo *r) { vkb_cnt_add(g_n_passes, 1); g_real_begin(cb, r); }

static void backend_init(void) {
    if (g_ok >= 0) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (g_ok < 0) {
        int ok = vk_init(NULL);   // already done by the window when there is one
        if (ok) {
            VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
            VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
            ok = vk_check(vk.vkCreateSemaphore(vkc.device, &si, NULL, &g_timeline), "timeline semaphore");
        }
        void retire_start(void);
        if (ok) retire_start();
        if (ok) {
            g_real_barrier = vk.vkCmdPipelineBarrier2; vk.vkCmdPipelineBarrier2 = count_barrier;
            g_real_begin = vk.vkCmdBeginRendering; vk.vkCmdBeginRendering = count_begin;
        }
        vkb_state_reset();
        VLOG("replayer %s", ok ? "on Vulkan" : "without a GPU: submissions complete at once");
        atomic_store(&g_ok, ok);
    }
    pthread_mutex_unlock(&mu);
}

void mtl_pool_add(const void *mem, uint64_t size) {
    backend_init();
    if (!g_ok || !mem || !size) return;
    uintptr_t align = vkc.host_pointer_alignment ? vkc.host_pointer_alignment : 4096;
    uintptr_t lo = (uintptr_t)mem, hi = lo + size, a = lo & ~(align - 1), b = (hi + align - 1) & ~(align - 1);
    // The import needs committed memory: guest pages are committed on first touch, so the pool is committed now.
    // A virtual pool is an inaccessible address range (totk_vpool_win.c): no memory to import (sparse binding, later).
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    if (a >= GUEST_LO && b <= GUEST_HI) plat_commit(a, b - a);
    else if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect == PAGE_NOACCESS) return;
#else
    if (a >= GUEST_LO && b <= GUEST_HI) plat_commit(a, b - a);
#endif
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!vk_check(vk.vkGetMemoryHostPointerPropertiesEXT(vkc.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                          (void *)a, &hp), "host pointer properties")) return;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, .size = b - a,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &buf), "pool buffer")) return;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, buf, &req);
    uint32_t type = memory_type(req.memoryTypeBits & hp.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)a};
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = b - a, .memoryTypeIndex = type};
    VkDeviceMemory dm;
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &dm), "pool import")) {
        VLOG("memory pool %p+%#llx: cannot import (memory types %#x / %#x)", mem, (unsigned long long)size, req.memoryTypeBits, hp.memoryTypeBits);
        vk.vkDestroyBuffer(vkc.device, buf, NULL);
        return;
    }
    vk.vkBindBufferMemory(vkc.device, buf, dm, 0);
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_npools == g_capools) {
        g_capools = g_capools ? g_capools * 2 : 64;
        g_pools = realloc(g_pools, (size_t)g_capools * sizeof *g_pools);
        if (!g_pools) die("pool table");
    }
    int i = g_npools;
    while (i > 0 && g_pools[i - 1].lo > lo) { g_pools[i] = g_pools[i - 1]; i--; }
    g_pools[i] = (pool_ent){lo, hi, a, buf, dm, mem, b - a};
    g_npools++;
    for (int j = i - 4; j <= i + 4; j++)   // overlapping pools: one copy each would diverge, so neither is copied
        if (j >= 0 && j < g_npools && j != i && g_pools[j].lo < hi && lo < g_pools[j].hi) {
            static int said;
            if (said++ < 8) VLOG("memory pool %p+%#llx overlaps pool %#llx..%#llx: neither is copied to device memory", mem,
                                 (unsigned long long)size, (unsigned long long)g_pools[j].lo, (unsigned long long)g_pools[j].hi);
            g_pools[i].no_shadow = g_pools[j].no_shadow = 1;
            if (g_pools[j].sh) { vkb_shadow_free(g_pools[j].sh); g_pools[j].sh = 0; }
        }
    g_pool_bytes += hi - lo;
    static unsigned said;
    if (said++ < 4) VLOG("memory pool %p+%#llx imported (%d pools, %.1f MiB)", mem, (unsigned long long)size, g_npools, g_pool_bytes / 1048576.0);
    vkb_rw_write_end(&g_pool_mu);
}

void mtl_pool_del(const void *mem) {
    if (g_ok != 1) return;
    pool_ent e = {0};
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].key == mem) {
            e = g_pools[i];
            memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_npools - i - 1) * sizeof *g_pools);
            g_npools--;
            g_pool_bytes -= e.hi - e.lo;
            break;
        }
    if (e.sh) vkb_shadow_free(e.sh);
    if (e.buf) wb_forget(e.buf);
    vkb_rw_write_end(&g_pool_mu);
    if (!e.buf) return;
    if (vkc.has_checkpoints) VLOG("memory pool %p %#llx..%#llx deleted (present %llu)", mem, (unsigned long long)e.lo, (unsigned long long)e.hi, (unsigned long long)vkb_frame());
    // The frontend drains the queue before it deletes a pool (hle_nvn.c x_drain); the GPU may still be finishing.
    mtl_finish();
    vk.vkDestroyBuffer(vkc.device, e.buf, NULL);
    vk.vkFreeMemory(vkc.device, e.mem, NULL);
}

void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; }   // zero-copy: nothing cached

static __thread int g_pool_last;   // the entry this thread found last (draws use a few pools over and over)
static const pool_ent *pool_find(uintptr_t addr, uint64_t len) {   // g_pool_mu held
    if (g_pool_last < g_npools && addr >= g_pools[g_pool_last].lo && addr + len <= g_pools[g_pool_last].hi) return &g_pools[g_pool_last];
    int lo = 0, hi = g_npools - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_pools[m].lo <= addr) { found = m; lo = m + 1; } else hi = m - 1;
    }
    for (int i = found; i >= 0 && i > found - 4; i--)   // nested / adjacent pools: the closest few that start below
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) { g_pool_last = i; return &g_pools[i]; }
    return NULL;
}

// ---------------- copies of the pools in device memory (discrete GPUs; memory: vkb_shadow.c) ----------------
// Game memory is host memory imported into Vulkan, so on a discrete GPU every vertex, index and constant a draw reads
// crosses PCIe: on the Great Sky Island 28 of the frame's 34 ms of GPU time were spent inside render passes, 7.6 ms with
// the pools copied to device memory (experiment, 2026-10-24). A pool the GPU uses gets a copy in device memory; in a
// submission, once the copy is current, every GPU access to the pool goes to it (vkb_pool_addr). Keeping it current:
//  - CPU writes: pages written since the copy was last brought up to date (the write watch, nvn_dirty_runs) are copied
//    in when the pool is first used in a submission. In a render pass (no copies there) a pool with written pages stays
//    in game memory until the pass ends, unless a shader writes it (then the pass ends); a clean one switches at once.
//    At the first pass of a submission, the pools used in the two before are brought up to date together.
//  - GPU writes (shader stores, buffer copies, uniform updates, texture-to-buffer copies) go to the copy and are copied
//    back to game memory at the end of the submission, through host memory and the retire thread (wb_merge), which
//    leaves alone the pages the CPU wrote meanwhile (the CPU reads the results after the game's wait).
//    [temporary] Page granularity: GPU writes into a page the CPU also wrote are lost (counted in the log line).
// Pages the copy back writes are not CPU writes; were the write watch to count them, they would only be copied in again.
// The game writes memory for a submission before it is submitted, and does not overwrite what the GPU still reads (it
// waits for the GPU first), so bringing a copy up to date early (ahead, or for the whole pool) does not change results.
// Integrated GPUs (the Steam Deck) read host memory at full speed: no copies there (vkb_shadow_on).
typedef struct { VkBuffer src, dst; VkDeviceSize soff, doff, n; uint64_t base, stamp; } wb_t;   // base: the pool's import address; stamp: its copy's
static wb_t *g_wb;                 // GPU-written ranges of copies in this submission: back to game memory at its end
static int g_nwb, g_capwb;
static VkBufferCopy *g_runs;       // copies being gathered (sync_gather / sync_flush)
static int g_nruns, g_caruns;
typedef struct { VkBuffer src, dst; int first, n; } run_grp;
static run_grp *g_grp;
static int g_ngrp, g_capgrp;
static uint64_t g_shadow_budget, g_presync_sub;
static uint64_t g_sh_in, g_sh_in_bytes, g_sh_out_bytes, g_sh_deferred, g_sh_scan_ns;   // the per-frame log line

static void shadow_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
}
static void run_put(VkBuffer src, VkBuffer dst, VkDeviceSize soff, VkDeviceSize doff, VkDeviceSize n) {
    if (g_nruns == g_caruns) {
        g_caruns = g_caruns ? g_caruns * 2 : 256;
        if (!(g_runs = realloc(g_runs, sizeof *g_runs * (size_t)g_caruns))) die("pool copy runs");
    }
    if (!g_ngrp || g_grp[g_ngrp - 1].src != src || g_grp[g_ngrp - 1].dst != dst) {
        if (g_ngrp == g_capgrp) {
            g_capgrp = g_capgrp ? g_capgrp * 2 : 64;
            if (!(g_grp = realloc(g_grp, sizeof *g_grp * (size_t)g_capgrp))) die("pool copy groups");
        }
        g_grp[g_ngrp++] = (run_grp){src, dst, g_nruns, 0};
    }
    g_runs[g_nruns++] = (VkBufferCopy){soff, doff, n};
    g_grp[g_ngrp - 1].n++;
    g_sh_in_bytes += n;
}
static void run_written(void *ctx, size_t off, size_t n) {   // nvn_dirty_runs: off from the pool's start
    const pool_ent *p = ctx;
    VkDeviceSize o = (p->lo - p->base) + off;
    run_put(p->buf, p->sbuf, o, p->soff + o, n);
}
static void sync_flush(void) {   // the gathered copies, outside a pass (threads recording: in the prologue)
    if (!g_nruns) return;
    int mt = t_recording;
    if (mt) vkb_pro_begin();
    else pass_end_why(PE_SHADOW);
    VkCommandBuffer cb = vkb_cmd();
    vkb_checkpoint("pool copies in", NULL, NULL);
    shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (int g = 0; g < g_ngrp; g++) vk.vkCmdCopyBuffer(cb, g_grp[g].src, g_grp[g].dst, (uint32_t)g_grp[g].n, &g_runs[g_grp[g].first]);
    shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    g_sh_in += (uint64_t)g_nruns;
    g_nruns = g_ngrp = 0;
    if (mt) vkb_pro_end();
}
// Gathers the copies that bring p's copy up to date for submission sub (g_pool_mu held). 1: current once they are
// recorded (sync_flush); 0: p stays in game memory for now (a pass is open, copies are needed and may_end is 0) or
// for good (no_shadow).
static int sync_gather(pool_ent *p, uint64_t sub, int may_end) {
    if (p->no_shadow) return 0;
    // GPU writes of an earlier submission not yet in game memory (wb_merge): the copy is the only current one
    if (p->wb_sub && p->wb_sub <= g_fence && !mtl_wait(p->wb_sub, 0)) may_end = 1;
    if (!may_end && vkb_in_pass() && p->defer_pass == vkb_pass_serial()) return 0;   // already found written in this pass
    if (!p->sh) {
        if (!g_shadow_budget) {   // half of the largest device-local heap
            for (uint32_t h = 0; h < vkc.memory.memoryHeapCount; h++)
                if ((vkc.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && vkc.memory.memoryHeaps[h].size / 2 > g_shadow_budget)
                    g_shadow_budget = vkc.memory.memoryHeaps[h].size / 2;
            VLOG("copies of memory pools: up to %llu MiB of device memory", (unsigned long long)(g_shadow_budget >> 20));
        }
        const char *why = !nvn_dirty_tracked((const void *)p->lo, p->hi - p->lo) ? "its writes are not tracked"
                        : vkb_shadow_bytes() + p->bytes > g_shadow_budget ? "over the device memory budget"
                        : !(p->sh = vkb_shadow_alloc(p->bytes, &p->sbuf, &p->soff)) ? "no device memory" : NULL;
        if (why) {
            p->no_shadow = 1;
            static int said;
            if (said++ < 16) VLOG("[temporary] memory pool %#llx+%#llx stays in game memory: %s", (unsigned long long)p->lo, (unsigned long long)(p->hi - p->lo), why);
            return 0;
        }
        p->filled = 0;
    }
    uint64_t t0 = runtime_now_ns();
    int first = g_nruns, grp = g_ngrp;
    uint64_t bytes0 = g_sh_in_bytes;
    if (!p->filled) {
        nvn_dirty_since((const void *)p->lo, p->hi - p->lo, UINT64_MAX);   // writes so far are in the whole copy
        run_put(p->buf, p->sbuf, 0, p->soff, p->bytes);
    } else if (nvn_dirty_runs((const void *)p->lo, p->hi - p->lo, p->stamp, 65536, run_written, p) < 0) {
        p->no_shadow = 1;   // its writes can no longer be told
        vkb_shadow_free(p->sh); p->sh = 0;
        return 0;
    }
    uint64_t stamp = nvn_dirty_now();
    g_sh_scan_ns += runtime_now_ns() - t0;
    if (g_nruns > first && vkb_in_pass() && !may_end) {   // copies need the pass to end: game memory for the rest of it
        g_sh_in_bytes = bytes0;
        g_nruns = first; g_ngrp = grp;
        if (grp) g_grp[grp - 1].n = first - g_grp[grp - 1].first;
        p->defer_pass = vkb_pass_serial();
        g_sh_deferred++;
        return 0;
    }
    p->filled = 1;
    p->stamp = stamp;
    p->sync_sub = sub;
    return 1;
}
static void wb_add(pool_ent *p, uint64_t addr, uint64_t len) {   // a GPU write to p's copy
    p->wb_sub = g_fence + 1;
    VkDeviceSize o = addr - p->base;
    if (g_nwb && g_wb[g_nwb - 1].dst == p->buf && o <= g_wb[g_nwb - 1].doff + g_wb[g_nwb - 1].n && g_wb[g_nwb - 1].doff <= o + len) {
        wb_t *w = &g_wb[g_nwb - 1];   // overlaps or touches the last one: merged
        VkDeviceSize lo = w->doff < o ? w->doff : o, hi = w->doff + w->n > o + len ? w->doff + w->n : o + len;
        w->soff -= w->doff - lo; w->doff = lo; w->n = hi - lo;
        return;
    }
    if (g_nwb == g_capwb) {
        g_capwb = g_capwb ? g_capwb * 2 : 256;
        if (!(g_wb = realloc(g_wb, sizeof *g_wb * (size_t)g_capwb))) die("pool copies back");
    }
    g_wb[g_nwb++] = (wb_t){p->sbuf, p->buf, p->soff + o, o, len, p->base, p->stamp};
}
static void wb_forget(VkBuffer host) {   // a pool deleted (g_pool_mu held): its writes have nowhere to go
    for (int k = 0; k < g_nwb;) if (g_wb[k].dst == host) g_wb[k] = g_wb[--g_nwb]; else k++;
}
static int wb_cmp(const void *a, const void *b) {
    const wb_t *x = a, *y = b;
    if (x->dst != y->dst) return (uintptr_t)x->dst < (uintptr_t)y->dst ? -1 : 1;
    return x->doff < y->doff ? -1 : x->doff > y->doff;
}
// The copy back goes through host memory of the submission's slot (wbst_t): the GPU copies the written ranges there,
// and when the submission has completed the retire thread copies them into game memory, page by page, except pages
// the CPU wrote since the copy was brought up to date (wb_merge). Copied straight into game memory, the whole range
// overwrote what the game's CPU wrote there meanwhile: about 45% of the ranges on the Great Sky Island (2026-10-24).
// The game's own writes are newer than the GPU's there (it writes a buffer for its next use after the GPU's), so the
// CPU's pages win. A slot is reused only after its submission has retired, so its host memory is free by then.
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize cap; } wbst_t;
static wbst_t g_wbst[VKB_SLOTS];
static uint64_t *g_wb_ranges;   // per range of the last shadow_writeback: guest address, bytes, stamp, offset in the slot's memory
static int g_wb_nranges;
static uint64_t g_wb_kept_pages, g_wb_pages;   // pages left to the CPU's writes / all pages copied back (the log line)
static uint8_t *wbst_get(int slot, VkDeviceSize need, VkBuffer *buf) {   // the slot's host memory, at least need bytes
    wbst_t *s = &g_wbst[slot];
    if (s->cap < need) {
        if (s->buf) { vk.vkDestroyBuffer(vkc.device, s->buf, NULL); vk.vkFreeMemory(vkc.device, s->mem, NULL); }
        *s = (wbst_t){0};
        VkDeviceSize cap = 1u << 20;
        while (cap < need) cap *= 2;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &s->buf), "copy back buffer")) die("copy back buffer");
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, s->buf, &req);
        uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)   // cached first: the CPU reads it
            for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++) {
                VkMemoryPropertyFlags f = vkc.memory.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits & (1u << i)) && (f & want) == want && (pass || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))) type = i;
            }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &s->mem), "copy back memory")) die("copy back memory");
        vk.vkBindBufferMemory(vkc.device, s->buf, s->mem, 0);
        void *p;
        if (!vk_check(vk.vkMapMemory(vkc.device, s->mem, 0, VK_WHOLE_SIZE, 0, &p), "copy back map")) die("copy back map");
        s->map = p; s->cap = cap;
    }
    *buf = s->buf;
    return s->map;
}
// Retire thread, the submission complete: its copied-back ranges into game memory, but for pages the CPU wrote since.
// These writes are the GPU's, not the CPU's (nvn_dirty_gpu_write_*): counted as the CPU's, a later submission's copy
// back to the same range would find them newer than its copy and keep them in place of its own GPU writes.
static void wb_put(uint64_t a, const uint8_t *src, uint64_t n) {
    void *tok = nvn_dirty_gpu_write_begin((void *)(uintptr_t)a, n);
    memcpy((void *)(uintptr_t)a, src, n);
    nvn_dirty_gpu_write_end(tok);
}
static void wb_merge(const uint64_t *r, int n, const uint8_t *src) {
    for (int k = 0; k < n; k++, r += 4) {
        uint64_t a = r[0], len = r[1], stamp = r[2];
        if (!nvn_dirty_since((const void *)(uintptr_t)a, len, stamp)) { wb_put(a, src + r[3], len); g_wb_pages += (len + 4095) / 4096; continue; }
        for (uint64_t p = a; p < a + len;) {   // page by page
            uint64_t e = ((p >> 12) + 1) << 12;
            if (e > a + len) e = a + len;
            g_wb_pages++;
            if (nvn_dirty_since((const void *)(uintptr_t)p, e - p, stamp)) g_wb_kept_pages++;
            else wb_put(p, src + r[3] + (p - a), e - p);
            p = e;
        }
    }
}
static void shadow_writeback(VkCommandBuffer cb) {   // end of a submission: GPU writes to the slot's host memory
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    g_wb_ranges = NULL; g_wb_nranges = 0;
    if (g_nwb) {
        qsort(g_wb, (size_t)g_nwb, sizeof *g_wb, wb_cmp);
        int m = 0;   // overlapping ranges of one pool merged
        for (int k = 1; k < g_nwb; k++) {
            wb_t *w = &g_wb[m];
            if (g_wb[k].dst == w->dst && g_wb[k].doff <= w->doff + w->n) {
                VkDeviceSize hi = g_wb[k].doff + g_wb[k].n;
                if (hi > w->doff + w->n) w->n = hi - w->doff;
                if (g_wb[k].stamp < w->stamp) w->stamp = g_wb[k].stamp;
            } else g_wb[++m] = g_wb[k];
        }
        g_nwb = m + 1;
        g_wb_ranges = malloc(sizeof *g_wb_ranges * 4 * (size_t)g_nwb);
        if (!g_wb_ranges) die("copy back ranges");
        VkDeviceSize total = 0;
        for (int k = 0; k < g_nwb; k++) {
            uint64_t *r = &g_wb_ranges[4 * k];
            r[0] = g_wb[k].base + g_wb[k].doff; r[1] = g_wb[k].n; r[2] = g_wb[k].stamp; r[3] = total;
            total += (g_wb[k].n + 15) & ~(VkDeviceSize)15;
        }
        g_wb_nranges = g_nwb;
        VkBuffer st;
        wbst_get(vkb_slot(), total, &st);
        vkb_checkpoint("pool copies out", NULL, NULL);
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        for (int k = 0; k < g_nwb;) {
            int e = k;
            VkBufferCopy r[64];
            int n = 0;
            while (e < g_nwb && g_wb[e].src == g_wb[k].src && n < 64) {
                r[n++] = (VkBufferCopy){g_wb[e].soff, g_wb_ranges[4 * e + 3], g_wb[e].n};
                g_sh_out_bytes += g_wb[e].n;
                e++;
            }
            vk.vkCmdCopyBuffer(cb, g_wb[k].src, st, (uint32_t)n, r);
            k = e;
        }
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                       VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
        g_nwb = 0;
    }
    vkb_rw_write_end(&g_pool_mu);
}
// The first pass of a submission: the pools used in the two before are brought up to date together (one pair of
// barriers), so that draws in passes find them current.
static void shadow_presync(void) {
    uint64_t sub = g_fence + 1;
    if (!vkb_shadow_on() || __atomic_load_n(&g_presync_sub, __ATOMIC_ACQUIRE) == sub) return;
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_presync_sub == sub) { vkb_rw_write_end(&g_pool_mu); return; }   // another thread did it meanwhile
    __atomic_store_n(&g_presync_sub, sub, __ATOMIC_RELEASE);
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].sh && g_pools[i].sync_sub != sub && g_pools[i].used_sub + 2 >= sub) sync_gather(&g_pools[i], sub, 1);
    sync_flush();
    vkb_rw_write_end(&g_pool_mu);
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len);
void vkb_pool_use(uint64_t addr, uint64_t len, int write) {
    if (g_ok != 1 || !addr || !len || !vkb_shadow_on()) return;
    uint64_t sub = g_fence + 1;
    if (!write) {   // the common case: a read of a pool already current in this submission
        vkb_rw_read(&g_pool_mu, LK_POOLS);
        pool_ent *p = pool_find_mut((uintptr_t)addr, len);
        int done = !p || p->no_shadow || p->sync_sub == sub;
        if (p) __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
        vkb_rw_read_end(&g_pool_mu);
        if (done) return;
    }
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        p->used_sub = sub;
        // threads recording: the copies go into the prologue, before every chunk (no pass to end)
        int ok = p->sync_sub == sub || sync_gather(p, sub, write || t_recording);
        sync_flush();
        if (ok && write) wb_add(p, addr, len);
    }
    vkb_rw_write_end(&g_pool_mu);
}
int vkb_pool_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_ok != 1) return 0;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        uint64_t sub = g_fence + 1;
        __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
        if (p->sh && p->sync_sub == sub) { *buf = p->sbuf; *off = p->soff + (addr - p->base); }
        else { *buf = p->buf; *off = addr - p->base; }
    }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len) { return (pool_ent *)pool_find(addr, len); }

int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    if (g_ok != 1) { *buf = NULL; *off = 0; return 0; }
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    const pool_ent *p = pool_find((uintptr_t)addr, len);
    if (p) { *buf = (void *)p->buf; *off = addr - p->base; }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}

// ---------------- submissions ----------------
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; } post_op;   // 1 event, 2 counter
typedef struct { VkCommandPool pool; VkCommandBuffer cb; uint64_t fence; } cmd_slot;
#define SLOTS VKB_SLOTS
static cmd_slot g_slots[SLOTS];
static int g_nslots, g_cur = -1;
static post_op *g_post;
static int g_npost, g_capost;
static uint64_t g_op_count[OP_COUNT_];
static int g_op_said[OP_COUNT_];

typedef struct job { uint64_t fence; post_op *ops; int n; int slot; uint64_t *wb; int nwb; struct job *next; } job;
static job *g_jobs, **g_jobs_tail = &g_jobs;

static void run_posts(const post_op *ops, int n) {
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1) {
            if (ops[i].mode == 1) *ops[i].val += ops[i].v;
            else *ops[i].val = ops[i].v;
        } else {
            // NVNcounterData {counter, timestamp}. Samples passed: [temporary] reported as visible until draws are replayed
            // (with 0 the game culls every queried object); timestamps and other types report counter 0, as on Metal.
            uint64_t rec[2] = {ops[i].type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, gpu_ticks()};
            guest_prepare_write((void *)(uintptr_t)ops[i].addr, sizeof rec);
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec);
        }
    }
}

static void cpend_done(uint64_t fence);   // conditional rendering (below): the submission's counter reports are in memory
static void complete(uint64_t fence) {
    pthread_mutex_lock(&g_mu);
    if (fence > g_done) g_done = fence;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
}

// ---- TOTK_VK_CHECKPOINTS=1 (development): a marker before each draw, dispatch and copy names it; when the device is
// lost, the markers the GPU last reached (VK_NV_device_diagnostic_checkpoints) are reported with what they name.
enum { CKPT_RING = 1 << 16 };
typedef struct { uint64_t frame; uint32_t n; const char *what; char k1[32], k2[32]; } ckpt_t;
static ckpt_t *g_ckpt;
static _Atomic uint32_t g_ckpt_n;
void vkb_checkpoint(const char *what, const char *k1, const char *k2) {
    if (!vkc.has_checkpoints) return;
    if (!g_ckpt && !(g_ckpt = calloc(CKPT_RING, sizeof *g_ckpt))) return;
    uint32_t n = atomic_fetch_add(&g_ckpt_n, 1), i = n % CKPT_RING;
    ckpt_t *c = &g_ckpt[i];
    c->frame = vkb_frame(); c->n = n; c->what = what;
    snprintf(c->k1, sizeof c->k1, "%s", k1 ? k1 : "-");
    snprintf(c->k2, sizeof c->k2, "%s", k2 ? k2 : "-");
    vk.vkCmdSetCheckpointNV(vkb_cmd(), (void *)(uintptr_t)(i + 1));
}
static void checkpoint_report(void) {
    if (!vkc.has_checkpoints || !g_ckpt) return;
    VkCheckpointDataNV d[16];
    uint32_t n = 16;
    for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
    vk_queue_lock();
    vk.vkGetQueueCheckpointDataNV(vkc.queue, &n, d);
    vk_queue_unlock();
    VLOG("device lost: %u checkpoints reached (last %u recorded)", n, (unsigned)atomic_load(&g_ckpt_n));
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t m = (uintptr_t)d[i].pCheckpointMarker;
        if (!m || m > CKPT_RING) continue;
        const ckpt_t *c = &g_ckpt[m - 1];
        VLOG("  stage %#llx: #%u present %llu %s %s %s", (unsigned long long)d[i].stage, c->n, (unsigned long long)c->frame, c->what, c->k1, c->k2);
        unsigned long long a = strtoull(c->k1, NULL, 16);
        if (a) {   // an address: what the memory and the pool table hold now
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION mi = {0};
            VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi);
            vkb_rw_read(&g_pool_mu, LK_POOLS);
            const pool_ent *p = pool_find((uintptr_t)a, 1);
            VLOG("    memory %#llx: state %#lx protect %#lx region %p+%#llx; pool %s %#llx..%#llx", a, mi.State, mi.Protect, mi.BaseAddress,
                 (unsigned long long)mi.RegionSize, p ? "registered" : "none", p ? (unsigned long long)p->lo : 0ull, p ? (unsigned long long)p->hi : 0ull);
            vkb_rw_read_end(&g_pool_mu);
#else
            vkb_rw_read(&g_pool_mu, LK_POOLS);
            const pool_ent *p = pool_find((uintptr_t)a, 1);
            VLOG("    memory %#llx: pool %s %#llx..%#llx", a, p ? "registered" : "none", p ? (unsigned long long)p->lo : 0ull, p ? (unsigned long long)p->hi : 0ull);
            vkb_rw_read_end(&g_pool_mu);
#endif
        }
    }
}

// ---- deferred destruction: objects given up while a submission is recorded may be used by its earlier commands (a
// texture view replaced in the middle of a frame, a texture the game finalizes). Destroying them after waiting for the
// last submitted work (as before) freed what the open submission still referred to: the GPU read a destroyed view and
// the device was lost (TOTK's item-get screen, 2026-10-24). Now each waits for the first submission after it was given up.
typedef struct dead { uint64_t fence; VkImageView view; VkImage img; VkDeviceMemory mem; struct dead *next; } dead;
static dead *g_dead_new, *g_dead;   // given up since the last submission (no fence yet) / waiting for their fence
static pthread_mutex_t g_dead_mu = PTHREAD_MUTEX_INITIALIZER;
void vkb_destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem) {
    dead *d = malloc(sizeof *d);
    if (!d) die("deferred destruction");
    *d = (dead){0, view, img, mem, NULL};
    pthread_mutex_lock(&g_dead_mu);
    d->next = g_dead_new; g_dead_new = d;
    pthread_mutex_unlock(&g_dead_mu);
}
static void dead_tag(uint64_t fence) {   // a submission: what was given up so far waits for it
    pthread_mutex_lock(&g_dead_mu);
    while (g_dead_new) { dead *d = g_dead_new; g_dead_new = d->next; d->fence = fence; d->next = g_dead; g_dead = d; }
    pthread_mutex_unlock(&g_dead_mu);
}
static void dead_reap(uint64_t done) {   // the GPU finished up to `done`
    dead *free_list = NULL;
    pthread_mutex_lock(&g_dead_mu);
    for (dead **p = &g_dead; *p;) {
        if ((*p)->fence <= done) { dead *d = *p; *p = d->next; d->next = free_list; free_list = d; } else p = &(*p)->next;
    }
    pthread_mutex_unlock(&g_dead_mu);
    while (free_list) {
        dead *d = free_list; free_list = d->next;
        if (d->view) vk.vkDestroyImageView(vkc.device, d->view, NULL);
        if (d->img) vk.vkDestroyImage(vkc.device, d->img, NULL);
        if (d->mem) vk.vkFreeMemory(vkc.device, d->mem, NULL);
        free(d);
    }
}

static void gpu_span(int slot, uint64_t fence);
// Retires submissions in order: waits for the GPU, writes their events and counters, completes their fence.
static void *retire_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-vk-retire");
    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_jobs) pthread_cond_wait(&g_cv, &g_mu);
        job *j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs) g_jobs_tail = &g_jobs;
        pthread_mutex_unlock(&g_mu);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_timeline,
                                  .pValues = &j->fence};
        VkResult r = vk.vkWaitSemaphores(vkc.device, &wi, UINT64_MAX);
        if (r != VK_SUCCESS) { vk_check(r, "vkWaitSemaphores"); checkpoint_report(); die("GPU submission %llu lost", (unsigned long long)j->fence); }
        if (totk_tl_on()) gpu_span(j->slot, j->fence);   // TOTK_TIMELINE: when the GPU ran this command buffer
        if (j->wb) { wb_merge(j->wb, j->nwb, g_wbst[j->slot].map); free(j->wb); }   // GPU writes into game memory
        run_posts(j->ops, j->n);
        cpend_done(j->fence);
        complete(j->fence);
        dead_reap(j->fence);
        free(j->ops);
        free(j);
    }
    return NULL;
}
void retire_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, retire_thread, NULL)) die("retire thread");
}

// GPU time of every submission (a timestamp at its start and end), summed per frame for the per-frame log line, and the
// time the recording thread waits for a free command slot (the GPU is behind).
static VkQueryPool g_tsq;
static int g_ts_used[SLOTS];
static uint64_t g_gpu_ns, g_slot_wait_ns;
static void gpu_time_begin(int slot) {   // into the slot's own command buffer (the submission's first)
    if (!g_tsq) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * SLOTS};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_tsq), "timestamp pool")) return;
        vk.vkResetQueryPool(vkc.device, g_tsq, 0, 2 * SLOTS);
    }
    if (g_ts_used[slot]) {   // the slot's previous submission is complete
        uint64_t t[2];
        if (vk.vkGetQueryPoolResults(vkc.device, g_tsq, (uint32_t)(2 * slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            g_gpu_ns += (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod);
        g_ts_used[slot] = 0;
    }
    vk.vkCmdResetQueryPool(g_slots[slot].cb, g_tsq, (uint32_t)(2 * slot), 2);
    vk.vkCmdWriteTimestamp2(g_slots[slot].cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_tsq, (uint32_t)(2 * slot));
}
// The GPU span of a completed command buffer on the host clock: it ended about when the retire thread saw its fence
// (the thread waits on fences in order), and lasted what its two timestamps say.
static void gpu_span(int slot, uint64_t fence) {
    uint64_t end = runtime_now_ns(), t[2];
    if (!g_tsq || slot < 0) return;
    if (vk.vkGetQueryPoolResults(vkc.device, g_tsq, (uint32_t)(2 * slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] <= t[0]) return;
    uint64_t dur = (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod);
    totk_tl_at(TL_GPU, end > dur ? end - dur : 0, fence, end);
}
static void gpu_time_end(int slot, VkCommandBuffer cb) {   // into the submission's last command buffer
    if (!g_tsq) return;
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_tsq, (uint32_t)(2 * slot + 1));
    g_ts_used[slot] = 1;
}

// A submission whose recording took over 100 ms: where the time went (log line "long submission"). The game waits for
// its recording at the present: these are the hitches when new parts of the world arrive.
extern uint64_t vkb_astc_ns, vkb_shader_load_ns;
static uint64_t g_sub_t0, g_sub_snap[6];
static void sub_snap(uint64_t v[6]) {
    v[0] = vkb_step_ns[PK_CREATE]; v[1] = vkb_step_ns[PK_UPLOAD]; v[2] = vkb_astc_ns; v[3] = vkb_shader_load_ns;
    v[4] = g_sh_scan_ns; v[5] = g_sh_in_bytes;
}
static void sub_long_check(uint64_t fence) {
    uint64_t now = runtime_now_ns(), v[6];
    if (!g_sub_t0 || now - g_sub_t0 < 100000000ull) return;
    sub_snap(v);
    static int said;
    if (said++ < 64)
        VLOG("long submission %llu: recorded in %.0f ms: new images %.0f ms, uploads %.0f ms (ASTC %.0f ms), shaders made %.0f ms, "
             "pool copy checks %.0f ms, pool copies in %.1f MiB; draws %llu", (unsigned long long)fence, (now - g_sub_t0) / 1e6,
             (v[0] - g_sub_snap[0]) / 1e6, (v[1] - g_sub_snap[1]) / 1e6, (v[2] - g_sub_snap[2]) / 1e6, (v[3] - g_sub_snap[3]) / 1e6,
             (v[4] - g_sub_snap[4]) / 1e6, (v[5] - g_sub_snap[5]) / 1048576.0, (unsigned long long)vkb_draw_count());
}

void mtl_sub_begin(void) {
    backend_init();
    g_sub_t0 = runtime_now_ns();
    sub_snap(g_sub_snap);
    g_npost = 0;
    if (g_ok != 1) return;
    pthread_mutex_lock(&g_mu);
    uint64_t done = g_done;
    pthread_mutex_unlock(&g_mu);
    int pick = -1;
    for (int i = 0; i < g_nslots; i++) if (g_slots[i].fence <= done) { pick = i; break; }
    if (pick < 0 && g_nslots < SLOTS) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = vkc.family};
        cmd_slot *s = &g_slots[g_nslots];
        if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &s->pool), "command pool")) die("command pool");
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, &s->cb), "command buffer")) die("command buffer");
        pick = g_nslots++;
    }
    if (pick < 0) {   // all in flight: the oldest
        pick = 0;
        for (int i = 1; i < g_nslots; i++) if (g_slots[i].fence < g_slots[pick].fence) pick = i;
        uint64_t t0 = runtime_now_ns();
        mtl_wait(g_slots[pick].fence, UINT64_MAX);
        g_slot_wait_ns += runtime_now_ns() - t0;
    }
    g_cur = pick;
    nvn_dirty_epoch_begin();   // write-watch scans: once per range in this submission (nvn_dirty_win.c)
    vkb_slot_reset(pick);   // its previous submission is complete: upload memory and descriptors are free
    vkb_mt_slot_reset(pick);
    vk.vkResetCommandPool(vkc.device, g_slots[pick].pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(g_slots[pick].cb, &bi);
    gpu_time_begin(pick);
}

static void post(post_op o) {
    if (g_npost == g_capost) {
        g_capost = g_capost ? g_capost * 2 : 64;
        g_post = realloc(g_post, (size_t)g_capost * sizeof *g_post);
        if (!g_post) die("post ops");
    }
    g_post[g_npost++] = o;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0}); }
static uint64_t g_counter_types[32];   // reports by NVN counter type (the per-frame log line)
// ---------- Conditional rendering (NVN SetRenderEnableConditional), evaluated in GPU order ----------
// Ported from MacNativeGames nvn_mtl.m (cpend_*, cond_eval; MIT). TOTK gates its ClearColor commands with it: the CPU
// writes counter=1 to request a clear and the TIMESTAMP_TOP report replayed right after the clear resets it to 0, so the
// clear runs once. Counter reports replayed earlier whose memory write has not happened yet (run_posts writes it when
// the submission retires) are kept here with the value they will write: 0 for timestamps and other types, unknown for
// samples passed. A conditional looks there first, then at game memory - the counter values the GPU would see.
// Replaying every gated clear (as before 2026-10-24) cleared light and history targets on frames the game had not
// asked for: menu and world lighting went black for a few frames after camera cuts.
typedef struct { uint64_t addr, fence, val; int known; } cpend_ent;
#define CPEND_N 4096
static cpend_ent g_cpend[CPEND_N];
static unsigned g_cpend_head, g_cpend_tail;
static pthread_mutex_t g_cpend_mu = PTHREAD_MUTEX_INITIALIZER;   // replay thread pushes, retire thread clears
static void cpend_push(uint64_t addr, uint64_t fence, uint64_t val, int known) {
    pthread_mutex_lock(&g_cpend_mu);
    g_cpend[g_cpend_head % CPEND_N] = (cpend_ent){addr, fence, val, known};
    g_cpend_head++;
    pthread_mutex_unlock(&g_cpend_mu);
}
static void cpend_done(uint64_t fence) {   // the reports of this submission are in memory now
    pthread_mutex_lock(&g_cpend_mu);
    if (g_cpend_head - g_cpend_tail > CPEND_N) g_cpend_tail = g_cpend_head - CPEND_N;
    for (unsigned i = g_cpend_tail; i != g_cpend_head; i++)
        if (g_cpend[i % CPEND_N].addr && g_cpend[i % CPEND_N].fence <= fence) g_cpend[i % CPEND_N].addr = 0;
    while (g_cpend_tail != g_cpend_head && !g_cpend[g_cpend_tail % CPEND_N].addr) g_cpend_tail++;
    pthread_mutex_unlock(&g_cpend_mu);
}
static int cpend_lookup(uint64_t addr, uint64_t *val) {   // 1 known (in *val), 0 pending but unknown, -1 none (read memory)
    int r = -1;
    pthread_mutex_lock(&g_cpend_mu);
    unsigned n = g_cpend_head - g_cpend_tail;
    if (n > CPEND_N) n = CPEND_N;
    for (unsigned k = 0; k < n; k++) {   // the latest report to the address
        const cpend_ent *e = &g_cpend[(g_cpend_head - 1 - k) % CPEND_N];
        if (e->addr == addr) { r = e->known; *val = e->val; break; }
    }
    pthread_mutex_unlock(&g_cpend_mu);
    return r;
}
static vkb_cnt g_cond_skipped;     // draws and clears not replayed (log line)
static uint64_t g_cond_unknown;    // conditionals taken as enabled (log line; the walk / one thread)
static int cond_eval(const p_render_cond *c) {   // 1: rendering enabled
    uint64_t x = 0, y = 0;
    int kx = cpend_lookup(c->addr, &x), ky = cpend_lookup(c->addr + 16, &y);
    void *bf; uint64_t off;
    if (kx == 0 || ky == 0 || !mtl_addr(c->addr, 32, &bf, &off)) { g_cond_unknown++; return 1; }
    if (kx < 0) memcpy(&x, (const void *)(uintptr_t)c->addr, 8);
    if (ky < 0) memcpy(&y, (const void *)(uintptr_t)(c->addr + 16), 8);
    return c->mode == 0 ? x == y : x != y;
}
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    g_counter_types[type & 31]++;
    cpend_push(addr, g_fence + 1, 0, type != NVN_COUNTER_TYPE_SAMPLES_PASSED);   // the counter word run_posts will write
    post((post_op){2, NULL, 0, 0, addr, type});
}

// Everything before is visible to everything after (step 1: transfers only; passes get precise barriers later).
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &di);
}

// TOTK_WATCH=<address>:<bytes>: GPU-side writes into that range (buffer copies, inline uniform updates) are logged too.
static int watched(uint64_t dst, uint64_t n) {
    static uint64_t lo, hi;
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("TOTK_WATCH");
        if (e) { char *end; lo = strtoull(e, &end, 0); hi = *end == ':' ? lo + strtoull(end + 1, NULL, 0) : lo; }
    }
    return hi > lo && dst < hi && lo < dst + n;
}

void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (watched(dst, n)) {
        static int said;
        if (said++ < 32) VLOG("[watch] present %llu: GPU copy %#llx -> %#llx (%llu bytes)", (unsigned long long)vkb_frame(),
                              (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    }
    if (g_ok != 1 || !n) { if (g_ok != 1 && n) memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n); return; }
    pass_end_why(PE_COPY);
    vkb_mirrors_evict(src, n);   // ranges that straddle a mirror: game memory first
    vkb_mirrors_evict(dst, n);
    vkb_pool_use(src, n, 0);
    vkb_pool_use(dst, n, 1);
    vkb_ucap_drop(dst, n);
    VkBuffer sb, db; VkDeviceSize so, dof;
    int from_cap = vkb_ucap_range(src, n, &sb, &so);   // the source as of the submit call
    if (watched(dst, n)) {
        float cv[4] = {0}, mv[4] = {0};
        vkb_ucap_read(src + 0x30, cv, 16);
        memcpy(mv, (const void *)(uintptr_t)(src + 0x30), 16);
        static int said;
        if (vkb_frame() >= 330 && said++ < 64) VLOG("[watch] present %llu: copy %#llx -> %#llx from the capture %d (why %d); source +0x30 captured %g %g %g %g, memory now %g %g %g %g",
                              (unsigned long long)vkb_frame(), (unsigned long long)src, (unsigned long long)dst, from_cap, vkb_ucap_why(src, n), cv[0], cv[1], cv[2], cv[3], mv[0], mv[1], mv[2], mv[3]);
    }
    if ((!from_cap && !vkb_gpu_addr(src, n, &sb, &so)) || !vkb_gpu_addr(dst, n, &db, &dof)) {
        vkb_audit(A_COPY_SKIPPED, NULL);
        static int said; if (said++ < 8) VLOG("copy %#llx -> %#llx (%llu bytes) outside the memory pools: skipped",
                                              (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
        return;
    }
    pass_end_why(PE_COPY);
    VkCommandBuffer cb = vkb_cmd();
    full_barrier(cb);
    VkBufferCopy r = {so, dof, n};
    vkb_checkpoint("copy buffer", NULL, NULL);
    vk.vkCmdCopyBuffer(cb, sb, db, 1, &r);
    full_barrier(cb);
}

static void update_buffer(uint64_t dst, uint64_t n, const uint8_t *data) {
    if (watched(dst, n)) {
        static int said;
        float f[4] = {0};
        memcpy(f, data, n < 16 ? n : 16);
        if (said++ < 32) VLOG("[watch] present %llu: uniform update %#llx+%llu from command memory %p: %g %g %g %g",
                              (unsigned long long)vkb_frame(), (unsigned long long)dst, (unsigned long long)n,
                              (const void *)data, f[0], f[1], f[2], f[3]);
    }
    if (g_ok != 1) { memcpy((void *)(uintptr_t)dst, data, n); return; }
    if (!n) return;
    pass_end_why(PE_UPDATE);
    vkb_mirrors_evict(dst, n);
    vkb_pool_use(dst, n, 1);
    vkb_ucap_drop(dst, n);   // an inline uniform update: later draws read the updated memory
    VkBuffer db; VkDeviceSize off;
    if (!vkb_gpu_addr(dst, n, &db, &off)) return;
    VkCommandBuffer cb = vkb_cmd();
    full_barrier(cb);
    // vkCmdUpdateBuffer: 4-byte aligned, at most 65536 bytes per call
    if (!(off & 3) && !(n & 3)) {
        for (uint64_t k = 0; k < n; k += 65536) vk.vkCmdUpdateBuffer(cb, db, off + k, n - k < 65536 ? n - k : 65536, data + k);
    } else {
        vkb_audit(A_UPDATE_SKIPPED, NULL);
        static int said; if (said++ < 4) VLOG("[temporary] unaligned uniform update %#llx+%llu: skipped", (unsigned long long)dst, (unsigned long long)n);
    }
    full_barrier(cb);
}

// ---------------- render passes (milestone 3.3 step 3) ----------------
// SetRenderTargets only records the targets; the pass (dynamic rendering) begins at the first command that renders and
// ends before anything that is not a draw or a clear (copies, other passes, the end of the submission).
static __thread p_set_rt g_rt;       // current render targets (per recording thread)
static _Atomic uint64_t g_ticket;   // presents so far
static __thread int g_in_pass;
static __thread uint32_t g_pass_fb;      // the current pass's feedback targets (vkb.h)
static __thread VkExtent2D g_pass_extent;

VkCommandBuffer vkb_cmd(void) { return t_cb ? t_cb : g_cur >= 0 ? g_slots[g_cur].cb : VK_NULL_HANDLE; }
int vkb_slot(void) { return g_cur; }
int vkb_in_pass(void) { return g_in_pass; }
const p_set_rt *vkb_rt(void) { return &g_rt; }
uint64_t vkb_frame(void) { return atomic_load(&g_ticket); }
int vkb_rt_count(void) { return g_rt.n < 0 ? 0 : g_rt.n > 8 ? 8 : g_rt.n; }
VkExtent2D vkb_rt_extent(void) { return g_pass_extent; }
int vkb_ready(void) { return g_ok == 1; }

// ---- TOTK_GPU_PASSPROF=<present>: GPU time of every render pass of that frame (a timestamp at its start and end; work
// between passes - copies, dispatches, barriers - as gaps). One timestamp pair per pass, not per draw: per-draw
// timestamps serialise the GPU and inflate small draws. The report names each pass by its targets, draws and first
// fragment shader, sorted by time (development).
enum { PP_MAX = 4096 };
typedef struct { uint32_t w, h, ncolor, fmt0, depth, draws; uint64_t draws0; char key[16]; } pp_info;
static VkQueryPool g_ppq;
static pp_info g_pp[PP_MAX];
static int g_ppn, g_pp_done, g_pp_open = -1;
static long long pp_frame(void) {
    static long long at = -1;
    if (at < 0) { const char *e = getenv("TOTK_GPU_PASSPROF"); at = e ? atoll(e) : 0; }
    return at;
}
static int pp_on(void) { return pp_frame() > 0 && !g_pp_done && vkb_frame() == (uint64_t)pp_frame(); }
void vkb_passprof_note(const char *key) {   // vkb_draw.c: the first fragment shader of the open pass
    if (g_pp_open >= 0 && !g_pp[g_pp_open].key[0] && key) snprintf(g_pp[g_pp_open].key, sizeof g_pp[0].key, "%.12s", key);
}
static void pp_begin(VkCommandBuffer cb, VkExtent2D ext) {
    if (!pp_on() || g_ppn >= PP_MAX / 2) return;
    if (!g_ppq) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = PP_MAX};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_ppq), "pass timestamp pool")) { g_pp_done = 1; return; }
        vk.vkResetQueryPool(vkc.device, g_ppq, 0, PP_MAX);
    }
    pp_info *p = &g_pp[g_ppn];
    memset(p, 0, sizeof *p);
    p->w = ext.width; p->h = ext.height;
    p->ncolor = (uint32_t)(g_rt.n < 0 ? 0 : g_rt.n);
    p->fmt0 = g_rt.n > 0 && g_rt.color[0] ? (uint32_t)g_rt.color[0]->d.format : 0;
    p->depth = g_rt.depth ? (uint32_t)g_rt.depth->d.format : 0;
    p->draws0 = vkb_draw_count();
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_ppq, (uint32_t)(2 * g_ppn));
    g_pp_open = g_ppn++;
}
static void pp_end(VkCommandBuffer cb) {
    if (g_pp_open < 0) return;
    g_pp[g_pp_open].draws = (uint32_t)(vkb_draw_count() - g_pp[g_pp_open].draws0);
    vk.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_ppq, (uint32_t)(2 * g_pp_open + 1));
    g_pp_open = -1;
}
static int pp_cmp(const void *a, const void *b) {
    double x = ((const double *)a)[0], y = ((const double *)b)[0];
    return x < y ? 1 : x > y ? -1 : 0;
}
static void pp_report(void) {   // at a present after the frame: wait for the GPU, read, print
    if (!g_ppn || g_pp_done || pp_frame() <= 0 || vkb_frame() <= (uint64_t)pp_frame()) return;
    g_pp_done = 1;
    mtl_finish();
    uint64_t *ts = malloc(sizeof *ts * 2 * (size_t)g_ppn);
    double (*row)[2] = malloc(sizeof *row * (size_t)g_ppn);
    if (!ts || !row) return;
    VkResult r = vk.vkGetQueryPoolResults(vkc.device, g_ppq, 0, (uint32_t)(2 * g_ppn), sizeof *ts * 2 * (size_t)g_ppn, ts, sizeof *ts,
                                          VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) { VLOG("pass profile: results %d", (int)r); return; }
    double per = vkc.props.limits.timestampPeriod / 1e6, inpass = 0, gaps = 0;
    for (int i = 0; i < g_ppn; i++) {
        row[i][0] = ts[2 * i + 1] > ts[2 * i] ? (double)(ts[2 * i + 1] - ts[2 * i]) * per : 0;
        row[i][1] = i;
        inpass += row[i][0];
        if (i && ts[2 * i] > ts[2 * i - 1]) gaps += (double)(ts[2 * i] - ts[2 * i - 1]) * per;
    }
    double span = (double)(ts[2 * g_ppn - 1] - ts[0]) * per;
    VLOG("pass profile of present %lld: %d passes, %.2f ms in passes, %.2f ms between them, %.2f ms first to last",
         pp_frame(), g_ppn, inpass, gaps, span);
    qsort(row, (size_t)g_ppn, sizeof *row, pp_cmp);
    for (int k = 0; k < g_ppn && k < 30; k++) {
        const pp_info *p = &g_pp[(int)row[k][1]];
        VLOG("  %7.3f ms  pass %4d  %ux%u  %u colour (fmt %#x) depth %#x  %u draws  fs %s", row[k][0], (int)row[k][1], p->w, p->h,
             p->ncolor, p->fmt0, p->depth, p->draws, p->key[0] ? p->key : "-");
    }
    free(ts); free(row);
}

static void pass_end(void) {
    if (!g_in_pass) return;
    vk.vkCmdEndRendering(vkb_cmd());
    pp_end(vkb_cmd());
    g_in_pass = 0;
}

static uint32_t level_size(uint32_t v, const view_t *view) {
    uint32_t base = view && view->set_levels ? view->base_level : 0, s = v >> base;
    return s ? s : 1;
}

// 0 when the targets cannot be rendered to (no image yet for a format). Without any target the pass has no attachments
// and covers `bare` (the draw's viewport): TOTK draws with no targets whose fragment shaders write storage buffers (the
// ambient light from the environment cube map goes to the scene buffer that the lighting reads).
// fb: targets that the pass's draws also sample (vkb.h): TOTK's G-buffer decals read the G-buffer they draw into and
// effects read the depth they test against, as on NVN where a bound target may be sampled.
static __thread uint64_t g_pass_serial;   // passes begun on this thread (vkb_pass_serial: per-pass state of the draws)
uint64_t vkb_pass_serial(void) { return g_pass_serial; }
static void shadow_presync(void);
static int pass_begin(VkExtent2D bare, uint32_t fb) {
    if (g_in_pass) return 1;
    shadow_presync();
    g_pass_serial++;
    if (!vkc.has_feedback_loop) fb = 0;
    g_pass_fb = fb;
    VkCommandBuffer cb = vkb_cmd();
    if (vkb_stores_take()) {   // storage writes of earlier draws are visible to this pass (vkb_draw.c g_stores_pending)
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &dep);
    }
    VkRenderingAttachmentInfo color[8] = {0}, depth = {0}, stencil = {0};
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = 1;
    int ncolor = g_rt.n < 0 ? 0 : g_rt.n > 8 ? 8 : g_rt.n, any = 0;
    for (int i = 0; i < ncolor; i++) {
        color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        vkb_image *im = g_rt.color[i] ? vkb_image_of(g_rt.color[i]) : NULL;
        const view_t *v = g_rt.has_cview[i] ? &g_rt.cview[i] : NULL;
        if (im && im->refresh_from) vkb_tex_refresh(cb, im);   // newer contents at this memory in another size (not in a pass yet)
        if (!im || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !vkb_view(im, v, 1, -1)) {
            if (im) vkb_audit(A_RT_NOT_ATTACHABLE, NULL);
            if (im) { static int said; if (said++ < 4) VLOG("[temporary] texture %llu (format %#x) as a render target: not attachable (compressed)",
                                                            (unsigned long long)im->serial, im->d.format); }
            continue;
        }
        int loop = (fb >> i) & 1;
        VkImageLayout layout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkb_image_use(cb, im, layout, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | (loop ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : 0),
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        color[i].imageView = vkb_view(im, v, 1, -1);
        vkb_image_written(im);
        color[i].imageLayout = layout;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        uint32_t lw = level_size(im->width, v), lh = level_size(im->height, v);
        if (lw < w) w = lw;
        if (lh < h) h = lh;
        any = 1;
    }
    vkb_image *dim = g_rt.depth ? vkb_image_of(g_rt.depth) : NULL;
    if (dim && dim->refresh_from) vkb_tex_refresh(cb, dim);
    if (dim && (dim->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        const view_t *v = g_rt.has_dview ? &g_rt.dview : NULL;
        int loop = (fb & VKB_FB_DEPTH) != 0;
        VkImageLayout layout = loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        vkb_image_use(cb, dim, layout,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | (loop ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : 0),
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | (loop ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0));
        VkRenderingAttachmentInfo a = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = vkb_view(dim, v, 1, -1),
            .imageLayout = layout, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        vkb_image_written(dim);
        if (dim->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) depth = a;
        if (dim->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) stencil = a;
        uint32_t lw = level_size(dim->width, v), lh = level_size(dim->height, v);
        if (lw < w) w = lw;
        if (lh < h) h = lh;
        any = 1;
    }
    if (!any) {
        if (ncolor || !bare.width || !bare.height) return 0;   // targets that cannot be attached: not drawn
        w = bare.width < vkc.props.limits.maxFramebufferWidth ? bare.width : vkc.props.limits.maxFramebufferWidth;
        h = bare.height < vkc.props.limits.maxFramebufferHeight ? bare.height : vkc.props.limits.maxFramebufferHeight;
    }
    g_pass_extent = (VkExtent2D){w, h};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = layers,
        .colorAttachmentCount = (uint32_t)ncolor, .pColorAttachments = color,
        .pDepthAttachment = depth.imageView ? &depth : NULL, .pStencilAttachment = stencil.imageView ? &stencil : NULL};
    vkb_checkpoint("begin pass", NULL, NULL);
    pp_begin(cb, g_pass_extent);
    vk.vkCmdBeginRendering(cb, &ri);
    g_in_pass = 1;
    return 1;
}

// TOTK_VK_TRACE=<n>[@<present>]: the first n render-target and clear commands (from that present on), with their
// textures (development).
static long trace_left(void) {
    static long left = -1;
    static unsigned long long from;
    if (left < 0) {
        const char *e = getenv("TOTK_VK_TRACE");
        left = e ? atol(e) : 0;
        from = e && strchr(e, '@') ? strtoull(strchr(e, '@') + 1, NULL, 10) : 0;
    }
    if (atomic_load(&g_ticket) < from) return 0;
    return left > 0 ? left-- : 0;
}
static void trace_tex(const char *what, int i, const tex_t *t, const view_t *v) {
    if (t) VLOG("  %s %d: texture %llu format %#x target %d %ux%ux%u levels %u%s layers %u+%u level %u format %#x @%p", what, i,
                (unsigned long long)t->serial, t->d.format, t->d.target, t->d.width, t->d.height, t->d.depth, t->d.levels,
                v ? " | view" : "", v && v->set_layers ? v->min_layer : 0, v && v->set_layers ? v->nlayers : 0,
                v && v->set_levels ? v->base_level : 0, v && v->set_format ? v->format : 0u, (const void *)t->mem);
}
static void trace_rt(void) {
    if (!trace_left()) return;
    VLOG("[trace] SetRenderTargets %d colour + %s", g_rt.n, g_rt.depth ? "depth" : "no depth");
    for (int i = 0; i < g_rt.n && i < 8; i++) trace_tex("colour", i, g_rt.color[i], g_rt.has_cview[i] ? &g_rt.cview[i] : NULL);
    trace_tex("depth", 0, g_rt.depth, g_rt.has_dview ? &g_rt.dview : NULL);
}

int vkb_pass_begin(VkExtent2D bare, uint32_t fb) { return pass_begin(bare, fb); }
uint32_t vkb_pass_fb(void) { return g_in_pass ? g_pass_fb : 0; }
vkb_cnt vkb_pass_end_n[PE_COUNT];   // passes ended, by reason (the per-frame log line)
static void pass_end_why(int why) { if (g_in_pass) vkb_cnt_add(vkb_pass_end_n[why], 1); pass_end(); }
void vkb_pass_end_r(int why) { pass_end_why(why); }
void vkb_pass_end(void) { pass_end_why(PE_OTHER); }

static void clear_color(const p_clear_color *c) {
    if (trace_left()) {
        VkRect2D s = {0};
        int has = vkb_scissor(&s);
        float v[4] = {0};
        int hv = vkb_viewport0(v);
        VLOG("[trace] ClearColor index %d mask %#x rgba %g %g %g %g scissor %s %d,%d %ux%u viewport %s %g,%g %gx%g", c->index, c->mask, c->rgba[0],
             c->rgba[1], c->rgba[2], c->rgba[3], has ? "set" : "none", s.offset.x, s.offset.y, s.extent.width, s.extent.height, hv ? "set" : "none",
             v[0], v[1], v[2], v[3]);
    }
    if (c->index < 0 || c->index >= 8 || c->index >= g_rt.n || !g_rt.color[c->index]) return;
    static int skip = -2;   // TOTK_VK_SKIP_CLEAR=<index>: colour clears of that target index are not replayed (development A/B)
    if (skip == -2) { const char *e = getenv("TOTK_VK_SKIP_CLEAR"); skip = e ? atoi(e) : -1; }
    if (c->index == skip) return;
    if ((c->mask & 0xF) != 0xF) {   // [temporary] per-channel masked clears need a draw (step 5)
        vkb_audit(A_CLEAR_MASKED, NULL);
        static int said; if (!said++) VLOG("[temporary] clear with channel mask %#x: not replayed yet", c->mask);
        if (!(c->mask & 0xF)) return;
    }
    if (!pass_begin((VkExtent2D){0, 0}, 0)) return;
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index, {.color = {{c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]}}}};
    vkb_image *im = vkb_image_of(g_rt.color[c->index]);
    const nvn_texdesc *d = im ? &im->d : NULL;
    if (d && (d->format == 0x27 || d->format == 0x28 || d->format == 0x2C || d->format == 0x2D || d->format == 0x2F || d->format == 0x30))
        for (int k = 0; k < 4; k++) a.clearValue.color.uint32[k] = (uint32_t)c->rgba[k];   // integer targets (NVN takes float values)
    VkClearRect r = {{{0, 0}, g_pass_extent}, 0, 1};
    vkb_checkpoint("clear attachment", NULL, NULL);
    vk.vkCmdClearAttachments(vkb_cmd(), 1, &a, 1, &r);
}

static void clear_ds(const p_clear_ds *c) {
    if (!g_rt.depth || !pass_begin((VkExtent2D){0, 0}, 0)) return;
    vkb_image *im = vkb_image_of(g_rt.depth);
    if (!im) return;
    VkImageAspectFlags asp = 0;
    if ((im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && c->depth_mask) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((im->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xFF)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp) return;
    if ((asp & VK_IMAGE_ASPECT_STENCIL_BIT) && (c->stencil_mask & 0xFF) != 0xFF) {
        static int said; if (!said++) VLOG("[temporary] stencil clear with write mask %#x: cleared fully", c->stencil_mask);
    }
    VkClearAttachment a = {asp, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    VkClearRect r = {{{0, 0}, g_pass_extent}, 0, 1};
    vkb_checkpoint("clear attachment", NULL, NULL);
    vk.vkCmdClearAttachments(vkb_cmd(), 1, &a, 1, &r);
}

// ClearTexture: a region of one texture outside a pass
static void clear_tex(const p_clear_tex *c) {
    pass_end_why(PE_CLEAR_TEX);
    vkb_image *im = vkb_image_of(c->tex);
    if (!im) return;
    VkCommandBuffer cb = vkb_cmd();
    const view_t *v = c->has_view ? &c->view : NULL;
    uint32_t level = v && v->set_levels ? v->base_level : 0, layer = v && v->set_layers ? v->min_layer : 0;
    uint32_t lw = im->width >> level, lh = im->height >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    int full = c->r.xoffset == 0 && c->r.yoffset == 0 && (uint32_t)c->r.width >= lw && (uint32_t)c->r.height >= lh;
    if (!full || (c->mask & 0xF) != 0xF) {   // [temporary] partial clears need a pass with a scissor (step 5)
        static int said; if (said++ < 4) VLOG("[temporary] partial ClearTexture %dx%d of %ux%u mask %#x: not replayed yet",
                                              c->r.width, c->r.height, lw, lh, c->mask);
        return;
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkb_image_written(im);
    uint32_t nlayers = c->r.depth > 0 ? (uint32_t)c->r.depth : 1;
    VkImageSubresourceRange range = {im->aspect, level, 1, layer + (uint32_t)c->r.zoffset, nlayers};
    if (im->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue cv;
        memcpy(&cv, &c->c, sizeof cv);
        vkb_checkpoint("clear texture", NULL, NULL);
        vk.vkCmdClearColorImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    } else {
        VkClearDepthStencilValue dv = {c->c.f[0], c->c.u[1]};
        vkb_checkpoint("clear depth texture", NULL, NULL);
        vk.vkCmdClearDepthStencilImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &range);
    }
}

void mtl_tex_release(tex_t *t) { vkb_tex_release(t); }

// ---------------- presentation ----------------

// TOTK_SHOT=<present number>:<file.bmp>: saves that presented frame (development check of the picture).
static VkBuffer g_shot_buf;
static VkDeviceMemory g_shot_mem;
static VkBuffer shot_record(VkCommandBuffer cb, uint64_t n, VkImage out, uint32_t w, uint32_t h) {
    static long long at = -1;
    if (at == -1) { const char *e = getenv("TOTK_SHOT"); at = e ? atoll(e) : 0; }
    if (at <= 0 || (uint64_t)at != n) return VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_shot_buf), "shot buffer")) return VK_NULL_HANDLE;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, g_shot_buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_shot_mem), "shot memory")) return VK_NULL_HANDLE;
    vk.vkBindBufferMemory(vkc.device, g_shot_buf, g_shot_mem, 0);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vk.vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot_buf, 1, &r);
    return g_shot_buf;
}
static void shot_write(uint64_t fence, uint32_t w, uint32_t h) {
    const char *e = getenv("TOTK_SHOT"), *path = e ? strchr(e, ':') : NULL;
    mtl_wait(fence, UINT64_MAX);
    void *px = NULL;
    if (path && vk.vkMapMemory(vkc.device, g_shot_mem, 0, VK_WHOLE_SIZE, 0, &px) == VK_SUCCESS) {
        FILE *f = fopen(path + 1, "wb");
        if (f) {   // 32-bit BGRA, top-down
            uint32_t size = w * h * 4;
            uint8_t hdr[54] = {'B', 'M'};
            uint32_t v;
            v = 54 + size; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4);
            int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            memcpy(hdr + 34, &size, 4);
            fwrite(hdr, 1, sizeof hdr, f);
            fwrite(px, 1, size, f);
            fclose(f);
            VLOG("frame saved to %s (%ux%u)", path + 1, w, h);
        }
        vk.vkUnmapMemory(vkc.device, g_shot_mem);
    }
    vk.vkDestroyBuffer(vkc.device, g_shot_buf, NULL);
    vk.vkFreeMemory(vkc.device, g_shot_mem, NULL);
    g_shot_buf = VK_NULL_HANDLE;
}
// TOTK_DUMP=<present number>:<dir>: the frame's render targets (development); TOTK_DUMP_EXIT=1 then exits
static void dump_check(uint64_t n) {
    static long long dump_at = -1;
    static const char *dump_dir;
    if (dump_at == -1) { const char *e = getenv("TOTK_DUMP"); dump_at = e ? atoll(e) : 0; dump_dir = e && strchr(e, ':') ? strchr(e, ':') + 1 : NULL; }
    if (dump_at > 0 && n >= (uint64_t)dump_at && dump_dir) {   // present numbers can skip: the first at or after it
        dump_at = -2;
        vkb_tex_dump(dump_dir);
        if (getenv("TOTK_DUMP_EXIT")) { fflush(NULL); _Exit(0); }   // short development runs: done after the dump
    }
}

uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_ticket, 1) + 1;
    if (n == 1 || n % 300 == 0) {
        uint64_t sl, sm;
        vkb_shader_stats(&sl, &sm);
        uint64_t dn, dsk;
        vkb_draw_stats(&dn, &dsk);
        VLOG("present %llu: shaders loaded %llu, without translation %llu; draws %llu, skipped %llu", (unsigned long long)n,
             (unsigned long long)sl, (unsigned long long)sm, (unsigned long long)dn, (unsigned long long)dsk);
        static uint64_t pb, pi, pp, pd, pn;
        if (n > pn) {
            double f = (double)(n - pn);
            static uint64_t pparts, pgpu, pwait;
            VLOG("memory pools: %d imported, %.1f MiB", g_npools, (double)g_pool_bytes / 1048576.0);
            {
                static uint64_t pct[32];
                char cl[256]; int cn = 0;
                for (int k = 0; k < 32; k++) if (g_counter_types[k] > pct[k]) { cn += snprintf(cl + cn, sizeof cl - (size_t)cn, " type %d: %.1f", k, (double)(g_counter_types[k] - pct[k]) / f); pct[k] = g_counter_types[k]; }
                if (cn) VLOG("per frame: counter reports by type:%s", cl);
            }
            {
                static uint64_t pc, pb, pn, ph, pd;
                uint64_t c, b, ns, h, d;
                vkb_ucap_stats(&c, &b, &ns, &h, &d);
                VLOG("per frame: uniforms copied at submit %.2f MiB in %.0f submissions (%.2f ms on the game thread), bindings from the copy %.0f, dropped by GPU writes %.1f",
                     (double)(b - pb) / 1048576.0 / f, (double)(c - pc) / f, (double)(ns - pn) / 1e6 / f, (double)(h - ph) / f, (double)(d - pd) / f);
                pc = c; pb = b; pn = ns; ph = h; pd = d;
            }
            if (vkb_shadow_on()) {
                static uint64_t pin, pib, pob, pdf, psc, pwp, pwk;
                VLOG("per frame: pool copies %.1f MiB of device memory; in %.0f copies %.2f MiB, back %.2f MiB, "
                     "left in game memory in a pass %.0f, write watch %.2f ms; pages copied back %.0f, left to CPU writes %.1f", (double)vkb_shadow_bytes() / 1048576.0,
                     (double)(g_sh_in - pin) / f, (double)(g_sh_in_bytes - pib) / 1048576.0 / f, (double)(g_sh_out_bytes - pob) / 1048576.0 / f,
                     (double)(g_sh_deferred - pdf) / f, (double)(g_sh_scan_ns - psc) / 1e6 / f,
                     (double)(g_wb_pages - pwp) / f, (double)(g_wb_kept_pages - pwk) / f);
                pwp = g_wb_pages; pwk = g_wb_kept_pages;
                pin = g_sh_in; pib = g_sh_in_bytes; pob = g_sh_out_bytes; pdf = g_sh_deferred; psc = g_sh_scan_ns;
            }
            VLOG("per frame: GPU %.1f ms (whole submissions), recording waited %.1f ms for a free command slot",
                 (double)(g_gpu_ns - pgpu) / 1e6 / f, (double)(g_slot_wait_ns - pwait) / 1e6 / f);
            pgpu = g_gpu_ns; pwait = g_slot_wait_ns;
            static uint64_t pl[LK_COUNT], pn[LK_COUNT];
            uint64_t l0 = atomic_load(&vkb_lock_wait_ns[0]), l1 = atomic_load(&vkb_lock_wait_ns[1]), l2 = atomic_load(&vkb_lock_wait_ns[2]);
            VLOG("per frame: lock waits (all threads) texture registry %.2f ms / %.0f, images %.2f ms / %.0f, pools %.2f ms / %.0f",
                 (double)(l0 - pl[0]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[0]) - pn[0]) / f,
                 (double)(l1 - pl[1]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[1]) - pn[1]) / f,
                 (double)(l2 - pl[2]) / 1e6 / f, (double)(atomic_load(&vkb_lock_waits[2]) - pn[2]) / f);
            for (int k = 0; k < LK_COUNT; k++) { pl[k] = atomic_load(&vkb_lock_wait_ns[k]); pn[k] = atomic_load(&vkb_lock_waits[k]); }
            static uint64_t ps[PK_COUNT], psn[PK_COUNT];
            VLOG("per frame: new images %.2f ms / %.1f, uploads %.2f ms / %.1f, queue submits %.2f ms / %.1f",
                 (double)(vkb_step_ns[0] - ps[0]) / 1e6 / f, (double)(vkb_step_n[0] - psn[0]) / f, (double)(vkb_step_ns[1] - ps[1]) / 1e6 / f,
                 (double)(vkb_step_n[1] - psn[1]) / f, (double)(vkb_step_ns[2] - ps[2]) / 1e6 / f, (double)(vkb_step_n[2] - psn[2]) / f);
            for (int k = 0; k < PK_COUNT; k++) { ps[k] = vkb_step_ns[k]; psn[k] = vkb_step_n[k]; }
            {   // ASTC as BC7 (vkb_bc7.c)
                static uint64_t ph, pmi, pe, pi, pens;
                uint64_t h, mi, en, dr, ens; size_t q;
                vkb_bc7_stats(&h, &mi, &en, &dr, &ens, &q);
                uint64_t vkb_bc7_images(void), bi = vkb_bc7_images();
                VLOG("per frame: ASTC as BC7: %.1f images, %.1f surfaces from the cache, %.1f not there, %.1f encoded in the background "
                     "(%.1f ms each), %.1f MiB queued, %llu dropped", (double)(bi - pi) / f, (double)(h - ph) / f, (double)(mi - pmi) / f,
                     (double)(en - pe) / f, en > pe ? (double)(ens - pens) / 1e6 / (double)(en - pe) : 0.0, q / 1048576.0, (unsigned long long)dr);
                ph = h; pmi = mi; pe = en; pi = bi; pens = ens;
            }
            static uint64_t pm, psh, ptb;
            uint64_t im_made, im_shared, im_back;
            vkb_tex_image_stats(&im_made, &im_shared, &im_back);
            VLOG("per frame: images made %.1f, shared with a live texture %.1f, taken back from the side %.1f",
                 (double)(im_made - pm) / f, (double)(im_shared - psh) / f, (double)(im_back - ptb) / f);
            pm = im_made; psh = im_shared; ptb = im_back;
            VLOG("per frame: %.0f draws, %.0f render passes, %.0f barriers (%.0f image layout changes), %.1f extra GPU submissions", (double)(dn - pd) / f,
                 (double)(vkb_cnt_sum(g_n_passes) - pp) / f, (double)(vkb_cnt_sum(g_n_barriers) - pb) / f,
                 (double)(vkb_cnt_sum(g_n_img_barriers) - pi) / f, (double)(g_parts - pparts) / f);
            pparts = g_parts;
            static const char *const why[PE_COUNT] = {"other", "set targets", "game barrier", "buffer copy", "uniform update", "texture clear",
                "end of submission", "after a storing draw", "snapshot", "sampled targets changed", "texture to prepare", "mirror", "dispatch", "texture copy", "pool copies"};
            static uint64_t prev[PE_COUNT];
            char line[512]; int at = 0;
            for (int k = 0; k < PE_COUNT; k++) {
                uint64_t ended = vkb_cnt_sum(vkb_pass_end_n[k]);
                if (ended > prev[k] && at < (int)sizeof line - 48)
                    at += snprintf(line + at, sizeof line - (size_t)at, " %s %.0f,", why[k], (double)(ended - prev[k]) / f);
                prev[k] = ended;
            }
            if (at) VLOG("passes ended per frame by:%s", line);
        }
        pb = vkb_cnt_sum(g_n_barriers); pi = vkb_cnt_sum(g_n_img_barriers); pp = vkb_cnt_sum(g_n_passes); pd = dn; pn = n;
    }
    vkb_tex_snap_flush();
    vkb_tex_lumlog(t, n);
    vkb_audit_present(n);
    dump_check(n);
    vkb_gprof_report();
    pp_report();
    static int shot_wanted = -1;
    if (shot_wanted < 0) shot_wanted = getenv("TOTK_SHOT") || getenv("TOTK_DUMP");
    vkb_image *im = g_ok == 1 && (vkp_active() || shot_wanted) ? vkb_image_of(t) : NULL;   // headless: only for TOTK_SHOT
    if (!im) { vkp_frame(); return n; }
    VkImage out;
    int slot = vkp_out_acquire(im->width, im->height, &out);
    if (slot < 0) return n;   // the window is behind: this frame is dropped
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = out,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &di);
    VkImageBlit r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}}};
    vk.vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    b.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT; b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT; b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vk.vkCmdPipelineBarrier2(cb, &di);
    VkBuffer shot = shot_record(cb, n, out, im->width, im->height);
    uint64_t fence = mtl_sub_commit();
    if (shot) shot_write(fence, im->width, im->height);
    vkp_out_ready(slot, g_timeline, fence, crop);
    return n;
}

// A long submission goes to the GPU in parts, split where the render targets change (the pass ends there anyway): the
// GPU starts on the first part while the rest is recorded. One part per NVN submission made the GPU wait for the whole
// recording: on the Great Sky Island ~20 ms of recording then ~26 ms of GPU time, one after the other (2026-10-24; as
// SavageWind found for BotW). Events and counters recorded in a part are signalled when that part completes.
// TOTK_VK_SPLIT=<draws> (default 0: one part per submission). Measured on the Great Sky Island without frame pacing: 45.6 ms
// a frame with one part, 49.4 ms with parts of 1000 draws (2026-10-24); with pool copies (GPU 12 ms, recording ~30 ms) 36.9 ms
// with parts of 1500 draws against 36-41 ms with one: no gain - off.
static uint64_t g_part_draws;   // vkb_draw_count() when the current part began
static void split_if_due(void) {
    static long long every = -1;
    if (every < 0) { const char *e = getenv("TOTK_VK_SPLIT"); every = e ? atoll(e) : 0; }
    if (g_ok != 1 || every <= 0 || g_cur < 0) return;
    uint64_t n = vkb_draw_count();
    if (n < g_part_draws) g_part_draws = n;
    if (n - g_part_draws < (uint64_t)every) return;
    mtl_sub_commit();
    mtl_sub_begin();
    g_part_draws = n;
    g_parts++;
}

static __thread int g_render_off;   // SetRenderEnable(false), or a conditional that evaluated to disabled
int vkb_render_suppressed(void) { return g_render_off; }

static const uint8_t *const *g_rest;
static int g_nrest;
void mtl_exec_rest(const uint64_t *h, int n) { g_rest = (const uint8_t *const *)h; g_nrest = n; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }

// The commands of a handle, in one of three ways: one thread replaying everything (EX_ONE), the walk of a submission
// before threads record it (EX_WALK: state, events and counters, conditionals, captured uniforms voided; cuts chunks),
// and a chunk on a recording thread (EX_CHUNK: up to the chunk's end, without events and counters). The position
// (t_seq) advances by one per command read, the same in all three.
enum { EX_ONE, EX_WALK, EX_CHUNK };
typedef struct mt_chunk mt_chunk;
static int walk_op(const uint8_t *p, int hidx);   // EX_WALK: a command at depth 0 (may cut a chunk before it)
static int exec_ops(const uint8_t *p, int depth, int mode, int hidx, const mt_chunk *c);
void mtl_exec(const uint8_t *p) {
    if (trace_left()) VLOG("[trace] command handle %p (submission %llu, present %llu)", (const void *)p, (unsigned long long)(g_fence + 1),
                           (unsigned long long)vkb_frame());
    exec_ops(p, 0, EX_ONE, 0, NULL);
}
static int chunk_end_at(const mt_chunk *c, int hidx, const uint8_t *p);
static void walk_command(uint32_t op, const uint8_t *a);
static int exec_ops(const uint8_t *p, int depth, int mode, int hidx, const mt_chunk *c) {   // 1: stopped at the chunk's end
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        if (mode == EX_CHUNK && !depth && chunk_end_at(c, hidx, p)) return 1;
        if (mode == EX_WALK && !depth) walk_op(p, hidx);
        t_seq++;
        const uint8_t *a = p + 8;
        if (mode != EX_CHUNK) g_op_count[h.op]++;
        if (mode == EX_WALK) {
            if (h.op == OP_END) return 0;
            if (h.op == OP_JUMP) { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            if (h.op == OP_CALL) { const uint8_t *t; memcpy(&t, a, 8); exec_ops(t, depth + 1, mode, hidx, c); }
            else walk_command(h.op, a);
            p += h.size;
            continue;
        }
        switch (h.op) {
        case OP_END: return 0;
        case OP_BEGIN: break;
        case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
        case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); exec_ops(t, depth + 1, mode, hidx, c); break; }
        case OP_SIGNAL_EVENT: {   // a chunk: the walk posted it
            if (mode == EX_CHUNK) break;
            volatile uint32_t *val; uint32_t m, v;
            memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
            mtl_sub_event(val, m, v);
            break;
        }
        case OP_REPORT_COUNTER: { if (mode == EX_CHUNK) break; uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
        case OP_RESET_COUNTER: break;
        case OP_COPY_B2B: {
            uint64_t s, d, n;
            memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
            mtl_sub_copy(s, d, n);
            break;
        }
        case OP_SET_RT: pass_end_why(PE_SET_RT); if (mode == EX_ONE) split_if_due(); memcpy(&g_rt, a, sizeof g_rt); trace_rt(); break;
        case OP_CLEAR_COLOR: if (g_render_off) vkb_cnt_add(g_cond_skipped, 1); else if (g_ok == 1) clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: if (g_render_off) vkb_cnt_add(g_cond_skipped, 1); else if (g_ok == 1) clear_ds((const p_clear_ds *)a); break;
        case OP_CLEAR_TEX: if (g_ok == 1) clear_tex((const p_clear_tex *)a); break;
        case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
        case OP_COPY_B2T: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: if (g_ok == 1) vkb_copy_tex_tex((const p_copy_t2t *)a); break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD:
            vkb_state(h.op, a);
            break;
        case OP_DRAW: case OP_DRAW_IDX:
            if (g_render_off) { vkb_cnt_add(g_cond_skipped, 1); break; }
            if (g_ok == 1) vkb_draw(a, h.op == OP_DRAW_IDX);
            break;
        case OP_DISPATCH: if (g_ok == 1) vkb_dispatch(a); break;
        case OP_BARRIER:   // [temporary] everything before is visible to everything after
            if (g_ok == 1) { pass_end_why(PE_BARRIER); full_barrier(vkb_cmd()); }
            break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_BIND_SAMP: break;
        // rendering enable / conditional rendering: draws and clears are skipped while off (dispatches are not)
        case OP_RENDER_ENABLE: g_render_off = !((const p_flag *)a)->value; break;
        case OP_RENDER_COND:   // a chunk: as the walk evaluated it, in order (in the command's pad: 1 enabled, 2 disabled)
            g_render_off = mode == EX_CHUNK ? ((const p_render_cond *)a)->pad == 2 : !cond_eval((const p_render_cond *)a);
            break;
        case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); update_buffer(d, n, a + 16); break; }
        default:
            vkb_audit(A_OP_NOT_REPLAYED, NULL);
            if (!g_op_said[h.op]++) VLOG("command %u not replayed yet (steps 2-5 of milestone 3.3)", h.op);
            break;
        }
        p += h.size;
    }
}

// Threads recording (vkb_mt_submit): the submission's command buffers after the slot's own, the last one for the work at
// its end (copies back, the GPU end time).
enum { MT_MAXCH = 96 };
static VkCommandBuffer g_mt_list[2 * MT_MAXCH + 2];
static int g_mt_nlist;
uint64_t mtl_sub_commit(void) {
    uint64_t fence = ++g_fence;
    post_op *ops = NULL;
    if (g_npost) {
        ops = malloc((size_t)g_npost * sizeof *ops);
        if (!ops) die("post ops");
        memcpy(ops, g_post, (size_t)g_npost * sizeof *ops);
    }
    int n = g_npost;
    g_npost = 0;
    if (g_ok != 1) { run_posts(ops, n); free(ops); complete(fence); return fence; }
    pass_end_why(PE_SUBMIT);
    sub_long_check(fence);
    VkCommandBuffer last = g_mt_nlist ? g_mt_list[g_mt_nlist - 1] : g_slots[g_cur].cb;
    vkb_mirrors_submit(last);   // written ranges back to game memory before the fence
    shadow_writeback(last);
    dead_tag(fence);
    gpu_time_end(g_cur, last);
    cmd_slot *s = &g_slots[g_cur];
    s->fence = fence;
    vk.vkEndCommandBuffer(s->cb);
    if (g_mt_nlist) vk.vkEndCommandBuffer(last);
    VkCommandBufferSubmitInfo cbi[2 * MT_MAXCH + 3];
    int ncbi = 0;
    cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    for (int i = 0; i < g_mt_nlist; i++)
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_mt_list[i]};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_timeline, .value = fence,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)ncbi, .pCommandBufferInfos = cbi,
                        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    uint64_t ts0 = runtime_now_ns();
    vk_queue_lock();
    VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    vkb_step_add(PK_SUBMIT, ts0);
    if (r != VK_SUCCESS) { vk_check(r, "vkQueueSubmit2"); die("GPU submission failed"); }
    job *j = malloc(sizeof *j);
    if (!j) die("retire job");
    *j = (job){fence, ops, n, g_cur, g_wb_ranges, g_wb_nranges, NULL};   // g_wb_ranges: in the slot's copy back memory
    g_wb_ranges = NULL;
    totk_tl(TL_COMMIT, fence, 1);
    pthread_mutex_lock(&g_mu);
    *g_jobs_tail = j;
    g_jobs_tail = &j->next;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    g_cur = -1;
    static uint64_t said_at;
    if (fence == 1 || fence >= said_at + 3000) {
        said_at = fence;
        VLOG("submission %llu: commands so far draw %llu, indexed %llu, dispatch %llu, clear %llu, set targets %llu, "
             "render enable %llu, conditional render %llu (draws and clears skipped %llu, conditionals unknown %llu)",
             (unsigned long long)fence, (unsigned long long)g_op_count[OP_DRAW], (unsigned long long)g_op_count[OP_DRAW_IDX],
             (unsigned long long)g_op_count[OP_DISPATCH], (unsigned long long)g_op_count[OP_CLEAR_COLOR],
             (unsigned long long)g_op_count[OP_SET_RT], (unsigned long long)g_op_count[OP_RENDER_ENABLE],
             (unsigned long long)g_op_count[OP_RENDER_COND], (unsigned long long)vkb_cnt_sum(g_cond_skipped), (unsigned long long)g_cond_unknown);
    }
    return fence;
}

// ---------------- several recording threads (vkb.h, "vkb_mt.c") ----------------
// One submission of the Great Sky Island is one handle of 5000-7000 draws; recording it took ~25 ms of one thread
// (2026-10-24), the most of the 33 ms frame. Here the translation thread walks it once in order (exec_ops EX_WALK:
// render state, events and counters, conditional rendering, the GPU writes that void captured uniforms) and cuts it into
// chunks of about MT_DRAWS draws at render target changes, each with the state at its start. Recording threads take the
// chunks as they are cut (the translation thread too, after the walk) and record each into a command buffer of their
// own, with their own caches, upload ring and descriptor memory. Work that has to come before every chunk goes into the
// prologue (texture uploads and pool copies: the CPU wrote those bytes before the submission). Images: each chunk keeps
// the layout every image is first needed in and the one it is left in (vkb_trk); a small command buffer before each
// chunk waits for all earlier work and moves its images there. The submission is then one vkQueueSubmit2 of: the slot's
// command buffer (GPU start time, captured uniforms, the move into the prologue's layouts), the prologue, and for each
// chunk its move and itself, then the end (copies back).
// [temporary] Mirrors of written storage buffers are not made (one thread only); TOTK_VK_RECORD_THREADS=1 records as before.
enum { MT_DRAWS = 400 };
struct mt_chunk {
    int h0, h1; const uint8_t *p0, *p1;   // its first command, and the one after its last (h1 == the handle count: the end)
    uint64_t seq;                         // the position before its first command
    void *state; p_set_rt rt; int render_off; int64_t stride;   // the state it starts in
    VkCommandBuffer cb;
    vkb_trk trk;
};
static mt_chunk g_ch[MT_MAXCH];
static int g_nch;
static const uint64_t *g_mt_h;
static int g_mt_n, g_mt_threads;
static pthread_mutex_t g_mt_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mt_cv = PTHREAD_COND_INITIALIZER, g_mt_done_cv = PTHREAD_COND_INITIALIZER;
static int g_mt_avail, g_mt_next, g_mt_done;   // chunks cut / taken / recorded
static uint64_t g_mt_draws;                    // draws in the chunk being cut
static void *g_walk_state;                     // where the walk ended: the next one starts there
static p_set_rt g_walk_rt;
static int g_walk_off, g_walk_have;
static int64_t g_walk_stride;
// the prologue: any recording thread, one at a time
static pthread_mutex_t g_pro_mu = PTHREAD_MUTEX_INITIALIZER;
static VkCommandBuffer g_pro_cb;
static vkb_trk g_pro_trk;
static __thread VkCommandBuffer t_pro_cb;
static __thread vkb_trk *t_pro_trk;
static __thread int t_pro_depth;
// command buffers per slot and recording thread; [VKB_MAXREC]: the prologue's, the moves' and the end's
typedef struct { VkCommandPool pool; VkCommandBuffer *cb; int n, used; } wpool_t;
static wpool_t g_wp[SLOTS][VKB_MAXREC + 1];

int vkb_mt_on(void) {
    static int n = -1;
    if (n < 0) {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int cpus = (int)si.dwNumberOfProcessors;
#else
        int cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 1) cpus = 4;
#endif
        const char *e = getenv("TOTK_VK_RECORD_THREADS");
        n = e && *e ? atoi(e) : cpus >= 8 ? 4 : cpus >= 4 ? 2 : 1;
        if (n > VKB_MAXREC) n = VKB_MAXREC;
        if (n < 1) n = 1;
        static const char *const diag[] = {"TOTK_VK_DRAWTRACE", "TOTK_VK_TRACE", "TOTK_SNAP", "TOTK_GPU_PROF", "TOTK_GPU_PASSPROF",
                                           "TOTK_VK_CHECKPOINTS", "TOTK_AUDIT", "TOTK_VK_SPLIT", "TOTK_VK_STORES_LOG", "TOTK_WATCH"};
        const char *why = NULL;   // development tools that follow the recording in order: one thread
        for (unsigned i = 0; i < sizeof diag / sizeof *diag; i++) if (getenv(diag[i])) why = diag[i];
        if (n > 1 && why) n = 1;
        if (n > 1 && g_ok != 1) n = 1;
        g_mt_threads = n;
        VLOG("recording threads: %d%s%s", n, why ? " (one: " : "", why ? why : "");
        if (why) VLOG("  (one thread while %s is set)", why);
    }
    return n > 1;
}

static VkCommandBuffer wp_get(int slot, int w) {   // a command buffer of that thread's pool for the slot, begun
    wpool_t *p = &g_wp[slot][w];
    if (!p->pool) {
        VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                      .queueFamilyIndex = vkc.family};
        if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &p->pool), "recording command pool")) die("command pool");
    }
    if (p->used == p->n) {
        int add = 16;
        if (!(p->cb = realloc(p->cb, sizeof *p->cb * (size_t)(p->n + add)))) die("command buffers");
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = p->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = (uint32_t)add};
        if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, p->cb + p->n), "command buffers")) die("command buffers");
        p->n += add;
    }
    VkCommandBuffer cb = p->cb[p->used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    return cb;
}
void vkb_mt_slot_reset(int slot) {   // the slot's previous submission has completed (no thread is recording)
    for (int w = 0; w <= VKB_MAXREC; w++)
        if (g_wp[slot][w].used) { vk.vkResetCommandPool(vkc.device, g_wp[slot][w].pool, 0); g_wp[slot][w].used = 0; }
}

void vkb_pro_begin(void) {
    if (!t_recording || t_pro_depth++) return;
    pthread_mutex_lock(&g_pro_mu);
    t_pro_cb = t_cb; t_pro_trk = vkb_trk_get();
    t_cb = g_pro_cb;
    vkb_trk_set(&g_pro_trk);
}
void vkb_pro_end(void) {
    if (!t_recording || --t_pro_depth) return;
    t_cb = t_pro_cb;
    vkb_trk_set(t_pro_trk);
    pthread_mutex_unlock(&g_pro_mu);
}

static int chunk_end_at(const mt_chunk *c, int hidx, const uint8_t *p) { return hidx == c->h1 && p == c->p1; }
static vkb_cnt g_mt_rec_ns;   // chunk recording time, all threads (the log line)
static void mt_record(mt_chunk *c) {
    uint64_t t0 = runtime_now_ns();
    c->cb = wp_get(g_cur, t_worker);
    t_cb = c->cb;
    t_recording = 1;
    vkb_trk_reset(&c->trk);
    vkb_trk_set(&c->trk);
    vkb_rec_begin();
    vkb_state_load(c->state);
    g_rt = c->rt;
    g_render_off = c->render_off;
    vkb_copy_stride(c->stride);
    g_in_pass = 0;
    vkb_stores_take();
    t_seq = c->seq;
    for (int i = c->h0; i < g_mt_n; i++) {
        const uint8_t *p = i == c->h0 ? c->p0 : (const uint8_t *)(uintptr_t)g_mt_h[i];
        if (chunk_end_at(c, i, p) || exec_ops(p, 0, EX_CHUNK, i, c)) break;
    }
    pass_end_why(PE_SUBMIT);
    vk.vkEndCommandBuffer(c->cb);
    vkb_trk_set(NULL);
    t_recording = 0;
    t_cb = VK_NULL_HANDLE;
    vkb_cnt_add(g_mt_rec_ns, runtime_now_ns() - t0);
}
static int mt_take(void) {   // the next chunk cut and not taken, or -1
    pthread_mutex_lock(&g_mt_mu);
    int k = g_mt_next < g_mt_avail ? g_mt_next++ : -1;
    pthread_mutex_unlock(&g_mt_mu);
    return k;
}
static void mt_recorded(void) {
    pthread_mutex_lock(&g_mt_mu);
    g_mt_done++;
    pthread_cond_broadcast(&g_mt_done_cv);
    pthread_mutex_unlock(&g_mt_mu);
}
static void *mt_thread(void *arg) {
    t_worker = (int)(intptr_t)arg;
    char name[32];
    snprintf(name, sizeof name, "totk-record-%d", t_worker);
    pthread_setname_np(name);
    { void plat_thread_boost(void); plat_thread_boost(); }   // the frame waits for these, as for the translation thread
    for (;;) {
        pthread_mutex_lock(&g_mt_mu);
        while (g_mt_next >= g_mt_avail) pthread_cond_wait(&g_mt_cv, &g_mt_mu);
        int k = g_mt_next++;
        pthread_mutex_unlock(&g_mt_mu);
        mt_record(&g_ch[k]);
        mt_recorded();
    }
    return NULL;
}

// ---- the walk
static void chunk_open(int hidx, const uint8_t *p) {   // a chunk starts at p, in the state the walk is in now
    mt_chunk *c = &g_ch[g_nch];
    c->h0 = hidx; c->p0 = p; c->seq = t_seq;
    if (!c->state && !(c->state = malloc(vkb_state_size()))) die("chunk state");
    vkb_state_save(c->state);
    c->rt = g_rt;
    c->render_off = g_render_off;
    c->stride = vkb_copy_stride_get();
    g_mt_draws = 0;
}
static void chunk_close(int hidx, const uint8_t *p) {   // the open chunk ends before p: recording threads may take it
    mt_chunk *c = &g_ch[g_nch];
    c->h1 = hidx; c->p1 = p;
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = ++g_nch;
    pthread_cond_broadcast(&g_mt_cv);
    pthread_mutex_unlock(&g_mt_mu);
}
static int walk_op(const uint8_t *p, int hidx) {   // before a command at depth 0: a new chunk at a render target change
    op_hdr h;
    memcpy(&h, p, sizeof h);
    if (h.op == OP_SET_RT && g_mt_draws >= MT_DRAWS && g_nch < MT_MAXCH - 1) {
        chunk_close(hidx, p);
        chunk_open(hidx, p);
    }
    return 0;
}
static void walk_command(uint32_t op, const uint8_t *a) {
    switch (op) {
    case OP_SIGNAL_EVENT: {
        volatile uint32_t *val; uint32_t m, v;
        memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4);
        mtl_sub_event(val, m, v);
        break;
    }
    case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
    case OP_COPY_B2B: { uint64_t d, n; memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); if (n) vkb_ucap_drop(d, n); break; }   // GPU writes
    case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (n) vkb_ucap_drop(d, n); break; }
    case OP_COPY_T2B: { uint64_t ad, len; if (vkb_copy_t2b_range((const p_copy_b2t *)a, &ad, &len)) vkb_ucap_drop(ad, len); break; }
    case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
    case OP_SET_RT: memcpy(&g_rt, a, sizeof g_rt); break;
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
    case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
    case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD:
        vkb_state(op, a);
        break;
    case OP_DRAW: case OP_DRAW_IDX:
        if (!g_render_off) { g_mt_draws++; vkb_draw_prescan(a, op == OP_DRAW_IDX, 0); }
        break;
    case OP_DISPATCH: g_mt_draws++; vkb_draw_prescan(a, 0, 1); break;
    case OP_RENDER_ENABLE: g_render_off = !((const p_flag *)a)->value; break;
    case OP_RENDER_COND: {   // evaluated here, in order; the chunk reads the answer from the command
        p_render_cond *r = (p_render_cond *)(uintptr_t)a;
        g_render_off = !cond_eval(r);
        r->pad = g_render_off ? 2 : 1;
        break;
    }
    default: break;
    }
}

// Before a chunk: everything earlier is done, and its images move into the layouts it needs first.
static void glue(VkCommandBuffer cb, vkb_trk *t) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkImageMemoryBarrier2 ib[64];
    int ni = 0, first = 1;
    for (int k = 0; k <= t->n; k++) {
        if (k < t->n) {
            vkb_trk_e *e = &t->e[k];
            if (e->im->layout == e->first) continue;
            ib[ni++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = e->im->layout, .newLayout = e->first,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = e->im->img,
                .subresourceRange = {e->im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
            if (ni < 64) continue;
        }
        if (ni || first) {
            VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = first ? 1u : 0u, .pMemoryBarriers = &mb,
                                    .imageMemoryBarrierCount = (uint32_t)ni, .pImageMemoryBarriers = ib};
            vk.vkCmdPipelineBarrier2(cb, &dep);
            ni = 0; first = 0;
        }
    }
    for (int k = 0; k < t->n; k++) t->e[k].im->layout = t->e[k].cur;
}

void vkb_mt_submit(const uint64_t *h, int n) {   // after mtl_sub_begin and the uniform capture; commits
    uint64_t t0 = runtime_now_ns();
    static uint64_t walk_ns, wall_ns, subs, chunks, said_at, glue_images;
    static int started;
    if (!started) {
        started = 1;
        for (int w = 1; w < g_mt_threads; w++) {
            pthread_t t;
            if (pthread_create(&t, NULL, mt_thread, (void *)(intptr_t)w) == 0) pthread_detach(t);
            else die("recording thread");
        }
    }
    g_mt_h = h; g_mt_n = n;
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = g_mt_next = g_mt_done = 0;
    g_nch = 0;
    pthread_mutex_unlock(&g_mt_mu);
    // the prologue: objects made on first use, the pool copies of the last submissions
    g_pro_cb = wp_get(g_cur, VKB_MAXREC);
    vkb_trk_reset(&g_pro_trk);
    t_recording = 1;
    vkb_pro_begin();
    vkb_draw_mt_init();
    vkb_pro_end();
    shadow_presync();
    t_recording = 0;
    uint64_t tw = runtime_now_ns();
    // the walk, from where the last one ended
    if (g_walk_have) { vkb_state_load(g_walk_state); g_rt = g_walk_rt; g_render_off = g_walk_off; vkb_copy_stride(g_walk_stride); }
    g_in_pass = 0;
    chunk_open(0, n ? (const uint8_t *)(uintptr_t)h[0] : NULL);
    for (int i = 0; i < n; i++) exec_ops((const uint8_t *)(uintptr_t)h[i], 0, EX_WALK, i, NULL);
    chunk_close(n, NULL);
    if (!g_walk_state && !(g_walk_state = malloc(vkb_state_size()))) die("walk state");
    vkb_state_save(g_walk_state);
    g_walk_rt = g_rt; g_walk_off = g_render_off; g_walk_stride = vkb_copy_stride_get(); g_walk_have = 1;
    walk_ns += runtime_now_ns() - tw;
    // this thread records chunks too, then waits for the others
    for (int k; (k = mt_take()) >= 0;) { mt_record(&g_ch[k]); mt_recorded(); }
    pthread_mutex_lock(&g_mt_mu);
    while (g_mt_done < g_nch) pthread_cond_wait(&g_mt_done_cv, &g_mt_mu);
    pthread_mutex_unlock(&g_mt_mu);
    // in order: the slot's command buffer moves images into the prologue's layouts, then each chunk's move and the chunk
    vk.vkEndCommandBuffer(g_pro_cb);
    glue(g_slots[g_cur].cb, &g_pro_trk);
    g_mt_nlist = 0;
    g_mt_list[g_mt_nlist++] = g_pro_cb;
    for (int k = 0; k < g_nch; k++) {
        VkCommandBuffer m = wp_get(g_cur, VKB_MAXREC);
        glue_images += (uint64_t)g_ch[k].trk.n;
        glue(m, &g_ch[k].trk);
        vk.vkEndCommandBuffer(m);
        g_mt_list[g_mt_nlist++] = m;
        g_mt_list[g_mt_nlist++] = g_ch[k].cb;
    }
    g_mt_list[g_mt_nlist++] = wp_get(g_cur, VKB_MAXREC);   // the end
    mtl_sub_commit();
    g_mt_nlist = 0;
    wall_ns += runtime_now_ns() - t0;
    subs++; chunks += (uint64_t)g_nch;
    if (subs >= said_at + 600) {   // the log line: where a submission's recording time goes
        said_at = subs;
        static uint64_t prev_rec;
        uint64_t rec = vkb_cnt_sum(g_mt_rec_ns);
        VLOG("recording threads: per submission %.1f ms until submitted (walk %.1f ms), chunks %.1f, chunk recording %.1f ms in all "
             "threads, images moved between chunks %.0f", wall_ns / 1e6 / 600, walk_ns / 1e6 / 600, chunks / 600.0,
             (rec - prev_rec) / 1e6 / 600, glue_images / 600.0);
        prev_rec = rec; wall_ns = walk_ns = chunks = glue_images = 0;
    }
}

uint64_t mtl_last_fence(void) { return g_fence; }
uint64_t mtl_done(void) { pthread_mutex_lock(&g_mu); uint64_t d = g_done; pthread_mutex_unlock(&g_mu); return d; }   // completed by the GPU

int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    pthread_mutex_lock(&g_mu);
    if (g_done >= fence || !timeout_ns) { int ok = g_done >= fence; pthread_mutex_unlock(&g_mu); return ok; }
    uint64_t end = timeout_ns == UINT64_MAX ? UINT64_MAX : runtime_now_ns() + timeout_ns;
    while (g_done < fence) {
        uint64_t now = runtime_now_ns();
        if (now >= end) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = end - now > 100000000 ? 100000000 : end - now;   // re-check at least every 100 ms
        ts.tv_nsec += (long)(wait % 1000000000);
        ts.tv_sec += (time_t)(wait / 1000000000) + ts.tv_nsec / 1000000000;
        ts.tv_nsec %= 1000000000;
        pthread_cond_timedwait(&g_cv, &g_mu, &ts);
    }
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }
