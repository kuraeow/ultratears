// Vulkan memory for the copies of game memory pools (backend_vk.c): blocks of BLOCK bytes, cut into pieces first-fit (a
// pool larger than a block gets a block of its own). One allocation per pool would exhaust the driver's allocation
// count (~4096 on Windows) with TOTK's ~2900 pools, and on Linux every allocation is one more buffer object the kernel
// checks at every submission. A freed piece is reused once the GPU has finished every submission recorded up to then.
// Two heaps: device memory for the copies of discrete GPUs ("pool shadows", a buffer per block), and host-visible
// memory for the pools themselves on Linux ("pool copies": each pool's own buffer is bound to its piece).
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
enum { BLOCK = 64u << 20, ALIGN = 256, HOST_ALIGN = 4096 };

typedef struct { VkDeviceSize off, size; } span;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size; uint32_t type; span *free; int nfree, capfree; } block_t;
typedef struct { int block; VkDeviceSize off, size; uint64_t fence; } piece;   // fence: freed, reusable when done
typedef struct {
    block_t *blocks;
    int nblocks;
    piece *pieces;               // handle - 1 -> piece
    int npieces, cappieces;
    int *pending;                // freed handles waiting for their fence
    int npending, cappending;
    uint64_t bytes;              // in use
} heap_t;
static heap_t g_dev, g_host;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

int vkb_shadow_on(void) {   // discrete GPUs: game memory is across PCIe from the GPU
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TOTK_VK_POOL_SHADOW");
        // with pool copies (Linux) the pools are Vulkan memory already, which the GPU of a discrete card reads across
        // PCIe as it read the imported pools: [temporary] no device-local copies on top of them yet
        v = vkb_pool_copies() ? 0 : e && *e ? *e != '0' : vkc.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        VLOG("copies of game memory in device memory: %s", v ? "on (discrete GPU)" : vkb_pool_copies() ? "off (pool copies)" : "off (memory shared with the GPU)");
    }
    return v;
}

