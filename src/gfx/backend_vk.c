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
#ifndef _WIN32
#define _GNU_SOURCE   // sched_getaffinity
#include <sched.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#define CreateDirectoryA(path, sec) mkdir(path, 0777)
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
#include "nvn/totk_vpool.h"

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
static int pass_open(void);
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static uint64_t gpu_ticks(void) { return runtime_now_ns() * 12 / 625; }   // the Switch's 19.2 MHz counter

// ---------------- memory pools ----------------
// sh..defer_pass: the pool's copy in device memory (below, "copies of the pools in device memory"); cmap..cstamp: the
// pool itself as a copy of game memory (Linux, "pool copies")
typedef struct { uintptr_t lo, hi, base; VkBuffer buf; VkDeviceMemory mem; const void *key; VkDeviceSize bytes;
                 VkBuffer sbuf; VkDeviceSize soff; uint32_t sh; uint8_t no_shadow, filled; uint64_t stamp, sync_sub, used_sub, defer_pass, wb_sub;
                 uint8_t *cmap; uint32_t ch; uint8_t cfilled; uint64_t cstamp, pc_sub; } pool_ent;
static pool_ent *g_pools;
static int g_npools, g_capools;
static _Atomic uint64_t g_pool_gen;   // the table changed (a pool added, removed or made unshadowed): vkb_pool_use's cache
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
/* Linux: writers first (TOTK_RWLOCK_WRITER=0: glibc's default, readers first). glibc's default lets readers in while a
   writer waits, and the recording threads read the pool table for nearly every draw, so a writer waited until the
   recording of the frame was done: the game's own streaming calls (a memory pool made: mtl_pool_add takes g_pool_mu
   for writing) took up to 11-23 ms on the Steam Deck and the friend's PC, the game's loader waited in them, its main
   thread for the loader - the 40-30-40 dips while the world streams in, Linux only (Windows' SRW locks let a waiting
   writer go first). Waits are counted as on Windows (the per-frame "lock
   waits" line showed 0.00 on Linux). No reader here takes the same lock again while holding it, which writer
   preference would deadlock. */
static pthread_mutex_t g_rw_init_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t *get_rw(vkb_rw *l) {
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&l->opaque, __ATOMIC_ACQUIRE);
    if (rw) return rw;
    pthread_mutex_lock(&g_rw_init_mu);
    if (!(rw = l->opaque)) {
        static int writer = -1;
        if (writer < 0) { const char *e = getenv("TOTK_RWLOCK_WRITER"); writer = !(e && *e == '0'); }
        rw = (pthread_rwlock_t *)calloc(1, sizeof(pthread_rwlock_t));
        pthread_rwlockattr_t a;
        pthread_rwlockattr_init(&a);
        if (writer) pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
        pthread_rwlock_init(rw, &a);
        pthread_rwlockattr_destroy(&a);
        __atomic_store_n((pthread_rwlock_t **)&l->opaque, rw, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_rw_init_mu);
    return rw;
}
static void rw_waited(int kind, uint64_t t0) {
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}
void vkb_rw_read(vkb_rw *l, int kind) {
    pthread_rwlock_t *rw = get_rw(l);
    if (!pthread_rwlock_tryrdlock(rw)) return;
    uint64_t t0 = runtime_now_ns();
    pthread_rwlock_rdlock(rw);
    rw_waited(kind, t0);
}
void vkb_rw_read_end(vkb_rw *l) { pthread_rwlock_unlock(get_rw(l)); }
void vkb_rw_write(vkb_rw *l, int kind) {
    pthread_rwlock_t *rw = get_rw(l);
    if (!pthread_rwlock_trywrlock(rw)) return;
    uint64_t t0 = runtime_now_ns();
    pthread_rwlock_wrlock(rw);
    rw_waited(kind, t0);
}
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

// Memory allocated through Vulkan (the memory log line every 300 presents), by kind: device-local (video memory),
// host memory, game memory imported (takes no video memory). Counted by wrapping vkAllocateMemory / vkFreeMemory.
enum { MK_DEVICE, MK_HOST, MK_IMPORT, MK_N };
static PFN_vkAllocateMemory g_real_alloc;
static PFN_vkFreeMemory g_real_free;
static _Atomic uint64_t g_mem_bytes[MK_N], g_mem_peak[MK_N], g_mem_count[MK_N];
enum { MEMTAB = 1 << 16 };
static struct { uint64_t mem, size; int kind, tag; } *g_memtab;   // open addressing by handle; mem 0 free, 1 removed
__thread int vkb_mtag;
static _Atomic uint64_t g_tag_bytes[MT_N];
static pthread_mutex_t g_memtab_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_retire;   // the retire thread: it completes the submissions, so it never waits for them
static _Atomic uint64_t g_oom_events, g_oom_recovered;
// Out of memory (the commit limit, the GPU heap's budget - L7, #31): give back the upload memory of idle slots, let
// the GPU finish what is in flight (the frees deferred to it: images, shaders, buffers) and try again, up to ~2 s.
// Pool copies have their own way out (they stay in game memory): one try after the reclaim, no wait.
static VkResult alloc_retry(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m,
                            int tag, VkResult r) {
    uint64_t n = atomic_fetch_add(&g_oom_events, 1) + 1, freed = 0, t0 = runtime_now_ns();
    int tries = tag == MT_POOL_COPY ? 1 : 40;
    for (int t = 0; t < tries && (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY); t++) {
        freed += vkb_ring_reclaim();
        if (tries > 1) {
            if (!t_retire) mtl_wait(mtl_last_fence(), 50000000);
            struct timespec ts = {0, 25000000};   // the page file may grow meanwhile
            nanosleep(&ts, NULL);
        }
        r = g_real_alloc(d, ai, a, m);
    }
    if (r == VK_SUCCESS) atomic_fetch_add(&g_oom_recovered, 1);
    if (n <= 20 || r != VK_SUCCESS)
        VLOG("out of memory for %llu MiB (use %d): %s after %.0f ms, %.0f MiB of upload memory given back (%llu so far, %llu recovered)",
             (unsigned long long)(ai->allocationSize >> 20), tag, r == VK_SUCCESS ? "allocated" : "still failing",
             (runtime_now_ns() - t0) / 1e6, freed / 1048576.0, (unsigned long long)n, (unsigned long long)atomic_load(&g_oom_recovered));
    return r;
}
static VkResult VKAPI_CALL count_alloc(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *m) {
    VkResult r = g_real_alloc(d, ai, a, m);
    int tag = vkb_mtag;
    vkb_mtag = MT_OTHER;
    int kind = MK_HOST;
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext) if (p->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) kind = MK_IMPORT;
    if (r != VK_SUCCESS && kind != MK_IMPORT) r = alloc_retry(d, ai, a, m, tag, r);
    if (r != VK_SUCCESS) return r;
    if (kind == MK_IMPORT) tag = -1;   // game memory, not ours
    else atomic_fetch_add(&g_tag_bytes[tag], ai->allocationSize);
    if (kind != MK_IMPORT && ai->memoryTypeIndex < vkc.memory.memoryTypeCount &&
        (vkc.memory.memoryHeaps[vkc.memory.memoryTypes[ai->memoryTypeIndex].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) kind = MK_DEVICE;
    uint64_t n = atomic_fetch_add(&g_mem_bytes[kind], ai->allocationSize) + ai->allocationSize, pk = atomic_load(&g_mem_peak[kind]);
    while (n > pk && !atomic_compare_exchange_weak(&g_mem_peak[kind], &pk, n)) {}
    atomic_fetch_add(&g_mem_count[kind], 1);
    pthread_mutex_lock(&g_memtab_mu);
    if (!g_memtab && !(g_memtab = calloc(MEMTAB, sizeof *g_memtab))) die("memory table");
    uint64_t h = (uint64_t)(uintptr_t)*m;
    for (uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 48), k = 0; k < MEMTAB; i = (i + 1) & (MEMTAB - 1), k++)
        if (g_memtab[i].mem <= 1) { g_memtab[i].mem = h; g_memtab[i].size = ai->allocationSize; g_memtab[i].kind = kind; g_memtab[i].tag = tag; break; }
    pthread_mutex_unlock(&g_memtab_mu);
    return r;
}
static void VKAPI_CALL count_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    if (m && g_memtab) {
        uint64_t h = (uint64_t)(uintptr_t)m;
        pthread_mutex_lock(&g_memtab_mu);
        for (uint32_t i = (uint32_t)(h * 0x9E3779B97F4A7C15ull >> 48), k = 0; k < MEMTAB && g_memtab[i].mem; i = (i + 1) & (MEMTAB - 1), k++)
            if (g_memtab[i].mem == h) {
                atomic_fetch_sub(&g_mem_bytes[g_memtab[i].kind], g_memtab[i].size);
                atomic_fetch_sub(&g_mem_count[g_memtab[i].kind], 1);
                if (g_memtab[i].tag >= 0) atomic_fetch_sub(&g_tag_bytes[g_memtab[i].tag], g_memtab[i].size);
                g_memtab[i].mem = 1;
                break;
            }
        pthread_mutex_unlock(&g_memtab_mu);
    }
    g_real_free(d, m, a);
}
// The memory log line: what the driver counts for this process per heap (VK_EXT_memory_budget) and what we allocated.
static void memory_log(void) {
    char drv[256] = "";
    if (vkc.has_memory_budget) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT b = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
        vk.vkGetPhysicalDeviceMemoryProperties2(vkc.gpu, &p);
        int n = 0;
        for (uint32_t h = 0; h < p.memoryProperties.memoryHeapCount && n < (int)sizeof drv - 64; h++)
            n += snprintf(drv + n, sizeof drv - (size_t)n, " heap %u%s %.0f of %.0f MiB;", h,
                          p.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " (video)" : "",
                          b.heapUsage[h] / 1048576.0, b.heapBudget[h] / 1048576.0);
    }
    VLOG("memory: driver (this process, used of budget):%s ours: video memory %.0f MiB in %llu (peak %.0f), of it pool copies %.0f; "
         "host %.0f MiB (peak %.0f); game memory imported %.0f MiB", drv[0] ? drv : " -", atomic_load(&g_mem_bytes[MK_DEVICE]) / 1048576.0,
         (unsigned long long)atomic_load(&g_mem_count[MK_DEVICE]), atomic_load(&g_mem_peak[MK_DEVICE]) / 1048576.0, vkb_shadow_bytes() / 1048576.0,
         atomic_load(&g_mem_bytes[MK_HOST]) / 1048576.0, atomic_load(&g_mem_peak[MK_HOST]) / 1048576.0, atomic_load(&g_mem_bytes[MK_IMPORT]) / 1048576.0);
    {   // ours by use (vkb_mtag)
        static const char *const nm[MT_N] = {"other", "images", "upload rings", "descriptors", "pool copies", "mirrors", "uniform copies",
                                             "write-back", "copy back"};
        char line[512]; int n = 0;
        for (int t = 0; t < MT_N && n < (int)sizeof line - 48; t++)
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %.0f MiB;", nm[t], atomic_load(&g_tag_bytes[t]) / 1048576.0);
        VLOG("memory: ours by use:%s", line);
    }
    vkb_shader_report();
    uint64_t rc, rb, rp, rpk, rmade, rfreed;
    vkb_ring_stats(&rc, &rb, &rp);
    vkb_ring_counts(&rpk, &rmade, &rfreed);
    VLOG("memory: upload rings %.0f MiB (peak %.0f) in %llu chunks over %llu slot x thread pairs; chunks made %llu, given back %llu",
         rb / 1048576.0, rpk / 1048576.0, (unsigned long long)rc, (unsigned long long)rp, (unsigned long long)rmade,
         (unsigned long long)rfreed);
    void plat_memory_map(void);   // runtime/platform.h
    static int map = -1;
    if (map < 0) map = getenv("TOTK_MEMMAP") != NULL;
    if (map) {   // development: the whole process by kind (slow)
        plat_memory_map();
        uint64_t vb, vn;
        totk_vpool_host_stats(&vb, &vn);
        VLOG("memory: CPU_NO_ACCESS physical pools' own storage %.0f MiB in %llu", vb / 1048576.0, (unsigned long long)vn);
        static const char *const tn[MT_N] = {"other", "images", "upload rings", "descriptors", "pool copies", "mirrors",
                                             "uniform copies", "write-back", "copy back"};
        char tl[384];
        int at = 0;
        for (int t = 0; t < MT_N; t++)
            at += snprintf(tl + at, sizeof tl - (size_t)at, " %s %.0f;", tn[t], atomic_load(&g_tag_bytes[t]) / 1048576.0);
        VLOG("memory: ours by use (MiB):%s", tl);
    }
}

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
            g_real_alloc = vk.vkAllocateMemory; vk.vkAllocateMemory = count_alloc;
            g_real_free = vk.vkFreeMemory; vk.vkFreeMemory = count_free;
        }
        vkb_state_reset();
        VLOG("replayer %s", ok ? "on Vulkan" : "without a GPU: submissions complete at once");
        atomic_store(&g_ok, ok);
    }
    pthread_mutex_unlock(&mu);
}

// Host memory [a, b) (aligned) imported as a buffer the GPU reads and writes in place. 0: not importable.
static int import_host(uintptr_t a, uintptr_t b, VkBuffer *out_buf, VkDeviceMemory *out_mem) {
    // The import needs committed memory: guest pages are committed on first touch, so the range is committed now.
    // A virtual pool is an inaccessible address range (totk_vpool_win.c): no memory to import (sparse binding, later).
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi;
    if (a >= GUEST_LO && b <= GUEST_HI) plat_commit(a, b - a);
    else if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect == PAGE_NOACCESS) return 0;
#else
    if (a >= GUEST_LO && b <= GUEST_HI) plat_commit(a, b - a);
#endif
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!vk_check(vk.vkGetMemoryHostPointerPropertiesEXT(vkc.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                          (void *)a, &hp), "host pointer properties")) return 0;
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, .size = b - a,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &buf), "pool buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, buf, &req);
    uint32_t type = memory_type(req.memoryTypeBits & hp.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)a};
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = b - a, .memoryTypeIndex = type};
    VkDeviceMemory dm;
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &dm), "pool import")) {
        VLOG("memory %#llx..%#llx: cannot import (memory types %#x / %#x)", (unsigned long long)a, (unsigned long long)b, req.memoryTypeBits, hp.memoryTypeBits);
        vk.vkDestroyBuffer(vkc.device, buf, NULL);
        return 0;
    }
    vk.vkBindBufferMemory(vkc.device, buf, dm, 0);
    *out_buf = buf; *out_mem = dm;
    return 1;
}

// ---------------- pool copies (Linux) ----------------
// amdgpu checks every imported host range (a userptr) at every submission, page by page: on the Steam Deck the two
// vkQueueSubmit2 calls of a frame took ~15 ms each with TOTK's ~1.1 GiB of imported pools (2026-11-01), and with
// imports the write watch cannot work (runtime/platform/linux.c). So on Linux each pool is a host-visible buffer of
// our own holding a copy of its game
// memory at the same offsets, and the two are kept the same around the GPU's work:
//  - CPU -> copy: right before a submission is submitted (pc_sync), the pools it uses get the pages written since their
//    copy was last brought up to date (the write watch, nvn_dirty_runs); a pool not tracked gets all of it.
//  - GPU -> game memory: the GPU writes recorded in a submission (vkb_pool_use) are copied from the copy into game
//    memory when it has completed (pc_writeback, on the retire thread, before its events and counters let the game
//    see it done), as GPU writes (nvn_dirty_gpu_write_*: the next sync does not take them for the CPU's).
//  - Both wrote: byte by byte, as the device copies on Windows do it (wb_merge, wb_bytes). A recorded GPU write is the
//    whole range a shader may store to, mostly far more than it stores, and TOTK binds the same ranges every frame, so
//    there is nearly always an earlier submission's write not back yet. When the copies left those ranges out whole
//    (our rule until 2026-11-01), the CPU's updates to the bytes the GPU does not store never
//    reached the GPU, and the copy back put the copy's old bytes over them in game memory (L20 on the Steam Deck: the
//    Depths ground not drawn, Link lit wrongly). Now each GPU write keeps game memory's bytes as of its submission's
//    sync (its snapshot, `was`): a byte that differs from it later is the CPU's, newer than the GPU's work (the game
//    writes a buffer for its next use after the GPU is done with it). Such bytes go into the copy at the next sync,
//    and the copy back keeps them in game memory (and marks their page written, so that the copy takes it again);
//    the other bytes of the range get the GPU's. A range over PC_SNAP_MAX, and a whole copy (a pool's first, or one
//    not tracked), keep the old rule: the GPU's bytes win.
// TOTK_POOL_IMPORT=1 imports instead (development); TOTK_POOL_COPY=1 copies on Windows too (development; with
// TOTK_VK_POOL_SHADOW=0 as on the Deck).
int vkb_pool_copies(void) {
    static int v = -1;
    if (v < 0) {
        const char *imp = getenv("TOTK_POOL_IMPORT"), *cp = getenv("TOTK_POOL_COPY");
#ifdef _WIN32
        v = cp && !strcmp(cp, "1");
#else
        v = !(imp && !strcmp(imp, "1"));
        (void)cp;
#endif
        if (imp && !strcmp(imp, "1")) v = 0;
        VLOG("memory pools: %s", v ? "copies of game memory in host-visible memory, kept current by the write watch" : "game memory imported");
    }
    return v;
}
static const pool_ent *pool_find(uintptr_t addr, uint64_t len);
// A GPU write into the copy of the pool whose buffer is buf (a pool made again at the same address is another one);
// fence 0: in the submission being recorded. was: game memory's bytes [lo, hi) at its submission's sync (NULL: none
// taken, the range is over PC_SNAP_MAX or it is still being recorded).
typedef struct { uint64_t lo, hi, fence; VkBuffer buf; uint8_t *was; } pc_range;
enum { PC_SNAP_MAX = 16 << 20 };
static pc_range *g_pc_pend;   // g_pc_mu: GPU writes not yet in game memory
static int g_npc_pend, g_cappc_pend;
static pthread_mutex_t g_pc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pc_in_bytes, g_pc_whole, g_pc_back_bytes, g_pc_kept, g_pc_snap_bytes, g_pc_ns;   // the per-frame log line (g_pc_mu)
static void pc_pend_add(VkBuffer buf, uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_pc_mu);
    for (int k = g_npc_pend - 1; k >= 0 && k >= g_npc_pend - 8; k--)   // written again in this submission
        if (!g_pc_pend[k].fence && g_pc_pend[k].buf == buf && g_pc_pend[k].lo == addr && g_pc_pend[k].hi == addr + len) { pthread_mutex_unlock(&g_pc_mu); return; }
    if (g_npc_pend == g_cappc_pend) {
        g_cappc_pend = g_cappc_pend ? g_cappc_pend * 2 : 1024;
        if (!(g_pc_pend = realloc(g_pc_pend, sizeof *g_pc_pend * (size_t)g_cappc_pend))) die("pool copies: GPU writes");
    }
    g_pc_pend[g_npc_pend++] = (pc_range){addr, addr + len, 0, buf, NULL};
    pthread_mutex_unlock(&g_pc_mu);
}
// g_pc_mu held: game memory [a, b) into p's copy. Inside an earlier submission's GPU write not back yet (from index i
// on) only the bytes the CPU changed since that write's snapshot; with no snapshot, or in a whole copy, none.
static void pc_put(const pool_ent *p, uint64_t a, uint64_t b, int i, int whole) {
    for (; i < g_npc_pend; i++) {
        const pc_range *r = &g_pc_pend[i];
        if (!r->fence || r->lo >= b || r->hi <= a) continue;
        uint64_t lo = r->lo > a ? r->lo : a, hi = r->hi < b ? r->hi : b;
        if (lo > a) pc_put(p, a, lo, i + 1, whole);
        if (!whole && r->was) {
            const uint8_t *g = (const uint8_t *)(uintptr_t)lo, *w = r->was + (lo - r->lo);
            uint8_t *c = p->cmap + (lo - p->base);
            for (uint64_t n = hi - lo, k = 0; k < n;) {
                uint64_t m = n - k < 64 ? n - k : 64;
                if (memcmp(g + k, w + k, m)) for (uint64_t x = k; x < k + m; x++) if (g[x] != w[x]) c[x] = g[x];
                k += m;
            }
            g_pc_in_bytes += hi - lo;
        }
        if (hi < b) pc_put(p, hi, b, i + 1, whole);
        return;
    }
    memcpy(p->cmap + (a - p->base), (const void *)(uintptr_t)a, b - a);
    g_pc_in_bytes += b - a;
}
static uint64_t g_pc_copy_ns;   // of g_pc_ns: copying (the rest is the write watch's scan; the log line, g_pc_mu)
static void pc_run(void *ctx, size_t off, size_t n) {   // nvn_dirty_runs: off from the pool's start
    const pool_ent *p = ctx;
    uint64_t t0 = runtime_now_ns();
    pc_put(p, p->lo + off, p->lo + off + n, 0, 0);
    g_pc_copy_ns += runtime_now_ns() - t0;
}
// Before submission `fence` is submitted (g_pool_mu read, g_pc_mu held): the pools it used brought up to date.
static void pc_sync_pool(pool_ent *p) {
    if (!p->cfilled) {   // the first use: all of it (writes up to now are in it)
        nvn_dirty_since((const void *)p->lo, p->hi - p->lo, UINT64_MAX);
        p->cstamp = nvn_dirty_now();
        uint64_t t0 = runtime_now_ns();
        pc_put(p, p->lo, p->hi, 0, 1);
        g_pc_copy_ns += runtime_now_ns() - t0;
        p->cfilled = 1;
        return;
    }
    // The stamp before the look: a page another thread's scan stamps meanwhile is copied again next time, not missed
    // (the pages this look finds written are newer than it too: copied twice, a few MiB a frame).
    uint64_t stamp = nvn_dirty_now();
    int r = nvn_dirty_runs((const void *)p->lo, p->hi - p->lo, p->cstamp, 0, pc_run, p);
    p->cstamp = stamp;
    // Not tracked: in the guest window anything may have written it, so all of it; outside (a pool whose storage is
    // ours, totk_vpool_win.c) the game cannot write it, and its first copy holds what the CPU gave it.
    if (r < 0 && p->lo >= GUEST_LO && p->hi <= GUEST_HI) { pc_put(p, p->lo, p->hi, 0, 1); g_pc_whole++; }
}
static void pc_sync(uint64_t fence) {
    if (!vkb_pool_copies()) return;
    uint64_t t0 = runtime_now_ns();
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pthread_mutex_lock(&g_pc_mu);
    // once a submission: with partial submission (vkb_mt_submit) at the first part that uses the pool, as the GPU may be
    // reading the copy for the parts already submitted
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].cmap && g_pools[i].pc_sub != fence && __atomic_load_n(&g_pools[i].used_sub, __ATOMIC_RELAXED) == fence) {
            pc_sync_pool(&g_pools[i]);
            g_pools[i].pc_sub = fence;
        }
    uint64_t ts = runtime_now_ns();
    for (int k = 0; k < g_npc_pend; k++) {   // this submission's GPU writes: game memory as the GPU starts from it
        pc_range *r = &g_pc_pend[k];
        if (r->fence) continue;
        r->fence = fence;
        if (r->hi - r->lo <= PC_SNAP_MAX && (r->was = malloc(r->hi - r->lo))) {
            memcpy(r->was, (const void *)(uintptr_t)r->lo, r->hi - r->lo);
            g_pc_snap_bytes += r->hi - r->lo;
        }
    }
    g_pc_copy_ns += runtime_now_ns() - ts;
    g_pc_ns += runtime_now_ns() - t0;
    pthread_mutex_unlock(&g_pc_mu);
    vkb_rw_read_end(&g_pool_mu);
}
// g_pc_mu held: write r of a completed submission, from p's copy into game memory. With a snapshot, a byte the CPU
// changed since it stays (its page is marked written again: an atomic no-op on one byte, for the next sync to copy);
// the GPU's bytes put in are also put into the snapshots of later writes not back yet (from index k on): for those
// they are what their GPU started from, not changes of the CPU.
static void pc_back(const pool_ent *p, const pc_range *r, int k) {
    guest_prepare_write((void *)(uintptr_t)r->lo, r->hi - r->lo);
    void *tok = nvn_dirty_gpu_write_begin((void *)(uintptr_t)r->lo, r->hi - r->lo);
    const uint8_t *c = p->cmap + (r->lo - p->base);
    uint8_t *g = (uint8_t *)(uintptr_t)r->lo;
    uint64_t n = r->hi - r->lo, kept = 0;
    // the first byte the CPU kept on each page (marked written again below), up to KMAX; more: found by a scan
    enum { KMAX = 64 };
    uint64_t kept_at[KMAX];
    // Blocks of 4 KiB where game memory still holds the snapshot (nearly all: the CPU rarely writes what the GPU
    // writes) go over whole; a block that differs, by 64 bytes; only a 64-byte piece that differs, byte by byte (the
    // retire thread took ~18 % of a Steam Deck core byte by byte, 2026-11-04).
    if (!r->was) memcpy(g, c, n);
    else
        for (uint64_t x = 0, kept_page = UINT64_MAX; x < n;) {
            uint64_t big = n - x < 4096 ? n - x : 4096;
            if (!memcmp(g + x, r->was + x, big)) { memcpy(g + x, c + x, big); x += big; continue; }
            for (uint64_t eb = x + big; x < eb;) {
                uint64_t m = eb - x < 64 ? eb - x : 64;
                if (!memcmp(g + x, r->was + x, m)) { memcpy(g + x, c + x, m); x += m; continue; }
                for (uint64_t e = x + m; x < e; x++) {
                    if (g[x] == r->was[x]) { g[x] = c[x]; continue; }
                    uint64_t page = (r->lo + x) >> 12;
                    if (page != kept_page) { if (kept < KMAX) kept_at[kept] = x; kept_page = page; kept++; }
                }
            }
        }
    nvn_dirty_gpu_write_end(tok);   // before the marks below: they are the CPU's
    if (kept <= KMAX)
        for (uint64_t i = 0; i < kept; i++) __atomic_fetch_or(&g[kept_at[i]], 0, __ATOMIC_RELAXED);
    else
        for (uint64_t x = 0; x < n; x++)   // the first kept byte of each page
            if (g[x] != c[x]) { __atomic_fetch_or(&g[x], 0, __ATOMIC_RELAXED); x += 0xfff - ((r->lo + x) & 0xfff); }
    g_pc_kept += kept;
    for (int j = k; j < g_npc_pend; j++) {   // the GPU's bytes into the snapshots of later writes over them
        pc_range *o = &g_pc_pend[j];
        if (o == r || !o->was || o->buf != r->buf || o->lo >= r->hi || o->hi <= r->lo) continue;
        uint64_t lo = o->lo > r->lo ? o->lo : r->lo, hi = o->hi < r->hi ? o->hi : r->hi;
        for (uint64_t a = lo; a < hi;) {   // where game memory now holds the copy's bytes (all but the kept ones): whole
            uint64_t m = hi - a < 4096 ? hi - a : 4096;
            if (!memcmp(g + (a - r->lo), c + (a - r->lo), m)) { memcpy(o->was + (a - o->lo), c + (a - r->lo), m); a += m; continue; }
            for (uint64_t e = a + m; a < e; a++) if (g[a - r->lo] == c[a - r->lo]) o->was[a - o->lo] = c[a - r->lo];
        }
    }
    g_pc_back_bytes += n;
}
// Retire thread, submission `fence` complete: its GPU writes from the copies into game memory (pc_back).
static void pc_writeback(uint64_t fence) {
    if (!vkb_pool_copies()) return;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pthread_mutex_lock(&g_pc_mu);
    for (int k = 0; k < g_npc_pend;) {
        pc_range r = g_pc_pend[k];
        if (!r.fence || r.fence > fence) { k++; continue; }
        g_pc_pend[k] = g_pc_pend[--g_npc_pend];   // out of the list first: pc_back walks the others
        const pool_ent *p = pool_find((uintptr_t)r.lo, r.hi - r.lo);
        if (p && p->buf == r.buf && p->cmap) pc_back(p, &r, 0);   // else the pool is gone, and with it what its GPU writes meant
        free(r.was);
    }
    pthread_mutex_unlock(&g_pc_mu);
    vkb_rw_read_end(&g_pool_mu);
}
// A pool as a host-visible copy of [a, b) (pc_sync fills it at its first use). 0: no memory.
static int pc_alloc(uintptr_t a, uintptr_t b, VkBuffer *out_buf, uint8_t **map, uint32_t *h) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = b - a,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &buf), "pool copy buffer")) return 0;
    if (!(*h = vkb_poolmem_alloc(buf, map))) {
        VLOG("memory %#llx..%#llx: no memory for its copy", (unsigned long long)a, (unsigned long long)b);
        vk.vkDestroyBuffer(vkc.device, buf, NULL);
        return 0;
    }
    *out_buf = buf;
    return 1;
}

