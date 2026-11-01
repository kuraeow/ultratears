// Descriptors in buffers (VK_EXT_descriptor_buffer): the target design (bindless, no descriptor set updates
// per draw). A draw's descriptors are bytes written into the submission's descriptor memory at the layout's binding
// offsets; the shaders find them through one offset set per draw (vkCmdSetDescriptorBufferOffsetsEXT). The bytes of each
// resource (a buffer range, a view with its sampler) come from the driver once per submission (vkGetDescriptorEXT) and
// are copied from then on. Before, each draw allocated a descriptor set and wrote it through vkUpdateDescriptorSets:
// with ~6000 draws a frame on the Great Sky Island, ~45 % of the replay thread was in the driver (2026-10-24).
// Within a submission no object a descriptor names is destroyed or recreated (destruction waits for the GPU, pools are
// deleted between submissions), so the per-submission caches cannot see a reused handle.
// TOTK_VK_DESCRIPTOR_SETS=1 keeps descriptor sets (A/B; also where the extension is missing).
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_shader.h"
#include "vkb_lock.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

int vkb_db_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_DESCRIPTOR_SETS");
        v = vkc.has_descriptor_buffer && !(e && *e && *e != '0');
        VLOG("descriptors: %s", v ? "descriptor buffers" : "descriptor sets");
    }
    return v;
}

// ---- the layouts: size and the offset of every binding
static VkDeviceSize g_size[2], g_off[2][128];
void vkb_db_layouts(const VkDescriptorSetLayout layout[2]) {
    for (int c = 0; c < 2; c++) {
        vk.vkGetDescriptorSetLayoutSizeEXT(vkc.device, layout[c], &g_size[c]);
        for (uint32_t b = 0; b < 128; b++) {
            int used = b % VKB_STAGE_BINDINGS < VKB_B_IMG + VKB_NIMG && (!c || b < VKB_STAGE_BINDINGS);   // compute: one stage
            if (used) vk.vkGetDescriptorSetLayoutBindingOffsetEXT(vkc.device, layout[c], b, &g_off[c][b]);
        }
    }
    VLOG("descriptor buffer: set sizes %llu (graphics) / %llu (compute) bytes", (unsigned long long)g_size[0], (unsigned long long)g_size[1]);
}

// ---- descriptor memory: per command slot and recording thread (vkb_mt.c), host-visible (device-local with resizable
// BAR), grown by replacing it
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; VkDeviceAddress addr; } dmem_t;
static dmem_t g_mem[VKB_SLOTS][VKB_MAXREC];
static dmem_t g_old[VKB_SLOTS][VKB_MAXREC][8];   // replaced during a submission: freed when the slot comes round again
static int g_nold[VKB_SLOTS][VKB_MAXREC];
// The memory starts at DMEM_MIN and doubles when a submission needs more; when a slot and thread used under a quarter
// of it over DMEM_TRIM of the slot's submissions, it is freed and made again at twice that use. Before: 16 MiB and up
// per slot and recording thread, kept for good - 448 MiB after a load and a run (2026-10-28, L7).
enum { DMEM_MIN = 2u << 20, DMEM_TRIM = 120 };
typedef struct { VkDeviceSize peak, want; int resets; } dtrim_t;
static dtrim_t g_trim[VKB_SLOTS][VKB_MAXREC];
static __thread uint64_t g_gen = 1;   // changes whenever this thread's memory changes (offsets made before are void)
static __thread uint64_t g_bound_rec, g_bound_gen;
static uint32_t g_type = UINT32_MAX;   // the same answer whichever thread finds it

