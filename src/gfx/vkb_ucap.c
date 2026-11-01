// Uniform contents as of the submit call (after MacNativeGames' nvn_mtl.m usnap / mtl_sub_capture_ubos, MIT).
// On the console nvnQueueSubmitCommands returns at once and the GPU reads the constants soon after; the game, having
// handed the frame over, rebuilds its per-object and per-frame constant pools on other threads right away. Our replay
// runs later (the translation thread) and the GPU later still, so draws read constants of the next frame or bytes the game
// cleared for refilling: TOTK's light parameters (c13 of its lighting shaders) read as zeros every other frame, and on a
// camera cut of the title menu the scene went black for 4-6 frames (2026-10-24; MacNativeGames saw distant meshes
// drawn in front of the camera from the same race).
// So the uniform ranges a submission binds are copied in the submit call (game thread, mtl_sub_capture_ubos), uploaded
// at the start of its replay into device memory of the submission's slot (mtl_sub_snapshot_ubos_cap), and bindings of
// those ranges read the copy (vkb_ucap_range). A GPU write to a copied range during the submission (inline uniform update,
// buffer copy, texture-to-buffer copy, storage buffer store) drops that copy from then on: later readers see the GPU's
// result in memory, as MacNativeGames does. TOTK_UBO_SNAPSHOT=0 reads live memory as before (A/B).
// "From then on" is the position of the command in the submission (vkb_seq): with several recording threads the walk of
// the submission (vkb_mt.c) drops in order and the threads only look, each at its own position.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "vkb.h"
#include "vkb_lock.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
// SSBO_MAX: TOTK binds one 1.4 MB storage buffer (per-object transforms / skinning) to most draws of a frame; read live,
// animated meshes and props tore into wedges (2026-10-28, F9 geom8/f6333).
enum { UBO_MAX = 65536, OTHER_MAX = 1 << 20, SSBO_MAX = 4 << 20, MERGE_MAX = 4 << 20 };

typedef struct { uint64_t lo, hi; } rng;
typedef struct { int n; rng *r; size_t *off; uint8_t *bytes; uint64_t done; } ucap;   // ranges sorted, disjoint; off: into bytes; done: the GPU had completed this submission at the capture

int vkb_ucap_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_UBO_SNAPSHOT"); on = !(e && *e == '0'); VLOG("uniform contents as of the submit call: %s", on ? "on" : "off (live memory, A/B)"); }
    return on;
}

static void collect(rng **v, int *n, int *cap, const uint8_t *p, int depth) {
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt command record at %p while copying uniforms (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth < 16) collect(v, n, cap, (const uint8_t *)(uintptr_t)t, depth + 1); }
        else if (h.op == OP_BIND_UBO || h.op == OP_BIND_SSBO || h.op == OP_COPY_B2B) {
            // uniform buffers; storage buffers (TOTK's light list, read by its light shaders) and the sources of buffer
            // copies (its light parameters reach their uniform buffer by a GPU copy every frame): all bytes the game
            // rewrites for the next frame as soon as it has submitted this one
            uint64_t addr, sz;
            if (h.op == OP_COPY_B2B) { memcpy(&addr, p + 8, 8); memcpy(&sz, p + 24, 8); if (sz > OTHER_MAX) sz = 0; }
            else {
                const p_bind_buf *b = (const p_bind_buf *)(p + 8);
                addr = b->addr; sz = b->size;
                if (h.op == OP_BIND_UBO && sz > UBO_MAX) sz = UBO_MAX;
                if (h.op == OP_BIND_SSBO && sz > SSBO_MAX) sz = 0;
            }
            if (addr && sz) {
                if (*n == *cap) { *cap = *cap ? *cap * 2 : 4096; if (!(*v = realloc(*v, sizeof **v * (size_t)*cap))) die("uniform capture ranges"); }
                (*v)[(*n)++] = (rng){addr, addr + sz};
            }
        }
        p += h.size;
    }
}
static int rng_cmp(const void *a, const void *b) { const rng *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }

static _Atomic uint64_t g_cap_bytes, g_cap_ns, g_caps;
void *mtl_sub_capture_ubos(const uint64_t *h, int n) {   // game thread, in the submit call
    if (!vkb_ucap_on()) return NULL;
    uint64_t t0 = runtime_now_ns();
    rng *v = NULL;
    int nv = 0, cv = 0;
    for (int i = 0; i < n; i++) collect(&v, &nv, &cv, (const uint8_t *)(uintptr_t)h[i], 0);
    ucap *c = calloc(1, sizeof *c);
    if (!c) die("uniform capture");
    c->done = mtl_done();   // before the bytes: a submission completing meanwhile only makes this conservative
    if (nv) {
        qsort(v, (size_t)nv, sizeof *v, rng_cmp);
        int k = -1;   // overlapping / touching bindings merged (up to MERGE_MAX); ranges outside every imported pool dropped
        for (int i = 0; i < nv; i++) {
            void *mb; uint64_t mo;
            if (k >= 0 && v[i].lo <= v[k].hi) {
                if (v[i].hi <= v[k].hi) continue;
                if (v[i].hi - v[k].lo <= MERGE_MAX && mtl_addr(v[k].lo, v[i].hi - v[k].lo, &mb, &mo)) { v[k].hi = v[i].hi; continue; }
            }
            if (!mtl_addr(v[i].lo, v[i].hi - v[i].lo, &mb, &mo)) continue;
            v[++k] = v[i];
        }
        c->n = k + 1;
        c->off = malloc(sizeof *c->off * (size_t)(c->n ? c->n : 1));
        if (!c->off) die("uniform capture");
        size_t total = 0;
        for (int i = 0; i < c->n; i++) { c->off[i] = total; total += (size_t)(v[i].hi - v[i].lo); }
        c->bytes = malloc(total ? total : 1);
        if (!c->bytes) die("uniform capture bytes");
        for (int i = 0; i < c->n; i++) memcpy(c->bytes + c->off[i], (const void *)(uintptr_t)v[i].lo, (size_t)(v[i].hi - v[i].lo));
        c->r = v; v = NULL;
        atomic_fetch_add(&g_cap_bytes, total);
    }
    free(v);
    atomic_fetch_add(&g_cap_ns, runtime_now_ns() - t0);
    atomic_fetch_add(&g_caps, 1);
    return c;
}

// The replay's table: the captured ranges in device memory of the slot (per-slot buffer, grown as needed).
typedef struct { uint64_t lo, hi; VkDeviceSize dev; _Atomic uint64_t drop_at; } urng;   // drop_at: the first position not served (UINT64_MAX: none)
static urng *g_u;
static int g_nu, g_capu, g_uslot = -1;
static const uint8_t *g_ubytes;   // the captured bytes (CPU reads of pointers kept in uniform buffers)
static size_t *g_uoff;
static ucap *g_cur;
typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize cap; } arena_t;
static arena_t g_arena[VKB_SLOTS];
static vkb_cnt g_hits;
static uint64_t g_drops;
// GPU writes of submissions recorded so far (inline updates, copies, stores), kept until the GPU has completed them: a
// capture taken before that holds the bytes from before the write, so ranges such a write overlaps read memory (the GPU
// timeline) instead of the copy.
typedef struct { uint64_t lo, hi, fence; } pend_t;
static pend_t *g_pend;
static int g_npend, g_cappend;

static int arena_get(int slot, VkDeviceSize need) {
    arena_t *a = &g_arena[slot];
    if (a->cap >= need) return 1;
    if (a->buf) { vk.vkDestroyBuffer(vkc.device, a->buf, NULL); vk.vkFreeMemory(vkc.device, a->mem, NULL); }   // the slot's last submission is done
    *a = (arena_t){0};
    VkDeviceSize cap = 1u << 20;
    while (cap < need) cap *= 2;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &a->buf), "uniform copy buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, a->buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = type};
    vkb_mtag = MT_UCAP;
    if (type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &a->mem), "uniform copy memory")) {
        vk.vkDestroyBuffer(vkc.device, a->buf, NULL); *a = (arena_t){0}; return 0;
    }
    vk.vkBindBufferMemory(vkc.device, a->buf, a->mem, 0);
    a->cap = cap;
    return 1;
}