void mtl_pool_add(const void *mem, uint64_t size) {
    backend_init();
    if (!g_ok || !mem || !size) return;
    int copy = vkb_pool_copies();
    uintptr_t align = copy ? 4096 : vkc.host_pointer_alignment ? vkc.host_pointer_alignment : 4096;
    uintptr_t lo = (uintptr_t)mem, hi = lo + size, a = lo & ~(align - 1), b = (hi + align - 1) & ~(align - 1);
    VkBuffer buf;
    VkDeviceMemory dm = VK_NULL_HANDLE;
    uint8_t *cmap = NULL;
    uint32_t ch = 0;
    if (copy ? !pc_alloc(a, b, &buf, &cmap, &ch) : !import_host(a, b, &buf, &dm)) return;
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    if (g_npools == g_capools) {
        g_capools = g_capools ? g_capools * 2 : 64;
        g_pools = realloc(g_pools, (size_t)g_capools * sizeof *g_pools);
        if (!g_pools) die("pool table");
    }
    int i = g_npools;
    while (i > 0 && g_pools[i - 1].lo > lo) { g_pools[i] = g_pools[i - 1]; i--; }
    g_pools[i] = (pool_ent){lo, hi, a, buf, dm, mem, b - a};
    g_pools[i].cmap = cmap; g_pools[i].ch = ch;
    g_npools++;
    atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release);
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
    else if (vkc.has_checkpoints || vkc.has_buffer_marker)   // with the deletions: a lost device's copies against the pool table
        VLOG("memory pool %p %#llx..%#llx imported as %#llx..%#llx (present %llu)", mem, (unsigned long long)lo, (unsigned long long)hi,
             (unsigned long long)a, (unsigned long long)b, (unsigned long long)vkb_frame());
    vkb_rw_write_end(&g_pool_mu);
}

static void destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem, VkBuffer buf);
static void wb_held_drop(uint64_t lo, uint64_t hi);   // GPU-only copy backs held (below)
void mtl_pool_del(const void *mem) {
    if (g_ok != 1) return;
    pool_ent e = {0};
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    for (int i = 0; i < g_npools; i++)
        if (g_pools[i].key == mem) {
            e = g_pools[i];
            memmove(&g_pools[i], &g_pools[i + 1], (size_t)(g_npools - i - 1) * sizeof *g_pools);
            g_npools--;
            atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release);
            g_pool_bytes -= e.hi - e.lo;
            break;
        }
    if (e.sh) vkb_shadow_free(e.sh);
    if (e.ch) vkb_poolmem_free(e.ch);   // reused after the submission being recorded, as the buffer goes (below)
    if (e.buf) wb_forget(e.buf);
    vkb_rw_write_end(&g_pool_mu);
    if (!e.buf) return;
    wb_held_drop(e.lo, e.hi);
    if (vkc.has_checkpoints || vkc.has_buffer_marker) VLOG("memory pool %p %#llx..%#llx deleted (present %llu)", mem, (unsigned long long)e.lo, (unsigned long long)e.hi, (unsigned long long)vkb_frame());
    // No wait for the GPU here (the translation thread deletes pools in queue order since 2026-10-28, hle_nvn.c
    // x_poolrel): the game waited for the GPU work that used the pool; its copy in device memory is reused after the
    // submission being recorded (vkb_shadow_free), the import goes after it too (below).
    // A submission of another game thread may be being recorded already and have copied from the pool (the copies of
    // recently used pools at its start, sync_gather): the import goes when that submission is done (destroy_later).
    destroy_later(VK_NULL_HANDLE, VK_NULL_HANDLE, e.mem, e.buf);
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
//    In a page the CPU also wrote, the GPU gets the bytes the CPU left unchanged (snap_take, wb_bytes); without a
//    snapshot (the CPU wrote there before the GPU write was recorded) the page stays the CPU's (counted in the log line).
// Pages the copy back writes are not CPU writes; were the write watch to count them, they would only be copied in again.
// The game writes memory for a submission before it is submitted, and does not overwrite what the GPU still reads (it
// waits for the GPU first), so bringing a copy up to date early (ahead, or for the whole pool) does not change results.
// Integrated GPUs (the Steam Deck) read host memory at full speed: no copies there (vkb_shadow_on).
typedef struct { VkBuffer src, dst; VkDeviceSize soff, doff, n; uint64_t base, stamp; } wb_t;   // base: the pool's import address; stamp: its copy's
static wb_t *g_wb;                 // GPU-written ranges of copies in this submission: back to game memory at its end
static int g_nwb, g_capwb;
static VkBufferCopy *g_runs;       // copies being gathered (sync_gather / sync_flush)
static int g_nruns, g_caruns;
typedef struct { VkBuffer src, dst; int first, n; uint64_t base; } run_grp;   // base: the source pool's import address
static run_grp *g_grp;
static int g_ngrp, g_capgrp;
static uint64_t g_shadow_budget, g_presync_sub;
static uint64_t g_sh_in, g_sh_in_bytes, g_sh_out_bytes, g_sh_deferred, g_sh_scan_ns;   // the per-frame log line