static int dmem_new(dmem_t *m, VkDeviceSize size) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &m->buf), "descriptor buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, m->buf, &req);
    if (g_type == UINT32_MAX) {
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && g_type == UINT32_MAX; i++)
            if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want &&
                vkc.memory.memoryHeaps[vkc.memory.memoryTypes[i].heapIndex].size >= (1ull << 30)) g_type = i;   // not the 256 MiB BAR window
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && g_type == UINT32_MAX; i++)
            if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) g_type = i;
    }
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = g_type};
    vkb_mtag = MT_DESC;
    if (g_type == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &m->mem), "descriptor memory")) {
        vk.vkDestroyBuffer(vkc.device, m->buf, NULL);
        return 0;
    }
    vk.vkBindBufferMemory(vkc.device, m->buf, m->mem, 0);
    void *p;
    if (!vk_check(vk.vkMapMemory(vkc.device, m->mem, 0, VK_WHOLE_SIZE, 0, &p), "map descriptor memory")) return 0;
    VkBufferDeviceAddressInfo ad = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = m->buf};
    m->map = p; m->size = size; m->used = 0; m->addr = vk.vkGetBufferDeviceAddress(vkc.device, &ad);
    return 1;
}
static void dmem_free(dmem_t *m) {
    if (!m->buf) return;
    vk.vkDestroyBuffer(vkc.device, m->buf, NULL);
    vk.vkFreeMemory(vkc.device, m->mem, NULL);
    memset(m, 0, sizeof *m);
}
void vkb_db_slot_reset(int slot) {   // the slot's previous submission is complete
    for (int w = 0; w < VKB_MAXREC; w++) {
        dmem_t *m = &g_mem[slot][w];
        dtrim_t *t = &g_trim[slot][w];
        VkDeviceSize used = m->used;   // the submission's use, memories replaced during it included
        for (int i = 0; i < g_nold[slot][w]; i++) { used += g_old[slot][w][i].used; dmem_free(&g_old[slot][w][i]); }
        g_nold[slot][w] = 0;
        if (used > t->peak) t->peak = used;
        m->used = 0;
        if (++t->resets >= DMEM_TRIM) {
            VkDeviceSize keep = t->peak * 2 > DMEM_MIN ? t->peak * 2 : DMEM_MIN;
            if (m->buf && m->size > keep * 2) {   // no submission uses it (the slot's last one is done)
                dmem_free(m);   // made again on the next draw of this slot and thread (vkb_db_set binds it anew)
                t->want = keep;
            }
            t->peak = 0;
            t->resets = 0;
        }
    }
}
uint64_t vkb_db_gen(void) { return g_gen; }

// ---- the bytes of one descriptor, per submission (tables per recording thread)
// An entry holds the device's largest descriptor: 64 bytes a combined image sampler on NVIDIA, 96 on RADV (Steam Deck:
// a fixed 63 stopped the game there at its first draw, 2026-10-29).
enum { CACHE = 1 << 15, DESC_MAX = 256 };
typedef struct { uint64_t rec, k0, k1, k2, k3; uint8_t len, bytes[]; } dcache_t;
static __thread uint8_t *g_cache;
static size_t g_stride;   // entry size: the header and the largest descriptor, 8-byte aligned
static vkb_cnt g_get_calls, g_cache_hits;
typedef struct { uint64_t rec; VkBuffer buf; VkDeviceAddress addr; } addr_t;
static __thread addr_t *g_addr;   // 4096

static VkDeviceAddress buffer_address(VkBuffer b, uint64_t rec) {
    if (!g_addr && !(g_addr = calloc(4096, sizeof *g_addr))) die("buffer addresses");
    uint32_t h = (uint32_t)(((uint64_t)(uintptr_t)b * 0x9e3779b97f4a7c15ull) >> 52);
    for (uint32_t k = h;; k = (k + 1) & 4095) {
        if (g_addr[k].rec == rec && g_addr[k].buf == b) return g_addr[k].addr;
        if (g_addr[k].rec != rec) {
            VkBufferDeviceAddressInfo ad = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b};
            g_addr[k].rec = rec; g_addr[k].buf = b; g_addr[k].addr = vk.vkGetBufferDeviceAddress(vkc.device, &ad);
            return g_addr[k].addr;
        }
    }
}
static size_t type_size(VkDescriptorType t) {
    switch (t) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return vkc.db.uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return vkc.db.storageBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return vkc.db.combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return vkc.db.storageImageDescriptorSize;
    default: return 0;
    }
}
static const uint8_t *descriptor(const VkWriteDescriptorSet *w, uint64_t rec, size_t *len) {
    uint64_t k0 = (uint64_t)w->descriptorType, k1, k2, k3;
    if (w->pBufferInfo) { k1 = (uint64_t)(uintptr_t)w->pBufferInfo->buffer; k2 = w->pBufferInfo->offset; k3 = w->pBufferInfo->range; }
    else { k1 = (uint64_t)(uintptr_t)w->pImageInfo->sampler; k2 = (uint64_t)(uintptr_t)w->pImageInfo->imageView; k3 = (uint64_t)w->pImageInfo->imageLayout; }
    if (!g_stride) {
        size_t big = 0;
        static const VkDescriptorType ty[] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        for (int i = 0; i < 4; i++) if (type_size(ty[i]) > big) big = type_size(ty[i]);
        if (big > DESC_MAX) die("descriptor of %zu bytes", big);
        g_stride = (sizeof(dcache_t) + big + 7) & ~(size_t)7;
    }
    if (!g_cache && !(g_cache = calloc(CACHE, g_stride))) die("descriptor cache");
    uint64_t h = (k0 * 0x9e3779b97f4a7c15ull) ^ (k1 * 0xc2b2ae3d27d4eb4full) ^ (k2 * 0x165667b19e3779f9ull) ^ (k3 * 0x27d4eb2f165667c5ull);
    uint32_t at = (uint32_t)(h >> 49) & (CACHE - 1);
    dcache_t *e = NULL;
    for (int probe = 0; probe < 16; probe++) {
        dcache_t *c = (dcache_t *)(g_cache + (size_t)((at + (uint32_t)probe) & (CACHE - 1)) * g_stride);
        if (c->rec == rec && c->k0 == k0 && c->k1 == k1 && c->k2 == k2 && c->k3 == k3) { vkb_cnt_add(g_cache_hits, 1); *len = c->len; return c->bytes; }
        if (c->rec != rec) { e = c; break; }
    }
    static __thread uint8_t spill[DESC_MAX];   // the table region is full: no caching for this one
    uint8_t *out = e ? e->bytes : spill;
    size_t n = type_size(w->descriptorType);
    VkDescriptorGetInfoEXT gi = {VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT, .type = w->descriptorType};
    VkDescriptorAddressInfoEXT ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT};
    if (w->pBufferInfo) {
        ai.address = buffer_address(w->pBufferInfo->buffer, rec) + w->pBufferInfo->offset;
        ai.range = w->pBufferInfo->range;
        if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) gi.data.pUniformBuffer = &ai; else gi.data.pStorageBuffer = &ai;
    } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) gi.data.pCombinedImageSampler = w->pImageInfo;
    else gi.data.pStorageImage = w->pImageInfo;
    vk.vkGetDescriptorEXT(vkc.device, &gi, n, out);
    vkb_cnt_add(g_get_calls, 1);
    if (e) { e->rec = rec; e->k0 = k0; e->k1 = k1; e->k2 = k2; e->k3 = k3; e->len = (uint8_t)n; }
    *len = n;
    return out;
}