static _Atomic int g_ninl;   // inline updates logged (below)
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *capv) {   // replay thread, the submission just begun
    ucap *c = capv ? capv : mtl_sub_capture_ubos(h, n);   // without the translation thread: the replay is in the submit call
    if (g_cur) { free(g_cur->r); free(g_cur->off); free(g_cur->bytes); free(g_cur); g_cur = NULL; }
    g_nu = 0; g_uslot = -1; g_ubytes = NULL; g_uoff = NULL;
    atomic_store_explicit(&g_ninl, 0, memory_order_release);   // a new submission: its own inline updates
    if (!c) return;
    g_cur = c;
    if (!c->n || vkb_slot() < 0) return;
    if (g_capu < c->n) { g_capu = c->n; if (!(g_u = realloc(g_u, sizeof *g_u * (size_t)g_capu))) die("uniform copy table"); }
    uint64_t done = mtl_done();
    int k = 0;
    for (int i = 0; i < g_npend; i++) if (g_pend[i].fence > done) g_pend[k++] = g_pend[i];   // completed writes are in memory
    g_npend = k;
    VkDeviceSize at = 0;   // each range keeps its address's position within 256 bytes: bindings stay aligned as in the pool
    for (int i = 0; i < c->n; i++) {
        at = ((at + 255) & ~(VkDeviceSize)255) + (c->r[i].lo & 255);
        g_u[i] = (urng){c->r[i].lo, c->r[i].hi, at, UINT64_MAX};
        for (int j = 0; j < g_npend && g_u[i].drop_at; j++)   // a GPU write the capture predates: read memory
            if (g_pend[j].fence > c->done && g_pend[j].lo < c->r[i].hi && c->r[i].lo < g_pend[j].hi) { g_u[i].drop_at = 0; g_drops++; }
        at += c->r[i].hi - c->r[i].lo;
    }
    int slot = vkb_slot();
    if (!arena_get(slot, at)) return;
    VkBuffer rb;
    VkDeviceSize ro;
    uint8_t *p = vkb_ring_alloc(at, 256, &rb, &ro);
    for (int i = 0; i < c->n; i++) memcpy(p + g_u[i].dev, c->bytes + c->off[i], (size_t)(c->r[i].hi - c->r[i].lo));
    VkCommandBuffer cb = vkb_cmd();
    VkBufferCopy r = {ro, 0, at};
    vk.vkCmdCopyBuffer(cb, rb, g_arena[slot].buf, 1, &r);
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
    g_nu = c->n; g_uslot = slot; g_ubytes = c->bytes; g_uoff = c->off;
}

// Inline uniform updates of the submission being replayed, in command order (L20): the CPU reads of uniform memory
// (pointers to storage buffers) see the ones before their own position. TOTK rewrites c6 of its Depths terrain between
// draws with the pointer to each piece of the mesh; the CPU read memory (the newest pointer) and every terrain draw took
// one piece's corners: the far terrain in shards. Appended by the walk of the submission (or by the replay on one
// thread) before any reader at a later position runs; a fixed array, so readers on other threads never see it move.
enum { INLINE_MAX = 1 << 16 };
typedef struct { uint64_t seq, addr, n; const uint8_t *data; } inl_t;
static inl_t *g_inl;
static _Atomic int g_ninl;
void vkb_ucap_inline(uint64_t addr, uint64_t n, const uint8_t *data) {
    if (vkb_mt_recording()) return;   // the walk of the submission logged it already (as vkb_ucap_drop)
    if (!g_inl && !(g_inl = malloc(sizeof *g_inl * INLINE_MAX))) die("inline uniform updates");
    int k = atomic_load_explicit(&g_ninl, memory_order_relaxed);
    if (k == INLINE_MAX) { static int said; if (!said++) VLOG("[temporary] more than %d inline uniform updates in a submission: later ones not seen by CPU reads", INLINE_MAX); return; }
    g_inl[k] = (inl_t){vkb_seq(), addr, n, data};
    atomic_store_explicit(&g_ninl, k + 1, memory_order_release);
}
static void inline_overlay(uint64_t addr, uint8_t *out, size_t n) {   // the updates before this position, newest last
    int cnt = atomic_load_explicit(&g_ninl, memory_order_acquire);
    uint64_t at = vkb_seq();
    int lo = 0, hi = cnt;   // the first update at or after this position
    while (lo < hi) { int m = (lo + hi) / 2; if (g_inl[m].seq < at) lo = m + 1; else hi = m; }
    uint64_t need = n;   // bytes not yet decided, newest update first
    uint8_t got[4096];
    if (!cnt || n > sizeof got) return;
    memset(got, 0, n);
    for (int i = lo - 1; i >= 0 && need; i--) {
        const inl_t *u = &g_inl[i];
        if (u->addr >= addr + n || addr >= u->addr + u->n) continue;
        for (uint64_t b = 0; b < n; b++)
            if (!got[b] && addr + b >= u->addr && addr + b < u->addr + u->n) { out[b] = u->data[addr + b - u->addr]; got[b] = 1; need--; }
    }
}