static void shadow_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
}
static void run_put(VkBuffer src, VkBuffer dst, VkDeviceSize soff, VkDeviceSize doff, VkDeviceSize n, uint64_t base) {
    if (g_nruns == g_caruns) {
        g_caruns = g_caruns ? g_caruns * 2 : 256;
        if (!(g_runs = realloc(g_runs, sizeof *g_runs * (size_t)g_caruns))) die("pool copy runs");
    }
    if (!g_ngrp || g_grp[g_ngrp - 1].src != src || g_grp[g_ngrp - 1].dst != dst) {
        if (g_ngrp == g_capgrp) {
            g_capgrp = g_capgrp ? g_capgrp * 2 : 64;
            if (!(g_grp = realloc(g_grp, sizeof *g_grp * (size_t)g_capgrp))) die("pool copy groups");
        }
        g_grp[g_ngrp++] = (run_grp){src, dst, g_nruns, 0, base};
    }
    g_runs[g_nruns++] = (VkBufferCopy){soff, doff, n};
    g_grp[g_ngrp - 1].n++;
    g_sh_in_bytes += n;
}
static void run_written(void *ctx, size_t off, size_t n) {   // nvn_dirty_runs: off from the pool's start
    const pool_ent *p = ctx;
    VkDeviceSize o = (p->lo - p->base) + off;
    run_put(p->buf, p->sbuf, o, p->soff + o, n, p->base);
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
    for (int g = 0; g < g_ngrp; g++) {
        if (vkc.has_checkpoints || vkc.has_buffer_marker) {   // which pool each copy reads (a lost device stopped in one, 2026-10-27)
            const VkBufferCopy *r = &g_runs[g_grp[g].first];
            VkDeviceSize n = 0;
            for (int k = 0; k < g_grp[g].n; k++) n += r[k].size;
            char k1[32], k2[32];
            snprintf(k1, sizeof k1, "%llx", (unsigned long long)(g_grp[g].base + r[0].srcOffset));
            snprintf(k2, sizeof k2, "%d runs %llu bytes", g_grp[g].n, (unsigned long long)n);
            vkb_checkpoint("pool copy in", k1, k2);
        }
        vk.vkCmdCopyBuffer(cb, g_grp[g].src, g_grp[g].dst, (uint32_t)g_grp[g].n, &g_runs[g_grp[g].first]);
    }
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
        run_put(p->buf, p->sbuf, 0, p->soff, p->bytes, p->base);
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
    // pool copies (Linux, a discrete GPU): the device copy is filled from the pool's copy, which pc_sync brings up to
    // date right before the submission - for the pools used in it, so this one counts as used
    if (p->cmap && __atomic_load_n(&p->used_sub, __ATOMIC_RELAXED) != sub) __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
    return 1;
}
// What a GPU write's range held before it, for wb_merge: in a page the CPU also wrote, the GPU gets every byte the CPU
// left unchanged (game memory still equal to "before") instead of the whole page going to the CPU. Page by page, the
// GPU's writes next to the CPU's were lost (about 20 pages a frame on the Great Sky Island) and the stale bytes came back
// into the copy at its next update: the game makes vertex data on the GPU in buffers it also writes from the CPU, and
// meshes got stray vertices (L13 in dev-local/docs/issues.md).
// "Before" comes from the copy itself, copied on the GPU just ahead of the write into host memory of the slot (bf_*): it
// is game memory as of the copy's last update, whatever the CPU writes meanwhile (the game writes the next frame while
// this one is recorded). Where the CPU has not written the range's pages since the update, game memory is "before"
// itself and is copied on the CPU (no GPU copy, no barriers); in a pass on one recording thread with pages the CPU
// wrote (no copy possible there), the page rule stays.
typedef struct wb_snap_s { uint64_t addr, len; uint8_t *data; int owned; } wb_snap_t;
static wb_snap_t *g_snaps;   // this submission's (shadow_writeback hands them to its retire job)
static int g_nsnaps, g_capsnaps;
static uint64_t g_snap_bytes;
enum { BF_CAP = 16u << 20 };
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; } bfst_t;
static bfst_t g_bfst[VKB_SLOTS];   // per slot: "before" copies, read by the retire thread once the submission is done
static VkDeviceSize g_bf_used;     // in the current submission's slot
static uint8_t *bf_map(int slot, VkBuffer *buf) {
    bfst_t *b = &g_bfst[slot];
    if (!b->map) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BF_CAP, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &b->buf), "copy back before-buffer")) return NULL;
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, b->buf, &req);
        uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)   // cached first: the CPU reads it
            for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++) {
                VkMemoryPropertyFlags f = vkc.memory.memoryTypes[i].propertyFlags;
                if ((req.memoryTypeBits & (1u << i)) && (f & want) == want && (pass || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT))) type = i;
            }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        void *m;
        vkb_mtag = MT_COPY_BACK;
        if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &b->mem), "copy back before-memory")) {
            vk.vkDestroyBuffer(vkc.device, b->buf, NULL); b->buf = VK_NULL_HANDLE; return NULL;
        }
        vk.vkBindBufferMemory(vkc.device, b->buf, b->mem, 0);
        if (!vk_check(vk.vkMapMemory(vkc.device, b->mem, 0, VK_WHOLE_SIZE, 0, &m), "copy back before-map")) return NULL;
        b->map = m;
    }
    *buf = b->buf;
    return b->map;
}
static wb_snap_t *snap_new(uint64_t addr, uint64_t len) {   // NULL: the same range already has one
    for (int k = g_nsnaps - 1; k >= 0 && k >= g_nsnaps - 8; k--)
        if (g_snaps[k].addr == addr && g_snaps[k].len == len) return NULL;   // written again: the first "before" counts
    if (g_nsnaps == g_capsnaps) {
        g_capsnaps = g_capsnaps ? g_capsnaps * 2 : 256;
        if (!(g_snaps = realloc(g_snaps, sizeof *g_snaps * (size_t)g_capsnaps))) die("copy back snapshots");
    }
    wb_snap_t *s = &g_snaps[g_nsnaps++];
    *s = (wb_snap_t){addr, len, NULL, 0};
    return s;
}
static int g_slot_sent;   // partial submission (vkb_mt_submit): parts of this submission are on the GPU
// late: partial submission sent parts that may hold GPU writes to this pool already (wb_add): a copy of the device
// copy made now, in a later part of the prologue, could hold them, so the page rule instead
// (g_slot_sent is read under g_pro_mu, which part_flush holds when it sets it: a part sent after the look is not missed)
static void snap_take(const pool_ent *p, uint64_t addr, uint64_t len, int late) {
    VkDeviceSize n = (len + 15) & ~(VkDeviceSize)15;
    VkBuffer bf;
    uint8_t *map;
    int cpu_wrote = nvn_dirty_since((const void *)(uintptr_t)addr, len, p->stamp);   // else game memory is "before" itself
    if (cpu_wrote && (t_recording || !vkb_in_pass()) && g_bf_used + n <= BF_CAP && (map = bf_map(vkb_slot(), &bf))) {
        if (t_recording) vkb_pro_begin();   // threads recording: in the prologue, after the copy's update there
        if (late && g_slot_sent) { if (t_recording) vkb_pro_end(); return; }   // the page rule
        wb_snap_t *s = snap_new(addr, len);
        if (!s) { if (t_recording) vkb_pro_end(); return; }
        s->data = map + g_bf_used;
        VkCommandBuffer cb = vkb_cmd();
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferCopy r = {p->soff + (addr - p->base), g_bf_used, len};
        vk.vkCmdCopyBuffer(cb, p->sbuf, bf, 1, &r);
        shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
        if (t_recording) vkb_pro_end();
        g_bf_used += n;
        return;
    }
    if (cpu_wrote) return;   // and no copy possible: the page rule
    if (g_snap_bytes + len > (64u << 20)) return;   // a bound: GPU writes per submission are far below it
    wb_snap_t *s = snap_new(addr, len);
    if (!s) return;
    if (!(s->data = malloc(len))) die("copy back snapshot");
    memcpy(s->data, (const void *)(uintptr_t)addr, len);
    s->owned = 1;
    g_snap_bytes += len;
}
static void wb_add(pool_ent *p, uint64_t addr, uint64_t len) {   // a GPU write to p's copy (g_pool_mu written, or read + g_wb_mu)
    int late = __atomic_load_n(&p->wb_sub, __ATOMIC_RELAXED) == g_fence + 1;   // GPU writes to it in this submission already
    __atomic_store_n(&p->wb_sub, g_fence + 1, __ATOMIC_RELAXED);
    snap_take(p, addr, len, late);
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
static uint64_t g_wb_byte_pages;               // pages both wrote, merged byte by byte (wb_bytes)
static wb_snap_t *g_wb_snaps;                  // the last shadow_writeback's snapshots, for its retire job
static int g_wb_nsnaps;
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
        vkb_mtag = MT_COPY_BACK;
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
// Page [p, e) the CPU wrote since the copy was current: the GPU's bytes (gpu: its byte p) where game memory still holds
// what a snapshot saw; 1 if a snapshot covered any of it.
static int wb_bytes(uint64_t p, uint64_t e, const uint8_t *gpu, const wb_snap_t *s, int ns) {
    void *tok = NULL;
    for (int k = 0; k < ns; k++) {
        uint64_t lo = s[k].addr > p ? s[k].addr : p, hi = s[k].addr + s[k].len < e ? s[k].addr + s[k].len : e;
        if (lo >= hi) continue;
        if (!tok) tok = nvn_dirty_gpu_write_begin((void *)(uintptr_t)p, e - p);
        uint8_t *g = (uint8_t *)(uintptr_t)lo;
        const uint8_t *was = s[k].data + (lo - s[k].addr), *now = gpu + (lo - p);
        for (uint64_t b = 0; b < hi - lo; b++) if (g[b] == was[b]) g[b] = now[b];
    }
    if (tok) nvn_dirty_gpu_write_end(tok);
    return tok != NULL;
}
// Pool copies with device copies (Linux, a discrete GPU): what the copy back put into game memory goes into the pool's
// own copy too. Reads of a pool without a current device copy (in a pass, before its update) go to that copy, which is
// game memory as it was at the last sync; on Windows they read game memory itself, which has the GPU's writes by then.
static void wb_to_pool_copy(uint64_t a, uint64_t len) {
    if (!vkb_pool_copies()) return;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    const pool_ent *p = pool_find((uintptr_t)a, len);
    if (p && p->cmap) memcpy(p->cmap + (a - p->base), (const void *)(uintptr_t)a, len);
    vkb_rw_read_end(&g_pool_mu);
}
static void wb_merge_one(uint64_t a, uint64_t len, uint64_t stamp, const uint8_t *src, const wb_snap_t *snaps, int nsnaps) {
    if (!nvn_dirty_since((const void *)(uintptr_t)a, len, stamp)) { wb_put(a, src, len); g_wb_pages += (len + 4095) / 4096; return; }
    for (uint64_t p = a; p < a + len;) {   // page by page
        uint64_t e = ((p >> 12) + 1) << 12;
        if (e > a + len) e = a + len;
        g_wb_pages++;
        if (nvn_dirty_since((const void *)(uintptr_t)p, e - p, stamp)) {
            if (wb_bytes(p, e, src + (p - a), snaps, nsnaps)) { g_wb_byte_pages++; p = e; continue; }
            g_wb_kept_pages++;
            static int logged, want = -1;   // TOTK_WB_LOG=1 (development): which GPU writes the CPU's pages replaced
            if (want < 0) want = getenv("TOTK_WB_LOG") != NULL;
            if (want && logged++ < 2000)
                VLOG("[wb] GPU write %#llx+%llu: page %#llx kept for the CPU's writes (frame %llu)", (unsigned long long)a,
                     (unsigned long long)len, (unsigned long long)p, (unsigned long long)vkb_frame());
        } else wb_put(p, src + (p - a), e - p);
        p = e;
    }
}
// GPU outputs the CPU reads without waiting: game memory gets them at a fixed point of the game's frames. TOTK's
// shrines analyse the scene's depth on the GPU (a compute pass into a 23 KiB buffer) and a game thread reads the result
// for the next frames' lights - not after the render thread's per-frame wait for the GPU, so on the Switch it simply
// finds the frame before's result, the GPU being done with it by then. Here the GPU finishes a frame later (the
// translation thread's lag, then the GPU), and the copy back reached game memory when that submission retired: before
// that read in one frame, after it in the next. The light shafts and the lit floor along the walls switched between
// the two results about one frame in six, and in ten when the game thread was slow (2026-11-07; ~40 of 60 frames with
// the game thread held 5 ms at its submit call; 3-4 with the copy back held as below, as with every frame waited for).
// So a GPU-only range (the CPU has not written it since the GPU write was recorded) copied back for a submission
// recorded after present L reaches game memory at present L + 3, on the game thread (mtl_wb_held_present): by then the
// GPU has done it (frames_ahead_wait has waited for present L + 1), so the reader finds the same frame's result every
// time, two frames old - at 60 fps as old as one frame on the Switch. A range that overlaps a held one is held too
// (order), and a range the CPU wrote goes now as before (the CPU's pages win; wb_merge_one). QueueFinish (mtl_finish)
// applies them all; a pool deleted drops its own. TOTK_WB_HOLD=0: at retirement, as before (A/B).
typedef struct wb_held { uint64_t frame, lo, len, stamp; struct wb_held *next; uint8_t data[]; } wb_held;
static wb_held *g_held, **g_held_tail = &g_held;
static pthread_mutex_t g_held_mu = PTHREAD_MUTEX_INITIALIZER;
static int wb_hold_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_WB_HOLD"); on = !(e && *e == '0'); VLOG("GPU-only copy backs: %s", on ? "at the present three after their frame" : "when their submission retires (TOTK_WB_HOLD=0)"); }
    return on;
}
static void wb_held_apply(wb_held *h) {
    wb_merge_one(h->lo, h->len, h->stamp, h->data, NULL, 0);
    wb_to_pool_copy(h->lo, h->len);
    free(h);
}
static void wb_held_take(uint64_t upto, int all) {   // applies the held ranges of frames <= upto, in order
    pthread_mutex_lock(&g_held_mu);
    while (g_held && (all || g_held->frame <= upto)) {
        wb_held *h = g_held;
        g_held = h->next;
        if (!g_held) g_held_tail = &g_held;
        wb_held_apply(h);
    }
    pthread_mutex_unlock(&g_held_mu);
}
void mtl_wb_held_present(uint64_t present) { if (present > 2) wb_held_take(present - 2, 0); }   // game thread, at each present (frames: label + 1)
static void wb_held_drop(uint64_t lo, uint64_t hi) {   // a pool deleted: its memory is the game's again
    pthread_mutex_lock(&g_held_mu);
    for (wb_held **pp = &g_held; *pp;) {
        wb_held *h = *pp;
        if (h->lo < hi && lo < h->lo + h->len) { *pp = h->next; free(h); } else pp = &h->next;
    }
    g_held_tail = &g_held;
    while (*g_held_tail) g_held_tail = &(*g_held_tail)->next;
    pthread_mutex_unlock(&g_held_mu);
}
static void wb_merge(const uint64_t *r, int n, const uint8_t *src, const wb_snap_t *snaps, int nsnaps, uint64_t frame) {
    int hold = frame && wb_hold_on();
    for (int k = 0; k < n; k++, r += 4) {
        if (hold) {
            int held = 0;
            pthread_mutex_lock(&g_held_mu);
            for (const wb_held *h = g_held; h && !held; h = h->next) held = h->lo < r[0] + r[1] && r[0] < h->lo + h->len;
            if (held || !nvn_dirty_since((const void *)(uintptr_t)r[0], r[1], r[2])) {
                wb_held *h = malloc(sizeof *h + r[1]);
                if (!h) die("held copy back");
                *h = (wb_held){frame, r[0], r[1], r[2], NULL};
                memcpy(h->data, src + r[3], r[1]);
                *g_held_tail = h; g_held_tail = &h->next;
                pthread_mutex_unlock(&g_held_mu);
                continue;
            }
            pthread_mutex_unlock(&g_held_mu);
        }
        wb_merge_one(r[0], r[1], r[2], src + r[3], snaps, nsnaps);
        wb_to_pool_copy(r[0], r[1]);
    }
}
static void shadow_writeback(VkCommandBuffer cb) {   // end of a submission: GPU writes to the slot's host memory
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    g_wb_ranges = NULL; g_wb_nranges = 0;
    g_wb_snaps = g_snaps; g_wb_nsnaps = g_nsnaps;   // taken by the job (also with no range: then only freed)
    g_snaps = NULL; g_nsnaps = g_capsnaps = 0; g_snap_bytes = 0; g_bf_used = 0;
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
// The GPU writes of this submission (g_wb, g_snaps and the slot's "before" buffer) when the pool table is only read:
// a write to a pool already current in the submission is recorded under the read lock and this one, so the recording
// threads do not wait for each other's uniform updates and stores (the write lock is for bringing a copy up to date).
static pthread_mutex_t g_wb_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pool_use_n[3];   // the per-frame log line: write lock to update a copy / for a write / writes under the read lock
// Per recording thread, the pools of its last reads that needed nothing more in this submission: a draw's buffers mostly
// lie in a few pools (uniforms, vertices, indices, storage), and each look took the table's lock, searched it and wrote
// the pool's used_sub, one cache line written by every recording thread (vkb_pool_use was ~6 % of each recording thread
// at Lookout Landing, 2026-11-02; with one pool remembered still ~5 %).
enum { POOL_HITS = 8 };
static __thread struct { uint64_t lo, hi, sub, gen; } t_pool_hit[POOL_HITS];
static __thread unsigned t_pool_hit_next;
static void pool_hit_note(const pool_ent *p, uint64_t addr, uint64_t len, uint64_t sub, uint64_t gen) {
    if (addr < p->lo || addr + len > p->hi) return;
    unsigned k = t_pool_hit_next++ % POOL_HITS;
    t_pool_hit[k].lo = p->lo; t_pool_hit[k].hi = p->hi; t_pool_hit[k].sub = sub; t_pool_hit[k].gen = gen;
}
static void used_mark(pool_ent *p, uint64_t sub) {   // read first: a store only when it changes
    if (__atomic_load_n(&p->used_sub, __ATOMIC_RELAXED) != sub) __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
}
// Per recording thread, the last GPU writes it recorded (exact ranges): a shader that stores to the same buffer draw
// after draw registered the same write every time, through the table's write path and its lock, though the first one
// in a submission already holds it (its copy back, its snapshot of what was there before: wb_add, pc_pend_add).
// TOTK_POOL_WRITE_MEMO=0: every time, as before (A/B).
enum { POOL_WRS = 8 };
static __thread struct { uint64_t lo, hi, sub, gen; } t_pool_wr[POOL_WRS];
static __thread unsigned t_pool_wr_next;
static void pool_use_impl(uint64_t addr, uint64_t len, int write, uint64_t sub, uint64_t gen);
void vkb_pool_use(uint64_t addr, uint64_t len, int write) {
    if (g_ok != 1 || !addr || !len) return;
    uint64_t sub = g_fence + 1, gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);
    if (!write)
        for (int k = 0; k < POOL_HITS; k++)
            if (t_pool_hit[k].sub == sub && t_pool_hit[k].gen == gen && addr >= t_pool_hit[k].lo && addr + len <= t_pool_hit[k].hi) return;
    static int memo = -1;
    if (memo < 0) { const char *e = getenv("TOTK_POOL_WRITE_MEMO"); memo = !(e && *e == '0'); }
    if (write && memo)
        for (int k = 0; k < POOL_WRS; k++)
            if (t_pool_wr[k].sub == sub && t_pool_wr[k].gen == gen && t_pool_wr[k].lo == addr && t_pool_wr[k].hi == addr + len) return;
    pool_use_impl(addr, len, write, sub, gen);
    if (write && memo) {
        unsigned k = t_pool_wr_next++ % POOL_WRS;
        t_pool_wr[k].lo = addr; t_pool_wr[k].hi = addr + len; t_pool_wr[k].sub = sub; t_pool_wr[k].gen = gen;
    }
}
static void pool_use_impl(uint64_t addr, uint64_t len, int write, uint64_t sub, uint64_t gen) {
    int shadow = vkb_shadow_on();
    VkBuffer pc_buf = VK_NULL_HANDLE;   // pool copies: a GPU write that lands in the pool's copy itself (pc_pend_add)
    uint64_t pc_n = 0;
    int pc_added = 0;
    if (vkb_pool_copies()) {   // the pool is synced before the submission (pc_sync); a GPU write goes back after it
        vkb_rw_read(&g_pool_mu, LK_POOLS);
        // a range running on past its pool's end (GPU addresses, vkb_span_addr): the pool it starts in, up to its end
        pool_ent *strict = pool_find_mut((uintptr_t)addr, len), *p = strict ? strict : pool_find_mut((uintptr_t)addr, 1);
        pc_buf = p ? p->buf : VK_NULL_HANDLE;
        pc_n = p && addr + len > p->hi ? p->hi - addr : len;
        if (p) used_mark(p, sub);
        if (p && !write && !shadow) pool_hit_note(p, addr, len, sub, gen);
        // with device copies (a discrete GPU) the write goes to the pool's device copy unless it has none
        pc_added = p && write && !(shadow && strict && !strict->no_shadow);
        vkb_rw_read_end(&g_pool_mu);
        if (pc_added) pc_pend_add(pc_buf, addr, pc_n);
    }
    if (!shadow) return;
    vkb_rw_read(&g_pool_mu, LK_POOLS);   // the common cases: a pool already current in this submission, or not copied
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    int done = !p || p->no_shadow || (p->sync_sub == sub && !write);
    if (p) used_mark(p, sub);
    if (p && done && !write) pool_hit_note(p, addr, len, sub, gen);
    int late = p && write && p->no_shadow && !pc_added && pc_buf == p->buf;   // lost its device copy since the look above
    if (!done && write && p->sync_sub == sub) {
        pthread_mutex_lock(&g_wb_mu);
        wb_add(p, addr, len);
        pthread_mutex_unlock(&g_wb_mu);
        atomic_fetch_add_explicit(&g_pool_use_n[2], 1, memory_order_relaxed);
        done = 1;
    }
    vkb_rw_read_end(&g_pool_mu);
    if (late) pc_pend_add(pc_buf, addr, pc_n);
    if (done) return;
    atomic_fetch_add_explicit(&g_pool_use_n[write ? 1 : 0], 1, memory_order_relaxed);
    vkb_rw_write(&g_pool_mu, LK_POOLS);
    p = pool_find_mut((uintptr_t)addr, len);   // the table may have changed between the locks
    int to_copy = 0;
    if (p) {
        p->used_sub = sub;
        // threads recording: the copies go into the prologue, before every chunk (no pass to end)
        int ok = p->sync_sub == sub || sync_gather(p, sub, write || t_recording);
        sync_flush();
        if (ok && write) wb_add(p, addr, len);
        to_copy = !ok && write && !pc_added && pc_buf == p->buf;   // pool copies: no device copy, so the write lands in its copy
    }
    vkb_rw_write_end(&g_pool_mu);
    if (to_copy) pc_pend_add(pc_buf, addr, pc_n);
}
// Per recording thread, the last pools whose GPU address cannot change for the rest of the submission (no device copy
// may come: none on this machine or the pool has none for good, or its copy is current): the table's lock and search
// were taken for every buffer a draw binds (the read lock alone ~7 % of each recording thread on the Steam Deck,
// 2026-11-04). Valid for one submission and one version of the table (g_pool_gen).
enum { ADDR_HITS = 4 };
static __thread struct { uint64_t lo, hi, sub, gen, base; VkBuffer buf; VkDeviceSize boff; } t_addr_hit[ADDR_HITS];
static __thread unsigned t_addr_next;
int vkb_pool_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_ok != 1) return 0;
    uint64_t sub = g_fence + 1, gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);
    static int on = -1;   // TOTK_POOL_ADDR_CACHE=0: the table every time, as before (A/B)
    if (on < 0) { const char *e = getenv("TOTK_POOL_ADDR_CACHE"); on = !(e && *e == '0'); }
    for (int k = 0; k < ADDR_HITS && on; k++)
        if (t_addr_hit[k].sub == sub && t_addr_hit[k].gen == gen && addr >= t_addr_hit[k].lo && addr + len <= t_addr_hit[k].hi) {
            *buf = t_addr_hit[k].buf; *off = t_addr_hit[k].boff + (addr - t_addr_hit[k].base);
            return 1;
        }
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        __atomic_store_n(&p->used_sub, sub, __ATOMIC_RELAXED);
        int shadow = p->sh && p->sync_sub == sub;
        if (shadow) { *buf = p->sbuf; *off = p->soff + (addr - p->base); }
        else { *buf = p->buf; *off = addr - p->base; }
        if (on && (shadow || !vkb_shadow_on() || p->no_shadow)) {
            unsigned k = t_addr_next++ % ADDR_HITS;
            t_addr_hit[k].lo = p->lo; t_addr_hit[k].hi = p->hi; t_addr_hit[k].sub = sub; t_addr_hit[k].gen = gen; t_addr_hit[k].base = p->base;
            t_addr_hit[k].buf = *buf; t_addr_hit[k].boff = shadow ? p->soff : 0;
        }
    }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len) { return (pool_ent *)pool_find(addr, len); }

uint64_t vkb_pool_room(uint64_t addr) {   // bytes from addr to the end of the imported pool holding it (0: none)
    if (g_ok != 1) return 0;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    const pool_ent *p = pool_find((uintptr_t)addr, 1);
    uint64_t n = p ? p->hi - addr : 0;
    vkb_rw_read_end(&g_pool_mu);
    return n;
}