static int block_add(heap_t *h, block_t b) {
    b.capfree = 16;
    b.free = malloc(sizeof *b.free * (size_t)b.capfree);
    if (!b.free) die("shadow free list");
    b.free[0] = (span){0, b.size};
    b.nfree = 1;
    h->blocks = realloc(h->blocks, sizeof *h->blocks * (size_t)(h->nblocks + 1));
    if (!h->blocks) die("shadow blocks");
    h->blocks[h->nblocks] = b;
    return h->nblocks++;
}
static int block_new(VkDeviceSize size) {   // device memory, one buffer over the block
    block_t b = {0};
    b.size = size;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &b.buf), "shadow buffer")) return -1;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, b.buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) type = i;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = type};
    vkb_mtag = MT_POOL_COPY;
    if (type == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &b.mem) != VK_SUCCESS) {
        vk.vkDestroyBuffer(vkc.device, b.buf, NULL);
        VLOG("shadow memory: no %llu MiB of device memory", (unsigned long long)(size >> 20));
        return -1;
    }
    vk.vkBindBufferMemory(vkc.device, b.buf, b.mem, 0);
    return block_add(&g_dev, b);
}
static int host_block_new(VkDeviceSize size, uint32_t type) {   // host-visible memory, mapped, buffers bound into it
    block_t b = {0};
    b.size = size;
    b.type = type;
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = size, .memoryTypeIndex = type};
    void *m;
    vkb_mtag = MT_POOL_COPY;
    if (vk.vkAllocateMemory(vkc.device, &ai, NULL, &b.mem) != VK_SUCCESS) {
        VLOG("pool copies: no %llu MiB of host-visible memory", (unsigned long long)(size >> 20));
        return -1;
    }
    if (!vk_check(vk.vkMapMemory(vkc.device, b.mem, 0, VK_WHOLE_SIZE, 0, &m), "pool copy map")) {
        vk.vkFreeMemory(vkc.device, b.mem, NULL);
        return -1;
    }
    b.map = m;
    return block_add(&g_host, b);
}
static void span_free(block_t *b, VkDeviceSize off, VkDeviceSize size) {   // back into the sorted list, merged
    int i = 0;
    while (i < b->nfree && b->free[i].off < off) i++;
    if (b->nfree == b->capfree) {
        b->capfree *= 2;
        b->free = realloc(b->free, sizeof *b->free * (size_t)b->capfree);
        if (!b->free) die("shadow free list");
    }
    memmove(&b->free[i + 1], &b->free[i], sizeof *b->free * (size_t)(b->nfree - i));
    b->free[i] = (span){off, size};
    b->nfree++;
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
        b->free[i].size += b->free[i + 1].size;
        memmove(&b->free[i + 1], &b->free[i + 2], sizeof *b->free * (size_t)(b->nfree - i - 2));
        b->nfree--;
    }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
        b->free[i - 1].size += b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], sizeof *b->free * (size_t)(b->nfree - i - 1));
        b->nfree--;
    }
}
static void reclaim(heap_t *h) {   // freed pieces whose GPU work is done
    for (int k = 0; k < h->npending;) {
        piece *p = &h->pieces[h->pending[k] - 1];
        if (!mtl_wait(p->fence, 0)) { k++; continue; }
        span_free(&h->blocks[p->block], p->off, p->size);
        p->size = 0;
        h->pending[k] = h->pending[--h->npending];
    }
}
// First fit in the blocks of memory type `type` (UINT32_MAX: any; offsets and sizes are multiples of the heap's
// alignment); -1: none has room.
static int fit(heap_t *h, VkDeviceSize size, uint32_t type, VkDeviceSize *at) {
    for (int i = 0; i < h->nblocks; i++) {
        block_t *b = &h->blocks[i];
        if (type != UINT32_MAX && b->type != type) continue;
        for (int k = 0; k < b->nfree; k++)
            if (b->free[k].size >= size) {
                *at = b->free[k].off;
                b->free[k].off += size; b->free[k].size -= size;
                if (!b->free[k].size) {
                    memmove(&b->free[k], &b->free[k + 1], sizeof b->free[0] * (size_t)(b->nfree - k - 1));
                    b->nfree--;
                }
                return i;
            }
    }
    return -1;
}
static void take_first(block_t *b, VkDeviceSize size) {   // a new block: its start
    b->free[0].off = size; b->free[0].size -= size;
    if (!b->free[0].size) b->nfree = 0;
}
static uint32_t piece_new(heap_t *h, int bi, VkDeviceSize at, VkDeviceSize size) {
    if (h->npieces == h->cappieces) {
        h->cappieces = h->cappieces ? h->cappieces * 2 : 1024;
        h->pieces = realloc(h->pieces, sizeof *h->pieces * (size_t)h->cappieces);
        if (!h->pieces) die("shadow pieces");
    }
    h->pieces[h->npieces] = (piece){bi, at, size, 0};
    h->bytes += size;
    return (uint32_t)++h->npieces;
}
static void piece_free(heap_t *h, uint32_t hd) {
    if (!hd) return;
    pthread_mutex_lock(&g_mu);
    piece *p = &h->pieces[hd - 1];
    p->fence = mtl_last_fence() + 1;   // the submission being recorded may still use it
    h->bytes -= p->size;
    if (h->npending == h->cappending) {
        h->cappending = h->cappending ? h->cappending * 2 : 256;
        h->pending = realloc(h->pending, sizeof *h->pending * (size_t)h->cappending);
        if (!h->pending) die("shadow pending");
    }
    h->pending[h->npending++] = (int)hd;
    pthread_mutex_unlock(&g_mu);
}

uint32_t vkb_shadow_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off) {
    size = (size + ALIGN - 1) & ~(VkDeviceSize)(ALIGN - 1);
    pthread_mutex_lock(&g_mu);
    reclaim(&g_dev);
    VkDeviceSize at = 0;
    int bi = fit(&g_dev, size, UINT32_MAX, &at);
    if (bi < 0) {
        bi = block_new(size > BLOCK ? size : BLOCK);
        if (bi < 0) { pthread_mutex_unlock(&g_mu); return 0; }
        at = 0;
        take_first(&g_dev.blocks[bi], size);
    }
    uint32_t h = piece_new(&g_dev, bi, at, size);
    *buf = g_dev.blocks[bi].buf;
    *off = at;
    pthread_mutex_unlock(&g_mu);
    return h;
}
void vkb_shadow_free(uint32_t h) { piece_free(&g_dev, h); }
uint64_t vkb_shadow_bytes(void) { return g_dev.bytes; }