// This thread's descriptor memory of the slot bound to the command buffer (rec): once per command buffer, and again
// whenever the memory was replaced.
void vkb_db_bind(VkCommandBuffer cb, int slot, uint64_t rec) {
    dmem_t *m = &g_mem[slot][vkb_worker()];
    if (!m->buf || (g_bound_rec == rec && g_bound_gen == g_gen)) return;
    VkDescriptorBufferBindingInfoEXT bb = {VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT, .address = m->addr,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT};
    vk.vkCmdBindDescriptorBuffersEXT(cb, 1, &bb);
    g_bound_rec = rec; g_bound_gen = g_gen;
}
// A draw's descriptors (the writes' dstSet is ignored) in the slot's descriptor memory: their offset there.
VkDeviceSize vkb_db_set(VkCommandBuffer cb, int slot, uint64_t rec, const VkWriteDescriptorSet *w, int n, int compute) {
    int wk = vkb_worker();
    dmem_t *m = &g_mem[slot][wk];
    VkDeviceSize a = vkc.db.descriptorBufferOffsetAlignment ? vkc.db.descriptorBufferOffsetAlignment : 64;
    VkDeviceSize at = (m->used + a - 1) & ~(a - 1);
    if (!m->buf || at + g_size[compute] > m->size) {   // more memory: the old one stays until the slot's submission is done
        VkDeviceSize size = m->size ? m->size * 2 : g_trim[slot][wk].want ? g_trim[slot][wk].want : DMEM_MIN;
        VkDeviceSize cap = vkc.db.maxResourceDescriptorBufferRange < vkc.db.maxSamplerDescriptorBufferRange
                         ? vkc.db.maxResourceDescriptorBufferRange : vkc.db.maxSamplerDescriptorBufferRange;
        if (cap && size > cap) size = cap;
        if (m->buf) {
            if (g_nold[slot][wk] == 8) die("descriptor memory: replaced 8 times in one submission");
            g_old[slot][wk][g_nold[slot][wk]++] = *m;
            memset(m, 0, sizeof *m);
        }
        if (!dmem_new(m, size)) die("descriptor memory");
        if (dm_stats()) VLOG("descriptor memory of slot %d, recording thread %d: %llu MiB", slot, wk, (unsigned long long)(size >> 20));
        g_gen++;
        at = 0;
    }
    vkb_db_bind(cb, slot, rec);
    uint64_t sub = mtl_last_fence() + 1;   // the bytes stay valid for the submission (rec: this command buffer)
    uint8_t *base = m->map + at;
    for (int i = 0; i < n; i++) {
        size_t len;
        const uint8_t *d = descriptor(&w[i], sub, &len);
        memcpy(base + g_off[compute][w[i].dstBinding], d, len);
    }
    m->used = at + g_size[compute];
    return at;
}
void vkb_db_stats(uint64_t *get_calls, uint64_t *cache_hits) { *get_calls = vkb_cnt_sum(g_get_calls); *cache_hits = vkb_cnt_sum(g_cache_hits); }