// ---- ranges across pools, in the GPU's address space (hle_nvn.c "GPU addresses, as the Switch's") ----
// A range the game binds that runs on past the end of its pool continues, on the Switch, into the pools that follow in
// the GPU's address space (TOTK's Depths terrain: vertices read through a buffer bound in the pool before theirs, L20).
// Here pools are buffers of their own, so such a range is put together: a device-local buffer laid out as the GPU
// address space, from the start of the pool the range starts in, filled from the pools' pieces at the start of a
// submission (outside a pass) and kept current from their written pages. Until it is filled a draw gets an empty buffer.
// [temporary] GPU writes into the pools behind a span are not seen by it until they are back in game memory.
uint64_t nvn_cpu2va(uint64_t cpu);
int nvn_va_pieces(uint64_t va, uint64_t len, void (*fn)(void *ctx, uint64_t cpu, uint64_t off, uint64_t n), void *ctx);
typedef struct { uint64_t va, len, used_sub, stamp; VkBuffer buf; VkDeviceMemory mem; int filled; } span_t;
enum { SPANS = 16 };
static span_t g_spans[SPANS];
static int g_nspans;
static pthread_mutex_t g_span_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_span_bytes_in;   // copied into spans (the per-frame log line)
int vkb_span_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    if (g_ok != 1 || !addr || !len || len > (1ull << 31)) return 0;
    uint64_t va = nvn_cpu2va(addr), sub = g_fence + 1;
    if (!va || va == addr) return 0;   // GPU addresses off, or not in a pool
    uint64_t lo = va;
    {   // from the start of the pool the range starts in: draws starting at other places in it share the span
        vkb_rw_read(&g_pool_mu, LK_POOLS);
        const pool_ent *p = pool_find((uintptr_t)addr, 1);
        if (p) lo = va - (addr - p->lo);
        vkb_rw_read_end(&g_pool_mu);
    }
    uint64_t hi = (va + len + 0xfff) & ~0xfffull;
    pthread_mutex_lock(&g_span_mu);
    int k = -1;
    for (int i = 0; i < g_nspans; i++) if (g_spans[i].va == lo) { k = i; break; }
    if (k >= 0 && g_spans[k].va + g_spans[k].len < hi) {   // reaches further than the span made so far: made again, larger
        destroy_later(VK_NULL_HANDLE, VK_NULL_HANDLE, g_spans[k].mem, g_spans[k].buf);
        g_spans[k] = g_spans[--g_nspans];
        k = -1;
    }
    if (k < 0) {
        if (g_nspans == SPANS) {   // the least recently used goes
            int o = 0;
            for (int i = 1; i < SPANS; i++) if (g_spans[i].used_sub < g_spans[o].used_sub) o = i;
            destroy_later(VK_NULL_HANDLE, VK_NULL_HANDLE, g_spans[o].mem, g_spans[o].buf);
            g_spans[o] = g_spans[--g_nspans];
        }
        span_t s = {lo, hi - lo, sub};
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = s.len,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
        VkMemoryRequirements req;
        if (vk.vkCreateBuffer(vkc.device, &bi, NULL, &s.buf) != VK_SUCCESS) { pthread_mutex_unlock(&g_span_mu); return 0; }
        vk.vkGetBufferMemoryRequirements(vkc.device, s.buf, &req);
        uint32_t type = UINT32_MAX;   // device-local
        for (uint32_t t = 0; t < vkc.memory.memoryTypeCount && type == UINT32_MAX; t++)
            if ((req.memoryTypeBits >> t & 1) && (vkc.memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &s.mem) != VK_SUCCESS) {
            vk.vkDestroyBuffer(vkc.device, s.buf, NULL);
            pthread_mutex_unlock(&g_span_mu);
            return 0;
        }
        vk.vkBindBufferMemory(vkc.device, s.buf, s.mem, 0);
        static int said;
        if (said++ < 16) VLOG("[gpu va] range %#llx+%#llx across pools (from %#llx): a buffer of %llu MiB laid out as the GPU address space",
                              (unsigned long long)va, (unsigned long long)len, (unsigned long long)addr, (unsigned long long)(s.len >> 20));
        k = g_nspans++;
        g_spans[k] = s;
    }
    span_t *s = &g_spans[k];
    s->used_sub = sub;
    int ok = s->filled;
    if (ok) { *buf = s->buf; *off = va - s->va; }
    pthread_mutex_unlock(&g_span_mu);
    return ok;
}
typedef struct { span_t *s; VkCommandBuffer cb; int whole; uint64_t bytes; } span_fill;
static void span_run(void *ctx, size_t off, size_t n) {   // nvn_dirty_runs inside one piece: ctx carries the piece
    struct piece { span_fill *f; uint64_t cpu, soff; } *pc = ctx;
    pool_ent *p = pool_find_mut((uintptr_t)(pc->cpu + off), n);   // g_pool_mu held (span_presync)
    if (p) {
        if (vkb_pool_copies()) __atomic_store_n(&p->used_sub, g_fence + 1, __ATOMIC_RELAXED);   // the copy reads it in this submission (pc_sync)
        VkBufferCopy c = {(pc->cpu + off) - p->base, pc->soff + off, n};
        vk.vkCmdCopyBuffer(pc->f->cb, p->buf, pc->f->s->buf, 1, &c);
        pc->f->bytes += n;
    }
}
static void span_piece(void *ctx, uint64_t cpu, uint64_t off, uint64_t n) {
    span_fill *f = ctx;
    struct piece { span_fill *f; uint64_t cpu, soff; } pc = {f, cpu, off};
    if (f->whole || nvn_dirty_runs((const void *)(uintptr_t)cpu, n, f->s->stamp, 65536, span_run, &pc) < 0) span_run(&pc, 0, n);
}
// At the first pass of a submission (shadow_presync's place): spans used lately, filled or brought up to date.
static void span_presync(void) {
    static _Atomic uint64_t done_sub;
    uint64_t sub = g_fence + 1;
    if (!g_nspans || atomic_load(&done_sub) == sub) return;
    pthread_mutex_lock(&g_span_mu);
    if (atomic_exchange(&done_sub, sub) == sub) { pthread_mutex_unlock(&g_span_mu); return; }
    vkb_rw_read(&g_pool_mu, LK_POOLS);   // the pool table, then the prologue: the order sync_flush takes them in
    int mt = t_recording, any = 0;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (int i = 0; i < g_nspans; i++) {
        span_t *s = &g_spans[i];
        if (s->used_sub + 2 < sub) continue;
        if (!cb) {
            if (mt) vkb_pro_begin(); else pass_end_why(PE_SHADOW);
            cb = vkb_cmd();
            shadow_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        span_fill f = {s, cb, !s->filled, 0};
        uint64_t stamp = nvn_dirty_now();
        nvn_va_pieces(s->va, s->len, span_piece, &f);
        s->stamp = stamp;
        s->filled = 1;
        g_span_bytes_in += f.bytes;
        any = 1;
    }
    if (cb) {
        if (any) shadow_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        if (mt) vkb_pro_end();
    }
    vkb_rw_read_end(&g_pool_mu);
    pthread_mutex_unlock(&g_span_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    if (g_ok != 1) { *buf = NULL; *off = 0; return 0; }
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    pool_ent *p = pool_find_mut((uintptr_t)addr, len);
    if (p) {
        *buf = (void *)p->buf; *off = addr - p->base;
        if (vkb_pool_copies()) __atomic_store_n(&p->used_sub, g_fence + 1, __ATOMIC_RELAXED);   // read by the GPU in this submission (pc_sync)
    }
    vkb_rw_read_end(&g_pool_mu);
    return p != NULL;
}

// ---------------- submissions ----------------
// 1 event, 2 counter report, 3 samples-passed reset (TOTK_OCCLUSION); for 2 and 3 mode 1: counted by occlusion
// queries, piece: the piece of the count the report or reset ends (oq_* below)
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, type; uint32_t piece; } post_op;
typedef struct { VkCommandPool pool; VkCommandBuffer cb; uint64_t fence; } cmd_slot;
#define SLOTS VKB_SLOTS
static cmd_slot g_slots[SLOTS];
static int g_nslots, g_cur = -1;
static post_op *g_post;
static int g_npost, g_capost;
static uint64_t g_op_count[OP_COUNT_];
static int g_op_said[OP_COUNT_];

typedef struct job { uint64_t fence; post_op *ops; int n; int slot; uint64_t *wb; int nwb; struct job *next; struct wb_snap_s *snaps; int nsnaps;
                     uint64_t frame, t_sub;   // frame, t_sub: the game frame it belongs to, when it went to the queue (gpu_frame)
                     struct oq_pair *oq; int noq; } job;   // its occlusion queries (TOTK_OCCLUSION)
static job *g_jobs, **g_jobs_tail = &g_jobs;

// ---------- Samples passed: Vulkan occlusion queries (TOTK_OCCLUSION=1; off: every report says "visible") ----------
// NVN's SAMPLES_PASSED counter is a running count on the queue: RESET_COUNTER zeroes it, each draw after it adds the
// samples that passed the depth and stencil tests, REPORT_COUNTER writes the count. TOTK resets, draws the proxies of
// what it tests, reports, 3-8 times a frame in the field (~21 in a fight), reads the counts back on the CPU and culls
// what counted 0. Counted here, with the walk and chunked recording on several threads:
// - Counting runs from a reset to the next report (draws outside are not counted); once the game reports twice without
//   a reset, every draw is counted from then on (the cumulative NVN counter). The resets and reports cut the command
//   stream into pieces, numbered in order. The walk (or one thread) keeps the state in g_oq_walk; each chunk starts with
//   a copy of it (chunk_open) and steps it through the same resets and reports, so the numbers agree without a lock.
// - The first counted draw in a pass begins a query (precise where the device has occlusionQueryPrecise); it ends with
//   the pass or at the next reset or report, in the command buffer it began in. Each ended query goes to the
//   submission's list as (piece, slot).
// - The retire thread, after the fence, goes through the posts in order: the queries of the pieces up to the one a post
//   ends are added to the count, a reset zeroes it, a report writes it. Queries of a piece that goes on into the next
//   submission are added at the end. Slots come from one ring and are read and reset on the host (hostQueryReset); a
//   full ring or a result not available after the fence counts as visible (one sample): a miss costs work, never an object.
typedef struct { uint32_t counting, reported, keep, piece; } oq_st;
typedef struct oq_pair { uint32_t piece, slot; } oq_pair;
#define OQ_N 4096
#define OQ_LOST UINT32_MAX              // a counted draw without a slot (ring full): visible
static int g_oq_on = -1;                // -1 not decided yet
static VkQueryPool g_oq_pool;
static VkQueryControlFlags g_oq_flags;
static uint8_t g_oq_busy[OQ_N];         // from its begin until the retire thread read and reset it
static uint32_t g_oq_next;              // next ring slot (atomic)
static oq_st g_oq_walk;                 // the walk's (or the one thread's) state, in command order
static __thread oq_st *t_oqs = &g_oq_walk;
static __thread int t_oq_open = -1;     // this thread's active query: slot, command buffer, piece
static __thread VkCommandBuffer t_oq_cb;
static __thread uint32_t t_oq_piece;
static pthread_mutex_t g_oq_mu = PTHREAD_MUTEX_INITIALIZER;
static oq_pair *g_oq_list;              // the submission being recorded: its ended queries
static int g_oq_n, g_oq_cap;
static uint64_t g_oq_count;             // retire thread: samples since the last reset
static uint64_t g_oq_stat[5];           // log line: reports, zero reports, queries, draws uncounted, results not available
static pthread_once_t g_oq_once = PTHREAD_ONCE_INIT;

static void oq_init(void) {
    int on = 0;
    const char *e = getenv("TOTK_OCCLUSION");
    if (e && *e == '1' && g_ok == 1) {
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f2);   // vk.c enables every feature the device has
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = OQ_N};
        if (!f12.hostQueryReset) VLOG("samples passed: no hostQueryReset, reported as visible");
        else if (vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_oq_pool), "occlusion query pool")) {
            vk.vkResetQueryPool(vkc.device, g_oq_pool, 0, OQ_N);
            g_oq_flags = f2.features.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
            on = 1;
            VLOG("samples passed: counted by occlusion queries (TOTK_OCCLUSION=1, %s)", g_oq_flags ? "precise" : "not precise");
        }
    }
    __atomic_store_n(&g_oq_on, on, __ATOMIC_RELEASE);
}
static int oq_on(void) {
    int v = __atomic_load_n(&g_oq_on, __ATOMIC_ACQUIRE);
    if (v >= 0) return v;
    pthread_once(&g_oq_once, oq_init);
    return g_oq_on;
}
static void oq_add(uint32_t piece, uint32_t slot) {
    pthread_mutex_lock(&g_oq_mu);
    if (g_oq_n == g_oq_cap) {
        g_oq_cap = g_oq_cap ? 2 * g_oq_cap : 64;
        if (!(g_oq_list = realloc(g_oq_list, sizeof *g_oq_list * (size_t)g_oq_cap))) die("occlusion queries");
    }
    g_oq_list[g_oq_n++] = (oq_pair){piece, slot};
    pthread_mutex_unlock(&g_oq_mu);
}
static void oq_close(void) {   // this thread's active query ends (pass end, reset, report)
    if (t_oq_open < 0) return;
    vk.vkCmdEndQuery(t_oq_cb, g_oq_pool, (uint32_t)t_oq_open);
    oq_add(t_oq_piece, (uint32_t)t_oq_open);
    t_oq_open = -1;
}
void vkb_oq_draw(VkCommandBuffer cb) {   // vkb_draw: a draw about to be recorded in the open pass
    if (!t_oqs->counting || t_oq_open >= 0) return;   // (counting is never set while TOTK_OCCLUSION is off)
    uint32_t q = __atomic_fetch_add(&g_oq_next, 1, __ATOMIC_RELAXED) % OQ_N;
    if (__atomic_exchange_n(&g_oq_busy[q], 1, __ATOMIC_ACQ_REL)) {
        __atomic_fetch_add(&g_oq_stat[3], 1, __ATOMIC_RELAXED);
        oq_add(t_oqs->piece, OQ_LOST);
        return;
    }
    vk.vkCmdBeginQuery(cb, g_oq_pool, q, g_oq_flags);
    __atomic_fetch_add(&g_oq_stat[2], 1, __ATOMIC_RELAXED);
    t_oq_open = (int)q; t_oq_cb = cb; t_oq_piece = t_oqs->piece;
}
static uint32_t oq_event(int report) {   // a samples-passed reset or report in command order: the piece it ends
    oq_st *s = t_oqs;
    oq_close();
    uint32_t piece = s->piece++;
    if (!report) { s->counting = 1; s->reported = 0; return piece; }
    if (s->reported && !s->keep) {
        s->keep = 1;
        if (s == &g_oq_walk) VLOG("samples passed reported twice without a reset: counting every draw from now on");
    }
    s->reported = 1;
    if (!s->keep) s->counting = 0;
    return piece;
}
static uint64_t oq_result(uint32_t slot) {   // retire thread, after the fence: the slot's samples, the slot free again
    if (slot == OQ_LOST) return 1;
    uint64_t v[2] = {0, 0};
    VkResult r = vk.vkGetQueryPoolResults(vkc.device, g_oq_pool, slot, 1, sizeof v, v, sizeof v,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if ((r != VK_SUCCESS && r != VK_NOT_READY) || !v[1]) { __atomic_fetch_add(&g_oq_stat[4], 1, __ATOMIC_RELAXED); v[0] = 1; }
    vk.vkResetQueryPool(vkc.device, g_oq_pool, slot, 1);
    __atomic_store_n(&g_oq_busy[slot], 0, __ATOMIC_RELEASE);
    return v[0];
}
static int oq_cmp(const void *a, const void *b) {
    int32_t d = (int32_t)(((const oq_pair *)a)->piece - ((const oq_pair *)b)->piece);
    return (d > 0) - (d < 0);
}

static void run_posts(const post_op *ops, int n, oq_pair *oq, int noq) {
    int qi = 0;
    if (noq > 1) qsort(oq, (size_t)noq, sizeof *oq, oq_cmp);
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1) {
            if (ops[i].mode == 1) *ops[i].val += ops[i].v;
            else *ops[i].val = ops[i].v;
        } else if (ops[i].mode == 1) {   // counted samples passed
            while (qi < noq && (int32_t)(oq[qi].piece - ops[i].piece) <= 0) g_oq_count += oq_result(oq[qi++].slot);
            if (ops[i].kind == 3) { g_oq_count = 0; continue; }
            uint64_t rec[2] = {g_oq_count, gpu_ticks()};
            g_oq_stat[0]++;
            if (!g_oq_count) g_oq_stat[1]++;
            guest_prepare_write((void *)(uintptr_t)ops[i].addr, sizeof rec);
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec);
        } else if (ops[i].kind == 2) {
            // NVNcounterData {counter, timestamp}. Samples passed: [temporary] reported as visible until draws are replayed
            // (with 0 the game culls every queried object); timestamps and other types report counter 0, as on Metal.
            uint64_t rec[2] = {ops[i].type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, gpu_ticks()};
            guest_prepare_write((void *)(uintptr_t)ops[i].addr, sizeof rec);
            memcpy((void *)(uintptr_t)ops[i].addr, rec, sizeof rec);
        }
    }
    while (qi < noq) g_oq_count += oq_result(oq[qi++].slot);   // a piece the next submission ends
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
// VK_AMD_buffer_marker: before each marked command the GPU writes its number at the top of the pipe (the commands before
// it have started) and at the bottom (they have finished) into host memory, which can be read after the device is lost
static VkBuffer g_mk_buf;
static VkDeviceMemory g_mk_mem;
static volatile uint32_t *g_mk;   // [0] top, [1] bottom: checkpoint number + 1
static int marker_init(void) {
    static int state;   // 0 not tried, 1 ready, -1 failed
    if (state) return state > 0;
    state = -1;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_mk_buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, g_mk_buf, &req);
    uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    void *m;
    if (type == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_mk_mem) != VK_SUCCESS) return 0;
    vk.vkBindBufferMemory(vkc.device, g_mk_buf, g_mk_mem, 0);
    if (vk.vkMapMemory(vkc.device, g_mk_mem, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) return 0;
    g_mk = m;
    g_mk[0] = g_mk[1] = 0;
    state = 1;
    VLOG("checkpoints: GPU-written markers on (VK_AMD_buffer_marker)");
    return 1;
}
void vkb_checkpoint(const char *what, const char *k1, const char *k2) {
    if (!vkc.has_checkpoints && !vkc.has_buffer_marker) return;
    if (!g_ckpt && !(g_ckpt = calloc(CKPT_RING, sizeof *g_ckpt))) return;
    uint32_t n = atomic_fetch_add(&g_ckpt_n, 1), i = n % CKPT_RING;
    ckpt_t *c = &g_ckpt[i];
    c->frame = vkb_frame(); c->n = n; c->what = what;
    snprintf(c->k1, sizeof c->k1, "%s", k1 ? k1 : "-");
    snprintf(c->k2, sizeof c->k2, "%s", k2 ? k2 : "-");
    VkCommandBuffer cb = vkb_cmd();
    if (vkc.has_checkpoints) vk.vkCmdSetCheckpointNV(cb, (void *)(uintptr_t)(i + 1));
    if (vkc.has_buffer_marker && marker_init()) {
        vk.vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_mk_buf, 0, n + 1);
        vk.vkCmdWriteBufferMarkerAMD(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_mk_buf, 4, n + 1);
    }
}
static const pool_ent *pool_find(uintptr_t addr, uint64_t len);
static void checkpoint_addr(const char *k1) {   // an address: what the memory and the pool table hold now
    unsigned long long a = strtoull(k1, NULL, 16);
    if (!a) return;
    vkb_rw_read(&g_pool_mu, LK_POOLS);
    const pool_ent *p = pool_find((uintptr_t)a, 1);
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mi = {0};
    VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi);
    VLOG("    memory %#llx: state %#lx protect %#lx region %p+%#llx; pool %s %#llx..%#llx", a, mi.State, mi.Protect, mi.BaseAddress,
         (unsigned long long)mi.RegionSize, p ? "registered" : "none", p ? (unsigned long long)p->lo : 0ull, p ? (unsigned long long)p->hi : 0ull);
#else
    VLOG("    memory %#llx: pool %s %#llx..%#llx", a, p ? "registered" : "none", p ? (unsigned long long)p->lo : 0ull, p ? (unsigned long long)p->hi : 0ull);
#endif
    vkb_rw_read_end(&g_pool_mu);
}
static void checkpoint_line(const char *how, uint32_t n) {
    const ckpt_t *c = &g_ckpt[n % CKPT_RING];
    if (c->n != n) { VLOG("  %s #%u: no longer in the ring", how, n); return; }
    VLOG("  %s #%u present %llu %s %s %s", how, n, (unsigned long long)c->frame, c->what, c->k1, c->k2);
    checkpoint_addr(c->k1);
}
static void checkpoint_report(void) {
    if (g_mk && g_ckpt) {   // the markers: commands after the bottom one up to the top one had started and not finished
        uint32_t top = g_mk[0], bottom = g_mk[1], last = atomic_load(&g_ckpt_n);
        VLOG("device lost: GPU markers: all finished before #%d, started up to #%d (last %u recorded)", (int)bottom - 1, (int)top - 1, last);
        if (bottom && top >= bottom) {
            uint32_t from = bottom - 1, to = top - 1 > from + 60 ? from + 60 : top - 1;
            for (uint32_t k = from; k <= to; k++) checkpoint_line(k == from ? "in flight (first)" : "in flight", k);
            if (top - 1 > to) VLOG("  ... %u more up to #%u", top - 1 - to, top - 1);
        }
        for (uint32_t k = top; k < top + 3 && k < last; k++) checkpoint_line("not started", k);
    }
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
        checkpoint_addr(c->k1);
    }
}

// ---- deferred destruction: objects given up while a submission is recorded may be used by its earlier commands (a
// texture view replaced in the middle of a frame, a texture the game finalizes). Destroying them after waiting for the
// last submitted work (as before) freed what the open submission still referred to: the GPU read a destroyed view and
// the device was lost (TOTK's item-get screen, 2026-10-24). Now each waits for the first submission after it was given up.
// Deleted memory pools' imports too (mtl_pool_del): another game thread's submission, recorded while the pool was being
// deleted, brought the copies of recently used pools up to date from it (2026-10-27, L16).
typedef struct dead { uint64_t fence; VkImageView view; VkImage img; VkDeviceMemory mem; VkBuffer buf; VkShaderEXT sh; struct dead *next; uint32_t imem; } dead;
static dead *g_dead_new, *g_dead;   // given up since the last submission (no fence yet) / waiting for their fence
static pthread_mutex_t g_dead_mu = PTHREAD_MUTEX_INITIALIZER;
static void destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem, VkBuffer buf) {
    dead *d = malloc(sizeof *d);
    if (!d) die("deferred destruction");
    *d = (dead){0, view, img, mem, buf, VK_NULL_HANDLE, NULL};
    pthread_mutex_lock(&g_dead_mu);
    d->next = g_dead_new; g_dead_new = d;
    pthread_mutex_unlock(&g_dead_mu);
}
void vkb_destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem) { destroy_later(view, img, mem, VK_NULL_HANDLE); }
void vkb_destroy_image_later(VkImage img, VkDeviceMemory mem, uint32_t imem) {
    if (!imem) { destroy_later(VK_NULL_HANDLE, img, mem, VK_NULL_HANDLE); return; }
    dead *d = malloc(sizeof *d);
    if (!d) die("deferred destruction");
    *d = (dead){0, VK_NULL_HANDLE, img, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, NULL};
    d->imem = imem;
    pthread_mutex_lock(&g_dead_mu);
    d->next = g_dead_new; g_dead_new = d;
    pthread_mutex_unlock(&g_dead_mu);
}
// A shader object no program holds any more (vkb_shader.c): the submission being recorded may still bind it.
void vkb_destroy_shader_later(VkShaderEXT sh) {
    dead *d = malloc(sizeof *d);
    if (!d) die("deferred destruction");
    *d = (dead){0, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, sh, NULL};
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
        if (d->sh) vk.vkDestroyShaderEXT(vkc.device, d->sh, NULL);
        if (d->view) vk.vkDestroyImageView(vkc.device, d->view, NULL);
        if (d->img) vk.vkDestroyImage(vkc.device, d->img, NULL);
        if (d->buf) vk.vkDestroyBuffer(vkc.device, d->buf, NULL);
        if (d->mem) vk.vkFreeMemory(vkc.device, d->mem, NULL);
        if (d->imem) vkb_imem_free(d->imem);   // after the image that was bound to it
        free(d);
    }
}

static void gpu_span(int slot, uint64_t fence);
/* The GPU's work per game frame (hle_nvn.c's long-frame log, mtl_gpu_ms): the submissions made after mtl_set_frame(f)
   count for frame f. For each: its GPU time (its two timestamps), when it ended (about when the retire thread saw its
   fence: it waits on fences in order) and so began, and how long after it went to the queue the GPU began it (behind
   earlier work). Filled by the retire thread; read 8 frames later. */