static int dropped(const urng *u) { return atomic_load_explicit(&u->drop_at, memory_order_acquire) <= vkb_seq(); }
static int find(uint64_t a) {   // the first range ending above a
    int lo = 0, hi = g_nu;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_u[m].hi <= a) lo = m + 1; else hi = m; }
    return lo;
}
int vkb_ucap_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_nu || g_uslot != vkb_slot()) return 0;   // another slot (a split submission): its copy is not this one's
    int i = find(addr);
    if (i >= g_nu || addr < g_u[i].lo || addr + size > g_u[i].hi || dropped(&g_u[i])) return 0;
    *buf = g_arena[g_uslot].buf;
    *off = g_u[i].dev + (addr - g_u[i].lo);
    vkb_cnt_add(g_hits, 1);
    return 1;
}
int vkb_ucap_why(uint64_t addr, uint64_t size) {   // development: 0 in the copy, 1 not captured, 2 dropped by a GPU write, 3 other slot
    if (!g_nu) return 1;
    if (g_uslot != vkb_slot()) return 3;
    int i = find(addr);
    if (i >= g_nu || addr < g_u[i].lo || addr + size > g_u[i].hi) return 1;
    return dropped(&g_u[i]) ? 2 : 0;
}
void vkb_ucap_read(uint64_t addr, void *out, size_t n) {   // CPU reads of uniform memory (pointers in uniform buffers)
    if (g_nu) {
        int i = find(addr);
        if (i < g_nu && addr >= g_u[i].lo && addr + n <= g_u[i].hi && !dropped(&g_u[i])) { memcpy(out, g_ubytes + g_uoff[i] + (addr - g_u[i].lo), n); return; }
    }
    memcpy(out, (const void *)(uintptr_t)addr, n);
    inline_overlay(addr, out, n);
}
void vkb_ucap_drop(uint64_t addr, uint64_t len) {   // a GPU write to [addr, addr+len): copies it overlaps are dropped
    if (vkb_mt_recording()) return;   // the walk of the submission dropped it already (vkb_mt.c)
    uint64_t at = vkb_seq();
    for (int i = find(addr); i < g_nu && g_u[i].lo < addr + len; i++)
        if (atomic_load(&g_u[i].drop_at) > at) { atomic_store_explicit(&g_u[i].drop_at, at, memory_order_release); g_drops++; }
    if (!vkb_ucap_on()) return;
    if (g_npend == g_cappend) { g_cappend = g_cappend ? g_cappend * 2 : 1024; if (!(g_pend = realloc(g_pend, sizeof *g_pend * (size_t)g_cappend))) die("pending GPU writes"); }
    g_pend[g_npend++] = (pend_t){addr, addr + len, mtl_last_fence() + 1};
}
void vkb_ucap_stats(uint64_t *caps, uint64_t *bytes, uint64_t *ns, uint64_t *hits, uint64_t *drops) {
    *caps = g_caps; *bytes = g_cap_bytes; *ns = g_cap_ns; *hits = vkb_cnt_sum(g_hits); *drops = g_drops;
}