// Host-visible memory for buf (a pool's own buffer), bound to it; *map: where the CPU sees it. 0: no memory.
// Which memory (TOTK_POOL_MEM, an A/B for the Steam Deck, where the GPU reads every vertex, index and uniform from these
// copies): cached (default) - cached memory where the device has it, the copies back of GPU writes read it on the CPU;
// on an AMD APU it is snooped system memory, which the GPU reads more slowly. uncached - host-visible memory without
// CPU caching (write-combined), read faster by the GPU, slowly by the CPU's copies back. device - device-local and
// host-visible (the APU's carve-out, a discrete GPU's BAR). A type that runs out falls back to the cached one.
static int pool_mem_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("TOTK_POOL_MEM");
        m = e && !strcmp(e, "uncached") ? 1 : e && !strcmp(e, "device") ? 2 : 0;
        VLOG("pool copies in %s memory (TOTK_POOL_MEM)", m == 1 ? "uncached host" : m == 2 ? "device-local host-visible" : "cached host");
    }
    return m;
}
static uint32_t pool_mem_type(uint32_t bits, int mode) {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int pass = 0; pass < 2; pass++)   // the mode's kind first, then any host-visible coherent memory
        for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = vkc.memory.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want) != want) continue;
            int cached = !!(f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT), dev = !!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (pass || (mode == 0 ? cached : mode == 1 ? !cached && !dev : dev)) return i;
        }
    return UINT32_MAX;
}
uint32_t vkb_poolmem_alloc(VkBuffer buf, uint8_t **map) {
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, buf, &req);
    int mode = pool_mem_mode();
    uint32_t type = pool_mem_type(req.memoryTypeBits, mode);
    if (type == UINT32_MAX) { VLOG("pool copies: no host-visible memory type for a pool"); return 0; }
    VkDeviceSize size = (req.size + HOST_ALIGN - 1) & ~(VkDeviceSize)(HOST_ALIGN - 1);
    int own = req.alignment > HOST_ALIGN || size > BLOCK;   // a block of its own (its start meets any alignment)
    pthread_mutex_lock(&g_mu);
    reclaim(&g_host);
    VkDeviceSize at = 0;
    int bi = own ? -1 : fit(&g_host, size, type, &at);
    if (bi < 0) {
        bi = host_block_new(own ? size : BLOCK, type);
        if (bi < 0 && mode) {   // that memory ran out: the cached kind from here on
            static int said;
            uint32_t t0 = pool_mem_type(req.memoryTypeBits, 0);
            if (!said++) VLOG("pool copies: TOTK_POOL_MEM memory ran out at %llu MiB, the rest in cached memory", (unsigned long long)(g_host.bytes >> 20));
            if (t0 != UINT32_MAX && t0 != type) {
                type = t0;
                bi = own ? -1 : fit(&g_host, size, type, &at);
                if (bi < 0) bi = host_block_new(own ? size : BLOCK, type);
                else goto found;
            }
        }
        if (bi < 0) { pthread_mutex_unlock(&g_mu); return 0; }
        at = 0;
        take_first(&g_host.blocks[bi], size);
    }
found:;
    uint32_t h = piece_new(&g_host, bi, at, size);
    block_t *b = &g_host.blocks[bi];
    VkResult r = vk.vkBindBufferMemory(vkc.device, buf, b->mem, at);
    *map = b->map + at;
    pthread_mutex_unlock(&g_mu);
    if (r != VK_SUCCESS) { vk_check(r, "pool copy bind"); piece_free(&g_host, h); return 0; }
    return h;
}
void vkb_poolmem_free(uint32_t h) { piece_free(&g_host, h); }
uint64_t vkb_poolmem_bytes(void) { return g_host.bytes; }