#define GF_N 64
static struct { uint64_t f, ns, beg, end; double lag_ms; unsigned subs; } g_gf[GF_N];
static _Atomic uint64_t g_gf_frame;
static int g_tsq_ready(void);
static VkQueryPool g_tsq_pool(void);
void mtl_set_frame(uint64_t f) { atomic_store(&g_gf_frame, f); }
static void gpu_frame(const job *j) {
    uint64_t t[2], end = runtime_now_ns();
    if (!g_tsq_ready() || j->slot < 0) return;
    if (vk.vkGetQueryPoolResults(vkc.device, g_tsq_pool(), (uint32_t)(2 * j->slot), 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || t[1] <= t[0]) return;
    uint64_t dur = (uint64_t)((double)(t[1] - t[0]) * vkc.props.limits.timestampPeriod), beg = end > dur ? end - dur : 0;
    __typeof__(g_gf[0]) *g = &g_gf[j->frame % GF_N];
    if (g->f != j->frame) memset(g, 0, sizeof *g), g->f = j->frame, g->beg = beg;
    g->ns += dur, g->subs++;
    if (beg < g->beg) g->beg = beg;
    if (end > g->end) g->end = end;
    double lag = beg > j->t_sub ? (beg - j->t_sub) / 1e6 : 0;
    if (lag > g->lag_ms) g->lag_ms = lag;
}
double mtl_gpu_ms(uint64_t f) { return g_gf[f % GF_N].f == f ? g_gf[f % GF_N].ns / 1e6 : -1; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    if (g_gf[f % GF_N].f != f || !g_gf[f % GF_N].subs) { *t0 = *t1 = 0; return 0; }
    *t0 = g_gf[f % GF_N].beg, *t1 = g_gf[f % GF_N].end;
    return 1;
}
double mtl_gpu_lag_ms(uint64_t f) { return g_gf[f % GF_N].f == f ? g_gf[f % GF_N].lag_ms : 0; }
// Retires submissions in order: waits for the GPU, writes their events and counters, completes their fence.
static void *retire_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-vk-retire");
    t_retire = 1;
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
        gpu_frame(j);
        pc_writeback(j->fence);   // pool copies: GPU writes into game memory, before the game is told
        { void vkb_tex_wb_retire(uint64_t fence); vkb_tex_wb_retire(j->fence); }   // GPU-drawn images copied at its end (vkb_tex.c), before it counts as done
        if (j->wb) { wb_merge(j->wb, j->nwb, g_wbst[j->slot].map, (const wb_snap_t *)j->snaps, j->nsnaps, j->frame + 1); free(j->wb); }   // GPU writes into game memory (GPU-only ones held: wb_merge)
        for (int k = 0; k < j->nsnaps; k++) if (((wb_snap_t *)j->snaps)[k].owned) free(((wb_snap_t *)j->snaps)[k].data);
        free(j->snaps);
        run_posts(j->ops, j->n, j->oq, j->noq);
        free(j->oq);
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
static int g_tsq_ready(void) { return g_tsq != VK_NULL_HANDLE; }
static VkQueryPool g_tsq_pool(void) { return g_tsq; }
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
enum { SUB_SNAP = 41 };   // [14..24]: vkb_tex.c's upload parts (vkb_tex_upload_parts); [25..40]: its slow BC7 reads
static uint64_t g_sub_t0, g_sub_snap[SUB_SNAP];
void vkb_tex_upload_parts(uint64_t v[11]);
void vkb_tex_slow_reads(uint64_t v[16]);
static void sub_snap(uint64_t v[SUB_SNAP]) {
    v[0] = vkb_step_ns[PK_CREATE]; v[1] = vkb_step_ns[PK_UPLOAD]; v[2] = vkb_astc_ns; v[3] = vkb_shader_load_ns;
    v[4] = g_sh_scan_ns; v[5] = g_sh_in_bytes;
    v[6] = vkb_step_ns[PK_ALLOC]; v[7] = vkb_step_ns[PK_UCAP]; v[8] = vkb_step_ns[PK_SYNC]; v[9] = vkb_step_ns[PK_SUBMIT]; v[10] = g_slot_wait_ns;
    v[11] = vkb_step_ns[PK_UPWAIT]; v[12] = vkb_step_ns[PK_SLOWDRAW]; v[13] = vkb_step_n[PK_SLOWDRAW];
    vkb_tex_upload_parts(&v[14]);
    vkb_tex_slow_reads(&v[25]);
}
static void sub_long_check(uint64_t fence) {
    uint64_t now = runtime_now_ns(), v[SUB_SNAP];
    if (!g_sub_t0 || now - g_sub_t0 < 100000000ull) return;
    sub_snap(v);
    static int said;
    if (said++ < 64)
        VLOG("long submission %llu: recorded in %.0f ms: waited for a free slot %.0f ms, new images %.0f ms, uploads %.0f ms (ASTC %.0f ms), "
             "shaders made %.0f ms, memory made %.0f ms, uniform snapshot %.0f ms, pool copy checks %.0f ms, pool copies in %.1f MiB; "
             "draws %llu (submit and pool sync after it: of the submission before %.0f / %.0f ms)", (unsigned long long)fence,
             (now - g_sub_t0) / 1e6, (v[10] - g_sub_snap[10]) / 1e6, (v[0] - g_sub_snap[0]) / 1e6, (v[1] - g_sub_snap[1]) / 1e6,
             (v[2] - g_sub_snap[2]) / 1e6, (v[3] - g_sub_snap[3]) / 1e6, (v[6] - g_sub_snap[6]) / 1e6, (v[7] - g_sub_snap[7]) / 1e6,
             (v[4] - g_sub_snap[4]) / 1e6, (v[5] - g_sub_snap[5]) / 1048576.0, (unsigned long long)vkb_draw_count(),
             (v[9] - g_sub_snap[9]) / 1e6, (v[8] - g_sub_snap[8]) / 1e6);
    if (said <= 64)
        VLOG("long submission %llu: uploads waiting for one another %.0f ms; draw calls over 1 ms after a shader change %llu, %.0f ms",
             (unsigned long long)fence, (v[11] - g_sub_snap[11]) / 1e6, (unsigned long long)(v[13] - g_sub_snap[13]), (v[12] - g_sub_snap[12]) / 1e6);
    if (said <= 64) {   // the uploads by part (vkb_tex.c UPX_*), over all recording threads
        uint64_t *a = &v[14], *b = &g_sub_snap[14];
        VLOG("long submission %llu: uploads by part: %llu prepared (%llu from virtual pools, %.1f MiB) and %llu inline; gathering "
             "from virtual pools %.0f ms, texel layout %.0f ms, BC7 keys %.0f ms, BC7 reads %.0f ms, upload memory %.0f ms, under the "
             "uploads lock %.0f ms (of it inline uploads %.0f ms)", (unsigned long long)fence, (unsigned long long)(a[7] - b[7]),
             (unsigned long long)(a[8] - b[8]), (a[10] - b[10]) / 1048576.0, (unsigned long long)(a[9] - b[9]), (a[0] - b[0]) / 1e6,
             (a[1] - b[1]) / 1e6, (a[2] - b[2]) / 1e6, (a[3] - b[3]) / 1e6, (a[4] - b[4]) / 1e6, (a[5] - b[5]) / 1e6, (a[6] - b[6]) / 1e6);
        // the textures whose BC7 reads went to the disk (over 1 ms each), by what the warming had done (vkb_tex.c PFS_*)
        uint64_t *c = &v[25], *d = &g_sub_snap[25], n[7], all = 0, ns = 0;
        for (int i = 0; i < 7; i++) n[i] = c[i] - d[i], all += n[i], ns += c[7 + i] - d[7 + i];
        if (all)
            VLOG("long submission %llu: slow BC7 reads: %llu textures (%.0f ms); warmed %llu (%.0f ms, warmed %.0f presents before), queued "
                 "not warmed yet %llu (%.0f ms), queue full %llu, taken for seen %llu, in a virtual pool %llu (%.0f ms), outside game memory %llu, "
                 "not known %llu (%.0f ms); initialized %.0f presents before on average", (unsigned long long)fence, (unsigned long long)all,
                 ns / 1e6, (unsigned long long)n[6], (c[13] - d[13]) / 1e6, n[6] ? (double)(c[15] - d[15]) / (double)n[6] : 0.0,
                 (unsigned long long)n[3], (c[10] - d[10]) / 1e6, (unsigned long long)n[4], (unsigned long long)n[5], (unsigned long long)n[1],
                 (c[8] - d[8]) / 1e6, (unsigned long long)n[2], (unsigned long long)n[0], (c[7] - d[7]) / 1e6,
                 all > n[0] ? (double)(c[14] - d[14]) / (double)(all - n[0]) : 0.0);
    }
}

int mtl_slot_free(void) {   // a submission can begin without waiting for the GPU (work that can wait checks first)
    if (g_ok != 1) return 0;
    if (g_nslots < SLOTS) return 1;
    pthread_mutex_lock(&g_mu);
    uint64_t done = g_done;
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < g_nslots; i++) if (g_slots[i].fence <= done) return 1;
    return 0;
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
    int oq = type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_on();
    post((post_op){2, NULL, (uint32_t)oq, 0, addr, type, oq ? oq_event(1) : 0});
}
static void sub_reset(uint32_t type) {   // RESET_COUNTER (the walk or one thread): only samples passed counts
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_on()) post((post_op){3, NULL, 1, 0, 0, type, oq_event(0)});
}
static void chunk_counter(uint32_t type, int report) {   // a chunk: the walk posted it; the count's state steps the same
    if (type == NVN_COUNTER_TYPE_SAMPLES_PASSED && oq_on()) oq_event(report);
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

// Inline updates kept in the copy (vkb_ucap.c) while a pass is open: written to memory when it ends, all at once.
typedef struct { uint64_t dst, n; const uint8_t *data; } ub_t;
static __thread ub_t *t_ub;
static __thread int t_nub, t_capub;
static void ub_write(VkCommandBuffer cb, uint64_t dst, uint64_t n, const uint8_t *data) {   // between full barriers
    VkBuffer db; VkDeviceSize off;
    if (!vkb_gpu_addr(dst, n, &db, &off)) return;
    // vkCmdUpdateBuffer: 4-byte aligned, at most 65536 bytes per call
    if (!(off & 3) && !(n & 3)) {
        for (uint64_t k = 0; k < n; k += 65536) vk.vkCmdUpdateBuffer(cb, db, off + k, n - k < 65536 ? n - k : 65536, data + k);
    } else {
        vkb_audit(A_UPDATE_SKIPPED, NULL);
        static int said; if (said++ < 4) VLOG("[temporary] unaligned uniform update %#llx+%llu: skipped", (unsigned long long)dst, (unsigned long long)n);
    }
}
static void ub_flush(VkCommandBuffer cb) {   // the pass has ended
    if (!t_nub) return;
    for (int i = 0; i < t_nub; i++) { vkb_mirrors_evict(t_ub[i].dst, t_ub[i].n); vkb_pool_use(t_ub[i].dst, t_ub[i].n, 1); }
    full_barrier(cb);
    for (int i = 0; i < t_nub; i++) ub_write(cb, t_ub[i].dst, t_ub[i].n, t_ub[i].data);
    full_barrier(cb);
    t_nub = 0;
}
int vkb_ub_deferred(uint64_t addr, uint64_t size) {   // a kept update of this pass not yet in memory overlaps the range
    for (int i = 0; i < t_nub; i++) if (t_ub[i].dst < addr + size && addr < t_ub[i].dst + t_ub[i].n) return 1;
    return 0;
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
    // Kept in the copy: later draws read a version with it, CPU reads see it (vkb_ucap.c); else later draws read the
    // updated memory and CPU reads (pointers) the updated bytes.
    int kept = vkb_ucap_update(dst, n, data);
    if (kept && pass_open()) {
        if (t_nub == t_capub) { t_capub = t_capub ? t_capub * 2 : 256; if (!(t_ub = realloc(t_ub, sizeof *t_ub * (size_t)t_capub))) die("kept uniform updates"); }
        t_ub[t_nub++] = (ub_t){dst, n, data};
        return;
    }
    pass_end_why(PE_UPDATE);
    vkb_mirrors_evict(dst, n);
    vkb_pool_use(dst, n, 1);
    VkCommandBuffer cb = vkb_cmd();
    full_barrier(cb);
    ub_write(cb, dst, n, data);
    full_barrier(cb);
}

// ---------------- render passes (milestone 3.3 step 3) ----------------
// SetRenderTargets only records the targets; the pass (dynamic rendering) begins at the first command that renders and
// ends before anything that is not a draw or a clear (copies, other passes, the end of the submission).
static __thread p_set_rt g_rt;       // current render targets (per recording thread)
static _Atomic uint64_t g_ticket;   // presents so far
static __thread int g_in_pass;
static int pass_open(void) { return g_in_pass; }
static __thread uint32_t g_pass_fb;      // the current pass's feedback targets (vkb.h)
static __thread VkExtent2D g_pass_extent;

VkCommandBuffer vkb_cmd(void) { return t_cb ? t_cb : g_cur >= 0 ? g_slots[g_cur].cb : VK_NULL_HANDLE; }
int vkb_slot(void) { return g_cur; }
int vkb_slot_idle(int s) { return s != g_cur && s < g_nslots && g_slots[s].fence <= mtl_done(); }   // (vkb_ring_reclaim)
int vkb_in_pass(void) { return g_in_pass; }
const p_set_rt *vkb_rt(void) { return &g_rt; }
uint64_t vkb_frame(void) { return atomic_load(&g_ticket); }
int vkb_rt_count(void) { return g_rt.n < 0 ? 0 : g_rt.n > 8 ? 8 : g_rt.n; }
static __thread int t_obj_slot = -1;   // pass_begin: the object motion target's slot in the open pass
int vkb_pass_obj_slot(void) { return g_in_pass ? t_obj_slot : -1; }
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

// A game barrier held inside a pass (OP_BARRIER below): made when the pass ends, for whatever comes after it.
static __thread int g_barrier_held;
static void pass_end(void) {
    if (!g_in_pass) return;
    oq_close();   // an occlusion query begun in the pass ends in it
    vk.vkCmdEndRendering(vkb_cmd());
    pp_end(vkb_cmd());
    vkb_tex_block_flush(vkb_cmd());
    g_in_pass = 0;
    ub_flush(vkb_cmd());   // the pass's kept uniform updates reach memory
    if (g_barrier_held) { g_barrier_held = 0; full_barrier(vkb_cmd()); }
}
// nvnCommandBufferBarrier inside a pass that only orders primitives / fragments and invalidates the texture cache
// (ORDER_PRIMITIVES 0x1, ORDER_FRAGMENTS 0x2, ORDER_FRAGMENTS_TILED 0x4, INVALIDATE_TEXTURE 0x10): held instead of ending
// the pass. TotK puts one (0x12) after each of its G-buffer decals, which read the G-buffer pixel they draw into: ~900 a
// frame in a shrine, each a pass ended and begun again behind a full drain - 11 of the frame's 17.6 GPU ms, and a game at
// 70 fps shown at 24 (2026-11-07). In a pass the draws write only its targets (a storing draw ends it); a later draw
// that samples one gets the barrier inside the pass by region (vkb_draw.c, VK_KHR_dynamic_rendering_local_read), the
// others read nothing written since the pass began, and the held barrier is made when the pass ends.
// TOTK_VK_HOLD_BARRIER=0: every game barrier ends the pass, as before.
static int hold_barrier(int bits) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_HOLD_BARRIER"); v = !(e && *e == '0') && !getenv("TOTK_VK_NO_LOCAL_BARRIER"); }
    return v && g_in_pass && vkc.has_local_read && !(bits & ~0x17);
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
static void span_presync(void);
// A pass's targets (2026-11-02): an image whose layout changes gets its barrier with the move (vkb_image_use); the others
// none of their own, only one barrier for the whole pass from the target writes before (an earlier pass's) to this
// pass's tests, output and sampling. Before, every target of every pass had its own barrier from ALL_COMMANDS: ~1700
// full GPU drains a frame on the RTX 3060 (587 passes, 3600 barriers), costlier on AMD. TOTK_VK_PASS_BARRIER=1: as before.
static int pass_barrier_old(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_PASS_BARRIER"); v = e && *e == '1'; }
    return v;
}
static int pass_begin(VkExtent2D bare, uint32_t fb) {
    if (g_in_pass) return 1;
    shadow_presync();
    span_presync();   // ranges across pools brought up to date (outside a pass)
    g_pass_serial++;
    if (!vkc.has_feedback_loop && !vkb_fb_general()) fb = 0;
    const VkImageLayout fb_layout = vkc.has_feedback_loop ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
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
        // A texture of several levels, layers or slices drawn for the first time, never uploaded: its game memory first.
        // The draw covers one part, the CPU may have filled the rest: TOTK writes level 0 of a 32x32x32 noise volume and
        // draws its mips from it (then compresses them into BC4, L21); the image counted as GPU-written from that first
        // draw on, level 0 was never uploaded and every level stayed zero.
        if (im && !im->gpu_written && !im->upload_stamp && (im->levels > 1 || im->layers > 1 || im->depth > 1)) vkb_tex_upload(cb, im);
        uint32_t bw, bh;
        VkImageView bview = im && im->compressed && !vkb_view(im, v, 1, -1) ? vkb_tex_block_target(cb, im, v, &bw, &bh) : VK_NULL_HANDLE;
        if (bview) {   // GPU compression: drawn into an image of blocks, copied into the texture at the pass end (L21)
            color[i].imageView = bview;
            color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            g_pass_fb &= ~(1u << i);   // the draws may sample the texture: not this image
            if (bw < w) w = bw;
            if (bh < h) h = bh;
            any = 1;
            continue;
        }
        if (!im || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !vkb_view(im, v, 1, -1)) {
            if (im) vkb_audit(A_RT_NOT_ATTACHABLE, NULL);
            if (im) {   // once per texture: which views of which textures (GPU compression into block-sized texels, L19)
                static uint64_t seen[64];
                static int nseen;
                int k = 0;
                while (k < nseen && seen[k] != im->serial) k++;
                if (k == nseen && nseen < 64) {
                    seen[nseen++] = im->serial;
                    VLOG("[temporary] texture %llu (format %#x %ux%u levels %u at %p) as a render target: not attachable (compressed); view %s%#x level %d, present %llu",
                         (unsigned long long)im->serial, im->d.format, im->width, im->height, im->levels, (const void *)im->storage,
                         v && v->set_format ? "format " : "no format ", v && v->set_format ? v->format : 0, v && v->set_levels ? v->base_level : 0,
                         (unsigned long long)vkb_frame());
                }
            }
            continue;
        }
        int loop = (fb >> i) & 1;
        VkImageLayout layout = loop ? fb_layout : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (pass_barrier_old() || !vkb_layout_is(im, layout))
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
    {   // the temporal upscaler's object motion target at a slot the scene's G-buffer pass leaves free (vkb_tsr.c)
        int os;
        VkImageView ov = any ? vkb_tsr_obj_target(&g_rt, &os) : VK_NULL_HANDLE;
        t_obj_slot = ov ? os : -1;
        if (ov) {
            for (int i = ncolor; i < os; i++) color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            color[os] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = ov,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (os >= ncolor) ncolor = os + 1;
        }
    }
    vkb_image *dim = g_rt.depth ? vkb_image_of(g_rt.depth) : NULL;
    if (dim && dim->refresh_from) vkb_tex_refresh(cb, dim);
    if (dim && (dim->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        const view_t *v = g_rt.has_dview ? &g_rt.dview : NULL;
        int loop = (fb & VKB_FB_DEPTH) != 0;
        VkImageLayout layout = loop ? fb_layout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (pass_barrier_old() || !vkb_layout_is(dim, layout))
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
    if (any && !pass_barrier_old()) {   // the target writes of earlier passes before this pass's (see pass_barrier_old)
        const VkPipelineStageFlags2 att = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = att,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = att | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &dep);
    }
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
// TOTK_RT_LOG=<NVN format>:<width>x<height> (development): render targets of that format and size, once per texture
// serial and level, with their memory and shape; and draws into one that conditional rendering skipped (terrain tiles
// baked into 0xd:260x260 targets, L19).
int vkb_rt_log_d(const nvn_texdesc *d) {   // TOTK_RT_LOG matches this descriptor (also copies, clears, uploads)
    static int fmt = -2;
    static unsigned w, h;
    if (fmt == -2) { const char *e = getenv("TOTK_RT_LOG"); fmt = e && sscanf(e, "%i:%ux%u", &fmt, &w, &h) == 3 ? fmt : -1; }
    return fmt >= 0 && d && (int)d->format == fmt && d->width == w && d->height == h;
}
int vkb_rt_log_match(const tex_t *t) { return t && vkb_rt_log_d(&t->d); }
static void rt_log(int skipped) {
    static uint64_t seen[8192];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    for (int i = 0; i < g_rt.n && i < 8; i++) {
        const tex_t *t = g_rt.color[i];
        if (!vkb_rt_log_match(t)) continue;
        unsigned level = g_rt.has_cview[i] && g_rt.cview[i].set_levels ? (unsigned)g_rt.cview[i].base_level : 0;
        // once per texture, level and present (a target drawn again later, as when a save loads, shows again)
        uint64_t k = ((t->serial << 5 | level) << 1 | (uint64_t)skipped) * 0x100000001B3ull ^ vkb_frame();
        pthread_mutex_lock(&mu);
        int dup = 0;
        for (unsigned j = (unsigned)(k * 0x9E3779B97F4A7C15ull >> 51), n = 0; n < 8192; j = (j + 1) & 8191, n++) {
            if (seen[j] == k + 1) { dup = 1; break; }
            if (!seen[j]) { seen[j] = k + 1; break; }
        }
        pthread_mutex_unlock(&mu);
        if (dup) continue;
        vkb_image *im = vkb_image_of(t);
        VLOG("[rtlog] %s texture %llu level %u (image %llu%s) format %#x %ux%u levels %u flags %#x at %p, present %llu",
             skipped ? "draw skipped (conditional rendering) into" : "target", (unsigned long long)t->serial, level,
             im ? (unsigned long long)im->serial : 0ull, im && im->gpu_written ? ", GPU-written" : "", t->d.format, t->d.width, t->d.height,
             t->d.levels, t->d.flags, (const void *)t->mem, (unsigned long long)vkb_frame());
    }
}
static void trace_rt(void) {
    rt_log(0);
    if (!trace_left()) return;
    VLOG("[trace] SetRenderTargets %d colour + %s", g_rt.n, g_rt.depth ? "depth" : "no depth");
    for (int i = 0; i < g_rt.n && i < 8; i++) trace_tex("colour", i, g_rt.color[i], g_rt.has_cview[i] ? &g_rt.cview[i] : NULL);
    trace_tex("depth", 0, g_rt.depth, g_rt.has_dview ? &g_rt.dview : NULL);
}

int vkb_pass_begin(VkExtent2D bare, uint32_t fb) { return pass_begin(bare, fb); }
uint32_t vkb_pass_fb(void) { return g_in_pass ? g_pass_fb : 0; }
vkb_cnt vkb_pass_end_n[PE_COUNT];   // passes ended, by reason (the per-frame log line)
static int rt_keep(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_RT_KEEP"); on = !e || *e != '0'; } return on; }
static void pass_end_why(int why) { if (g_in_pass) vkb_cnt_add(vkb_pass_end_n[why], 1); pass_end(); }
void vkb_pass_end_r(int why) { pass_end_why(why); }
void vkb_pass_end(void) { pass_end_why(PE_OTHER); }

// The area a clear covers: on NVN (as in GL) clears are limited to the scissor rectangle. TOTK builds the Depths'
// revealed-area mask (500x400) in 50x50 tiles, clearing each tile under its scissor before drawing it; clearing the
// whole target wiped the tiles drawn before, so the mask stayed empty: the Depths' ground dark and its map unrevealed
// (L20, 2026-10-27). TOTK_VK_CLEAR_SCISSOR=0: whole target as before (A/B).
static VkRect2D clear_rect(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VK_CLEAR_SCISSOR"); on = !e || *e != '0'; }
    VkRect2D full = {{0, 0}, g_pass_extent}, s;
    if (!on || !vkb_scissor(&s)) return full;
    int32_t x0 = s.offset.x, y0 = s.offset.y, x1 = x0 + (int32_t)s.extent.width, y1 = y0 + (int32_t)s.extent.height;
    if (x1 > (int32_t)g_pass_extent.width) x1 = (int32_t)g_pass_extent.width;
    if (y1 > (int32_t)g_pass_extent.height) y1 = (int32_t)g_pass_extent.height;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}
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
    VkClearRect r = {clear_rect(), 0, 1};
    if (!r.rect.extent.width || !r.rect.extent.height) return;
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
    VkClearRect r = {clear_rect(), 0, 1};
    if (!r.rect.extent.width || !r.rect.extent.height) return;
    vkb_checkpoint("clear attachment", NULL, NULL);
    vk.vkCmdClearAttachments(vkb_cmd(), 1, &a, 1, &r);
}

// ClearTexture: a region of one texture outside a pass
static void clear_tex(const p_clear_tex *c) {
    pass_end_why(PE_CLEAR_TEX);
    vkb_image *im = vkb_image_of(c->tex);
    if (!im) return;
    if (vkb_rt_log_match(c->tex)) VLOG("[rtlog] clear texture %llu (image %llu), present %llu", (unsigned long long)c->tex->serial, (unsigned long long)im->serial, (unsigned long long)vkb_frame());
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

// TOTK_SHOT=<present number>[+<count>]:<file.bmp>: saves that presented frame (development check of the picture); with a
// count, that many presents in a row to <file>_<present>.bmp, copied into one buffer without waiting on the GPU in between
// (written once the last one is done), so the frames keep their usual timing (flicker between frames).
static VkBuffer g_shot_buf;
static VkDeviceMemory g_shot_mem;
static long long g_shot_at = -1, g_shot_count = 1;
static uint64_t g_shot_n[256];
static int g_shot_got;
static VkBuffer shot_record(VkCommandBuffer cb, uint64_t n, VkImage out, uint32_t w, uint32_t h) {
    if (g_shot_at == -1) {
        const char *e = getenv("TOTK_SHOT"), *c = e ? strchr(e, '+') : NULL;
        g_shot_at = e ? atoll(e) : 0;
        if (c) g_shot_count = atoll(c + 1);
        if (g_shot_count < 1 || g_shot_count > 256) g_shot_count = 1;
    }
    // the buffer is made 30 presents ahead: a large allocation at the first frame shot would upset that frame's timing
    if (g_shot_at <= 0 || n + 30 < (uint64_t)g_shot_at || g_shot_got >= g_shot_count) return VK_NULL_HANDLE;
    VkDeviceSize one = (VkDeviceSize)w * h * 4;
    if (!g_shot_buf) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = one * g_shot_count, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_shot_buf), "shot buffer")) { g_shot_at = 0; return VK_NULL_HANDLE; }
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, g_shot_buf, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
            if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                type = i;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_shot_mem), "shot memory")) { g_shot_at = 0; return VK_NULL_HANDLE; }
        vk.vkBindBufferMemory(vkc.device, g_shot_buf, g_shot_mem, 0);
    }
    if (n < (uint64_t)g_shot_at) return VK_NULL_HANDLE;
    VkBufferImageCopy r = {.bufferOffset = one * g_shot_got, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vk.vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_shot_buf, 1, &r);
    g_shot_n[g_shot_got++] = n;
    return g_shot_got == g_shot_count ? g_shot_buf : VK_NULL_HANDLE;
}
static void shot_write(uint64_t fence, uint32_t w, uint32_t h) {
    const char *e = getenv("TOTK_SHOT"), *path = e ? strchr(e, ':') : NULL;
    mtl_wait(fence, UINT64_MAX);
    uint8_t *px = NULL;
    if (path && vk.vkMapMemory(vkc.device, g_shot_mem, 0, VK_WHOLE_SIZE, 0, (void **)&px) == VK_SUCCESS) {
        uint32_t size = w * h * 4;
        for (int k = 0; k < g_shot_got; k++) {
            char name[1024];
            const char *dot = strrchr(path + 1, '.');
            if (g_shot_count == 1 || !dot) snprintf(name, sizeof name, "%s", path + 1);
            else snprintf(name, sizeof name, "%.*s_%llu%s", (int)(dot - path - 1), path + 1, (unsigned long long)g_shot_n[k], dot);
            FILE *f = fopen(name, "wb");
            if (!f) continue;
            uint8_t hdr[54] = {'B', 'M'};   // 32-bit BGRA, top-down
            uint32_t v;
            v = 54 + size; memcpy(hdr + 2, &v, 4);
            v = 54; memcpy(hdr + 10, &v, 4);
            v = 40; memcpy(hdr + 14, &v, 4);
            memcpy(hdr + 18, &w, 4);
            int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
            hdr[26] = 1; hdr[28] = 32;
            memcpy(hdr + 34, &size, 4);
            fwrite(hdr, 1, sizeof hdr, f);
            fwrite(px + (size_t)size * k, 1, size, f);
            fclose(f);
        }
        VLOG("%d frame(s) saved to %s (%ux%u)", g_shot_got, path + 1, w, h);
        vk.vkUnmapMemory(vkc.device, g_shot_mem);
    }
    vk.vkDestroyBuffer(vkc.device, g_shot_buf, NULL);
    vk.vkFreeMemory(vkc.device, g_shot_mem, NULL);
    g_shot_buf = VK_NULL_HANDLE;
}
static _Atomic long long g_dump_key_at;   // vkb_dump_request (F9)
// F9's own lines (its draw trace, [dump], [vcheck]: tens of thousands) go to <dir>/f<N>/f9.log from the key to the dump,
// the game's log keeps one line per snapshot. TOTK_DUMP_KEY_LOG_MB (default 16): past it the rest of the snapshot's
// lines are dropped. The file is flushed at the dump and closed at the next F9: a line already on its way still lands.
static FILE *_Atomic g_f9_log;
static FILE *g_f9_prev;
static _Atomic long long g_f9_bytes;
static atomic_int g_f9_over;
static long long g_f9_cap;
static char g_f9_dir[512];
FILE *dm_log_out(void) {
    FILE *f = atomic_load_explicit(&g_f9_log, memory_order_acquire);
    return !f ? stderr : atomic_load_explicit(&g_f9_over, memory_order_relaxed) ? NULL : f;
}
void dm_log_wrote(FILE *f, int n) {
    if (atomic_fetch_add(&g_f9_bytes, n) + n < g_f9_cap || atomic_exchange(&g_f9_over, 1)) return;
    fprintf(f, "[totk-gfx] [limit] f9.log reached %lld MB (TOTK_DUMP_KEY_LOG_MB): the rest of this snapshot's lines are not written\n",
            g_f9_cap >> 20);
}

// F9 range check (development): the index, vertex, constant and storage ranges the traced draws read (vkb_vcheck_add), compared once the
// GPU is idle between the pool's copy in device memory and game memory. A range that differs where the CPU has not
// written since the copy's update means draws read different data than the game holds (the copy went wrong).
// Also, for the vertex and index ranges: their game memory hashed when the draw was recorded; a range the traced frames
// read with different contents is one the CPU rewrites frame by frame (TOTK skins on the GPU: cloth, L13).
typedef struct { uint64_t addr, len, hash; uint32_t draw; int kind; } vrange_t;
static vrange_t *g_vr;
static int g_nvr, g_capvr;
static pthread_mutex_t g_vr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t vr_hash(uint64_t addr, uint64_t len) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i + 8 <= len; i += 8) { uint64_t v; memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; }
    return h;
}
void vkb_vcheck_add(uint64_t addr, uint64_t len, uint32_t draw, int kind) {
    uint64_t h = len <= (64u << 20) ? vr_hash(addr, len) : 0;
    pthread_mutex_lock(&g_vr_mu);
    int dup = 0;
    for (int k = g_nvr - 1; k >= 0 && k >= g_nvr - 32 && !dup; k--) dup = g_vr[k].addr == addr && g_vr[k].len == len && g_vr[k].hash == h;
    if (!dup && g_nvr < 60000) {
        if (g_nvr == g_capvr && !(g_vr = realloc(g_vr, sizeof *g_vr * (size_t)(g_capvr = g_capvr ? g_capvr * 2 : 1024)))) die("range check");
        g_vr[g_nvr++] = (vrange_t){addr, len, h, draw, kind};
    }
    pthread_mutex_unlock(&g_vr_mu);
}
static int vr_cmp(const void *a, const void *b) {
    const vrange_t *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return x->draw < y->draw ? -1 : x->draw > y->draw;
}
static void vchange_report(void) {   // g_vr_mu held
    vrange_t *s = malloc(sizeof *s * (size_t)(g_nvr ? g_nvr : 1));
    if (!s) return;
    memcpy(s, g_vr, sizeof *s * (size_t)g_nvr);
    qsort(s, (size_t)g_nvr, sizeof *s, vr_cmp);
    int said = 0;
    for (int i = 0; i < g_nvr;) {
        int j = i, changed = 0;
        while (j < g_nvr && s[j].addr == s[i].addr && s[j].len == s[i].len) { changed |= s[j].hash != s[i].hash; j++; }
        static const char *const kn[] = {"indices", "vertices", "constants", "storage"};
        if (changed && said++ < 400)
            VLOG("[vchange] %s %#llx+%llu: contents differ between the traced draws #%u and #%u (%d reads)", kn[s[i].kind & 3],
                 (unsigned long long)s[i].addr, (unsigned long long)s[i].len, s[i].draw, s[j - 1].draw, j - i);
        i = j;
    }
    VLOG("[vchange] %d ranges read with different contents in the traced frames", said);
    free(s);
}
static pool_ent *pool_find_mut(uintptr_t addr, uint64_t len);
static void vcheck_run(void) {
    enum { CAP = 64u << 20 };
    VkBuffer st = VK_NULL_HANDLE;
    VkDeviceMemory sm = VK_NULL_HANDLE;
    uint8_t *map = NULL;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = CAP, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &st), "range check buffer")) return;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, st, &req);
    uint32_t type = UINT32_MAX, want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    void *m;
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &sm), "range check memory")) { vk.vkDestroyBuffer(vkc.device, st, NULL); return; }
    vk.vkBindBufferMemory(vkc.device, st, sm, 0);
    if (!vk_check(vk.vkMapMemory(vkc.device, sm, 0, VK_WHOLE_SIZE, 0, &m), "range check map")) goto out;
    map = m;
    static const char *const kinds[] = {"indices", "vertices", "constants", "storage"};
    pthread_mutex_lock(&g_vr_mu);
    vchange_report();
    int checked = 0, bad = 0, cpu = 0, said = 0;
    for (int k = 0; k < g_nvr;) {   // in batches that fit the staging buffer
        typedef struct { vrange_t r; VkDeviceSize at; uint64_t stamp; } item;
        static item it[4096];
        int n = 0;
        VkDeviceSize used = 0;
        mtl_sub_begin();
        VkCommandBuffer cb = vkb_cmd();
        vkb_rw_read(&g_pool_mu, LK_POOLS);
        for (; k < g_nvr && n < 4096; k++) {
            const vrange_t *r = &g_vr[k];
            if (used + r->len > CAP) { if (!n) { k++; continue; } break; }   // larger than the buffer: not checked
            pool_ent *p = pool_find_mut((uintptr_t)r->addr, r->len);
            if (!p || !p->sh || !p->filled) continue;   // read from game memory itself: nothing to compare
            VkBufferCopy c = {p->soff + (r->addr - p->base), used, r->len};
            vk.vkCmdCopyBuffer(cb, p->sbuf, st, 1, &c);
            it[n++] = (item){*r, used, p->stamp};
            used += (r->len + 15) & ~15ull;
        }
        vkb_rw_read_end(&g_pool_mu);
        mtl_wait(mtl_sub_commit(), UINT64_MAX);
        for (int i = 0; i < n; i++) {
            const uint8_t *g = (const uint8_t *)(uintptr_t)it[i].r.addr, *c = map + it[i].at;
            uint64_t diff = 0, first = 0;
            for (uint64_t b = 0; b < it[i].r.len; b++) if (g[b] != c[b]) { if (!diff++) first = b; }
            checked++;
            if (!diff) continue;
            int wrote = nvn_dirty_since(g, it[i].r.len, it[i].stamp);
            if (wrote) { cpu++; continue; }
            bad++;
            if (said++ < 200)
                VLOG("[vcheck] draw #%u %s %#llx+%llu: the copy differs from game memory in %llu bytes (first at +%llu), no CPU write since its update",
                     it[i].r.draw, kinds[it[i].r.kind & 3], (unsigned long long)it[i].r.addr, (unsigned long long)it[i].r.len,
                     (unsigned long long)diff, (unsigned long long)first);
        }
    }
    VLOG("[vcheck] %d ranges of the traced draws compared: %d differ with no CPU write, %d differ where the CPU wrote since", checked, bad, cpu);
    g_nvr = 0;
    pthread_mutex_unlock(&g_vr_mu);
out:
    vk.vkDestroyBuffer(vkc.device, st, NULL);
    vk.vkFreeMemory(vkc.device, sm, NULL);
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
    static long long auto_at = -1;   // TOTK_DUMP_KEY_AT=<present>: F9 pressed there (scripted runs)
    if (auto_at == -1) { const char *e = getenv("TOTK_DUMP_KEY_AT"); auto_at = e ? atoll(e) : 0; }
    if (auto_at > 0 && n >= (uint64_t)auto_at) { auto_at = 0; vkb_dump_request(); }
    long long key_at = atomic_load(&g_dump_key_at);   // F9 with TOTK_DUMP_KEY=<dir>: <dir>/f<frame>, the game goes on
    if (key_at > 0 && n >= (uint64_t)key_at && atomic_compare_exchange_strong(&g_dump_key_at, &key_at, 0)) {
        vkb_tex_dump(g_f9_dir);
        vcheck_run();
        vkb_tex_vcheck_run();
        vkb_draw_trace_arm(0, 0);   // the trace ends with the snapshot: later draws stay out of f9.log and of the game's log
        VLOG("F9: frame %llu dumped to %s (its draws traced above)", (unsigned long long)n, g_f9_dir);
        FILE *lf = atomic_exchange(&g_f9_log, NULL);
        if (lf) fflush(lf);
        VLOG("F9: snapshot done: %s", g_f9_dir);
    }
}
// TOTK_DUMP_KEY=<dir> (development: catching what a person sees while playing): F9 traces the draws of the next frames
// (TOTK_VK_DRAWTRACE) and dumps the render targets of the frame after them into <dir>/f<frame>, its lines in f9.log
// there. Those frames are recorded on one thread (vkb_mt_on), the rest of the game as usual. TOTK_DUMP_KEY_MAX (default
// 20): snapshots per run, then F9 does nothing (each holds the frame's render targets, a few hundred MB).
void vkb_dump_request(void) {
    const char *d = getenv("TOTK_DUMP_KEY");
    if (!d || !*d || atomic_load(&g_dump_key_at)) return;
    static int taken;
    int max = getenv("TOTK_DUMP_KEY_MAX") ? atoi(getenv("TOTK_DUMP_KEY_MAX")) : 20;
    if (taken >= max) {
        if (taken++ == max) VLOG("F9: %d snapshots taken, the limit (TOTK_DUMP_KEY_MAX): no more in this run", max);
        return;
    }
    taken++;
    if (!g_f9_cap) g_f9_cap = (getenv("TOTK_DUMP_KEY_LOG_MB") ? atoll(getenv("TOTK_DUMP_KEY_LOG_MB")) : 16) << 20;
    uint64_t f = vkb_frame();
    snprintf(g_f9_dir, sizeof g_f9_dir, "%s/f%llu", d, (unsigned long long)f + 3);
    CreateDirectoryA(d, NULL);
    CreateDirectoryA(g_f9_dir, NULL);
    char path[600];
    snprintf(path, sizeof path, "%s/f9.log", g_f9_dir);
    FILE *lf = fopen(path, "w");
    VLOG("F9: snapshot %d of %d: %s (%s)", taken, max, g_f9_dir, lf ? "its lines in f9.log there" : "f9.log not created: its lines here");
    if (lf) {
        if (g_f9_prev) fclose(g_f9_prev);
        g_f9_prev = lf;
        atomic_store(&g_f9_bytes, 0);
        atomic_store(&g_f9_over, 0);
        atomic_store_explicit(&g_f9_log, lf, memory_order_release);
    }
    vkb_draw_trace_arm(f + 1, getenv("TOTK_DUMP_KEY_DRAWS") ? atoll(getenv("TOTK_DUMP_KEY_DRAWS")) : 6000);   // about two frames on the island
    atomic_store(&g_dump_key_at, (long long)f + 3);
    VLOG("F9: tracing from frame %llu, dump at %llu", (unsigned long long)f + 1, (unsigned long long)f + 3);
}

static __thread uint64_t t_present_slot;   // mtl_present_slot: when the next present may be shown
void mtl_present_slot(uint64_t ns) { t_present_slot = ns; }

uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s;
    uint64_t n = atomic_fetch_add(&g_ticket, 1) + 1;
    if (dm_stats() && (n == 1 || n % 300 == 0)) {
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
                static uint64_t pq[5];
                if (g_oq_on > 0) {
                    uint64_t q[5];
                    for (int k = 0; k < 5; k++) q[k] = __atomic_load_n(&g_oq_stat[k], __ATOMIC_RELAXED);
                    VLOG("per frame: samples passed (TOTK_OCCLUSION) %.1f reports, %.1f of them 0; %.1f queries, %.2f draws uncounted, %.2f results not available",
                         (double)(q[0] - pq[0]) / f, (double)(q[1] - pq[1]) / f, (double)(q[2] - pq[2]) / f, (double)(q[3] - pq[3]) / f,
                         (double)(q[4] - pq[4]) / f);
                    memcpy(pq, q, sizeof q);
                }
            }
            {
                static uint64_t pc, pb, pn, ph, pd;
                uint64_t c, b, ns, h, d;
                vkb_ucap_stats(&c, &b, &ns, &h, &d);
                VLOG("per frame: uniforms copied at submit %.2f MiB in %.0f submissions (%.2f ms on the game thread), bindings from the copy %.0f, dropped by GPU writes %.1f",
                     (double)(b - pb) / 1048576.0 / f, (double)(c - pc) / f, (double)(ns - pn) / 1e6 / f, (double)(h - ph) / f, (double)(d - pd) / f);
                pc = c; pb = b; pn = ns; ph = h; pd = d;
                static uint64_t pk, pv, pvb, pvh, pz;
                uint64_t k, v, vb, vh, z;
                vkb_ucap_keep_stats(&k, &v, &vb, &vh, &z);
                VLOG("per frame: inline uniform updates kept in the copy %.1f (versions built %.1f, %.2f MiB; reused %.1f), readers of memory not yet written %.2f",
                     (double)(k - pk) / f, (double)(v - pv) / f, (double)(vb - pvb) / 1048576.0 / f, (double)(vh - pvh) / f, (double)(z - pz) / f);
                pk = k; pv = v; pvb = vb; pvh = vh; pz = z;
            }
            if (vkb_pool_copies()) {
                static uint64_t pci, pcw, pcb, pcn, pck, pcs;
                pthread_mutex_lock(&g_pc_mu);
                uint64_t ci = g_pc_in_bytes, cw = g_pc_whole, cb = g_pc_back_bytes, cn = g_pc_ns, ck = g_pc_kept, cs = g_pc_snap_bytes,
                         cc = g_pc_copy_ns;
                int pend = g_npc_pend;
                pthread_mutex_unlock(&g_pc_mu);
                static uint64_t pcc;
                VLOG("per frame: pool copies (%.1f MiB of host-visible memory) brought up to date %.2f MiB (%.1f pools untracked: whole) "
                     "in %.2f ms before the submits (copying %.2f ms, the rest the write watch's scan), GPU writes into game memory "
                     "%.3f MiB (snapshots %.3f MiB, %.1f pages with the CPU's newer bytes kept); %d GPU writes not back yet",
                     (double)vkb_poolmem_bytes() / 1048576.0, (double)(ci - pci) / 1048576.0 / f, (double)(cw - pcw) / f,
                     (double)(cn - pcn) / 1e6 / f, (double)(cc - pcc) / 1e6 / f, (double)(cb - pcb) / 1048576.0 / f,
                     (double)(cs - pcs) / 1048576.0 / f, (double)(ck - pck) / f, pend);
                pci = ci; pcw = cw; pcb = cb; pcn = cn; pck = ck; pcs = cs; pcc = cc;
                {   // runtime/platform/linux.c: regions of the write watch not scanned (read-only until written)
                    void plat_watch_freeze_stats(uint64_t *, uint64_t *, uint64_t *);
                    static uint64_t pth, psk;
                    uint64_t fm, th, sk;
                    plat_watch_freeze_stats(&fm, &th, &sk);
                    if (fm || th) VLOG("per frame: write watch: %llu MiB frozen (not scanned), %.0f MiB of scans skipped, %.2f thaws by a write",
                                       (unsigned long long)fm, (double)(sk - psk) / f, (double)(th - pth) / f);
                    pth = th; psk = sk;
                }
            }
            if (vkb_shadow_on()) {
                static uint64_t pin, pib, pob, pdf, psc, pwp, pwk, pwb, pu0, pu1, pu2;
                VLOG("per frame: pool copies (holding %.1f MiB of video memory in all) in %.0f copies %.2f MiB, back %.2f MiB, "
                     "left in game memory in a pass %.0f, write watch %.2f ms; pages copied back %.0f, left to CPU writes %.1f, merged by bytes %.1f; pool uses under the write lock %.0f to update + %.0f writes, writes under the read lock %.0f", (double)vkb_shadow_bytes() / 1048576.0,
                     (double)(g_sh_in - pin) / f, (double)(g_sh_in_bytes - pib) / 1048576.0 / f, (double)(g_sh_out_bytes - pob) / 1048576.0 / f,
                     (double)(g_sh_deferred - pdf) / f, (double)(g_sh_scan_ns - psc) / 1e6 / f,
                     (double)(g_wb_pages - pwp) / f, (double)(g_wb_kept_pages - pwk) / f, (double)(g_wb_byte_pages - pwb) / f,
                     (double)(g_pool_use_n[0] - pu0) / f, (double)(g_pool_use_n[1] - pu1) / f, (double)(g_pool_use_n[2] - pu2) / f);
                pu0 = g_pool_use_n[0]; pu1 = g_pool_use_n[1]; pu2 = g_pool_use_n[2];
                pwp = g_wb_pages; pwk = g_wb_kept_pages; pwb = g_wb_byte_pages;
                pin = g_sh_in; pib = g_sh_in_bytes; pob = g_sh_out_bytes; pdf = g_sh_deferred; psc = g_sh_scan_ns;
            }
            memory_log();
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
                // uploads waiting for the cache's pages (the disk), and the warming ahead of them (vkb_tex_prefetch)
                static uint64_t prn, ptc, ppq, ppd;
                uint64_t rn, tc, pq, pd, psf;
                vkb_bc7_read_stats(&rn, &tc);
                void vkb_tex_prefetch_stats(uint64_t *, uint64_t *, uint64_t *);
                vkb_tex_prefetch_stats(&pq, &pd, &psf);
                VLOG("per frame: BC7 cache reads by uploads %.2f ms; warmed ahead: %.1f textures queued, %.1f surfaces read, %.1f not queued (queue full)",
                     (double)(rn - prn) / 1e6 / f, (double)(pq - ppq) / f, (double)(tc - ptc) / f, (double)(pd - ppd) / f);
                prn = rn; ptc = tc; ppq = pq; ppd = pd;
                (void)psf;
                { void vkb_mem_ready_log(double frames); vkb_mem_ready_log(f); }   // TOTK_VK_MEM_READY
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
    vkp_out_ready(slot, g_timeline, fence, crop, t_present_slot);
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
        case OP_REPORT_COUNTER: {
            uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
            if (mode == EX_CHUNK) chunk_counter((uint32_t)t, 1); else mtl_sub_counter(ad, (uint32_t)t);
            break;
        }
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (mode == EX_CHUNK) chunk_counter((uint32_t)t, 0); else sub_reset((uint32_t)t); break; }
        case OP_COPY_B2B: {
            uint64_t s, d, n;
            memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8);
            vkb_tex_b2b_log(s, d, n);
            mtl_sub_copy(s, d, n);
            break;
        }
        case OP_SET_RT:
            // The same targets again (TOTK sets them before many draws): the pass goes on. Ending it stored and loaded
            // every attachment and drained the GPU between draws into the same targets. TOTK_VK_RT_KEEP=0: a new pass every time, as before.
            if (!(g_in_pass && rt_keep() && !memcmp(&g_rt, a, sizeof g_rt))) {
                pass_end_why(PE_SET_RT); if (mode == EX_ONE) split_if_due(); memcpy(&g_rt, a, sizeof g_rt);
            }
            if (mode == EX_ONE) vkb_tsr_walk_rt(&g_rt);   // one thread: the walk's look comes here (vkb_tsr.c)
            vkb_tsr_rec_rt();
            trace_rt(); break;
        case OP_CLEAR_COLOR: if (g_render_off) vkb_cnt_add(g_cond_skipped, 1); else if (g_ok == 1) clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: if (g_render_off) vkb_cnt_add(g_cond_skipped, 1); else if (g_ok == 1) clear_ds((const p_clear_ds *)a); break;
        case OP_CLEAR_TEX: if (g_ok == 1) clear_tex((const p_clear_tex *)a); break;
        case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
        case OP_COPY_B2T: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 1); break;
        case OP_COPY_T2B: if (g_ok == 1) vkb_copy_buf_tex((const p_copy_b2t *)a, 0); break;
        case OP_COPY_T2T: if (g_ok == 1) vkb_copy_tex_tex((const p_copy_t2t *)a); break;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
            vkb_state(h.op, a);
            break;
        case OP_DRAW: case OP_DRAW_IDX:
            if (g_render_off) { vkb_cnt_add(g_cond_skipped, 1); rt_log(1); break; }
            if (mode == EX_ONE) vkb_draw_tsr_walk();
            if (g_ok == 1) vkb_draw(a, h.op == OP_DRAW_IDX);
            break;
        case OP_DISPATCH: if (g_ok == 1) vkb_dispatch(a); break;
        case OP_BARRIER:   // [temporary] everything before is visible to everything after
            { int vkb_draw_tracing(void); if (vkb_draw_tracing()) VLOG("[draw] barrier: bits %#x, in a pass %d", (unsigned)*(const int32_t *)a, vkb_in_pass()); }
            if (g_ok == 1 && hold_barrier(*(const int32_t *)a)) g_barrier_held = 1;
            else if (g_ok == 1) { pass_end_why(PE_BARRIER); full_barrier(vkb_cmd()); }
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
// UPQ_MAX: uploads of a submission sent in parts each in a command buffer of its own (vkb_upload_begin), at most
enum { MT_MAXCH = 96, UPQ_MAX = 1024, MT_LIST = 2 * MT_MAXCH + 4 + 2 * VKB_MAXREC + 2 * (UPQ_MAX + VKB_MAXREC) };
static VkCommandBuffer g_mt_list[MT_LIST];   // (+ the uploads' command buffers, vkb_upload_begin)
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
    if (g_ok != 1) { run_posts(ops, n, NULL, 0); free(ops); complete(fence); return fence; }
    pass_end_why(PE_SUBMIT);
    sub_long_check(fence);
    VkCommandBuffer last = g_mt_nlist ? g_mt_list[g_mt_nlist - 1] : g_slots[g_cur].cb;
    vkb_mirrors_submit(last);   // written ranges back to game memory before the fence
    { void vkb_tex_wb_fence_record(VkCommandBuffer cb, uint64_t fence); vkb_tex_wb_fence_record(last, fence); }   // images the CPU reads after this fence (grass)
    shadow_writeback(last);
    dead_tag(fence);
    gpu_time_end(g_cur, last);
    cmd_slot *s = &g_slots[g_cur];
    s->fence = fence;
    if (!g_slot_sent) vk.vkEndCommandBuffer(s->cb);
    if (g_mt_nlist) vk.vkEndCommandBuffer(last);
    static VkCommandBufferSubmitInfo cbi[MT_LIST + 1];   // (the translation thread only)
    int ncbi = 0;
    if (!g_slot_sent) cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    g_slot_sent = 0;
    for (int i = 0; i < g_mt_nlist; i++)
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_mt_list[i]};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_timeline, .value = fence,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)ncbi, .pCommandBufferInfos = cbi,
                        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    uint64_t tps = runtime_now_ns();
    pc_sync(fence);   // pool copies: the CPU's writes into the pools this submission uses
    vkb_step_add(PK_SYNC, tps);
    uint64_t ts0 = runtime_now_ns();
    vk_queue_lock();
    VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    vkb_step_add(PK_SUBMIT, ts0);
    if (r != VK_SUCCESS) { vk_check(r, "vkQueueSubmit2"); die("GPU submission failed"); }
    job *j = malloc(sizeof *j);
    if (!j) die("retire job");
    *j = (job){fence, ops, n, g_cur, g_wb_ranges, g_wb_nranges, NULL, (struct wb_snap_s *)g_wb_snaps, g_wb_nsnaps, atomic_load(&g_gf_frame), ts0};   // g_wb_ranges: in the slot's copy back memory
    g_wb_ranges = NULL; g_wb_snaps = NULL; g_wb_nsnaps = 0;
    if (g_oq_n) {   // its occlusion queries (all ended: every pass of it has ended)
        pthread_mutex_lock(&g_oq_mu);
        if (!(j->oq = malloc(sizeof *j->oq * (size_t)g_oq_n))) die("occlusion queries");
        memcpy(j->oq, g_oq_list, sizeof *j->oq * (size_t)g_oq_n);
        j->noq = g_oq_n;
        g_oq_n = 0;
        pthread_mutex_unlock(&g_oq_mu);
    }
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
// Between chunks (2026-11-02): no wait for all earlier work. The game's own barriers are replayed inside the chunks
// (OP_BARRIER), so a chunk boundary needs only what the cut itself adds: after the prologue everything (its uploads and
// pool copies), into a pass cut inside its targets' writes (it begins again with LOAD), and after a chunk whose shaders
// stored the storage writes (that flag is its recording thread's: the next chunk may be recorded on another). Before,
// a full ALL_COMMANDS barrier drained the GPU before each of the 16-32 chunks of a frame. TOTK_VK_CHUNK_BARRIER=1: the full barrier between all chunks, as before.
// [temporary] Mirrors of written storage buffers are not made (one thread only); TOTK_VK_RECORD_THREADS=1 records as before.
// A pass longer than 5/4 of the chunk size (the Great Sky Island's G-buffer pass: ~2100 draws, one chunk of 12 of the
// submission's 14 ms while the other threads waited) is cut inside: the chunk ends its pass and the next one begins it
// again with LOAD (every pass loads its targets), after the barrier between chunks. Depth-only passes (the shadow maps,
// ~1100 draws) are not cut: with them cut the sun's shadows went missing in one autotest of two (2026-10-27, not
// understood yet).
// The chunk size follows the threads: a submission waits for its longest chunk, so about four chunks a thread of the
// previous submission's draws (2026-10-31, the Depths run: chunks of ~490 draws, the longest 8.5 ms of a 10 ms
// submission while three threads waited), between MT_MIN and MT_DRAWS; TOTK_VK_CHUNK_DRAWS=<n> sets it.
enum { MT_DRAWS = 400, MT_MIN = 96, PART_FIRST = 64 };   // PART_FIRST: partial submission's first chunk (walk_op)
static int part_on(void);
static void part_flush(void);
static int g_chunk_draws = MT_DRAWS;   // this submission's: a chunk ends at a render target change after it, inside a pass after 5/4 of it
static uint64_t g_prev_draws;          // the draws of the previous submission
struct mt_chunk {
    int h0, h1; const uint8_t *p0, *p1;   // its first command, and the one after its last (h1 == the handle count: the end)
    uint64_t seq;                         // the position before its first command
    void *state; p_set_rt rt; int render_off; int64_t stride; oq_st oq;   // the state it starts in
    VkCommandBuffer cb;
    vkb_trk trk;
    uint64_t draws, ns;                   // the log line: its draws and dispatches, its recording time
    int split, stores;                    // it begins inside a cut pass; its shaders left storage writes pending
    int done;                             // recorded (g_mt_mu): partial submission sends the recorded ones in order
    uint64_t tsr[6];                      // the temporal upscaler's jitter state at its start (vkb_tsr.c)
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
static int g_pro_any;   // g_pro_mu: something was recorded into the prologue's current command buffer
// command buffers per slot and recording thread; [VKB_MAXREC]: the moves' and the end's (the translation thread);
// [VKB_MAXREC + 1]: the prologue's (a new one may be begun while the moves' are recorded: a pool is used by one thread)
typedef struct { VkCommandPool pool; VkCommandBuffer *cb; int n, used; } wpool_t;
enum { WP_MOVES = VKB_MAXREC, WP_PRO = VKB_MAXREC + 1 };
static wpool_t g_wp[SLOTS][VKB_MAXREC + 2];

int vkb_mt_on(void) {
    static int n = -1;
    if (n < 0) {
        /* The logical processors the process may run on (its affinity): only a machine with fewer than three records
           with fewer threads. */
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int cpus = (int)si.dwNumberOfProcessors;
        DWORD_PTR proc_mask, sys_mask;
        if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) && proc_mask) {
            int k = 0;
            for (DWORD_PTR m = proc_mask; m; m &= m - 1) k++;
            if (k < cpus) cpus = k;
        }
#else
        cpu_set_t set;
        int cpus = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (cpus < 1) cpus = 4;
#endif
        // Three on every machine (the player's decision, 2026-11-06, from the runs on the PC, the friend's machine and
        // the Steam Deck: three was the best of them; the rule "processors less two, at most 4" gave 4 on 6 and 8
        // processors and was not better), fewer only where the process has fewer processors.
        const char *e = getenv("TOTK_VK_RECORD_THREADS");
        n = e && *e ? atoi(e) : cpus < 3 ? cpus : 3;
        if (n > VKB_MAXREC) n = VKB_MAXREC;
        if (n < 1) n = 1;
        static const char *const diag[] = {"TOTK_VK_DRAWTRACE", "TOTK_VK_TRACE", "TOTK_SNAP", "TOTK_GPU_PROF", "TOTK_GPU_PASSPROF",
                                           "TOTK_VK_CHECKPOINTS", "TOTK_AUDIT", "TOTK_VK_SPLIT", "TOTK_VK_STORES_LOG", "TOTK_WATCH"};
        const char *why = NULL;   // development tools that follow the recording in order: one thread
        for (unsigned i = 0; i < sizeof diag / sizeof *diag; i++) if (getenv(diag[i])) why = diag[i];
        if (n > 1 && why) n = 1;
        if (n > 1 && g_ok != 1) n = 1;
        g_mt_threads = n;
        VLOG("recording threads: %d of %d logical processors%s%s", n, cpus, why ? " (one: " : "", why ? why : "");
        if (why) VLOG("  (one thread while %s is set)", why);
    }
    return n > 1 && !atomic_load(&g_dump_key_at);   // F9 (TOTK_DUMP_KEY): its traced frames on one thread, in order
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
    for (int w = 0; w <= WP_PRO; w++)
        if (g_wp[slot][w].used) { vk.vkResetCommandPool(vkc.device, g_wp[slot][w].pool, 0); g_wp[slot][w].used = 0; }
}

void vkb_pro_begin(void) {
    if (!t_recording || t_pro_depth++) return;
    pthread_mutex_lock(&g_pro_mu);
    g_pro_any = 1;
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
    oq_st oqs = c->oq;   // samples-passed counting as the walk was at the chunk's start
    t_oqs = &oqs;
    g_in_pass = 0;
    vkb_stores_take();
    t_seq = c->seq;
    vkb_tsr_rec_begin(c->tsr);
    for (int i = c->h0; i < g_mt_n; i++) {
        const uint8_t *p = i == c->h0 ? c->p0 : (const uint8_t *)(uintptr_t)g_mt_h[i];
        if (chunk_end_at(c, i, p) || exec_ops(p, 0, EX_CHUNK, i, c)) break;
    }
    pass_end_why(PE_SUBMIT);
    c->stores = vkb_stores_take();
    t_oqs = &g_oq_walk;
    vk.vkEndCommandBuffer(c->cb);
    vkb_trk_set(NULL);
    t_recording = 0;
    t_cb = VK_NULL_HANDLE;
    c->ns = runtime_now_ns() - t0;
    vkb_cnt_add(g_mt_rec_ns, c->ns);
}
static int mt_take(void) {   // the next chunk cut and not taken, or -1
    pthread_mutex_lock(&g_mt_mu);
    int k = g_mt_next < g_mt_avail ? g_mt_next++ : -1;
    pthread_mutex_unlock(&g_mt_mu);
    return k;
}
static void mt_recorded(mt_chunk *c) {
    pthread_mutex_lock(&g_mt_mu);
    c->done = 1;
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
    { const char *e = getenv("TOTK_DIRTY_SHARED"); if (!(e && *e == '0')) nvn_dirty_epoch_reader(); }
    // TOTK_RECORD_NICE=<n> (experiment): this thread below the game's threads, which hold the three virtual cores and
    // waited 55-86 ms a frame for one on the Steam Deck while the cores were 68 % busy (2026-11-05)
    { const char *e = getenv("TOTK_RECORD_NICE"); void plat_thread_nice(int); if (e && *e) plat_thread_nice(atoi(e)); }   // its texture checks use the submission's write watch scans (nvn_dirty_win.c; =0: own scans, as before)
    for (;;) {
        pthread_mutex_lock(&g_mt_mu);
        while (g_mt_next >= g_mt_avail) pthread_cond_wait(&g_mt_cv, &g_mt_mu);
        int k = g_mt_next++;
        pthread_mutex_unlock(&g_mt_mu);
        mt_record(&g_ch[k]);
        mt_recorded(&g_ch[k]);
        if (part_on()) part_flush();   // the recorded prefix to the GPU now, not when the translation thread looks next
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
    c->oq = g_oq_walk;
    c->split = 0;
    c->done = 0;
    vkb_tsr_state_save(c->tsr);
    g_mt_draws = 0;
}
static void chunk_close(int hidx, const uint8_t *p) {   // the open chunk ends before p: recording threads may take it
    mt_chunk *c = &g_ch[g_nch];
    c->h1 = hidx; c->p1 = p;
    c->draws = g_mt_draws;
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = ++g_nch;
    pthread_cond_broadcast(&g_mt_cv);
    pthread_mutex_unlock(&g_mt_mu);
}
static int walk_op(const uint8_t *p, int hidx) {   // before a command at depth 0: a new chunk at a render target change
    op_hdr h;
    memcpy(&h, p, sizeof h);
    // partial submission: the first chunk short, so that the GPU has work soon after the walk (part_flush)
    uint64_t lim = g_nch == 0 && part_on() && g_chunk_draws > PART_FIRST ? PART_FIRST : (uint64_t)g_chunk_draws;
    int split = (h.op == OP_DRAW || h.op == OP_DRAW_IDX) && g_mt_draws >= lim * 5 / 4 && g_rt.n > 0;   // inside a pass with colour targets
    if (((h.op == OP_SET_RT && g_mt_draws >= lim) || split) && g_nch < MT_MAXCH - 1) {
        chunk_close(hidx, p);
        chunk_open(hidx, p);
        g_ch[g_nch].split = split && h.op != OP_SET_RT;
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
    case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); sub_reset((uint32_t)t); break; }
    case OP_COPY_B2B: { uint64_t d, n; memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); if (n) vkb_ucap_drop(d, n); break; }   // GPU writes
    case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); if (n) vkb_ucap_update(d, n, a + 16); break; }
    case OP_COPY_T2B: { uint64_t ad, len; if (vkb_copy_t2b_range((const p_copy_b2t *)a, &ad, &len)) vkb_ucap_drop(ad, len); break; }
    case OP_COPY_STRIDE: vkb_copy_stride(((const p_copy_stride *)a)->stride); break;
    case OP_SET_RT: memcpy(&g_rt, a, sizeof g_rt); vkb_tsr_walk_rt(&g_rt); break;
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_STENCIL:
    case OP_POLY_OFFSET: case OP_BLEND_COLOR: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_UBO:
    case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_IMG: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_PRIM_RESTART:
        vkb_state(op, a);
        break;
    case OP_DRAW: case OP_DRAW_IDX:
        if (!g_render_off) { g_mt_draws++; vkb_draw_prescan(a, op == OP_DRAW_IDX, 0); vkb_draw_tsr_walk(); }
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

// Before a chunk: its images move into the layouts it needs first, with what the cut needs (see above). full: everything
// earlier is done (after the prologue); attach: the targets' writes before a pass cut inside; stores: storage writes.
enum { GLUE_FULL = 1, GLUE_ATTACH = 2, GLUE_STORES = 4 };
static void glue(VkCommandBuffer cb, vkb_trk *t, int need) {
    static int all = -1;
    if (all < 0) { const char *e = getenv("TOTK_VK_CHUNK_BARRIER"); all = e && *e == '1'; }
    if (all) need = GLUE_FULL;
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    if (!(need & GLUE_FULL)) {
        mb.srcStageMask = mb.dstStageMask = 0; mb.srcAccessMask = mb.dstAccessMask = 0;
        if (need & GLUE_ATTACH) {
            const VkPipelineStageFlags2 att = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            mb.srcStageMask |= att; mb.dstStageMask |= att;
            mb.srcAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            mb.dstAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        }
        if (need & GLUE_STORES) {   // as pass_begin's after storage writes
            mb.srcStageMask |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask |= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            mb.dstAccessMask |= VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
    }
    VkImageMemoryBarrier2 ib[64];
    int ni = 0, first = mb.srcStageMask != 0;
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
        if (ni || first) {   // first: the memory barrier, with the first batch of moves or alone
            VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = first ? 1u : 0u, .pMemoryBarriers = &mb,
                                    .imageMemoryBarrierCount = (uint32_t)ni, .pImageMemoryBarriers = ib};
            vk.vkCmdPipelineBarrier2(cb, &dep);
            ni = 0; first = 0;
        }
    }
    for (int k = 0; k < t->n; k++) t->e[k].im->layout = t->e[k].cur;
    if (t->n) atomic_fetch_add_explicit(&vkb_layout_epoch_all, 1, memory_order_relaxed);
}

// ---- partial submission (on by default since 2026-11-06; TOTK_VK_PARTIAL_SUBMIT=0: off) ----
// The GPU stood idle while a submission was recorded, and the game then waited for that same submission: on the
// player's PC (2026-11-05, TOTK_TIMELINE, frames 4200-4530) 4.3 of the 6.8 ms of recording before each wait of 5 ms
// and more (frame-compare.md 3b). So the translation thread sends the chunks recorded so far, in order, while the
// others are still being recorded; the last part is the submission itself (the end, its fence, its retire
// job). A part's chunks are glued as before; the slot's command buffer goes with the first part.
// The prologue goes in parts too: a part closes the prologue's command buffer (g_pro_mu) and it is placed before the
// part's first chunk, behind a full barrier; work for the prologue after that goes into a new one, placed before the
// next part's first chunk. That keeps the prologue's rule "before every chunk that may use it": a chunk sees an upload
// or a pool's device copy as done (upload_stamp, sync_sub) only after the prologue work was recorded under g_pro_mu,
// and it is recorded before this thread sees it done and takes g_pro_mu to close the part, so the work is in this
// part's prologue or an earlier one. Pool copies (Linux) are synced once a submission, at the first part using them
// (pc_sync). [temporary] The GPU time of a submission (its two timestamps) includes the waits between its parts.
// Second version (2026-11-06): the first one sent its first part 7.4-9.4 ms into a recording of 6-18 ms (the friend's
// RX 570, the Steam Deck; frame-compare.md 3и) - after the prologue, the walk and a chunk of the translation thread's
// own - so the GPU still began when the frame was nearly recorded, and the game's thread waited for "record, then
// draw" as before (12-21 ms in long frames). Now the first chunk is short (PART_FIRST draws), every thread that records a chunk sends the
// recorded prefix (g_part_mu: one at a time), the first part as soon as the walk is done, and the texture uploads of
// the recording threads go with the parts (each in a command buffer of its own, vkb_upload_begin) instead of all into
// the prologue under one lock.
enum { PART_DRAWS = 64, PART_MAX = 16 };
static int g_part_next, g_part_n, g_part_full;   // the first chunk not sent; parts sent; the next chunk's glue waits for all
static uint64_t g_part_t, g_glue_images;           // when the last part went; images moved between chunks (the log line)
static pthread_mutex_t g_part_mu = PTHREAD_MUTEX_INITIALIZER;   // part_flush and the submission's end, one at a time
static int g_walk_done, g_part_closing;            // (g_part_mu) parts may go; the submission is sending the rest
static uint64_t g_part_t0;                         // when the submission's recording began
// uploads with partial submission: each in its own command buffer, ended by its thread and queued until a part takes it
typedef struct { VkCommandBuffer cb; vkb_trk trk; } upq_e;
static upq_e *g_upq[VKB_MAXREC];
static int g_upq_n[VKB_MAXREC], g_upq_cap[VKB_MAXREC], g_upq_taken[VKB_MAXREC];
static pthread_mutex_t g_upq_mu[VKB_MAXREC];
static pthread_once_t g_upq_once = PTHREAD_ONCE_INIT;
static _Atomic int g_upq_total;   // queued this submission (UPQ_MAX: then into the prologue, as without the split)
static void upq_init(void) { for (int w = 0; w < VKB_MAXREC; w++) pthread_mutex_init(&g_upq_mu[w], NULL); }
// The uploads queued so far into list, each after its move (the first with a full barrier); g_pro_mu held.
static void uploads_take(VkCommandBuffer *list, int *n) {
    pthread_once(&g_upq_once, upq_init);
    int first = 1;
    for (int w = 0; w < VKB_MAXREC; w++) {
        pthread_mutex_lock(&g_upq_mu[w]);   // an upload being recorded now is waited for (its image may be marked uploaded)
        for (int i = g_upq_taken[w]; i < g_upq_n[w]; i++) {
            VkCommandBuffer m = wp_get(g_cur, WP_MOVES);
            glue(m, &g_upq[w][i].trk, first ? GLUE_FULL : 0);
            first = 0;
            vk.vkEndCommandBuffer(m);
            list[(*n)++] = m;
            list[(*n)++] = g_upq[w][i].cb;
        }
        g_upq_taken[w] = g_upq_n[w];
        pthread_mutex_unlock(&g_upq_mu[w]);
    }
    if (!first) g_part_full = 1;   // the chunk after them sees their writes
}
static uint64_t g_part_subs, g_part_parts, g_part_first_ns;   // the log line: submissions sent in parts, parts, first part after the start
static uint64_t g_part_held;   // (g_part_mu) parts held back: they would have begun with a full barrier (the log line)
// Third version (2026-11-06): a part after the first goes only when it needs no full barrier - no prologue work and no
// queued upload since the last part. Such work is placed before the part's first chunk behind a full barrier (glue
// GLUE_FULL, g_part_full: the prologue's writes, and its writes over what earlier parts still read), and a full barrier
// waits for all the GPU's earlier work: the GPU drained at every such part. While the world streams in, the prologue
// gets work (pool device copies, uploads) nearly every part: the friend's RX 570 (1080p, limit 60; 0.3.1-75) had 5-7
// parts a submission and frames of 33-48 ms in bursts exactly where the game streamed (54 in ~2 min of world, against 8
// with the first version and 20 without parts; frame-compare.md 0). The same cost once took the full barriers out from
// between chunks (83cc638). A part held back goes with the next one or with the submission, as with no part at all: always correct.
// TOTK_VK_PART_DRAIN=1: parts with a full barrier too, as the second version (A/B).
static int part_drain_ok(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_PART_DRAIN");
        v = e && *e == '1';
        VLOG("partial submission: %s", v ? "parts go with a full barrier too (TOTK_VK_PART_DRAIN=1, the second version)"
                                         : "a part after the first goes only without a full barrier (TOTK_VK_PART_DRAIN=1: as before)");
    }
    return v;
}
// Fourth version (2026-11-06, night): a part held back for a full barrier goes all the same when the GPU has done
// everything sent before it, for then the barrier waits for nothing. The third version held it whatever the GPU did:
// right for the friend's RX 570, busy all the time (a drain there was a stall), wrong for the Steam Deck, whose GPU ran
// through the first part and stood idle until the end of the recording - and parts were held exactly while the world
// streamed in (texture uploads queued, prologue work), so the Deck dipped 40-30-40 in the same places, in the dense ones
// frames over 33 ms 55 a minute against 34 with the second version (frame-compare.md 0). Each part signals g_part_sem;
// idle: it and the submissions before reached their values. TOTK_VK_PART_IDLE=0: held whatever the GPU does (third).
static VkSemaphore g_part_sem;   // (g_part_mu) parts' own timeline: part g_part_seq signals g_part_seq
static uint64_t g_part_seq, g_part_idle_sent;   // (g_part_mu) parts sent; held-back parts sent for an idle GPU (the log line)
static int part_idle_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_PART_IDLE");
        v = !(e && *e == '0');
        VLOG("partial submission: %s", v ? "a part held back for a full barrier goes when the GPU is idle (TOTK_VK_PART_IDLE=0: held)"
                                         : "a part held back for a full barrier waits whatever the GPU does (TOTK_VK_PART_IDLE=0)");
    }
    return v;
}
static int gpu_idle(void) {   // (g_part_mu) the GPU has done every submission and part sent so far
    if (!g_part_sem) return 0;
    uint64_t main = 0, part = 0;
    if (vk.vkGetSemaphoreCounterValue(vkc.device, g_timeline, &main) != VK_SUCCESS ||
        vk.vkGetSemaphoreCounterValue(vkc.device, g_part_sem, &part) != VK_SUCCESS) return 0;
    return main >= __atomic_load_n(&g_fence, __ATOMIC_ACQUIRE) && part >= g_part_seq;
}
static int uploads_queued(void) {   // uploads not yet taken by a part
    pthread_once(&g_upq_once, upq_init);
    for (int w = 0; w < VKB_MAXREC; w++) {
        pthread_mutex_lock(&g_upq_mu[w]);
        int q = g_upq_n[w] > g_upq_taken[w];
        pthread_mutex_unlock(&g_upq_mu[w]);
        if (q) return 1;
    }
    return 0;
}
static int part_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_PARTIAL_SUBMIT");
        v = !(e && *e == '0');   // on by default since 2026-11-06 (the player's PC: frames over 25 ms 100 -> 23 a minute)
        VLOG("partial submission: %s", v ? "recorded chunks go to the GPU in order while the rest are recorded (TOTK_VK_PARTIAL_SUBMIT=0: off)"
                                         : "off, the submission in one call (TOTK_VK_PARTIAL_SUBMIT=0)");
    }
    return v;
}
// The prologue's command buffer so far into list (with its moves); a new one begun. g_pro_mu held, or no thread recording.
static void pro_close(VkCommandBuffer *list, int *n, int more) {
    if (!g_slot_sent) {   // the first: the slot's command buffer moves its images
        vk.vkEndCommandBuffer(g_pro_cb);
        glue(g_slots[g_cur].cb, &g_pro_trk, GLUE_FULL);
        list[(*n)++] = g_pro_cb;
    } else if (g_pro_any) {
        vk.vkEndCommandBuffer(g_pro_cb);
        VkCommandBuffer m = wp_get(g_cur, WP_MOVES);
        glue(m, &g_pro_trk, GLUE_FULL);
        vk.vkEndCommandBuffer(m);
        list[(*n)++] = m;
        list[(*n)++] = g_pro_cb;
        g_part_full = 1;   // the chunk after it sees the prologue's writes
    } else if (!more) vk.vkEndCommandBuffer(g_pro_cb);   // empty: not sent
    else return;           // empty: kept for what comes
    if (more) {
        g_pro_cb = wp_get(g_cur, WP_PRO);
        vkb_trk_reset(&g_pro_trk);
        g_pro_any = 0;
    }
}
static void chunks_glue(int a, int b, VkCommandBuffer *list, int *n) {   // chunks [a, b), each after its move
    for (int k = a; k < b; k++) {
        VkCommandBuffer m = wp_get(g_cur, WP_MOVES);
        g_glue_images += (uint64_t)g_ch[k].trk.n;
        int need = k == 0 || g_part_full ? GLUE_FULL : (g_ch[k].split ? GLUE_ATTACH : 0) | (g_ch[k - 1].stores ? GLUE_STORES : 0);
        g_part_full = 0;
        glue(m, &g_ch[k].trk, need);
        vk.vkEndCommandBuffer(m);
        list[(*n)++] = m;
        list[(*n)++] = g_ch[k].cb;
    }
}
// After the walk, on any thread that records chunks: the chunks recorded in order since the last part, if enough.
static void part_flush(void) {
    if (pthread_mutex_trylock(&g_part_mu)) return;   // another thread is sending: it sees what is recorded
    if (!g_walk_done || g_part_closing || g_part_n >= PART_MAX) { pthread_mutex_unlock(&g_part_mu); return; }
    pthread_mutex_lock(&g_mt_mu);
    int e = g_part_next;
    uint64_t draws = 0;
    while (e < g_nch && g_ch[e].done) draws += g_ch[e++].draws;
    int all = g_nch;
    pthread_mutex_unlock(&g_mt_mu);
    uint64_t now = runtime_now_ns();
    // all recorded: the submission itself sends them; after the first part, at most one a millisecond
    if (e == g_part_next || e == all || draws < (g_part_n ? PART_DRAWS : 1) || (g_part_n && now - g_part_t < 1000000)) {
        pthread_mutex_unlock(&g_part_mu);
        return;
    }
    static VkCommandBuffer list[MT_LIST];   // (g_part_mu)
    int n = 0;
    int first = !g_slot_sent;
    pthread_mutex_lock(&g_pro_mu);   // no prologue work meanwhile; glue writes image layouts the prologue's work may change
    if (!first && !part_drain_ok() && (g_pro_any || uploads_queued())) {   // it would drain the GPU: held back, unless idle
        if (!(part_idle_on() && gpu_idle())) {
            pthread_mutex_unlock(&g_pro_mu);
            g_part_held++;
            pthread_mutex_unlock(&g_part_mu);
            return;
        }
        g_part_idle_sent++;
    }
    if (!g_part_sem) {   // the parts' timeline, made at the first part
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
        VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (!vk_check(vk.vkCreateSemaphore(vkc.device, &ci, NULL, &g_part_sem), "parts' timeline semaphore")) g_part_sem = VK_NULL_HANDLE;
    }
    pro_close(list, &n, 1);
    uploads_take(list, &n);
    chunks_glue(g_part_next, e, list, &n);
    g_slot_sent = 1;   // under g_pro_mu: snap_take reads it there
    pthread_mutex_unlock(&g_pro_mu);
    g_part_next = e;
    static VkCommandBufferSubmitInfo cbi[MT_LIST + 1];
    int ncbi = 0;
    if (first) {
        vk.vkEndCommandBuffer(g_slots[g_cur].cb);
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_slots[g_cur].cb};
    }
    for (int i = 0; i < n; i++)
        cbi[ncbi++] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = list[i]};
    VkSemaphoreSubmitInfo psig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_part_sem, .value = g_part_seq + 1,
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};   // gpu_idle
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)ncbi, .pCommandBufferInfos = cbi,
                        .signalSemaphoreInfoCount = g_part_sem ? 1u : 0u, .pSignalSemaphoreInfos = &psig};
    uint64_t tps = runtime_now_ns();
    pc_sync(g_fence + 1);   // pool copies these chunks use (each pool once a submission)
    vkb_step_add(PK_SYNC, tps);
    uint64_t ts0 = runtime_now_ns();
    vk_queue_lock();
    VkResult r = vk.vkQueueSubmit2(vkc.queue, 1, &si, VK_NULL_HANDLE);
    vk_queue_unlock();
    vkb_step_add(PK_SUBMIT, ts0);
    if (r != VK_SUCCESS) { vk_check(r, "vkQueueSubmit2 (part)"); die("GPU submission failed"); }
    if (first) g_part_first_ns += ts0 - g_part_t0;
    if (g_part_sem) g_part_seq++;
    g_part_n++;
    g_part_t = runtime_now_ns();
    pthread_mutex_unlock(&g_part_mu);
}

// ---- texture uploads in a command buffer per recording thread (TOTK_VK_UPLOAD_SPLIT=1, 2026-11-06) ----
// Every upload of a submission went into the shared prologue under one lock (g_up_mu in vkb_tex.c, g_pro_mu here): the
// BC7 image made, the layout change and the copies. On the Steam Deck the work under it and the waits for it were 50-90
// ms of a long submission while the world streamed in (2026-11-06, the uploads-by-part line), the recording threads
// taking turns. With the switch a prepared upload (vkb_tex.c plan_record) goes into a command buffer of its recording
// thread's own, with its own layout tracking, under a lock of the image alone; these command buffers run after the
// prologue and before every chunk, as the prologue's uploads did (the CPU wrote the bytes before the submission). An
// image is uploaded once a submission (the image's lock and its upload stamp), so no two of them touch it. Uploads that
// are not prepared (ASTC decoded on the CPU, untracked memory) go into the prologue as before. With partial submission
// each upload is a command buffer of its own that the next part takes (uploads_take), at most UPQ_MAX a submission.
// [experimental] On by default since 2026-11-06 (the player's decision; TOTK_VK_UPLOAD_SPLIT=0: one prologue, as before).
// The Steam Deck, ~6 min with against ~2 min without: waits for one another's uploads 12-50 -> 0-11 ms in long
// submissions, frames over 50 ms 13 -> 7 a minute. One black artifact over half the screen once, not yet told apart
// from the old black holes on AMD (GitHub #38): if such reports come, test with =0 first.
static VkCommandBuffer g_upcb[VKB_MAXREC];
static vkb_trk g_uptrk[VKB_MAXREC];
static __thread VkCommandBuffer t_up_cb_save;
static __thread vkb_trk *t_up_trk_save;
static int up_split_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_UPLOAD_SPLIT");
        v = !(e && *e == '0');
        VLOG("texture uploads: %s", v ? "a command buffer per recording thread, a lock per image [experimental] (TOTK_VK_UPLOAD_SPLIT=0: one prologue)"
                                      : "one prologue, one lock (TOTK_VK_UPLOAD_SPLIT=0)");
    }
    return v;
}
int vkb_upload_split(void) {
    return t_recording && !t_pro_depth && up_split_on() && (!part_on() || atomic_load(&g_upq_total) < UPQ_MAX);
}
void vkb_upload_begin(void) {   // (vkb_upload_split): records into this thread's uploads command buffer from here
    int w = t_worker;
    t_up_cb_save = t_cb; t_up_trk_save = vkb_trk_get();
    if (part_on()) {   // its own command buffer, queued for the next part when it ends (uploads_take)
        pthread_once(&g_upq_once, upq_init);
        pthread_mutex_lock(&g_upq_mu[w]);   // until vkb_upload_end
        if (g_upq_n[w] == g_upq_cap[w]) {
            int cap = g_upq_cap[w] ? g_upq_cap[w] * 2 : 64;
            upq_e *q = realloc(g_upq[w], sizeof *q * (size_t)cap);
            if (!q) die("upload queue");
            memset(q + g_upq_cap[w], 0, sizeof *q * (size_t)(cap - g_upq_cap[w]));
            g_upq[w] = q; g_upq_cap[w] = cap;
        }
        upq_e *e = &g_upq[w][g_upq_n[w]];
        e->cb = wp_get(g_cur, w);
        vkb_trk_reset(&e->trk);
        atomic_fetch_add(&g_upq_total, 1);
        t_cb = e->cb;
        vkb_trk_set(&e->trk);
        return;
    }
    if (!g_upcb[w]) { g_upcb[w] = wp_get(g_cur, w); vkb_trk_reset(&g_uptrk[w]); }
    t_cb = g_upcb[w];
    vkb_trk_set(&g_uptrk[w]);
}
void vkb_upload_end(void) {
    if (part_on()) {
        int w = t_worker;
        vk.vkEndCommandBuffer(t_cb);
        g_upq_n[w]++;
        pthread_mutex_unlock(&g_upq_mu[w]);
    }
    t_cb = t_up_cb_save; vkb_trk_set(t_up_trk_save);
}
static void uploads_close(VkCommandBuffer *list, int *n) {   // after the prologue: each thread's uploads, after its move
    for (int w = 0; w < VKB_MAXREC; w++) {
        if (!g_upcb[w]) continue;
        vk.vkEndCommandBuffer(g_upcb[w]);
        VkCommandBuffer m = wp_get(g_cur, WP_MOVES);
        glue(m, &g_uptrk[w], GLUE_FULL);
        vk.vkEndCommandBuffer(m);
        list[(*n)++] = m;
        list[(*n)++] = g_upcb[w];
        g_upcb[w] = VK_NULL_HANDLE;
        g_part_full = 1;   // the chunk after them sees their writes
    }
}

void vkb_mt_submit(const uint64_t *h, int n) {   // after mtl_sub_begin and the uniform capture; commits
    uint64_t t0 = runtime_now_ns();
    static uint64_t walk_ns, wall_ns, subs, chunks, said_at, longest_ns, longest_draws, pro_ns, rec_ns, tail_ns;
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
    {
        static int fixed = -1;
        if (fixed < 0) { const char *e = getenv("TOTK_VK_CHUNK_DRAWS"); fixed = e ? atoi(e) : 0; if (fixed < 0) fixed = 0; }
        uint64_t want = g_prev_draws / (uint64_t)(g_mt_threads * 4);
        g_chunk_draws = fixed ? fixed : want < MT_MIN ? MT_MIN : want > MT_DRAWS ? MT_DRAWS : (int)want;
    }
    pthread_mutex_lock(&g_mt_mu);
    g_mt_avail = g_mt_next = g_mt_done = 0;
    g_nch = 0;
    pthread_mutex_unlock(&g_mt_mu);
    // the prologue: objects made on first use, the pool copies of the last submissions
    g_pro_cb = wp_get(g_cur, WP_PRO);
    vkb_trk_reset(&g_pro_trk);
    g_pro_any = 0;
    int part = part_on();
    pthread_mutex_lock(&g_part_mu);   // (a recording thread may still be leaving part_flush of the last submission)
    g_part_next = g_part_n = g_part_full = 0;
    g_walk_done = g_part_closing = 0;
    g_part_t0 = t0;
    pthread_mutex_unlock(&g_part_mu);
    memset(g_upcb, 0, sizeof g_upcb);
    memset(g_upq_n, 0, sizeof g_upq_n);
    memset(g_upq_taken, 0, sizeof g_upq_taken);
    atomic_store(&g_upq_total, 0);
    g_slot_sent = 0;
    t_recording = 1;
    vkb_pro_begin();
    vkb_draw_mt_init();
    vkb_pro_end();
    shadow_presync();
    t_recording = 0;
    uint64_t tw = runtime_now_ns();
    pro_ns += tw - t0;
    // the walk, from where the last one ended
    if (g_walk_have) { vkb_state_load(g_walk_state); g_rt = g_walk_rt; g_render_off = g_walk_off; vkb_copy_stride(g_walk_stride); }
    g_in_pass = 0;
    chunk_open(0, n ? (const uint8_t *)(uintptr_t)h[0] : NULL);
    for (int i = 0; i < n; i++) exec_ops((const uint8_t *)(uintptr_t)h[i], 0, EX_WALK, i, NULL);
    chunk_close(n, NULL);
    if (!g_walk_state && !(g_walk_state = malloc(vkb_state_size()))) die("walk state");
    vkb_state_save(g_walk_state);
    g_walk_rt = g_rt; g_walk_off = g_render_off; g_walk_stride = vkb_copy_stride_get(); g_walk_have = 1;
    uint64_t tr = runtime_now_ns();
    walk_ns += tr - tw;
    // this thread records chunks too, then waits for the others (partial submission: sending what is recorded in order)
    if (part) {
        pthread_mutex_lock(&g_part_mu);
        g_walk_done = 1;
        pthread_mutex_unlock(&g_part_mu);
        part_flush();
    }
    for (int k; (k = mt_take()) >= 0;) { mt_record(&g_ch[k]); mt_recorded(&g_ch[k]); if (part) part_flush(); }
    pthread_mutex_lock(&g_mt_mu);
    while (g_mt_done < g_nch) {
        pthread_cond_wait(&g_mt_done_cv, &g_mt_mu);
        if (part) { pthread_mutex_unlock(&g_mt_mu); part_flush(); pthread_mutex_lock(&g_mt_mu); }
    }
    pthread_mutex_unlock(&g_mt_mu);
    if (part) {   // no part from here on: the rest goes with the submission
        pthread_mutex_lock(&g_part_mu);
        g_part_closing = 1;
        pthread_mutex_unlock(&g_part_mu);
    }
    uint64_t tt = runtime_now_ns();
    rec_ns += tt - tr;
    // in order: the slot's command buffer moves images into the prologue's layouts, then each chunk's move and the chunk
    // (after parts: the prologue's work since the last part, then the chunks not sent)
    g_mt_nlist = 0;
    pro_close(g_mt_list, &g_mt_nlist, 0);
    if (part) uploads_take(g_mt_list, &g_mt_nlist);
    else uploads_close(g_mt_list, &g_mt_nlist);
    chunks_glue(g_part_next, g_nch, g_mt_list, &g_mt_nlist);
    g_mt_list[g_mt_nlist++] = wp_get(g_cur, WP_MOVES);   // the end
    if (g_part_n) { g_part_subs++; g_part_parts += (uint64_t)g_part_n + 1; }
    mtl_sub_commit();
    g_mt_nlist = 0;
    uint64_t te = runtime_now_ns();
    tail_ns += te - tt;
    wall_ns += te - t0;
    subs++; chunks += (uint64_t)g_nch;
    {   // the longest chunk: the submission waits for it
        uint64_t mx = 0, md = 0, all = 0;
        for (int i = 0; i < g_nch; i++) { all += g_ch[i].draws; if (g_ch[i].ns > mx) { mx = g_ch[i].ns; md = g_ch[i].draws; } }
        if (all > 200) g_prev_draws = all;   // small submissions (a copy, a clear) keep the size of the frame's
        longest_ns += mx; longest_draws += md;
    }
    if (dm_stats() && subs >= said_at + 600) {   // the log line: where a submission's recording time goes
        said_at = subs;
        static uint64_t prev_rec;
        uint64_t rec = vkb_cnt_sum(g_mt_rec_ns);
        VLOG("recording threads: per submission %.1f ms until submitted (walk %.1f ms), chunks %.1f, chunk recording %.1f ms in all "
             "threads, the longest chunk %.1f ms (%.0f draws), images moved between chunks %.0f", wall_ns / 1e6 / 600, walk_ns / 1e6 / 600,
             chunks / 600.0, (rec - prev_rec) / 1e6 / 600, longest_ns / 1e6 / 600, longest_draws / 600.0, g_glue_images / 600.0);
        VLOG("recording threads: phases per submission: prologue %.1f ms, walk %.1f, chunks until the last is recorded %.1f, "
             "glue and submit %.1f", pro_ns / 1e6 / 600, walk_ns / 1e6 / 600, rec_ns / 1e6 / 600, tail_ns / 1e6 / 600);
        if (part)
            VLOG("partial submission: %.0f %% of submissions in parts, %.1f parts each, the first part sent %.1f ms after the start "
                 "(the whole: %.1f ms); parts held back from a full barrier %.1f times a submission, sent for an idle GPU %.1f",
                 100.0 * g_part_subs / 600, g_part_subs ? (double)g_part_parts / g_part_subs : 0.0,
                 g_part_subs ? g_part_first_ns / 1e6 / g_part_subs : 0.0, wall_ns / 1e6 / 600, g_part_held / 600.0, g_part_idle_sent / 600.0);
        prev_rec = rec; wall_ns = walk_ns = chunks = g_glue_images = longest_ns = longest_draws = pro_ns = rec_ns = tail_ns = 0;
        g_part_subs = g_part_parts = g_part_first_ns = g_part_held = g_part_idle_sent = 0;
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
void mtl_finish(void) { mtl_wait(g_fence, UINT64_MAX); wb_held_take(0, 1); }   // everything done: the held copy backs too
void mtl_drain(void) { mtl_finish(); }
