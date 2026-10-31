// NVN textures as Vulkan images (see vkb.h). Contents uploaded from game memory come with drawing (milestone 3.3
// step 4/5); here images exist for render targets, clears, copies and presentation.
// The NVN format table follows LASRecomp runtime/nvn/vulkan/textures.c (MIT, refs/LASRecomp): verified on a Switch
// game on Vulkan. Changes: ASTC, which desktop GPUs lack, becomes BC7 / BC1 from the cache made at build time
// (vkb_bc7.c), else RGBA8 decoded on upload; DEPTH24_STENCIL8 is D24S8 where the GPU can attach it, D32S8 otherwise
// (AMD, Steam Deck).
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#define _mkdir(path) mkdir(path, 0777)
#endif

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "nvn/nvn_dirty.h"
#include "tex/astc.h"
#include "tex/bc7.h"
#include "tex/nvn_deswizzle.h"
#include "nvn/totk_vpool.h"
#include "vkb_audit.h"
#include "vkb_lock.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static int supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// Depth buffers are kept as 32-bit float (D16 and D24 too). TOTK classifies lighting tiles with an EQUAL depth test:
// one pass writes gl_FragDepth = material id, the lighting passes draw quads at the same z. In a UNORM depth buffer
// the two values reach the buffer through different conversions (fragment depth export and rasterised z), whose
// rounding may differ by one step on PC GPUs - the tiles failed the test and the world was unlit (2026-10-23, D16).
// Maxwell rounds both the same way; a float buffer stores both exactly. The precision only grows.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        f = supports(VK_FORMAT_D32_SFLOAT_S8_UINT, need) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
        VLOG("DEPTH24_STENCIL8 -> %s", f == VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

VkFormat vkb_format(int f, VkImageAspectFlags *aspect, int *astc) {
    if (aspect) *aspect = f == 0x31 ? VK_IMAGE_ASPECT_STENCIL_BIT
                        : f == 0x35 || f == 0x36 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                        : f >= 0x32 && f <= 0x34 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (astc) *astc = f >= 0x79 && f <= 0x94;
    if (f >= 0x79 && f <= 0x86) return VK_FORMAT_R8G8B8A8_UNORM;   // ASTC LDR
    if (f >= 0x87 && f <= 0x94) return VK_FORMAT_R8G8B8A8_SRGB;    // ASTC sRGB
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;            case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;             case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;          case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;           case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;            case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;            case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;          case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;           case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;       case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;        case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;         case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;         case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8A8_UNORM;      // RGB8: expanded on upload
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;      case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;       case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;  case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;   case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;   case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x31: return VK_FORMAT_S8_UINT;             case 0x32: return VK_FORMAT_D32_SFLOAT;   // D16: see depth24_stencil8
    case 0x33: return VK_FORMAT_D32_SFLOAT;          case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return depth24_stencil8();            case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;
    // 16-bit packed formats: NVN names the components from the least significant bit (RGB565: R in bits 0-4), Vulkan
    // from the most significant one. The Vulkan format with the same bits, and the components put back in place by the
    // views (packed_swizzle): RGBA4 -> R4G4B4A4 read A,B,G,R; RGB5 / RGB5A1 -> A1R5G5B5 read B,G,R; RGB565 -> R5G6B5
    // read B,G,R (formats every device samples). TOTK's Depths terrain normal (RGB5A1) and blend (RGB565) maps read
    // with R and B swapped projected the ground textures along the wrong axis and mixed the wrong materials (as
    // totk-pc-recomp found, vk_formats.c). [temporary] Rendered into, they would store the components swapped.
    case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case 0x3A: case 0x3B: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;     case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;      case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;     case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK;     case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK;     case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK;   case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM;      case 0x71: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x76: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x95: return VK_FORMAT_R5G6B5_UNORM_PACK16;   // BGR565: B in bits 0-4, as Vulkan's R5G6B5
    case 0x96: case 0x97: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;   // BGR5, BGR5A1
    case 0x98: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;   // A1BGR5: A in bit 0
    case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;   // BGRX8, BGRA8
    case 0x9B: case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// ---------------- images: one per memory and shape ----------------
// The game re-initialises texture objects over the same memory with the same shape (its render targets every frame,
// copies of objects, pool entries of finalised ones): on the console that is one texture that keeps its contents. Each
// serial used to get an image of its own: ~30 new images a frame on the Great Sky Island (~8 ms of the replay thread in
// vkCreateImage / vkAllocateMemory), each starting empty where the console kept the previous frame's contents
// (2026-10-24; SavageWind found the same in BotW). Serials now map to entries shared by memory and shape. An entry no
// serial holds stays aside for RECYCLE_FRAMES presents (the next frame's object takes it back, contents included) and is
// then destroyed once the GPU is done with it. Taken back after the CPU wrote its memory meanwhile, it uploads again.
#define VIEWS 16   // per texture: levels of a mip chain drawn one by one, layers, formats, shader types
typedef struct entry {
    vkb_image im; VkImageView view[VIEWS]; uint8_t view_key[VIEWS][17]; int nviews;
    int refs, shaped;                       // serials holding it; in the shape index (it has memory)
    uint64_t released, released_stamp;      // aside since this present / dirty-tracking generation
    uint8_t has_layout, layout[8];
    struct entry *shape_next, *all_next, **all_pprev;
    struct entry *twin;                     // its copy for draws that sample it while it is their target (vkb_tex_feedback_copy)
    struct entry **blk;                     // a compressed texture drawn through block views: per level, the target (vkb_tex_block_target)
} entry;
typedef struct { uint64_t serial; entry *e; } tslot;   // serial 0: free; e NULL: removed
static tslot *g_tab;
static uint32_t g_cap, g_n, g_tomb;   // live serials, removed slots until the next grow
static entry *g_all;                  // every entry (dumps, alias logs)
enum { SHAPES = 4096, RECYCLE_FRAMES = 120, RECYCLE_MAX = 512 };
static entry *g_shape[SHAPES];
static entry *g_aside[RECYCLE_MAX];
static int g_naside;
static uint64_t g_images_made, g_images_shared, g_images_taken_back;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static tslot *slot(uint64_t serial) {   // g_mu held; g_cap a power of two
    uint32_t h = (uint32_t)(serial * 0x9E3779B97F4A7C15ull >> 40) & (g_cap - 1);
    while (g_tab[h].serial && g_tab[h].serial != serial) h = (h + 1) & (g_cap - 1);
    return &g_tab[h];
}
static void grow(void) {
    tslot *old = g_tab;
    uint32_t n = g_cap;
    g_cap = g_cap ? g_cap * 2 : 1024;
    g_tab = calloc(g_cap, sizeof *g_tab);
    if (!g_tab) die("image table");
    for (uint32_t i = 0; i < n; i++) if (old[i].serial && old[i].e) *slot(old[i].serial) = old[i];
    g_tomb = 0;
    free(old);
}
static uint32_t shape_of(const void *mem, const nvn_texdesc *d) {
    uint64_t h = (uint64_t)(uintptr_t)mem * 0x9E3779B97F4A7C15ull;
    h ^= ((uint64_t)(uint32_t)d->format << 32 | d->width) * 0xC2B2AE3D27D4EB4Full;
    h ^= ((uint64_t)d->height << 32 | d->levels) * 0x165667B19E3779F9ull;
    return (uint32_t)(h >> 52) & (SHAPES - 1);
}
static int same_shape(const entry *e, const tex_t *t) {
    const nvn_texdesc *a = &e->im.d, *b = &t->d;
    return e->im.storage == t->mem && a->target == b->target && a->format == b->format && a->flags == b->flags &&
           a->samples == b->samples && a->width == b->width && a->height == b->height && a->depth == b->depth &&
           a->levels == b->levels && a->stride == b->stride && a->pkg_block_h == b->pkg_block_h &&
           e->has_layout == t->has_layout && (!t->has_layout || !memcmp(e->layout, t->layout, sizeof e->layout));
}
// ---- GPU-drawn contents into game memory (write-back) ----
// On the Switch the GPU draws into game memory and the CPU sees the result there: TOTK bakes its terrain tiles on the
// GPU and later moves the tile cache in memory with the CPU. Here GPU contents live only in the images, so the moved
// bytes were zeros: dark tiles without grass, the Depths' ground black (L19, 2026-10-27). An image the GPU writes whose
// game memory the CPU can access is queued (vkb_image_written); once it has not been written for WB_SETTLE presents,
// its levels and layers are copied to host memory at a present, and laid out into game memory (nvn_swizzle) at a later
// one, when the GPU is done with the copy (vkb_tex_writeback_frame, translation thread, nothing waits). Render targets
// drawn every frame never settle and cost nothing. TOTK_GPU_WB=0: off (A/B).
// WB_FRAME_BYTES: copied per present at most (the layout into game memory of that much takes ~2 ms on the CPU).
enum { WB_SETTLE = 2, WB_ARENA = 64u << 20, WB_FRAME_BYTES = 4u << 20, WB_MAXJOBS = 512 };
int mtl_slot_free(void);
static entry **g_wbq;                 // queued images (wb_pending), g_wbq_mu
static int g_nwbq, g_capwbq;
static pthread_mutex_t g_wbq_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { entry *e; uint64_t seq, fence, at, bytes; } wb_job;   // e NULL: dropped meanwhile; at: offset in the arena
static wb_job g_wbj[WB_MAXJOBS];      // copies in submission order (g_mu held)
static int g_nwbj;
static void wbq_remove(entry *e) {    // g_wbq_mu held
    for (int i = 0; i < g_nwbq; i++) if (g_wbq[i] == e) { g_wbq[i] = g_wbq[--g_nwbq]; break; }
    e->im.wb_pending = 0;
}
enum { BLK_LEVELS = 16 };
static void side_free(entry *t) {    // an image kept beside an entry (twin, block target): destroyed when the GPU is done with it
    for (int i = 0; i < t->nviews; i++) vkb_destroy_later(t->view[i], VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkb_destroy_later(VK_NULL_HANDLE, t->im.img, t->im.mem);
    free(t);
}
static void entry_drop(entry *e) {   // g_mu held: out of the indexes, destroyed when the GPU is done with it
    if (e->im.wb_pending) { pthread_mutex_lock(&g_wbq_mu); wbq_remove(e); pthread_mutex_unlock(&g_wbq_mu); }
    for (int i = 0; i < g_nwbj; i++) if (g_wbj[i].e == e) g_wbj[i].e = NULL;
    if (e->shaped) {
        entry **p = &g_shape[shape_of(e->im.storage, &e->im.d)];
        while (*p && *p != e) p = &(*p)->shape_next;
        if (*p) *p = e->shape_next;
    }
    *e->all_pprev = e->all_next;
    if (e->all_next) e->all_next->all_pprev = e->all_pprev;
    for (int i = 0; i < e->nviews; i++) vkb_destroy_later(e->view[i], VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkb_destroy_later(VK_NULL_HANDLE, e->im.img, e->im.mem);
    if (e->twin) side_free(e->twin);
    if (e->blk) {
        for (int l = 0; l < BLK_LEVELS; l++) if (e->blk[l]) side_free(e->blk[l]);
        free(e->blk);
    }
    free(e);
}

// ---- without attachment feedback loops (VK_EXT_attachment_feedback_loop_layout + _dynamic_state: AMD's Windows drivers
// lack the second, 2026-10-29): a draw that samples one of its own targets reads a copy of it taken just before the
// draw. It read an empty texture before - the lighting and compositing passes of TOTK do this, and every discrete AMD
// card showed a black screen (RX 570 with the shader object layer: the 3D scene without anything composed over it).
// The copy is the target's twin: an image of the same format, size, levels and layers made once and kept, an entry of
// its own so that the draw's views of it are made as for any texture. Recorded outside a pass (the caller ends it).
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);   // below
static _Atomic uint64_t g_fb_copies;   // copies made (vkb_tex_feedback_copies)
uint64_t vkb_tex_feedback_copies(void) { return g_fb_copies; }
static int twin_make(entry *src) {
    entry *t = calloc(1, sizeof *t);
    if (!t) return 0;
    const vkb_image *s = &src->im;
    VkImageType type = s->depth > 1 ? VK_IMAGE_TYPE_3D : s->height == 1 && (s->d.target == NVN_TEXTURE_TARGET_1D ||
                       s->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    int ms = s->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || s->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;   // as create()
    if (type == VK_IMAGE_TYPE_2D && !ms && s->layers >= 6 && s->width == s->height) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = s->format,
        .extent = {s->width, s->height, s->depth}, .mipLevels = s->levels, .arrayLayers = s->layers,
        .samples = ms && s->d.samples > 1 ? (VkSampleCountFlagBits)s->d.samples : VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.vkCreateImage(vkc.device, &ci, NULL, &t->im.img) != VK_SUCCESS) { free(t); return 0; }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, t->im.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkb_mtag = MT_IMAGE;
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &t->im.mem) != VK_SUCCESS) {
        vk.vkDestroyImage(vkc.device, t->im.img, NULL);
        free(t);
        return 0;
    }
    vk.vkBindImageMemory(vkc.device, t->im.img, t->im.mem, 0);
    VkImage img = t->im.img;
    VkDeviceMemory mem = t->im.mem;
    t->im = *s;   // the source's description (format, size, NVN texture for its views) ...
    t->im.img = img; t->im.mem = mem; t->im.layout = VK_IMAGE_LAYOUT_UNDEFINED;   // ... with its own image
    t->im.usage = ci.usage; t->im.gpu_written = 1; t->im.storage = NULL; t->im.cpu_visible = 0; t->im.wb_pending = 0;
    t->im.refresh_from = NULL; t->im.uploading = 0;
    src->twin = t;
    return 1;
}
vkb_image *vkb_tex_feedback_copy(VkCommandBuffer cb, vkb_image *im) {
    entry *e = (entry *)im;
    static int said;
    vkb_lock(&g_mu, LK_IMAGES);
    entry *t = e->twin;
    if (t && (t->im.format != im->format || t->im.width != im->width || t->im.height != im->height ||
              t->im.depth != im->depth || t->im.levels != im->levels || t->im.layers != im->layers)) {
        side_free(t);
        e->twin = t = NULL;
    }
    if (!t && twin_make(e)) t = e->twin;
    pthread_mutex_unlock(&g_mu);
    if (!t) return im;
    if (!said++) VLOG("no attachment feedback loops: draws that sample their own target read a copy (texture %llu first)",
                      (unsigned long long)im->serial);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkb_image_use(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy r[16];
    uint32_t n = im->levels < 16 ? im->levels : 16;
    for (uint32_t l = 0; l < n; l++) {
        uint32_t w = im->width >> l ? im->width >> l : 1, h = im->height >> l ? im->height >> l : 1, d = im->depth >> l ? im->depth >> l : 1;
        r[l] = (VkImageCopy){.srcSubresource = {im->aspect, l, 0, im->depth > 1 ? 1 : im->layers},
                             .dstSubresource = {im->aspect, l, 0, im->depth > 1 ? 1 : im->layers}, .extent = {w, h, d}};
    }
    vkb_checkpoint("feedback copy", NULL, NULL);
    vk.vkCmdCopyImage(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, r);
    vkb_image_use(cb, &t->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    atomic_fetch_add(&g_fb_copies, 1);
    return &t->im;
}
// ---- GPU compression into BC formats (L21): TOTK draws the blocks of BC4 textures (its gloom noise volumes slice by
// slice, and a 32x32 texture the sky reads) through a view of an uncompressed format whose texel is one block (RGBA16UI
// for BC4's 8 bytes). The draws were left out and the textures stayed empty. A Vulkan driver attaches such a view only
// if it allows block-texel views as targets (none for slices of a 3D image), so the draws go to an uncompressed image of
// the level's size in blocks (one per level, kept, the level's layers or 3D slices as its layers), and the pass end
// copies it into the level: copies between a compressed and an uncompressed format of one block size are core Vulkan.
// A new one starts from the level's contents. TOTK does this at startup: a few KiB per pass. TOTK_VK_BLOCK_TARGETS=0: off.
typedef struct { entry *src, *blk; uint32_t level; } blk_pending;
static __thread blk_pending t_blk[8];   // this recording thread's pass: block targets to copy at its end
static __thread int t_nblk;
static entry *blk_make(const entry *src, uint32_t level, VkFormat f, int nvn_format) {   // g_mu held
    const vkb_image *s = &src->im;
    int bw = 1, bh = 1;
    nvn_fmt_bpb(s->d.format, &bw, &bh);
    uint32_t lw = s->width >> level ? s->width >> level : 1, lh = s->height >> level ? s->height >> level : 1;
    uint32_t w = (lw + (uint32_t)bw - 1) / (uint32_t)bw, h = (lh + (uint32_t)bh - 1) / (uint32_t)bh;
    uint32_t layers = s->depth > 1 ? (s->depth >> level ? s->depth >> level : 1) : s->layers;
    entry *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vk.vkCreateImage(vkc.device, &ci, NULL, &t->im.img) != VK_SUCCESS) { free(t); return NULL; }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, t->im.img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkb_mtag = MT_IMAGE;
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &t->im.mem) != VK_SUCCESS) {
        vk.vkDestroyImage(vkc.device, t->im.img, NULL);
        free(t);
        return NULL;
    }
    vk.vkBindImageMemory(vkc.device, t->im.img, t->im.mem, 0);
    VkImage img = t->im.img;
    VkDeviceMemory mem = t->im.mem;
    t->im = *s;   // the texture's description, as a single-level 2D (array) texture of the view format
    t->im.img = img; t->im.mem = mem; t->im.layout = VK_IMAGE_LAYOUT_UNDEFINED; t->im.format = f; t->im.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    t->im.width = w; t->im.height = h; t->im.depth = 1; t->im.layers = layers; t->im.levels = 1;
    t->im.astc = t->im.bc7 = t->im.compressed = 0; t->im.usage = ci.usage; t->im.gpu_written = 1; t->im.storage = NULL;
    t->im.cpu_visible = 0; t->im.wb_pending = 0; t->im.refresh_from = NULL; t->im.uploading = 0;
    t->im.d.format = nvn_format; t->im.d.width = w; t->im.d.height = h; t->im.d.depth = 1; t->im.d.levels = 1;
    t->im.d.target = layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D;
    return t;
}
// Between the texture's level and its block target (into_texture: target -> level). The extent is in the source's texels.
static void blk_copy(VkCommandBuffer cb, vkb_image *im, vkb_image *b, uint32_t level, int into_texture) {
    vkb_image_use(cb, into_texture ? b : im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkb_image_use(cb, into_texture ? im : b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int is3d = im->depth > 1;
    uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
    VkImageSubresourceLayers bs = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, b->layers}, ts = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, is3d ? 1 : b->layers};
    VkImageCopy r = {.srcSubresource = into_texture ? bs : ts, .dstSubresource = into_texture ? ts : bs,
                     .extent = {into_texture ? b->width : lw, into_texture ? b->height : lh, is3d ? b->layers : 1}};
    vkb_checkpoint("block target copy", NULL, NULL);
    vk.vkCmdCopyImage(cb, into_texture ? b->img : im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, into_texture ? im->img : b->img,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}
VkImageView vkb_tex_block_target(VkCommandBuffer cb, vkb_image *im, const view_t *v, uint32_t *w, uint32_t *h) {
    static int on = -1;
    if (on < 0) { const char *s = getenv("TOTK_VK_BLOCK_TARGETS"); on = !s || *s != '0'; }
    entry *e = (entry *)im;
    if (!on || !im->compressed || im->bc7 || !v || !v->set_format || t_nblk == 8) return VK_NULL_HANDLE;
    int bw = 1, bh = 1, vbw = 1, vbh = 1;
    int bpb = nvn_fmt_bpb(im->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    VkFormat f = vkb_format(v->format, NULL, NULL);
    uint32_t level = v->set_levels ? v->base_level : 0;
    if (!bpb || bpb != vbpb || vbw != 1 || vbh != 1 || f == VK_FORMAT_UNDEFINED || level >= im->levels || level >= BLK_LEVELS)
        return VK_NULL_HANDLE;
    vkb_lock(&g_mu, LK_IMAGES);
    if (!e->blk) e->blk = calloc(BLK_LEVELS, sizeof *e->blk);
    entry *t = e->blk ? e->blk[level] : NULL;
    if (t && t->im.format != f) { side_free(t); e->blk[level] = t = NULL; }
    int fresh = 0;
    if (!t && e->blk && (t = blk_make(e, level, f, v->format))) e->blk[level] = t, fresh = 1;
    pthread_mutex_unlock(&g_mu);
    if (!t) return VK_NULL_HANDLE;
    static int said;
    if (fresh && said++ < 16)
        VLOG("block target: texture %llu (format %#x %ux%ux%u level %u) drawn as format %#x %ux%u, %u layers",
             (unsigned long long)im->serial, im->d.format, im->width, im->height, im->depth > 1 ? im->depth : im->layers, level,
             v->format, t->im.width, t->im.height, t->im.layers);
    if (fresh) blk_copy(cb, im, &t->im, level, 0);
    view_t pv = *v;
    pv.set_levels = 0; pv.base_level = pv.nlevels = 0;
    VkImageView view = vkb_view(&t->im, &pv, 1, -1);
    if (!view) return VK_NULL_HANDLE;
    vkb_image_use(cb, &t->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    int k = 0;
    while (k < t_nblk && t_blk[k].blk != t) k++;
    if (k == t_nblk) t_blk[t_nblk++] = (blk_pending){e, t, level};
    *w = t->im.width;
    *h = t->im.height;
    return view;
}
void vkb_tex_block_flush(VkCommandBuffer cb) {
    for (int k = 0; k < t_nblk; k++) {
        blk_copy(cb, &t_blk[k].src->im, &t_blk[k].blk->im, t_blk[k].level, 1);
        vkb_image_written(&t_blk[k].src->im);
    }
    t_nblk = 0;
}
static void aside_purge(void) {   // g_mu held: entries aside too long (or too many) go
    uint64_t now = vkb_frame();
    while (g_naside && (g_naside >= RECYCLE_MAX || g_aside[0]->released + RECYCLE_FRAMES < now)) {
        entry *e = g_aside[0];
        memmove(g_aside, g_aside + 1, (size_t)--g_naside * sizeof g_aside[0]);
        entry_drop(e);
    }
}
void vkb_tex_image_stats(uint64_t *made, uint64_t *shared, uint64_t *taken_back) {
    *made = g_images_made; *shared = g_images_shared; *taken_back = g_images_taken_back;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int create(vkb_image *im, const tex_t *t) {
    const nvn_texdesc *d = &t->d;
    VkImageAspectFlags aspect;
    int astc;
    VkFormat f = vkb_format(d->format, &aspect, &astc);
    if (f == VK_FORMAT_UNDEFINED || d->target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    VkImageType type = d->target == NVN_TEXTURE_TARGET_3D ? VK_IMAGE_TYPE_3D
                     : d->target == NVN_TEXTURE_TARGET_1D || d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    uint32_t w = d->width ? d->width : 1, h = type == VK_IMAGE_TYPE_1D ? 1 : (d->height ? d->height : 1);
    uint32_t depth = type == VK_IMAGE_TYPE_3D ? (d->depth ? d->depth : 1) : 1, layers = type == VK_IMAGE_TYPE_3D ? 1 : (L.layers ? L.layers : 1);
    uint32_t levels = d->levels ? d->levels : 1;
    int ms = d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags feat = 0;
    { VkFormatProperties p; vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &p); feat = p.optimalTilingFeatures; }
    if (aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    } else if (!astc) {
        if (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    // a target may be sampled while bound (backend_vk.c pass_begin)
    if (vkc.has_feedback_loop && (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))
        usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;   // NVN views may reinterpret the format
    int compressed = !astc && f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (compressed) flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;   // read a block as RGBA16UI / RG32UI (TOTK's GPU BC encoder)
    if (type == VK_IMAGE_TYPE_2D && !ms && layers >= 6 && w == h) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f,
        .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
        .samples = ms && d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vk.vkCreateImage(vkc.device, &ci, NULL, &im->img);
    if (r != VK_SUCCESS) {
        static int said;
        if (said++ < 16) VLOG("texture %llu (format %#x target %d %ux%ux%u, %u levels, %u layers): vkCreateImage %d",
                              (unsigned long long)t->serial, d->format, d->target, w, h, depth, levels, layers, (int)r);
        return 0;
    }
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, im->img, &req);
    // [temporary] one allocation per texture; a suballocator before the open world (thousands of textures)
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    vkb_mtag = MT_IMAGE;
    if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &im->mem), "image memory")) {
        vk.vkDestroyImage(vkc.device, im->img, NULL);
        return 0;
    }
    vk.vkBindImageMemory(vkc.device, im->img, im->mem, 0);
    im->serial = t->serial; im->d = *d; im->format = f; im->aspect = aspect; im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->width = w; im->height = h; im->depth = depth; im->layers = layers; im->levels = levels; im->astc = astc;
    im->storage = t->mem;
    im->usage = usage;
    im->compressed = compressed;
    return 1;
}

// ---- one memory, several sizes: TOTK's dynamic resolution re-creates its render targets at another size over the same
// memory (1600x896 <-> 1432x808 on the Great Sky Island when the camera turns to the sky and back). On the console the
// new objects see what was rendered last through the old ones; here each size is an image of its own, and coming back
// to a size took back its image as it was frames earlier: depth from when the camera faced the sky, so the sea of
// clouds was drawn over the whole scene, and the lighting of the title menu read another shot's targets - the scene went
// dark for 4-6 frames (2026-10-24). As GPU emulators' texture caches do, an image that a texture object starts using
// takes the contents of the image last written at the same memory in the same format but another size, scaled
// (vkb_tex_refresh, before its next use outside a pass). Off (TOTK_VK_SIZE_REFRESH=1 to try): TOTK also reuses that memory
// for small transient targets of the same format (its environment probe renders 64x64 into the main depth's memory), so
// "the last written" was often such a transient and the full-size target got its stretched copy; the drop-outs stayed.
// The fix is to keep the size fixed (no dynamic resolution).
static _Atomic uint64_t g_write_seq;
static uint64_t g_refreshes;

// ---- TOTK_DEPTH_AUDIT (vkb.h) ----
// The depth images the GPU drew are recorded by value (game memory range, format, size, write_seq), so a colour texture
// over the same memory that is read after the depth was drawn shows: on the Switch it would see the depth's bytes; here
// GPU-drawn depth stays in its D32F image (no write-back of depth).
enum { DA_MAX = 256 };
typedef struct { const uint8_t *at; uint64_t size, seq, serial, frame; uint32_t format, w, h; } da_depth;
static da_depth g_da[DA_MAX];
static int g_nda;
static pthread_mutex_t g_da_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_da_reads, g_da_hits, g_da_bias[5];   // bias: D16, D24, D32F, D32F_S8, other / none
int vkb_depth_audit_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_DEPTH_AUDIT"); on = e && *e == '1'; }
    return on;
}
static void da_written(const vkb_image *im) {
    if (!im->storage || !(im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    nvn_texlayout L;
    nvn_tex_layout(&im->d, &L);
    pthread_mutex_lock(&g_da_mu);
    int i = 0;
    while (i < g_nda && g_da[i].serial != im->serial) i++;
    if (i == g_nda) {
        if (g_nda < DA_MAX) g_nda++;
        else { i = 0; for (int k = 1; k < g_nda; k++) if (g_da[k].seq < g_da[i].seq) i = k; }   // the oldest makes room
    }
    g_da[i] = (da_depth){im->storage, L.size, im->write_seq, im->serial, vkb_frame(), im->d.format, im->width, im->height};
    pthread_mutex_unlock(&g_da_mu);
}
void vkb_depth_audit_read(const vkb_image *im, const char *how) {
    if (!im || !im->storage || (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    atomic_fetch_add(&g_da_reads, 1);
    nvn_texlayout L;
    nvn_tex_layout(&im->d, &L);
    pthread_mutex_lock(&g_da_mu);
    for (int i = 0; i < g_nda; i++) {
        const da_depth *d = &g_da[i];
        if (d->seq <= im->write_seq || !(im->storage < d->at + d->size && d->at < im->storage + L.size)) continue;
        uint64_t n = atomic_fetch_add(&g_da_hits, 1) + 1;
        if (n <= 40 || !(n & (n - 1)))   // the first 40, then at powers of two
            VLOG("[depth audit] %s of colour texture %llu (format %#x %ux%u, %llu bytes at depth %+lld, %s %llu) over depth %llu "
                 "(format %#x %ux%u, %llu bytes, drawn at present %llu) - present %llu, %llu so far", how, (unsigned long long)im->serial,
                 im->d.format, im->width, im->height, (unsigned long long)L.size, (long long)(im->storage - d->at),
                 im->gpu_written ? "GPU-written at present" : "from game memory, upload", im->gpu_written ? (unsigned long long)im->write_frame
                 : (unsigned long long)im->upload_stamp, (unsigned long long)d->serial, d->format, d->w, d->h, (unsigned long long)d->size,
                 (unsigned long long)d->frame, (unsigned long long)vkb_frame(), (unsigned long long)n);
        break;
    }
    pthread_mutex_unlock(&g_da_mu);
}
void vkb_depth_audit_bias(const vkb_image *depth, float units, float factor) {
    if (units == 0 && factor == 0) return;
    uint32_t f = depth ? depth->d.format : 0;
    int k = f == 0x32 ? 0 : f == 0x33 || f == 0x35 ? 1 : f == 0x34 ? 2 : f == 0x36 ? 3 : 4;
    static const char *const name[5] = {"D16", "D24", "D32F", "D32F_S8", "other / none"};
    uint64_t n = atomic_fetch_add(&g_da_bias[k], 1) + 1;
    if (n <= 4 || !(n & (n - 1)))
        VLOG("[depth audit] depth bias units %g factor %g on a %s target (format %#x %ux%u) - present %llu, %llu such draws so far",
             units, factor, name[k], f, depth ? depth->width : 0, depth ? depth->height : 0, (unsigned long long)vkb_frame(),
             (unsigned long long)n);
}

void vkb_image_written(vkb_image *im) {
    im->gpu_written = 1;
    im->write_seq = atomic_fetch_add(&g_write_seq, 1) + 1;
    im->write_frame = vkb_frame();
    if (vkb_depth_audit_on()) da_written(im);
    if (im->cpu_visible && !__atomic_exchange_n(&im->wb_pending, 1, __ATOMIC_SEQ_CST)) {   // into the write-back queue
        pthread_mutex_lock(&g_wbq_mu);
        if (g_nwbq == g_capwbq && !(g_wbq = realloc(g_wbq, sizeof *g_wbq * (size_t)(g_capwbq = g_capwbq ? g_capwbq * 2 : 256)))) die("write-back queue");
        g_wbq[g_nwbq++] = (entry *)im;   // the image is the entry's first member
        pthread_mutex_unlock(&g_wbq_mu);
    }
}
static void refresh_mark(entry *e) {   // g_mu held: e is starting to be used by a new texture object
    entry *best = NULL;
    for (entry *o = g_all; o; o = o->all_next)
        if (o != e && o->im.storage == e->im.storage && o->im.format == e->im.format && o->im.gpu_written && o->im.aspect == e->im.aspect &&
            (o->im.width != e->im.width || o->im.height != e->im.height) && o->im.write_seq > e->im.write_seq &&
            (!best || o->im.write_seq > best->im.write_seq)) best = o;
    e->im.refresh_from = best ? &best->im : NULL;
}
int vkb_tex_refresh(VkCommandBuffer cb, vkb_image *im) {
    vkb_image *src = im->refresh_from;
    im->refresh_from = NULL;
    if (!src || vkb_in_pass()) return 0;
    vkb_lock(&g_mu, LK_IMAGES);
    int alive = 0;   // the source may have been destroyed since it was marked
    for (entry *o = g_all; o && !alive; o = o->all_next) alive = &o->im == src;
    pthread_mutex_unlock(&g_mu);
    if (!alive || src->write_seq <= im->write_seq) return 0;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatProperties fp;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, im->format, &fp);
    if ((fp.optimalTilingFeatures & need) != need) return 0;
    int linear = im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkb_image_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    uint32_t layers = src->layers < im->layers ? src->layers : im->layers;
    VkImageBlit r = {.srcSubresource = {im->aspect, 0, 0, layers}, .srcOffsets = {{0, 0, 0}, {(int32_t)src->width, (int32_t)src->height, 1}},
                     .dstSubresource = {im->aspect, 0, 0, layers}, .dstOffsets = {{0, 0, 0}, {(int32_t)im->width, (int32_t)im->height, 1}}};
    vkb_checkpoint("refresh from another size", NULL, NULL);
    vk.vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                      linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    im->gpu_written = 1;
    im->write_seq = src->write_seq;
    g_refreshes++;
    static int said;
    if (said++ < 8) VLOG("image %llu (%ux%u, format %#x) takes the contents of image %llu (%ux%u) at the same memory", (unsigned long long)im->serial,
                         im->width, im->height, im->d.format, (unsigned long long)src->serial, src->width, src->height);
    return 1;
}

// Recording threads look up the same serials draw after draw: a small cache per thread, valid for one submission (no
// image is freed while a submission is recorded: releases run on the translation thread between submissions).
typedef struct { uint64_t serial, sub; vkb_image *im; } icache_t;
static __thread icache_t *t_icache;
vkb_image *vkb_image_of(const tex_t *t) {
    if (!t || !t->serial || !vkb_ready()) return NULL;
    icache_t *c = NULL;
    uint64_t sub = mtl_last_fence() + 1;
    if (vkb_mt_recording()) {
        if (!t_icache && !(t_icache = calloc(1024, sizeof *t_icache))) die("image cache");
        c = &t_icache[(t->serial * 0x9E3779B97F4A7C15ull) >> 54];
        if (c->serial == t->serial && c->sub == sub) return c->im;
    }
    vkb_lock(&g_mu, LK_IMAGES);
    if ((g_n + g_tomb + 1) * 2 >= g_cap) grow();
    tslot *s = slot(t->serial);
    if (s->serial == t->serial && s->e) {
        vkb_image *found = &s->e->im;
        pthread_mutex_unlock(&g_mu);
        if (c) *c = (icache_t){t->serial, sub, found};
        return found;
    }
    entry *e = NULL;
    static int no_share = -1;   // TOTK_VK_NO_IMAGE_SHARE=1 (development A/B): an image per texture object, as before
    if (no_share < 0) { const char *ns = getenv("TOTK_VK_NO_IMAGE_SHARE"); no_share = ns && *ns == '1'; }
    if (t->mem && !no_share) for (e = g_shape[shape_of(t->mem, &t->d)]; e && !same_shape(e, t); e = e->shape_next) {}
    if ((!e || !e->refs) && t->mem) {   // a new or taken-back image over memory another live image uses: what differs (the game sees one texture there)
        static int said;
        static long long from = -1;   // TOTK_SHAPE_LOG=<present>: from that present on (development)
        if (from < 0) { const char *f = getenv("TOTK_SHAPE_LOG"); from = f ? atoll(f) : 0; }
        for (entry *o = g_all; o && from && vkb_frame() >= (uint64_t)from && said < 48; o = o->all_next) {
            if (o == e || o->im.storage != t->mem || !o->refs) continue;
            const nvn_texdesc *a = &o->im.d, *b = &t->d;
            said++;
            VLOG("[shape] texture %llu (%s) over the memory of image %llu (%p): target %u/%u format %#x/%#x flags %#x/%#x %ux%ux%u/%ux%ux%u "
                 "levels %u/%u samples %u/%u stride %u/%u block_h %u/%u swizzle %u%u%u%u/%u%u%u%u layout %d/%d%s",
                 (unsigned long long)t->serial, e ? "taken back" : "new", (unsigned long long)o->im.serial, t->mem, a->target, b->target, a->format, b->format,
                 a->flags, b->flags, a->width, a->height, a->depth, b->width, b->height, b->depth, a->levels, b->levels, a->samples, b->samples,
                 a->stride, b->stride, a->pkg_block_h, b->pkg_block_h, a->swizzle_r, a->swizzle_g, a->swizzle_b, a->swizzle_a,
                 b->swizzle_r, b->swizzle_g, b->swizzle_b, b->swizzle_a, o->has_layout, t->has_layout,
                 o->has_layout && t->has_layout && memcmp(o->layout, t->layout, sizeof o->layout) ? " (layouts differ)" : "");
        }
    }
    if (e && !e->refs) {   // taken back from the side: written by the CPU meanwhile -> its new contents upload
        int k = 0;
        while (k < g_naside && g_aside[k] != e) k++;
        if (k < g_naside) memmove(&g_aside[k], &g_aside[k + 1], (size_t)(--g_naside - k) * sizeof g_aside[0]);
        nvn_texlayout L;
        nvn_tex_layout(&e->im.d, &L);
        int wrote = nvn_dirty_tracked(e->im.storage, L.size) && nvn_dirty_since(e->im.storage, L.size, e->released_stamp);
        if (wrote) { e->im.gpu_written = 0; e->im.upload_stamp = 0; }
        if (vkb_rt_log_d(&e->im.d))
            VLOG("[rtlog] image %llu taken back by texture %llu: %s, present %llu", (unsigned long long)e->im.serial, (unsigned long long)t->serial,
                 wrote ? "game memory written meanwhile: uploaded from it again" : "contents kept", (unsigned long long)vkb_frame());
        g_images_taken_back++;
    } else if (e) g_images_shared++;
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e) die("image entry");
        uint64_t t0 = runtime_now_ns();
        int ok = create(&e->im, t);
        vkb_step_add(PK_CREATE, t0);
        if (!ok) { free(e); pthread_mutex_unlock(&g_mu); return NULL; }
        e->has_layout = t->has_layout;
        memcpy(e->layout, t->layout, sizeof e->layout);
        e->im.cpu_visible = t->mem && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS);
        if (vkb_rt_log_d(&t->d)) VLOG("[rtlog] new image %llu for texture %llu at %p, present %llu", (unsigned long long)e->im.serial,
                                      (unsigned long long)t->serial, (const void *)t->mem, (unsigned long long)vkb_frame());
        if (t->mem) { uint32_t h = shape_of(t->mem, &t->d); e->shape_next = g_shape[h]; g_shape[h] = e; e->shaped = 1; }
        e->all_next = g_all; e->all_pprev = &g_all;
        if (g_all) g_all->all_pprev = &e->all_next;
        g_all = e;
        g_images_made++;
    }
    if (t->mem && getenv("TOTK_VK_SIZE_REFRESH")) refresh_mark(e);   // [experiment, off] picks transient aliases (see above)
    e->refs++;
    if (s->serial == t->serial) g_tomb--; else s->serial = t->serial;
    s->e = e;
    g_n++;
    pthread_mutex_unlock(&g_mu);
    if (c) *c = (icache_t){t->serial, sub, &e->im};
    return &e->im;
}

// ---- layouts as a chunk records them (vkb_mt.c): an image the chunk has not used yet is taken to be in the layout it
// is first needed in (no barrier recorded); the command buffer between chunks moves it there from where the previous
// chunk left it. One thread recording (t_trk NULL): the image's own layout, a barrier at every change.
static __thread vkb_trk *t_trk;
void vkb_trk_set(vkb_trk *t) { t_trk = t; }
vkb_trk *vkb_trk_get(void) { return t_trk; }
void vkb_trk_reset(vkb_trk *t) {
    t->n = 0;
    if (t->tab) memset(t->tab, 0xff, sizeof *t->tab * (size_t)t->tcap);
}
static uint32_t trk_hash(const vkb_image *im, int cap) { return (uint32_t)(((uint64_t)(uintptr_t)im * 0x9E3779B97F4A7C15ull) >> 40) & (uint32_t)(cap - 1); }
static vkb_trk_e *trk_find(vkb_trk *t, const vkb_image *im) {
    if (!t->tcap) return NULL;
    for (uint32_t h = trk_hash(im, t->tcap);; h = (h + 1) & (uint32_t)(t->tcap - 1)) {
        int k = t->tab[h];
        if (k < 0) return NULL;
        if (t->e[k].im == im) return &t->e[k];
    }
}
static vkb_trk_e *trk_add(vkb_trk *t, vkb_image *im, VkImageLayout l) {
    if ((t->n + 1) * 2 > t->tcap) {   // the index stays at most half full
        int cap = t->tcap ? t->tcap * 2 : 256;
        int *tab = malloc(sizeof *tab * (size_t)cap);
        if (!tab) die("layout tracking");
        memset(tab, 0xff, sizeof *tab * (size_t)cap);
        for (int k = 0; k < t->n; k++) {
            uint32_t h = trk_hash(t->e[k].im, cap);
            while (tab[h] >= 0) h = (h + 1) & (uint32_t)(cap - 1);
            tab[h] = k;
        }
        free(t->tab); t->tab = tab; t->tcap = cap;
    }
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 128;
        if (!(t->e = realloc(t->e, sizeof *t->e * (size_t)t->cap))) die("layout tracking");
    }
    vkb_trk_e *e = &t->e[t->n];
    *e = (vkb_trk_e){im, l, l};
    uint32_t h = trk_hash(im, t->tcap);
    while (t->tab[h] >= 0) h = (h + 1) & (uint32_t)(t->tcap - 1);
    t->tab[h] = t->n++;
    return e;
}
int vkb_layout_is(vkb_image *im, VkImageLayout l) {
    if (!t_trk) return im->layout == l;
    vkb_trk_e *e = trk_find(t_trk, im);
    if (!e) { trk_add(t_trk, im, l); return 1; }
    return e->cur == l;
}
VkImageLayout vkb_layout(vkb_image *im) {
    if (!t_trk) return im->layout;
    vkb_trk_e *e = trk_find(t_trk, im);
    return e ? e->cur : im->layout;
}

void vkb_image_use(VkCommandBuffer cb, vkb_image *im, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    VkImageLayout old = im->layout;
    if (t_trk) {
        vkb_trk_e *e = trk_find(t_trk, im);
        if (!e) { trk_add(t_trk, im, layout); return; }   // its first use in the chunk: the barrier between chunks
        old = e->cur;
        e->cur = layout;
    } else im->layout = layout;
    // [temporary] every use waits for all earlier work on the image: precise per-pass barriers come with the pass tracker
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = stage, .dstAccessMask = access, .oldLayout = old,
        .newLayout = layout, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->img, .subresourceRange = {im->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &di);
}

static VkComponentSwizzle swz(uint8_t s, VkComponentSwizzle identity) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A;
    default: return identity;
    }
}

// The image component holding NVN's R, G, B, A of a 16-bit packed format (vkb_format); 0: as named.
static int packed_swizzle(int f, VkComponentSwizzle c[4]) {
    static const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B, A = VK_COMPONENT_SWIZZLE_A;
    if (f == 0x39) { c[0] = A; c[1] = B; c[2] = G; c[3] = R; return 1; }
    if (f >= 0x3A && f <= 0x3C) { c[0] = B; c[1] = G; c[2] = R; c[3] = A; return 1; }
    return 0;
}

// Formats without alpha read a = 1 (RGBX, RGB5, RGB DXT1, BGR5, BGRX): NVN_FORMAT values.
static int alpha_one(int f) {
    return f == 0x37 || f == 0x3A || f == 0x41 || f == 0x45 || (f >= 0x6D && f <= 0x78) || f == 0x96 || f == 0x99 || f == 0x9B;
}

VkImageView vkb_view(vkb_image *im, const view_t *v, int attachment, int view_type) {
    entry *e = (entry *)im;   // im is the first member
    uint8_t key[17] = {0};
    if (v) memcpy(key, v, 16);
    key[16] = (uint8_t)(attachment ? 0xFF : view_type + 1);
    // Recording threads look views up without a lock: entries are only appended (a full table takes no more), each
    // published by the count after it is written.
    int nv = __atomic_load_n(&e->nviews, __ATOMIC_ACQUIRE);
    for (int i = 0; i < nv; i++) if (!memcmp(e->view_key[i], key, sizeof key)) return e->view[i];
    uint32_t base = v && v->set_levels ? v->base_level : 0, nlev = v && v->set_levels && v->nlevels ? v->nlevels : im->levels - base;
    uint32_t layer = v && v->set_layers ? v->min_layer : 0, nlay = v && v->set_layers && v->nlayers ? v->nlayers : im->layers - layer;
    if (attachment) nlev = 1;
    if (attachment == 1 && !(im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return VK_NULL_HANDLE;
    if (attachment == 2 && !(im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    VkFormat f = im->format;
    if (v && v->set_format && !im->astc) {
        VkFormat vf = vkb_format(v->format, NULL, NULL);
        if (vf != VK_FORMAT_UNDEFINED) f = vf;
    }
    int reinterpret_block = im->compressed && !(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK);
    if (reinterpret_block) {   // [temporary] an uncompressed view of a compressed image: one level, one layer (and never a target)
        if (attachment) return VK_NULL_HANDLE;
        nlev = 1;
        nlay = 1;
    }
    if (attachment) {   // [temporary] a 16-bit packed target stores its components swapped (vkb_format): seen once if TOTK does it
        VkComponentSwizzle pk[4];
        static int said;
        if (packed_swizzle(v && v->set_format ? v->format : im->d.format, pk) && !said++)
            VLOG("[temporary] texture %llu (format %#x) as a render target or storage image: its components are stored swapped",
                 (unsigned long long)im->serial, v && v->set_format ? v->format : im->d.format);
    }
    if (attachment == 2) {   // storage views: the UNORM form of sRGB formats
        if (f == VK_FORMAT_R8G8B8A8_SRGB) f = VK_FORMAT_R8G8B8A8_UNORM;
        else if (f == VK_FORMAT_B8G8R8A8_SRGB) f = VK_FORMAT_B8G8R8A8_UNORM;
    }
    int target = v && v->set_target ? v->target : im->d.target;
    VkImageViewType type = im->depth > 1 ? VK_IMAGE_VIEW_TYPE_3D
        : target == NVN_TEXTURE_TARGET_1D ? VK_IMAGE_VIEW_TYPE_1D : target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
        : target == NVN_TEXTURE_TARGET_CUBEMAP && !attachment ? VK_IMAGE_VIEW_TYPE_CUBE
        : target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && !attachment ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
        : nlay > 1 || target == NVN_TEXTURE_TARGET_2D_ARRAY || target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
        : VK_IMAGE_VIEW_TYPE_2D;
    if (type == VK_IMAGE_VIEW_TYPE_3D && attachment) { type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; nlay = im->depth; }
    if (!attachment && view_type >= 0 && im->depth <= 1) {   // as the shader declares it (layer counts must fit the type)
        type = (VkImageViewType)view_type;
        if (((type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && nlay < 6) || type == VK_IMAGE_VIEW_TYPE_3D)
            vkb_audit(A_TEX_VIEW_ADJUSTED, NULL);
        if ((type == VK_IMAGE_VIEW_TYPE_CUBE && nlay < 6) || (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && nlay < 6))
            type = nlay > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlay = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nlay -= nlay % 6;
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) nlay = 1;
        if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // a 2D image declared 3D: closest valid view
    } else if (!attachment && view_type == VK_IMAGE_VIEW_TYPE_3D && im->depth > 1) type = VK_IMAGE_VIEW_TYPE_3D;
    if (type == VK_IMAGE_VIEW_TYPE_3D) { layer = 0; nlay = 1; }
    if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY && im->depth > 1) {   // slices of a 3D image: the depth of the view's level
        uint32_t dl = im->depth >> base ? im->depth >> base : 1;
        nlev = 1;
        if (layer + nlay > dl) { if (layer >= dl) layer = 0; nlay = dl - layer; }
    }
    if (im->depth <= 1 && layer + nlay > im->layers) { if (layer >= im->layers) layer = 0; nlay = im->layers - layer; }
    if (base + nlev > im->levels) { if (base >= im->levels) base = 0; nlev = im->levels - base; }
    // swizzle: the view's, else the texture's own (TextureBuilder SetSwizzle); formats without alpha read a = 1
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (!attachment) {
        if (v && v->set_swizzle)
            cm = (VkComponentMapping){swz(v->swz[0], VK_COMPONENT_SWIZZLE_R), swz(v->swz[1], VK_COMPONENT_SWIZZLE_G),
                                      swz(v->swz[2], VK_COMPONENT_SWIZZLE_B), swz(v->swz[3], VK_COMPONENT_SWIZZLE_A)};
        else if (im->d.swizzle_r | im->d.swizzle_g | im->d.swizzle_b | im->d.swizzle_a)
            cm = (VkComponentMapping){swz((uint8_t)im->d.swizzle_r, VK_COMPONENT_SWIZZLE_R), swz((uint8_t)im->d.swizzle_g, VK_COMPONENT_SWIZZLE_G),
                                      swz((uint8_t)im->d.swizzle_b, VK_COMPONENT_SWIZZLE_B), swz((uint8_t)im->d.swizzle_a, VK_COMPONENT_SWIZZLE_A)};
        VkComponentSwizzle pk[4];
        if (!im->astc && packed_swizzle(v && v->set_format ? v->format : im->d.format, pk)) {   // NVN's components back in place
            VkComponentSwizzle *s[4] = {&cm.r, &cm.g, &cm.b, &cm.a};
            for (int i = 0; i < 4; i++) {
                int k = *s[i] == VK_COMPONENT_SWIZZLE_IDENTITY ? i : *s[i] >= VK_COMPONENT_SWIZZLE_R ? (int)(*s[i] - VK_COMPONENT_SWIZZLE_R) : -1;
                if (k >= 0) *s[i] = pk[k];
            }
        }
        if (alpha_one(v && v->set_format ? v->format : im->d.format)) {
            if (cm.a == VK_COMPONENT_SWIZZLE_IDENTITY || cm.a == VK_COMPONENT_SWIZZLE_A) cm.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageAspectFlags aspect = im->aspect;
    if (!attachment && aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;   // sampled: depth
    // the view's own usage: an image made for storage too must not make every view storage (sRGB views cannot be)
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = attachment == 2 ? VK_IMAGE_USAGE_STORAGE_BIT
               : attachment == 1 ? im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
               : VK_IMAGE_USAGE_SAMPLED_BIT};
    // views used in the feedback loop layout: as attachment and sampled at once (a sampled view in another format
    // keeps plain sampling: the format may not be renderable)
    if ((im->usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) && (attachment == 1 || (!attachment && f == im->format)))
        vu.usage = im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &vu, .image = im->img, .viewType = type, .format = f,
                                .components = cm, .subresourceRange = {aspect, base, nlev, layer, nlay}};
    vkb_lock(&g_mu, LK_IMAGES);
    nv = e->nviews;
    for (int i = 0; i < nv; i++)
        if (!memcmp(e->view_key[i], key, sizeof key)) { pthread_mutex_unlock(&g_mu); return e->view[i]; }   // made meanwhile
    VkImageView view = VK_NULL_HANDLE;
    if (!vk_check(vk.vkCreateImageView(vkc.device, &ci, NULL, &view), "image view")) { pthread_mutex_unlock(&g_mu); return VK_NULL_HANDLE; }
    if (nv < VIEWS) {
        memcpy(e->view_key[nv], key, sizeof key);
        e->view[nv] = view;
        __atomic_store_n(&e->nviews, nv + 1, __ATOMIC_RELEASE);
    } else {   // [temporary] many distinct views of one texture: this one is not kept (destroyed when the GPU is done with it)
        static int said; if (said++ < 4) VLOG("texture %llu: more than %d views", (unsigned long long)im->serial, VIEWS);
        vkb_destroy_later(view, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    pthread_mutex_unlock(&g_mu);
    return view;
}

void vkb_tex_release(const tex_t *t) {
    if (!t || !t->serial || !vkb_ready()) return;
    vkb_lock(&g_mu, LK_IMAGES);
    tslot *s = g_cap ? slot(t->serial) : NULL;
    if (s && s->serial == t->serial && s->e) {
        entry *e = s->e;
        s->e = NULL; g_n--; g_tomb++;
        if (--e->refs == 0) {
            if (!e->shaped) entry_drop(e);   // no memory: nothing can take it back
            else {
                if (g_naside == RECYCLE_MAX) { entry *o = g_aside[0]; memmove(g_aside, g_aside + 1, (size_t)--g_naside * sizeof g_aside[0]); entry_drop(o); }
                e->released = vkb_frame();
                e->released_stamp = nvn_dirty_now();
                g_aside[g_naside++] = e;
            }
        }
    }
    aside_purge();
    pthread_mutex_unlock(&g_mu);
}

// ---------------- contents from game memory ----------------
// Every level and layer: block-linear (or linear) storage -> rows of blocks in upload memory (nvn_deswizzle) -> image.
// ASTC: the blocks are decoded to RGBA8 on the CPU first ([temporary], phase 5: BC7 at build time). Depth / stencil and
// 3-component formats are not uploaded yet (render targets; logged once).
// The CPU cannot write game memory while a submission is replayed (the replay runs on the submitting thread), so one
// check per texture per submission is enough; a texture found changed is uploaded before its next use (the pass ends
// for it). Without this, textures the CPU updates while drawn every frame (the font atlas: new glyphs, the save slot
// thumbnails) kept their first contents.
// Game memory without a write watch (Linux: imported into Vulkan, amdgpu re-maps it for writing after every
// protection change, so userfaultfd / soft-dirty report every page written all the time): CPU updates of a texture are
// found by comparing its contents with a hash taken at the upload: at most every other submission (about once a
// frame), within a byte budget per submission (TOTK_TEX_HASH_MB, default 16); textures over the budget wait for a later
// submission, and the ones just compared make room for them, so the cost stays bounded however many textures there are. The font atlas (new glyphs)
// and the save slot thumbnails need this.
enum { HASH_MAX = 16 << 20, HASH_EVERY = 2 };
static uint64_t tex_hash(const uint8_t *p, size_t n) {
    const uint64_t K = 0x9e3779b97f4a7c15ull;
    uint64_t a = n, b = K, c = ~(uint64_t)n, d = K ^ 0x5bd1e995ull, w[4];
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(w, p + i, 32);
        a = (a ^ w[0]) * K; b = (b ^ w[1]) * K; c = (c ^ w[2]) * K; d = (d ^ w[3]) * K;
        a ^= a >> 29; b ^= b >> 29; c ^= c >> 29; d ^= d >> 29;
    }
    for (; i < n; i++) a = (a ^ p[i]) * K;
    uint64_t h = (a ^ (b * 3) ^ (c * 5) ^ (d * 7)) * K;
    h ^= h >> 32;
    return h ? h : 1;
}
static uint64_t g_hash_budget;
static uint64_t g_hash_sub, g_hash_bytes;
static uint64_t g_hash_checks, g_hash_changed, g_hash_bytes_total, g_hash_ns;
void vkb_tex_hash_stats(uint64_t *checks, uint64_t *bytes, uint64_t *ns, uint64_t *changed) {
    *checks = __atomic_load_n(&g_hash_checks, __ATOMIC_RELAXED); *bytes = __atomic_load_n(&g_hash_bytes_total, __ATOMIC_RELAXED);
    *ns = __atomic_load_n(&g_hash_ns, __ATOMIC_RELAXED); *changed = g_hash_changed;
}
static int tex_content_changed(vkb_image *im, size_t size, uint64_t sub) {
    if (!im->content_hash) return 0;
    if (im->hash_sub == sub || (!im->hash_hot && im->hash_sub && sub - im->hash_sub < HASH_EVERY)) return 0;
    if (!g_hash_budget) {
        const char *e = getenv("TOTK_TEX_HASH_MB");
        g_hash_budget = (uint64_t)(e ? atoi(e) : 16) << 20;
        if (!g_hash_budget) g_hash_budget = 1;
    }
    uint64_t at = __atomic_load_n(&g_hash_sub, __ATOMIC_ACQUIRE);
    if (at != sub && __atomic_compare_exchange_n(&g_hash_sub, &at, sub, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_hash_bytes, 0, __ATOMIC_RELEASE);
    if (__atomic_add_fetch(&g_hash_bytes, size, __ATOMIC_ACQ_REL) > g_hash_budget && !im->hash_hot) return 0;   // next time
    im->hash_sub = sub;
    uint64_t t0 = runtime_now_ns(), h = tex_hash(im->storage, size);
    __atomic_add_fetch(&g_hash_ns, runtime_now_ns() - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_hash_bytes_total, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_hash_checks, 1, __ATOMIC_RELAXED);
    if (h == im->content_hash) return 0;
    im->hash_hot = 1;
    if (g_hash_changed++ < 16)
        VLOG("texture %llu (format %#x %ux%u): contents changed in game memory; uploading again", (unsigned long long)im->serial,
             im->d.format, im->width, im->height);
    return 1;
}

int vkb_tex_stale(vkb_image *im) {
    uint64_t sub = mtl_last_fence() + 1;
    if (!im->upload_stamp || im->gpu_written || !im->storage) return 0;
    if (im->cpu_written) return 1;
    if (im->vpool_gen && im->vpool_gen != totk_vpool_generation()) return 1;   // a virtual pool's mapping changed
    if (__atomic_load_n(&im->dirty_checked, __ATOMIC_ACQUIRE) != sub) {
        int mt = vkb_mt_recording();
        if (mt) {   // one thread checks; the others wait for its answer
            vkb_lock(&g_mu, LK_IMAGES);
            if (im->dirty_checked == sub) { pthread_mutex_unlock(&g_mu); return im->stale; }
        }
        nvn_texlayout L;
        nvn_tex_layout(&im->d, &L);
        // a virtual pool: the physical pages behind it; [temporary] untracked memory (CPU-no-access pools hold GPU data)
        // counts as unchanged: uploaded once
        int v = im->vpool_gen ? totk_vpool_dirty_since((uintptr_t)im->storage, L.size, im->upload_stamp) : -1;
        im->stale = v >= 0 ? v
                  : nvn_dirty_tracked(im->storage, L.size) ? nvn_dirty_since(im->storage, L.size, im->upload_stamp)
                  : tex_content_changed(im, L.size, sub);
        static long long log_serial = -1;   // TOTK_VK_STALE_LOG=<texture serial>: the check's answers (development)
        if (log_serial == -1) { const char *e = getenv("TOTK_VK_STALE_LOG"); log_serial = e ? atoll(e) : 0; }
        if (log_serial && (long long)im->serial == log_serial)
            VLOG("[stale] texture %llu submission %llu: tracked %d stale %d (stamp %llu now %llu)", (unsigned long long)im->serial,
                 (unsigned long long)sub, nvn_dirty_tracked(im->storage, L.size), im->stale, (unsigned long long)im->upload_stamp,
                 (unsigned long long)nvn_dirty_now());
        __atomic_store_n(&im->dirty_checked, sub, __ATOMIC_RELEASE);
        if (mt) pthread_mutex_unlock(&g_mu);
    }
    return im->stale;
}

// ASTC is decoded on the CPU (desktop GPUs have no ASTC); entering the world, decoding new textures on the recording
// thread held single submissions for 0.3-2.3 s (28% of that thread there, Great Sky Island, 2026-10-24). A texture's
// levels, layers and slices are gathered (astc_add) and decoded together (astc_run) by ASTC_WORKERS threads and the
// recording thread, in pieces of ASTC_CHUNK block rows: most of TOTK's textures and mip levels are small, so splitting
// texture by texture left the workers idle.
// ASTC -> BC7 when the port is built (vkb_bc7.c, tools/bc7_build.py) takes most decoding out of play.
enum { ASTC_WORKERS = 4, ASTC_CHUNK = 4 };   // block rows per piece taken
typedef struct { const uint8_t *src; uint8_t *dst; uint32_t w, h, bw, bh, first; int srgb; } astc_item;   // first: its first piece
static astc_item *g_ai;
static int g_nai, g_capai;
static uint32_t g_apieces;                  // pieces of all items
static _Atomic uint32_t g_anext;            // the next piece to take
static uint64_t g_aj_serial;
static int g_aj_open, g_aj_active;          // a batch is being worked on / workers in it
static void (*g_aj_fn)(void);               // the batch's work: pieces until none is left
static pthread_mutex_t g_aj_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aj_cv = PTHREAD_COND_INITIALIZER, g_aj_done_cv = PTHREAD_COND_INITIALIZER;
uint64_t vkb_astc_ns;                       // decoding time on the recording thread (long submission log)
static void astc_add(const uint8_t *src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, int srgb, uint8_t *dst) {
    if (g_nai == g_capai) {
        g_capai = g_capai ? g_capai * 2 : 64;
        if (!(g_ai = realloc(g_ai, sizeof *g_ai * (size_t)g_capai))) die("ASTC items");
    }
    g_ai[g_nai++] = (astc_item){src, dst, w, h, bw, bh, g_apieces, srgb};
    g_apieces += ((h + bh - 1) / bh + ASTC_CHUNK - 1) / ASTC_CHUNK;
}
static void astc_work(void) {   // pieces until none is left
    int k = 0;
    for (;;) {
        uint32_t p = atomic_fetch_add(&g_anext, 1);
        if (p >= g_apieces) return;
        while (k + 1 < g_nai && g_ai[k + 1].first <= p) k++;   // pieces are taken in order: the item only moves forward
        while (k > 0 && g_ai[k].first > p) k--;
        const astc_item *it = &g_ai[k];
        uint32_t r = (p - it->first) * ASTC_CHUNK;
        astc_decode_ldr_rows(it->src, it->w, it->h, it->bw, it->bh, it->srgb, it->dst, r, r + ASTC_CHUNK);
    }
}
static void *astc_worker(void *unused) {
    (void)unused;
    pthread_setname_np("totk-astc");
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_aj_mu);
        while (g_aj_serial == seen) pthread_cond_wait(&g_aj_cv, &g_aj_mu);
        seen = g_aj_serial;
        int open = g_aj_open;
        void (*fn)(void) = g_aj_fn;
        if (open) g_aj_active++;
        pthread_mutex_unlock(&g_aj_mu);
        if (!open) continue;
        fn();
        pthread_mutex_lock(&g_aj_mu);
        g_aj_active--;
        pthread_cond_broadcast(&g_aj_done_cv);
        pthread_mutex_unlock(&g_aj_mu);
    }
    return NULL;
}
static void astc_start(void) {
    for (int i = 0; i < ASTC_WORKERS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, astc_worker, NULL) == 0) pthread_detach(t);
    }
}
// fn on the workers and the calling thread until it has no piece left; returns when every worker that took part is
// done (one batch at a time: uploads hold g_up_mu or run on the one recording thread)
static void pool_run(void (*fn)(void), int wide) {
    if (wide) {
        static pthread_once_t once = PTHREAD_ONCE_INIT;
        pthread_once(&once, astc_start);
        pthread_mutex_lock(&g_aj_mu);
        g_aj_fn = fn;
        g_aj_open = 1;
        g_aj_serial++;
        pthread_cond_broadcast(&g_aj_cv);
        pthread_mutex_unlock(&g_aj_mu);
    }
    fn();
    if (wide) {
        pthread_mutex_lock(&g_aj_mu);
        g_aj_open = 0;   // a worker that wakes late takes nothing
        while (g_aj_active) pthread_cond_wait(&g_aj_done_cv, &g_aj_mu);
        pthread_mutex_unlock(&g_aj_mu);
    }
}
static void astc_run(void) {   // the gathered items, decoded; then the batch is empty
    if (!g_nai) return;
    uint64_t t0 = runtime_now_ns();
    atomic_store(&g_anext, 0);
    pool_run(astc_work, g_apieces > 2);
    g_nai = 0; g_apieces = 0;
    vkb_astc_ns += runtime_now_ns() - t0;
}

static int tex_upload(VkCommandBuffer cb, vkb_image *im);
// Several threads recording: into the submission's prologue, which runs before every chunk (the CPU wrote the bytes
// before the submission, so earlier is as good; a GPU write to the image in a chunk still comes after it).
static pthread_mutex_t g_up_mu = PTHREAD_MUTEX_INITIALIZER;   // one upload at a time: the ASTC batch is shared
int vkb_tex_upload(VkCommandBuffer cb, vkb_image *im) {
    uint64_t stamp = __atomic_load_n(&im->upload_stamp, __ATOMIC_ACQUIRE);
    if (vkb_mt_recording() && __atomic_load_n(&im->uploading, __ATOMIC_ACQUIRE)) {   // another thread is uploading it: its
        pthread_mutex_lock(&g_up_mu);                                                 // image may change (BC7), wait
        pthread_mutex_unlock(&g_up_mu);
        stamp = __atomic_load_n(&im->upload_stamp, __ATOMIC_ACQUIRE);
    }
    if (!im->storage || im->gpu_written || (stamp && !vkb_tex_stale(im))) return 0;   // the common case: nothing to do
    uint64_t t0 = runtime_now_ns();
    int r;
    if (vkb_mt_recording()) {
        pthread_mutex_lock(&g_up_mu);
        vkb_pro_begin();
        r = tex_upload(vkb_cmd(), im);   // checks again: another thread may have uploaded it meanwhile
        vkb_pro_end();
        pthread_mutex_unlock(&g_up_mu);
    } else r = tex_upload(cb, im);
    vkb_step_add(PK_UPLOAD, t0);
    return r;
}
// ---- ASTC as BC7 (vkb_bc7.c). At its first upload an ASTC texture becomes a BC7 image when the cache holds every one of
// its surfaces (no view of it exists yet, nothing has drawn with it); its contents then come from the cache. Otherwise
// it stays RGBA8, decoded as before, and each surface is queued for encoding: the next time it is in the cache.
// [temporary] 1D and 3D ASTC textures stay RGBA8.
static int to_bc7(vkb_image *im, int srgb, int fmt) {
    entry *e = (entry *)im;
    if (__atomic_load_n(&e->nviews, __ATOMIC_ACQUIRE) || im->gpu_written || im->depth > 1 || im->d.target == NVN_TEXTURE_TARGET_1D ||
        im->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) return 0;
    VkFormat f = fmt == VKB_BC_1 ? (srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK)
                                 : (srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK);
    VkFormatProperties fp;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return 0;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = im->layers >= 6 && im->width == im->height ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {im->width, im->height, 1}, .mipLevels = im->levels,
        .arrayLayers = im->layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vk.vkCreateImage(vkc.device, &ci, NULL, &img) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(vkc.device, img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem;
    vkb_mtag = MT_IMAGE;
    if (ai.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &mem) != VK_SUCCESS) { vk.vkDestroyImage(vkc.device, img, NULL); return 0; }
    vk.vkBindImageMemory(vkc.device, img, mem, 0);
    vkb_destroy_later(VK_NULL_HANDLE, im->img, im->mem);   // the RGBA8 image no command has used
    im->img = img; im->mem = mem; im->format = f; im->usage = usage; im->compressed = 1; im->bc7 = fmt == VKB_BC_1 ? 2 : 1;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}
typedef struct {
    const uint8_t *rows; uint8_t *at, *rgba, *blocks; vkb_bc7_key k; uint32_t level, layer, lw, lh, ld; int have, fmt;
    VkDeviceSize slice, bytes, off; VkBuffer buf; size_t n;
} surf_t;
static uint64_t g_bc7_images;   // images made BC7 (the log line)
// The surfaces of an ASTC texture are prepared on the decoding workers too: a big array texture has hundreds to over a
// thousand (121 layers of 11 levels), each laid out, hashed for the key and looked up - 16 -> 10 ms for a 40.6 MiB
// array (2026-10-29). Same buffers as before: no memory of its own. The copies of the cached blocks stay on this thread:
// on the workers they took longer (20-73 ms against 33). Most of what is left of such an upload is fresh upload memory:
// the ring grows by a chunk of its size, which the driver clears (up to 77 ms) or the copy faults in, every time - the
// chunks past RING_BASE go back after ~4 s (plan.md).
static struct { const nvn_texdesc *d; const uint8_t *src; surf_t *sf; uint32_t ns; int bw, bh, srgb; } g_sj;
static _Atomic uint32_t g_snext;
static void surf_work(void) {
    for (;;) {
        uint32_t i = atomic_fetch_add(&g_snext, 1);
        if (i >= g_sj.ns) return;
        surf_t *s = &g_sj.sf[i];
        uint32_t pitch = (s->lw + (uint32_t)g_sj.bw - 1) / (uint32_t)g_sj.bw * 16;
        if (nvn_deswizzle(g_sj.d, s->level, s->layer, g_sj.src, s->at, pitch)) continue;
        s->rows = s->at;
        s->k = vkb_bc7_key_of(s->rows, (size_t)s->bytes, s->lw, s->lh, s->ld, (uint32_t)g_sj.bw, (uint32_t)g_sj.bh, g_sj.srgb);
        size_t len;
        s->have = vkb_bc7_lookup(s->k, &s->fmt, &len) &&
                  len == (s->fmt == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld));
    }
}
static void surf_run(void) {
    atomic_store(&g_snext, 0);
    pool_run(surf_work, g_sj.ns >= 8);
}
// The ASTC blocks in rows are for the CPU only (the cache key, a miss decoded): laid out in ordinary memory. They were
// put in upload memory and read back from it for the key - memory the CPU reads very slowly (uncached, often on the GPU):
// 25-48 ms for a 240x384 array texture of 60 layers and 9 levels, the hitches in the Depths (2026-10-29).
static void astc_upload(VkCommandBuffer cb, vkb_image *im, const uint8_t *src, int bw, int bh, int srgb) {
    const nvn_texdesc *d = &im->d;
    int ns = (int)(im->levels * im->layers), k = 0, all = 1, fmt = -1;
    surf_t *sf = calloc((size_t)(ns ? ns : 1), sizeof *sf);
    if (!sf) die("ASTC surfaces");
    size_t total = 0;
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        total += (size_t)((lw + bw - 1) / bw) * 16 * ((lh + bh - 1) / bh) * ld * im->layers;
    }
    uint8_t *arena = malloc(total ? total : 1), *at = arena;
    if (!arena) die("ASTC blocks");
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        uint32_t bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh, pitch = bx * 16;
        VkDeviceSize slice = (VkDeviceSize)pitch * by, bytes = slice * ld;
        for (uint32_t layer = 0; layer < im->layers; layer++, k++) {
            sf[k] = (surf_t){.at = at, .level = level, .layer = layer, .lw = lw, .lh = lh, .ld = ld, .slice = slice, .bytes = bytes};
            at += bytes;
        }
    }
    g_sj.d = d; g_sj.src = src; g_sj.sf = sf; g_sj.ns = (uint32_t)ns; g_sj.bw = bw; g_sj.bh = bh; g_sj.srgb = srgb;
    surf_run();
    for (int i = 0; i < ns; i++) {
        surf_t *s = &sf[i];
        all &= s->rows && s->have && (fmt < 0 || s->fmt == fmt);   // one format for the whole image
        if (s->rows && s->have && fmt < 0) fmt = s->fmt;
    }
    if (!im->bc7 && all && ns && to_bc7(im, srgb, fmt)) {
        g_bc7_images++;
        static int said;   // TOTK_BC7_LOG=1: every image made BC7 (development)
        if (getenv("TOTK_BC7_LOG") && said++ < 400)
            VLOG("[bc7] texture %llu format %#x %ux%u target %d levels %u layers %u%s", (unsigned long long)im->serial, d->format, im->width,
                 im->height, d->target, im->levels, im->layers, srgb ? " sRGB" : "");
    }
    if (!im->bc7 && getenv("TOTK_BC7_LOG")) {   // not in the cache (development: where the rest of the ASTC comes from)
        int have = 0;
        for (int i = 0; i < ns; i++) have += sf[i].have;
        static int said;
        if (said++ < 400)
            VLOG("[bc7] texture %llu format %#x %ux%u target %d levels %u layers %u: %d of %d surfaces in the cache, storage %p",
                 (unsigned long long)im->serial, d->format, im->width, im->height, d->target, im->levels, im->layers, have, ns, (const void *)im->storage);
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (im->bc7) {   // blocks from the cache (a surface missing from it: encoded now)
        int bcf = im->bc7 == 2 ? VKB_BC_1 : VKB_BC_7;
        for (int i = 0; i < ns; i++) {
            surf_t *s = &sf[i];
            if (!s->rows) continue;
            s->n = bcf == VKB_BC_1 ? bc1_size(s->lw, s->lh, s->ld) : bc7_size(s->lw, s->lh, s->ld);
            s->blocks = vkb_ring_alloc(s->n, 16, &s->buf, &s->off);
            if (!vkb_bc7_read(s->k, s->blocks, s->n)) {
                VkBuffer rb;
                VkDeviceSize ro;
                uint8_t *rgba = vkb_ring_alloc((VkDeviceSize)s->lw * s->lh * 4 * s->ld, 16, &rb, &ro);
                for (uint32_t z = 0; z < s->ld; z++)
                    astc_add(s->rows + s->slice * z, s->lw, s->lh, (uint32_t)bw, (uint32_t)bh, srgb, rgba + (VkDeviceSize)s->lw * s->lh * 4 * z);
                astc_run();
                vkb_bc7_encode_now(s->k, rgba, s->lw, s->lh, s->ld, srgb, bcf, s->blocks);
            }
            VkBufferImageCopy r = {.bufferOffset = s->off, .bufferRowLength = (s->lw + 3) / 4 * 4, .bufferImageHeight = (s->lh + 3) / 4 * 4,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1}, .imageExtent = {s->lw, s->lh, s->ld}};
            vkb_checkpoint("upload texture (BC7)", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, s->buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
    } else {   // decoded to RGBA8 (all surfaces together), then queued for encoding
        for (int i = 0; i < ns; i++) {
            surf_t *s = &sf[i];
            if (!s->rows) continue;
            VkBuffer buf;
            VkDeviceSize off;
            s->rgba = vkb_ring_alloc((VkDeviceSize)s->lw * s->lh * 4 * s->ld, 16, &buf, &off);
            for (uint32_t z = 0; z < s->ld; z++)
                astc_add(s->rows + s->slice * z, s->lw, s->lh, (uint32_t)bw, (uint32_t)bh, srgb, s->rgba + (VkDeviceSize)s->lw * s->lh * 4 * z);
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = s->lw, .bufferImageHeight = s->lh,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s->level, s->layer, 1}, .imageExtent = {s->lw, s->lh, s->ld}};
            vkb_checkpoint("upload texture", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
        astc_run();
        for (int i = 0; i < ns; i++)
            if (sf[i].rgba && !sf[i].have) vkb_bc7_queue(sf[i].k, sf[i].rgba, sf[i].lw, sf[i].lh, sf[i].ld, srgb);
    }
    free(sf);
    free(arena);
}
uint64_t vkb_bc7_images(void) { return g_bc7_images; }

// Uploads over 20 ms (the first 300): which texture and why it was uploaded again. On the Steam Deck the same ~1 s of
// uploads came back every few submissions (2026-10-29); on the PC there are a few, while loading.
static void slow_upload_log(const vkb_image *im, const char *why, uint64_t t0, uint64_t size, int vpool) {
    uint64_t dt = runtime_now_ns() - t0;
    static _Atomic int said;
    if (dt < 20000000 || atomic_fetch_add(&said, 1) >= 300) return;
    VLOG("slow upload: texture %llu format %#x %ux%ux%u, %u levels, %u layers, %.1f MiB%s%s: %.1f ms (%s)", (unsigned long long)im->serial,
         im->d.format, im->width, im->height, im->depth, im->levels, im->layers, size / 1048576.0, vpool ? ", in a virtual pool" : "",
         im->astc ? ", ASTC" : "", dt / 1e6, why);
}
static int tex_upload(VkCommandBuffer cb, vkb_image *im) {
    if (!im->storage || im->gpu_written) return 0;
    const nvn_texdesc *d = &im->d;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    if (im->upload_stamp && !vkb_tex_stale(im)) return 0;   // unchanged since the upload (checked once per submission)
    // why (the slow-upload log line below): first time, a CPU texel write, a virtual pool remapped, its pages written
    const char *why = !im->upload_stamp ? "first" : im->cpu_written ? "CPU texel write"
                    : im->vpool_gen && im->vpool_gen != totk_vpool_generation() ? "virtual pool remapped" : "pages written";
    uint64_t up_t0 = runtime_now_ns();
    im->stale = 0;
    im->cpu_written = 0;
    if (vkb_rt_log_d(d)) VLOG("[rtlog] image %llu uploaded from game memory %p, present %llu", (unsigned long long)im->serial, (const void *)im->storage,
                              (unsigned long long)vkb_frame());
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    // Depth textures the game fills from the CPU (D16 / D32F, stored as D32_SFLOAT here): converted to floats on the way.
    // TOTK sets one up at every camera cut of the title menu (192x192 D16, read by the chain that lights its environment
    // probes); left empty, the probe and so the scene went dark for 4-6 frames (2026-10-24). [temporary] D24 (0x33) and
    // the depth-stencil formats: the packing of their words is not settled yet.
    int depth_up = im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT && (d->format == 0x32 || d->format == 0x34) && bw == 1 && bh == 1;
    if ((im->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !depth_up) || !bpb || (d->format >= 0x19 && d->format <= 0x24)) {
        im->not_uploadable = 1;
        static int said; if (said++ < 4) VLOG("[temporary] texture %llu format %#x: contents not uploaded", (unsigned long long)im->serial, d->format);
        im->upload_stamp = nvn_dirty_now();
        return 0;
    }
    if (!im->upload_stamp && dm_stats()) {   // first upload: does a GPU-written texture use the same memory? (aliasing, development log)
        static int said;
        vkb_lock(&g_mu, LK_IMAGES);
        for (entry *e = g_all; e && said < 32; e = e->all_next) {
            if (&e->im == im || !e->im.gpu_written || !e->im.storage) continue;
            nvn_texlayout L2;
            nvn_tex_layout(&e->im.d, &L2);
            if (e->im.storage < im->storage + L.size && im->storage < e->im.storage + L2.size) {
                said++;
                VLOG("[alias] texture %llu (format %#x %ux%u) is read from memory that GPU-written texture %llu (format %#x %ux%u) uses",
                     (unsigned long long)im->serial, d->format, im->width, im->height, (unsigned long long)e->im.serial,
                     e->im.d.format, e->im.width, e->im.height);
            }
        }
        pthread_mutex_unlock(&g_mu);
    }
    __atomic_store_n(&im->uploading, 1, __ATOMIC_SEQ_CST);   // before the stamp: a thread that sees the stamp waits (vkb_tex_upload)
    // The writes so far are collected first: the write watch is read lazily, and the game's writes that filled the
    // texture before its first upload, seen only at the next look, came after the stamp - the same texture uploaded
    // again at once ("first", then "pages written": 37 of 119 slow uploads in a session, 2026-10-29).
    if (!im->vpool_gen) nvn_dirty_protect(im->storage, L.size);
    __atomic_store_n(&im->upload_stamp, nvn_dirty_now(), __ATOMIC_RELEASE);   // before reading: a write during the upload shows as dirty next time
    // the same for the contents hash (untracked memory only; a virtual pool's addresses are not readable as such)
    im->content_hash = !im->vpool_gen && L.size <= HASH_MAX && !nvn_dirty_tracked(im->storage, L.size) ? tex_hash(im->storage, L.size) : 0;
    im->hash_sub = 0;
    // A texture in a virtual pool (TOTK streams world textures this way): its addresses are only reserved, the bytes
    // are in physical pools' pages mapped through the page table — gathered into a copy first. It uploads again when
    // the pages behind it are written or the mapping changes ([temporary]: any MapVirtual; Vulkan sparse binding later).
    const uint8_t *src = im->storage;
    uint8_t *gathered = NULL;
    uint64_t gen = totk_vpool_generation();
    if (totk_vpool_gather((uintptr_t)im->storage, 0, NULL)) {   // (length 0: only whether it is in a virtual pool)
        gathered = malloc(L.size ? L.size : 1);
        if (!gathered) die("texture gather");
        totk_vpool_gather((uintptr_t)im->storage, L.size, gathered);
        src = gathered;
        im->vpool_gen = gen;
    }
    int srgb = d->format >= 0x87 && d->format <= 0x94;
    if (im->astc && vkb_bc7_on() && bpb == 16) {
        astc_upload(cb, im, src, bw, bh, srgb);
        slow_upload_log(im, why, up_t0, L.size, gathered != NULL);
        free(gathered);
        __atomic_store_n(&im->uploading, 0, __ATOMIC_RELEASE);
        return 1;
    }
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t ld = im->depth >> level ? im->depth >> level : 1;
        uint32_t bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh, pitch = bx * (uint32_t)bpb;
        VkDeviceSize slice = (VkDeviceSize)pitch * by, bytes = slice * ld;
        for (uint32_t layer = 0; layer < im->layers; layer++) {
            VkBuffer buf;
            VkDeviceSize off;
            // D16 is converted on the CPU: its rows in ordinary memory (read back from upload memory they cost 30-100 ms
            // a depth texture, 2026-10-29); everything else goes to the GPU as laid out.
            static __thread uint8_t *d16;
            static __thread size_t d16_cap;
            int convert = depth_up && bpb == 2;
            if (convert && bytes > d16_cap) {
                free(d16);
                d16_cap = (size_t)bytes;
                if (!(d16 = malloc(d16_cap))) die("D16 texels");
            }
            uint8_t *rows = convert ? d16 : vkb_ring_alloc(bytes, 16, &buf, &off);
            if (nvn_deswizzle(d, level, layer, src, rows, pitch)) continue;
            VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
                                   .imageSubresource = {depth_up ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1},
                                   .imageExtent = {lw, lh, ld}};
            if (depth_up && bpb == 2) {   // D16 -> 32-bit float depth
                float *f = vkb_ring_alloc((VkDeviceSize)lw * lh * ld * 4, 16, &buf, &off);
                for (uint64_t k = 0; k < (uint64_t)lw * lh * ld; k++) { uint16_t v; memcpy(&v, rows + 2 * k, 2); f[k] = v / 65535.0f; }
                r.bufferOffset = off;
            }
            if (im->astc) {   // decoded texels, slice by slice
                uint8_t *rgba = vkb_ring_alloc((VkDeviceSize)lw * lh * 4 * ld, 16, &buf, &off);
                for (uint32_t z = 0; z < ld; z++)
                    astc_add(rows + slice * z, lw, lh, (uint32_t)bw, (uint32_t)bh, srgb, rgba + (VkDeviceSize)lw * lh * 4 * z);
                r.bufferOffset = off; r.bufferRowLength = lw; r.bufferImageHeight = lh;
            }
            vkb_checkpoint("upload texture", NULL, NULL);
            vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        }
    }
    astc_run();   // the copies recorded above read these texels when the submission runs
    slow_upload_log(im, why, up_t0, L.size, gathered != NULL);
    free(gathered);
    __atomic_store_n(&im->uploading, 0, __ATOMIC_RELEASE);
    return 1;
}

// ---------------- CPU reads of texels (NVN ReadTexels) ----------------
// TOTK reads GPU results on the CPU this way; the null backend's zeros made the title screen several times too bright.
// GPU-written images: the region is copied to upload memory and read after the GPU is done. Others: the game memory is
// the truth, de-laid-out as for an upload. Tightly packed rows unless the caller gives strides (block rows for BC).
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!t || !r || !dst || r->width <= 0 || r->height <= 0) return;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t bx = (uint32_t)(r->width + bw - 1) / bw, by = (uint32_t)(r->height + bh - 1) / bh, nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t wb = (size_t)bx * (size_t)bpb;
    if (!row) row = wb;
    if (!img) img = row * by;
    vkb_image *im = vkb_image_of(t);
    if (vkb_depth_audit_on()) {
        if (im && (im->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
            VLOG("[depth audit] ReadTexels of depth texture %llu (format %#x %ux%u, %s): depth is not written back to game memory",
                 (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, im->gpu_written ? "GPU-written" : "from game memory");
        else vkb_depth_audit_read(im, "ReadTexels");
    }
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    static int calls;
    if (calls++ < 8) VLOG("ReadTexels: texture %llu format %#x level %u region %dx%dx%d, %s", (unsigned long long)t->serial, t->d.format,
                          level, r->width, r->height, r->depth, im && im->gpu_written ? "GPU-written" : "game memory");
    if (im && im->gpu_written && !im->astc && bpb && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !(t->d.format >= 0x19 && t->d.format <= 0x24)) {
        mtl_sub_begin();
        VkCommandBuffer cb = vkb_cmd();
        VkBuffer b;
        VkDeviceSize off;
        uint8_t *px = vkb_ring_alloc((VkDeviceSize)wb * by * nz, 16, &b, &off);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy c = {.bufferOffset = off, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, is3d ? 0 : layer0 + (uint32_t)r->zoffset, is3d ? 1 : nz},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, is3d ? nz : 1}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
        mtl_wait(mtl_sub_commit(), UINT64_MAX);
        for (uint32_t z = 0; z < nz; z++)
            for (uint32_t y = 0; y < by; y++) memcpy(dst + z * img + y * row, px + ((size_t)z * by + y) * wb, wb);
        return;
    }
    // from game memory: the whole level / layer, then the region
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    uint32_t ld = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    uint32_t lbx = (lw + bw - 1) / bw, lby = (lh + bh - 1) / bh, pitch = lbx * (uint32_t)bpb;
    uint8_t *lin = bpb && t->mem ? malloc((size_t)pitch * lby * ld) : NULL;
    for (uint32_t z = 0; z < nz; z++) {
        uint32_t layer = is3d ? 0 : layer0 + (uint32_t)r->zoffset + z, slice = is3d ? (uint32_t)r->zoffset + z : 0;
        int ok = lin && (is3d ? z == 0 : 1) ? !nvn_deswizzle(&t->d, level, layer, t->mem, lin, pitch) : lin != NULL;
        for (uint32_t y = 0; y < by; y++) {
            uint8_t *o = dst + z * img + y * row;
            uint32_t sy = (uint32_t)r->yoffset / bh + y, sx = (uint32_t)r->xoffset / bw;
            if (ok && sy < lby && slice < ld && sx + bx <= lbx) memcpy(o, lin + ((size_t)slice * lby + sy) * pitch + (size_t)sx * bpb, wb);
            else memset(o, 0, wb);
        }
    }
    free(lin);
    static int said;
    if (!bpb && said++ < 4) VLOG("[temporary] ReadTexels: texture %llu format %#x not read", (unsigned long long)t->serial, t->d.format);
}

// ---------------- CPU writes of texels (NVN WriteTexels) ----------------
// The region goes into the game memory in the texture's own layout (the truth for textures the CPU fills: the next
// upload sees the pages dirty); an image the GPU has written gets the region copied in directly as well. TOTK fills
// its light falloff table this way: with the null backend's no-op the lights read garbage and the title screen was
// several times too bright (2026-10-23).
// Two parts (TOTK, 2026-10-28): the game memory at once on the game's thread (mtl_write_texels_mem), the image on the
// translation thread after the submissions queued before the write (mtl_write_texels_image) - the game thread waited for
// the whole queue before (~6 s in a 17-minute session).
int mtl_write_texels_mem(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (!t || !r || !src || r->width <= 0 || r->height <= 0 || !t->mem) return 0;
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { static int said; if (said++ < 4) VLOG("[temporary] WriteTexels: texture %llu format %#x not written", (unsigned long long)t->serial, t->d.format); return 0; }
    uint32_t bx = (uint32_t)(r->width + bw - 1) / bw, by = (uint32_t)(r->height + bh - 1) / bh, nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t wb = (size_t)bx * (size_t)bpb;
    if (!row) row = wb;
    if (!img) img = row * by;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1;
    uint32_t ld = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    uint32_t lbx = (lw + bw - 1) / bw, lby = (lh + bh - 1) / bh, pitch = lbx * (uint32_t)bpb;
    uint32_t sx = (uint32_t)r->xoffset / bw, sy = (uint32_t)r->yoffset / bh;
    if (sx + bx > lbx || sy + by > lby) { VLOG("WriteTexels: region outside texture %llu", (unsigned long long)t->serial); return 0; }
    uint8_t *lin = malloc((size_t)pitch * lby * ld);
    if (!lin) die("WriteTexels");
    uint8_t *storage = (uint8_t *)t->mem;
    for (uint32_t z = 0; z < (is3d ? 1 : nz); z++) {   // 3D: one pass over the level's slices
        uint32_t layer = is3d ? 0 : layer0 + (uint32_t)r->zoffset + z;
        if (nvn_deswizzle(&t->d, level, layer, storage, lin, pitch)) { VLOG("WriteTexels: texture %llu layout not supported", (unsigned long long)t->serial); break; }
        for (uint32_t s = 0; s < (is3d ? nz : 1); s++) {
            uint32_t slice = is3d ? (uint32_t)r->zoffset + s : 0;
            if (slice >= ld) break;
            for (uint32_t y = 0; y < by; y++)
                memcpy(lin + ((size_t)slice * lby + sy + y) * pitch + (size_t)sx * bpb, src + (is3d ? s : z) * img + y * row, wb);
        }
        nvn_swizzle(&t->d, level, layer, lin, pitch, storage);
    }
    free(lin);
    return 1;
}
// The image: told the game memory changed, or (GPU-written) the region copied in. src: as given to mtl_write_texels_mem.
void mtl_write_texels_image(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t bx = (uint32_t)(r->width + bw - 1) / bw, by = (uint32_t)(r->height + bh - 1) / bh, nz = r->depth > 0 ? (uint32_t)r->depth : 1;
    size_t wb = (size_t)bx * (size_t)bpb;
    if (!row) row = wb;
    if (!img) img = row * by;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    vkb_image *im = vkb_image_of(t);
    if (im && !im->gpu_written) im->cpu_written = 1;   // the pool may not be write-watched: say so directly
    if (im && im->gpu_written && !im->astc && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !(t->d.format >= 0x19 && t->d.format <= 0x24)) {
        mtl_sub_begin();
        VkCommandBuffer cb = vkb_cmd();
        VkBuffer b;
        VkDeviceSize off;
        uint8_t *px = vkb_ring_alloc((VkDeviceSize)wb * by * nz, 16, &b, &off);
        for (uint32_t z = 0; z < nz; z++)
            for (uint32_t y = 0; y < by; y++) memcpy(px + ((size_t)z * by + y) * wb, src + z * img + y * row, wb);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {.bufferOffset = off, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, is3d ? 0 : layer0 + (uint32_t)r->zoffset, is3d ? 1 : nz},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, is3d ? nz : 1}};
        vkb_checkpoint("write texels", NULL, NULL);
        vk.vkCmdCopyBufferToImage(cb, b, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        mtl_wait(mtl_sub_commit(), UINT64_MAX);
    }
    static int calls;
    if (calls++ < 8) VLOG("WriteTexels: texture %llu format %#x level %u region %dx%dx%d at %d,%d,%d%s", (unsigned long long)t->serial,
                          t->d.format, level, r->width, r->height, r->depth, r->xoffset, r->yoffset, r->zoffset,
                          im && im->gpu_written ? " (GPU-written image updated too)" : "");
}
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (mtl_write_texels_mem(t, level, layer0, r, src, row, img)) mtl_write_texels_image(t, level, layer0, r, src, row, img);
}

// ---------------- development: dump the frame's render targets ----------------
// TOTK_DUMP=<present number>:<dir>: every image the GPU has written (level 0, layer 0) in a format read back here
// (RGBA8 / BGRA8 / R11G11B10F / RGBA16F) is saved as <dir>/<serial>_<format>_<w>x<h>.bmp after that present.
static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * (float)(1 << e) / 32768.0f;
    return s ? -v : v;
}
static float uf(uint32_t bits, int mbits) {   // unsigned small float: 5-bit exponent, mbits mantissa (R11G11B10F)
    uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    if (e == 0) return m / (float)(1 << mbits) / 16384.0f;
    if (e == 31) return 65504.0f;
    return (1.0f + m / (float)(1 << mbits)) * (float)(1 << e) / 32768.0f;
}
static uint8_t tone(float v) { v = v / (1.0f + v); return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }   // HDR: Reinhard

// Bytes per texel of the formats the dump reads back (0: not dumped); depth: the float depth aspect.
static uint32_t dump_bpp(const vkb_image *im, int *depth) {
    VkFormat f = im->format;
    *depth = f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;   // depth: the float values, as R32F
    if (im->astc || im->depth > 1 || (im->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !*depth)) return 0;
    return f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
           f == VK_FORMAT_R32_SFLOAT || f == VK_FORMAT_R16G16_UNORM || *depth) ? 4 : f == VK_FORMAT_R16_SFLOAT ? 2 : 0;
}
// Records the copy of level 0, layer `layer` into upload memory (valid until the submission's slot is reused).
static uint8_t *dump_copy(VkCommandBuffer cb, vkb_image *im, uint32_t layer, uint32_t *bpp) {
    int depth;
    *bpp = dump_bpp(im, &depth);
    if (!*bpp) return NULL;
    VkBuffer b;
    VkDeviceSize off;
    uint8_t *px = vkb_ring_alloc((VkDeviceSize)im->width * im->height * *bpp, 16, &b, &off);
    vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1},
                           .imageExtent = {im->width, im->height, 1}};
    vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
    return px;
}
// <dir>/<prefix><serial>_<format>_<w>x<h>.bmp (float formats Reinhard-mapped) and value statistics in the log.
static void dump_save(const char *dir, const char *prefix, const vkb_image *im, const uint8_t *px, uint32_t bpp) {
    uint32_t w = im->width, h = im->height, size = w * h * 4;
    uint8_t *out = malloc(size);
    if (!out) return;
    for (uint32_t p = 0; p < w * h; p++) {
        const uint8_t *s = px + (size_t)p * bpp;
        uint8_t *o = out + (size_t)p * 4;   // BMP: B G R A
        switch (im->format) {
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            uint16_t c[4]; memcpy(c, s, 8);
            o[2] = tone(half_to_float(c[0])); o[1] = tone(half_to_float(c[1])); o[0] = tone(half_to_float(c[2])); o[3] = 255;
            break;
        }
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
            uint32_t v; memcpy(&v, s, 4);
            o[2] = tone(uf(v & 0x7FF, 6)); o[1] = tone(uf((v >> 11) & 0x7FF, 6)); o[0] = tone(uf(v >> 22, 5)); o[3] = 255;
            break;
        }
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
        case VK_FORMAT_R16G16_UNORM: o[2] = s[1]; o[1] = s[3]; o[0] = 0; o[3] = 255; break;   // the high bytes: R red, G green
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
            { float v; memcpy(&v, s, 4); o[0] = o[1] = o[2] = tone(v < 0 ? -v : v); o[3] = 255; break; }
        case VK_FORMAT_R16_SFLOAT: { uint16_t v; memcpy(&v, s, 2); float fv = half_to_float(v); o[0] = o[1] = o[2] = tone(fv < 0 ? -fv : fv); o[3] = 255; break; }
        default: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = 255; break;
        }
    }
    if (im->format == VK_FORMAT_R16_SFLOAT) {   // a table (light falloff): its corners and middle row
        const uint16_t *h16 = (const uint16_t *)px;
        char row[512];
        int rn = 0;
        for (uint32_t x = 0; x < w && rn < 480; x += w / 16 ? w / 16 : 1) rn += snprintf(row + rn, sizeof row - rn, " %.4g", half_to_float(h16[(size_t)(h / 2) * w + x]));
        VLOG("[dump] %stexture %llu R16F %ux%u: row %u:%s; corners %g %g %g %g", prefix, (unsigned long long)im->serial, w, h, h / 2, row,
             half_to_float(h16[0]), half_to_float(h16[w - 1]), half_to_float(h16[(size_t)(h - 1) * w]), half_to_float(h16[(size_t)h * w - 1]));
    }
    if (im->format == VK_FORMAT_R32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT || im->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {   // single channel: the values
        double sum = 0; float lo = 3e38f, hi = -3e38f;
        for (uint32_t p = 0; p < w * h; p++) { float v; memcpy(&v, px + (size_t)p * 4, 4); sum += v; if (v < lo) lo = v; if (v > hi) hi = v; }
        VLOG("[dump] %stexture %llu %s %ux%u: min %g mean %g max %g, centre %g", prefix, (unsigned long long)im->serial,
             im->format == VK_FORMAT_R32_SFLOAT ? "R32F" : "depth", w, h, lo, sum / (w * h), hi, ((const float *)px)[(size_t)(h / 2) * w + w / 2]);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s%llu_%02x_%ux%u.bmp", dir, prefix, (unsigned long long)im->serial, im->d.format, w, h);
    FILE *f = fopen(path, "wb");
    if (f) {
        uint8_t hdr[54] = {'B', 'M'};
        uint32_t v = 54 + size; memcpy(hdr + 2, &v, 4);
        v = 54; memcpy(hdr + 10, &v, 4);
        v = 40; memcpy(hdr + 14, &v, 4);
        memcpy(hdr + 18, &w, 4);
        int32_t nh = -(int32_t)h; memcpy(hdr + 22, &nh, 4);
        hdr[26] = 1; hdr[28] = 32;
        memcpy(hdr + 34, &size, 4);
        fwrite(hdr, 1, sizeof hdr, f);
        fwrite(out, 1, size, f);
        fclose(f);
    }
    free(out);
}

// ---------------- development: snapshots in the middle of a frame ----------------
// TOTK_SNAP=<present>:<shader key prefix>:<dir>: before the first draw of that present whose vertex or fragment shader
// key starts so, every texture the draw samples (and its render targets) is copied; saved as <dir>/snap_... at the
// next present. Answers "what did this pass see", which the end-of-frame dump cannot.
typedef struct { vkb_image *im; uint8_t *px; uint32_t bpp; char prefix[48]; } snap_t;
static snap_t g_snap[64];
static int g_nsnap;
static const char *g_snap_dir;
static int g_snap_key;   // the index of the key of the draw being snapped (file names)
static long long g_snap_at_jump;   // TOTK_SNAP_ON_JUMP=1: the TOTK_SNAP frame is replaced by the present after the first upward jump (lumlog)
int vkb_tex_snap_wanted(const char *vkey, const char *fkey, uint64_t frame) {   // keys: k1,k2,...: the first draw of each
    static long long at = -1;
    static char keys[8][65];
    static int nkeys, done[8];
    if (at == -1) {
        const char *e = getenv("TOTK_SNAP");
        at = e ? atoll(e) : 0;
        const char *k = e ? strchr(e, ':') : NULL, *d = k ? strchr(k + 1, ':') : NULL;
        if (!k || !d) at = 0;
        for (const char *p = k ? k + 1 : NULL; p && p < d && nkeys < 8;) {
            size_t n = strcspn(p, ",:");
            if (n && n < 65) { memcpy(keys[nkeys], p, n); keys[nkeys++][n] = 0; }
            p += n + (p[n] == ',');
            if (*p == ':' || p >= d) break;
        }
        if (d) g_snap_dir = d + 1;
    }
    if (g_snap_at_jump) at = g_snap_at_jump;
    if (at <= 0 || frame != (uint64_t)at) return 0;
    for (int i = 0; i < nkeys; i++) {
        size_t n = strlen(keys[i]);
        if (done[i] || !((vkey && !strncmp(vkey, keys[i], n)) || (fkey && !strncmp(fkey, keys[i], n)))) continue;
        done[i] = 1;   // one draw per key
        g_snap_key = i;
        return 1;
    }
    return 0;
}
void vkb_tex_snap(VkCommandBuffer cb, vkb_image *im, const char *what) {
    if (!im) return;
    for (uint32_t layer = 0; layer < (im->layers > 1 ? 2u : 1u) && g_nsnap < 64; layer++) {
        snap_t *s = &g_snap[g_nsnap];
        s->px = dump_copy(cb, im, layer, &s->bpp);
        if (!s->px) return;
        s->im = im;
        snprintf(s->prefix, sizeof s->prefix, "snap%d_%s%s_%04llx_", g_snap_key, what, layer ? "_l1" : "",
                 (unsigned long long)((uintptr_t)im->img & 0xffff));   // the key's index; the VkImage's low bits: same image or not
        g_nsnap++;
    }
}
void vkb_tex_snap_flush(void) {
    if (!g_nsnap) return;
    mtl_finish();
    _mkdir(g_snap_dir);
    for (int k = 0; k < g_nsnap; k++) {
        dump_save(g_snap_dir, g_snap[k].prefix, g_snap[k].im, g_snap[k].px, g_snap[k].bpp);
        const vkb_image *im = g_snap[k].im;
        if (g_snap[k].bpp == 8) {   // RGBA16F: per-channel means and pixels below 1 (flags and masks)
            uint32_t np = im->width * im->height, below[4] = {0};
            double ch[4] = {0};
            for (uint32_t p = 0; p < np; p++) {
                uint16_t h[4]; memcpy(h, g_snap[k].px + (size_t)p * 8, 8);
                for (int q = 0; q < 4; q++) { float v = half_to_float(h[q]); ch[q] += v; if (v < 1.0f) below[q]++; }
            }
            VLOG("snapshot %s%ux%u: rgba %.4g %.4g %.4g %.4g, below 1: %u %u %u %u of %u", g_snap[k].prefix, im->width, im->height,
                 ch[0] / np, ch[1] / np, ch[2] / np, ch[3] / np, below[0], below[1], below[2], below[3], np);
        }
    }
    VLOG("snapshot: %d images to %s", g_nsnap, g_snap_dir);
    g_nsnap = 0;
}

// TOTK_SAMEMEM=<present>: at that present, every group of live images at exactly the same memory, with what differs
// (development: the game sees one texture where the backend has several images).
static int by_storage(const void *a, const void *b) {
    const entry *x = *(entry *const *)a, *y = *(entry *const *)b;
    return (uintptr_t)x->im.storage < (uintptr_t)y->im.storage ? -1 : (uintptr_t)x->im.storage > (uintptr_t)y->im.storage;
}
static void samemem_report(uint64_t present) {
    static long long at = -1;
    if (at == -1) { const char *e = getenv("TOTK_SAMEMEM"); at = e ? atoll(e) : 0; }
    if (at <= 0 || present != (uint64_t)at) return;
    vkb_lock(&g_mu, LK_IMAGES);
    int n = 0;
    for (entry *e = g_all; e; e = e->all_next) if (e->refs && e->im.storage) n++;
    entry **v = malloc(sizeof *v * (size_t)(n ? n : 1));
    n = 0;
    for (entry *e = g_all; v && e; e = e->all_next) if (e->refs && e->im.storage) v[n++] = e;
    if (v) qsort(v, (size_t)n, sizeof *v, by_storage);
    int groups = 0;
    for (int i = 0; v && i < n;) {
        int j = i + 1;
        while (j < n && v[j]->im.storage == v[i]->im.storage) j++;
        if (j - i > 1) {
            groups++;
            for (int k = i; k < j; k++) {
                const nvn_texdesc *d = &v[k]->im.d;
                VLOG("[samemem] %p image %llu refs %d: target %u format %#x flags %#x %ux%ux%u levels %u samples %u stride %u block_h %u "
                     "swizzle %u%u%u%u layout %d %02x%02x%02x%02x%02x%02x%02x%02x gpu_written %d", v[k]->im.storage, (unsigned long long)v[k]->im.serial,
                     v[k]->refs, d->target, d->format, d->flags, d->width, d->height, d->depth, d->levels, d->samples, d->stride, d->pkg_block_h,
                     d->swizzle_r, d->swizzle_g, d->swizzle_b, d->swizzle_a, v[k]->has_layout, v[k]->layout[0], v[k]->layout[1], v[k]->layout[2],
                     v[k]->layout[3], v[k]->layout[4], v[k]->layout[5], v[k]->layout[6], v[k]->layout[7], v[k]->im.gpu_written);
            }
        }
        i = j;
    }
    VLOG("[samemem] present %llu: %d live images, %d memory addresses with more than one", (unsigned long long)present, n, groups);
    free(v);
    pthread_mutex_unlock(&g_mu);
}

// TOTK_LUMLOG=<present>:<count>: the mean of the presented image for count presents from that one (development:
// frame-to-frame flicker). Waits for the GPU at each of those presents.
// TOTK_TEXLOG=<address>,...: within the TOTK_LUMLOG presents, the mean of every layer (level 0) of the images at those
// memory addresses (development: how a GPU-built texture, such as an environment probe, evolves frame by frame).
static void texlog(uint64_t present) {
    static uint64_t addr[8], serial[8];
    static int n = -1;
    if (n < 0) {
        n = 0;
        const char *e = getenv("TOTK_TEXLOG");
        for (const char *p = e; p && *p && n < 8;) {   // s<serial>: the image with that serial (the draw trace's numbers)
            serial[n] = *p == 's' ? strtoull(p + 1, NULL, 10) : 0;
            addr[n++] = *p == 's' ? 0 : strtoull(p, NULL, 16);
            p = strchr(p, ','); if (p) p++;
        }
    }
    for (int k = 0; k < n; k++) {
        vkb_image *im = NULL;
        vkb_lock(&g_mu, LK_IMAGES);
        for (entry *e = g_all; e; e = e->all_next)
            if (e->refs && (serial[k] ? e->im.serial == serial[k] : (uint64_t)(uintptr_t)e->im.storage == addr[k])) { im = &e->im; break; }
        pthread_mutex_unlock(&g_mu);
        if (!im) continue;
        char line[512];
        int ln = snprintf(line, sizeof line, "[texlog] present %llu %#llx image %llu (game memory %p, %ux%u):", (unsigned long long)present,
                          (unsigned long long)addr[k], (unsigned long long)im->serial, (const void *)im->storage, im->width, im->height);
        for (uint32_t layer = 0; layer < im->layers && layer < 6; layer++) {
            mtl_sub_begin();
            uint32_t bpp;
            uint8_t *px = dump_copy(vkb_cmd(), im, layer, &bpp);
            mtl_wait(mtl_sub_commit(), UINT64_MAX);
            if (!px) continue;
            double s = 0, ch[4] = {0};
            uint32_t np = im->width * im->height, below1[4] = {0};
            for (uint32_t p = 0; p < np; p++) {
                if (bpp == 4 && im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {   // 11/11/10-bit unsigned floats
                    uint32_t v; memcpy(&v, px + (size_t)p * 4, 4);
                    uint32_t c[3] = {v & 0x7ff, (v >> 11) & 0x7ff, v >> 22}, mb[3] = {6, 6, 5};
                    for (int q = 0; q < 3; q++) {
                        uint32_t e = c[q] >> mb[q], m = c[q] & ((1u << mb[q]) - 1);
                        s += (e ? ldexp(1.0 + m / (double)(1u << mb[q]), (int)e - 15) : ldexp(m / (double)(1u << mb[q]), -14)) / 3.0;
                    }
                }
                else if (bpp == 4) s += (px[(size_t)p * 4] + px[(size_t)p * 4 + 1] + px[(size_t)p * 4 + 2]) / 3.0;
                else if (bpp == 8) {   // RGBA16F: per channel below
                    uint16_t h[4]; memcpy(h, px + (size_t)p * 8, 8);
                    for (int q = 0; q < 4; q++) { float v = half_to_float(h[q]); ch[q] += v; if (v < 1.0f) below1[q]++; }
                    s += (half_to_float(h[0]) + half_to_float(h[1]) + half_to_float(h[2])) / 3.0;
                }
            }
            ln += snprintf(line + ln, sizeof line - (size_t)ln, " %.4g", np ? s / np : 0);
            if (bpp == 8 && np)
                ln += snprintf(line + ln, sizeof line - (size_t)ln, " [rgba %.3g %.3g %.3g %.3g, below 1: %u %u %u %u]", ch[0] / np, ch[1] / np, ch[2] / np,
                               ch[3] / np, below1[0], below1[1], below1[2], below1[3]);
        }
        VLOG("%s", line);
    }
}

// TOTK_MEMLOG=<address>:<bytes>[,...]: within the TOTK_LUMLOG presents (the GPU done with the frame), a summary of game
// memory there - nonzero 32-bit words, the sum of their absolute float values, an FNV hash - and with TOTK_LUMLOG_DIR the
// bytes as <dir>/mem_<address>_<present>.bin (development: GPU-built buffers such as a light list, frame by frame).
static void memlog(uint64_t present) {
    static uint64_t addr[4], len[4];
    static int n = -1;
    if (n < 0) {
        n = 0;
        for (const char *p = getenv("TOTK_MEMLOG"); p && *p && n < 4;) {
            char *e;
            addr[n] = strtoull(p, &e, 16);
            len[n] = *e == ':' ? strtoull(e + 1, &e, 0) : 0;
            if (len[n]) n++;
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    for (int k = 0; k < n; k++) {
        const uint8_t *m = (const uint8_t *)(uintptr_t)addr[k];
        uint64_t nz = 0, h = 1469598103934665603ull;
        double s = 0;
        for (uint64_t i = 0; i + 4 <= len[k]; i += 4) {
            uint32_t w; float f;
            memcpy(&w, m + i, 4); memcpy(&f, m + i, 4);
            if (w) { nz++; if (f == f && f < 1e30f && f > -1e30f) s += f < 0 ? -f : f; }
            h = (h ^ w) * 1099511628211ull;
        }
        VLOG("[memlog] present %llu %#llx+%llu: nonzero words %llu, sum |f| %.6g, hash %016llx", (unsigned long long)present,
             (unsigned long long)addr[k], (unsigned long long)len[k], (unsigned long long)nz, s, (unsigned long long)h);
        const char *dir = getenv("TOTK_LUMLOG_DIR");
        if (dir && *dir) {
            _mkdir(dir);
            char path[512];
            snprintf(path, sizeof path, "%s/mem_%llx_%llu.bin", dir, (unsigned long long)addr[k], (unsigned long long)present);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(m, 1, (size_t)len[k], f); fclose(f); }
        }
    }
}

void vkb_tex_dump(const char *dir);
void vkb_tex_lumlog(const tex_t *t, uint64_t present) {
    samemem_report(present);
    {
        static long long at = -1, count;
        if (at == -1) { const char *e = getenv("TOTK_LUMLOG"); at = e ? atoll(e) : 0; count = e && strchr(e, ':') ? atoll(strchr(e, ':') + 1) : 0; }
        if (at > 0 && present >= (uint64_t)at && present < (uint64_t)(at + count)) { texlog(present); }
    }
    static long long at = -1, count;
    if (at == -1) { const char *e = getenv("TOTK_LUMLOG"); at = e ? atoll(e) : 0; count = e && strchr(e, ':') ? atoll(strchr(e, ':') + 1) : 0; }
    if (at <= 0 || present < (uint64_t)at || present >= (uint64_t)(at + count)) return;
    vkb_image *im = vkb_image_of(t);
    if (!im) return;
    mtl_sub_begin();
    uint32_t bpp;
    uint8_t *px = dump_copy(vkb_cmd(), im, 0, &bpp);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    if (!px || bpp != 4) return;
    double sum[3] = {0}, low = 0;   // low: the lower half (the ground on the island; clouds there when it is missing)
    for (uint32_t p = 0; p < im->width * im->height; p += 7)
        for (int c = 0; c < 3; c++) { sum[c] += px[(size_t)p * 4 + c]; if (p / im->width >= im->height / 2) low += px[(size_t)p * 4 + c]; }
    double n = (double)(im->width * im->height / 7) * 255.0;
    static uint64_t last_draws;
    uint64_t draws = vkb_draw_count();
    VLOG("[lum] present %llu: mean %.3f %.3f %.3f, lower half %.3f, draws %llu", (unsigned long long)present, sum[0] / n, sum[1] / n, sum[2] / n,
         low / (n * 1.5), (unsigned long long)(draws - last_draws));
    last_draws = draws;
    memlog(present);   // the GPU has finished this frame (waited for above)
    // and each of those frames as <dir>/f<present>_<serial>...bmp; with TOTK_LUMLOG_JUMP=<delta> only frames where the lower
    // half's mean jumps by more than delta, with the frame before and the two after (catching a short drop-out)
    const char *dir = getenv("TOTK_LUMLOG_DIR"), *jump = getenv("TOTK_LUMLOG_JUMP");
    static double prev_low = -1;
    static uint8_t *prev_px;
    static uint64_t prev_present;
    static int save_left;
    double lowm = low / (n * 1.5);
    if (dir && *dir) {
        _mkdir(dir);
        char prefix[32];
        size_t bytes = (size_t)im->width * im->height * bpp;
        int save = !jump;
        if (jump && prev_low >= 0 && (lowm - prev_low > atof(jump) || prev_low - lowm > atof(jump))) {
            save = 1; save_left = 2;
            static int jumps;
            if (prev_px && jumps++ < 8) { snprintf(prefix, sizeof prefix, "f%llu_before_", (unsigned long long)prev_present); dump_save(dir, prefix, im, prev_px, bpp); }
            VLOG("[lum] jump at present %llu: lower half %.3f -> %.3f", (unsigned long long)present, prev_low, lowm);
            // 1: after the first jump up (the drop-out ends); down[<n>]: the n-th drop (default the first)
            const char *soj = getenv("TOTK_SNAP_ON_JUMP");
            int down = soj && !strncmp(soj, "down", 4);
            static int drops;
            if (down && lowm < prev_low) drops++;
            if (soj && !g_snap_at_jump && (down ? lowm < prev_low && drops >= (atoi(soj + 4) > 0 ? atoi(soj + 4) : 1) : lowm > prev_low)) {
                g_snap_at_jump = (long long)vkb_frame() + 1;   // the next frame recorded (the drop-out lasts a few)
                VLOG("[lum] snapshot set for present %lld", g_snap_at_jump);
            }
            static int dumps;   // TOTK_LUMLOG_JUMP_DUMP=1: the first jumps also dump every render target of the frame
            if (getenv("TOTK_LUMLOG_JUMP_DUMP") && dumps++ < 6) {
                char d2[512];
                snprintf(d2, sizeof d2, "%s/jump%llu", dir, (unsigned long long)present);
                _mkdir(d2);
                vkb_tex_dump(d2);
            }
        } else if (save_left > 0) { save = 1; save_left--; }
        static long every = -1;   // TOTK_LUMLOG_EVERY=<n>: also every n-th frame (where the scripted run went)
        if (every < 0) every = getenv("TOTK_LUMLOG_EVERY") ? atol(getenv("TOTK_LUMLOG_EVERY")) : 0;
        if (every > 0 && present % (uint64_t)every == 0) save = 1;
        if (save) { snprintf(prefix, sizeof prefix, "f%llu_", (unsigned long long)present); dump_save(dir, prefix, im, px, bpp); }
        if (jump) {   // the frame, kept for "before" (the ring memory is reused)
            if (!prev_px) prev_px = malloc(bytes);
            if (prev_px) memcpy(prev_px, px, bytes);
            prev_present = present;
        }
    }
    prev_low = lowm;
}


// Write-back (see "GPU-drawn contents into game memory" above). The arena: host memory the copies land in, used as a
// ring in job order.
static VkBuffer g_wb_buf;
static VkDeviceMemory g_wb_mem;
static uint8_t *g_wb_map;
static int g_wb_coherent;
static uint64_t g_wb_head;   // where the next job's copies go
static int wb_arena(void) {
    static int state;   // 0 not tried, 1 ready, -1 failed
    if (state) return state > 0;
    state = -1;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WB_ARENA, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_wb_buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, g_wb_buf, &req);
    // cached: the CPU reads it back (reading uncached host memory is many times slower)
    uint32_t type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    void *m;
    vkb_mtag = MT_WRITEBACK;
    if (type == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_wb_mem) != VK_SUCCESS) { vk.vkDestroyBuffer(vkc.device, g_wb_buf, NULL); return 0; }
    vk.vkBindBufferMemory(vkc.device, g_wb_buf, g_wb_mem, 0);
    if (vk.vkMapMemory(vkc.device, g_wb_mem, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) return 0;
    g_wb_map = m;
    g_wb_coherent = (vkc.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    state = 1;
    VLOG("write-back of GPU-drawn images: %u MiB of host memory, %s", WB_ARENA >> 20, g_wb_coherent ? "coherent" : "cached, not coherent");
    return 1;
}
static int wb_alloc(uint64_t n, uint64_t *at) {   // g_mu held: n bytes of the arena after the jobs still waiting
    if (!g_nwbj) g_wb_head = 0;
    uint64_t tail = g_nwbj ? g_wbj[0].at : 0;
    if (!g_nwbj || g_wb_head >= tail) {
        if (WB_ARENA - g_wb_head >= n) { *at = g_wb_head; g_wb_head += n; return 1; }
        if (g_nwbj && tail > n) { *at = 0; g_wb_head = n; return 1; }   // wrapped
        return 0;
    }
    if (tail - g_wb_head > n) { *at = g_wb_head; g_wb_head += n; return 1; }
    return 0;
}
// Contents laid out as NVN does (what the upload copies byte for byte), one level and layer after another.
static uint64_t wb_size(const vkb_image *im) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(im->d.format, &bw, &bh);
    if (!bpb || im->astc || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || im->depth > 1 || !im->storage || (im->d.format >= 0x19 && im->d.format <= 0x24)) return 0;
    uint64_t n = 0;
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        n += ((uint64_t)(lw + bw - 1) / bw * bpb * ((lh + bh - 1) / bh) + 15) / 16 * 16 * im->layers;
    }
    return n;
}
// Records (record) or lays out into game memory (!record) every level and layer of im at offset at of the arena.
static void wb_parts(VkCommandBuffer cb, const vkb_image *im, uint64_t at, int record) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(im->d.format, &bw, &bh);
    for (uint32_t level = 0; level < im->levels; level++) {
        uint32_t lw = im->width >> level ? im->width >> level : 1, lh = im->height >> level ? im->height >> level : 1;
        uint32_t bx = (lw + bw - 1) / bw, by = (lh + bh - 1) / bh, pitch = bx * (uint32_t)bpb;
        uint64_t n = ((uint64_t)pitch * by + 15) / 16 * 16;
        for (uint32_t layer = 0; layer < im->layers; layer++, at += n) {
            if (record) {
                VkBufferImageCopy r = {.bufferOffset = at, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
                                       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, .imageExtent = {lw, lh, 1}};
                vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_wb_buf, 1, &r);
            } else nvn_swizzle(&im->d, level, layer, g_wb_map + at, pitch, (void *)im->storage);
        }
    }
}
// The pool lets the CPU at it (cpu_visible); a virtual pool's addresses are only reserved (its bytes are in physical
// pools' pages), so its textures are left out. (VirtualQuery over the write-watched guest window took ~2 ms a call.)
static int wb_memory_ok(const vkb_image *im) { return im->cpu_visible && !totk_vpool_gather((uintptr_t)im->storage, 0, NULL); }
void vkb_tex_writeback_frame(void) {
    static int on = -1;
    static uint64_t frames, st_n, st_bytes, st_ns, st_skip, st_queued, st_max, st_lay;
    if (on < 0) { const char *e = getenv("TOTK_GPU_WB"); on = !e || *e != '0'; }
    if (!on || !vkb_ready() || !wb_arena()) return;
    uint64_t t0 = runtime_now_ns(), now = vkb_frame(), done = mtl_done();
    vkb_lock(&g_mu, LK_IMAGES);
    // copies the GPU has finished: into game memory, if the texture is still the game's and was not drawn again since
    int k = 0;
    for (; k < g_nwbj && g_wbj[k].fence <= done; k++) {
        wb_job *j = &g_wbj[k];
        entry *e = j->e;
        if (!e || !e->refs || e->im.write_seq != j->seq) { st_skip++; continue; }
        if (!g_wb_coherent) {   // this job's range only (the CPU caches of the whole arena would cost milliseconds)
            VkDeviceSize atom = vkc.props.limits.nonCoherentAtomSize ? vkc.props.limits.nonCoherentAtomSize : 64;
            VkDeviceSize lo = j->at / atom * atom, hi = (j->at + j->bytes + atom - 1) / atom * atom;
            VkMappedMemoryRange mr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = g_wb_mem, .offset = lo, .size = hi > WB_ARENA ? VK_WHOLE_SIZE : hi - lo};
            vk.vkInvalidateMappedMemoryRanges(vkc.device, 1, &mr);
        }
        wb_parts(VK_NULL_HANDLE, &e->im, j->at, 0);
        st_n++; st_bytes += j->bytes;
    }
    if (k) memmove(g_wbj, g_wbj + k, sizeof *g_wbj * (size_t)(g_nwbj -= k));
    uint64_t t1 = runtime_now_ns();
    st_lay += t1 - t0;
    // images that settled: their copies into the arena, in a submission of their own (not waited for: with every
    // command buffer in flight they wait for a later present)
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t fence = mtl_last_fence() + 1, bytes = 0;
    int first = g_nwbj, can = mtl_slot_free();
    pthread_mutex_lock(&g_wbq_mu);
    for (int i = 0; i < g_nwbq;) {
        entry *e = g_wbq[i];
        vkb_image *im = &e->im;
        if (im->write_seq == im->wb_seq) { wbq_remove(e); continue; }   // nothing new
        if (now - im->write_frame < WB_SETTLE) { i++; continue; }     // still being drawn
        uint64_t n = wb_size(im), at;
        if (!e->refs || !n || !wb_memory_ok(im)) { im->wb_seq = im->write_seq; wbq_remove(e); continue; }
        if (!can || (bytes && bytes + n > WB_FRAME_BYTES) || g_nwbj == WB_MAXJOBS || !wb_alloc(n, &at)) { i++; continue; }   // a later present
        if (!cb) { mtl_sub_begin(); cb = vkb_cmd(); }
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        wb_parts(cb, im, at, 1);
        g_wbj[g_nwbj++] = (wb_job){e, im->write_seq, fence, at, n};
        im->wb_seq = im->write_seq;
        bytes += n;
        wbq_remove(e);
    }
    st_queued += (uint64_t)g_nwbq;
    pthread_mutex_unlock(&g_wbq_mu);
    pthread_mutex_unlock(&g_mu);
    if (cb) {
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &dep);
        uint64_t f = mtl_sub_commit();
        if (f != fence) {   // (one thread commits: never) the jobs wait for the fence they really have
            vkb_lock(&g_mu, LK_IMAGES);
            for (int i = first; i < g_nwbj; i++) if (g_wbj[i].fence == fence) g_wbj[i].fence = f;
            pthread_mutex_unlock(&g_mu);
        }
    }
    uint64_t dt = runtime_now_ns() - t0;
    st_ns += dt;
    if (dt > st_max) st_max = dt;
    if (++frames % 300 == 0 && dm_stats()) {
        VLOG("per frame: GPU-drawn images written back to game memory %.2f (%.3f MiB), %.2f skipped (texture gone or drawn again), "
             "%.1f queued; %.3f ms on the translation thread (%.3f laying out, longest frame %.2f ms)", st_n / 300.0, st_bytes / 300.0 / 1048576.0,
             st_skip / 300.0, st_queued / 300.0, st_ns / 300.0 / 1e6, st_lay / 300.0 / 1e6, st_max / 1e6);
        st_n = st_bytes = st_ns = st_skip = st_queued = st_max = st_lay = 0;
    }
}

// TOTK_RT_LOG (development): at every present, how many bytes of each image of that format and size are not zero
// (level 0, layer 0; the GPU waited for): when a target fills and when it is emptied again.
void vkb_tex_rtlog_frame(void) {
    static int on = -1;
    if (on < 0) on = getenv("TOTK_RT_LOG") != NULL;
    if (!on || !vkb_ready()) return;
    typedef struct { vkb_image *im; uint8_t *px; uint64_t n; } item;
    item it[16];
    int n = 0;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    vkb_lock(&g_mu, LK_IMAGES);
    for (entry *e = g_all; e && n < 16; e = e->all_next) {
        vkb_image *im = &e->im;
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(im->d.format, &bw, &bh);
        if (!vkb_rt_log_d(&im->d) || !bpb || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT) continue;
        uint32_t bx = (im->width + bw - 1) / bw, by = (im->height + bh - 1) / bh;
        VkBuffer b;
        VkDeviceSize off;
        uint64_t bytes = (uint64_t)bx * by * (uint32_t)bpb;
        uint8_t *px = vkb_ring_alloc(bytes, 16, &b, &off);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {im->width, im->height, 1}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
        it[n++] = (item){im, px, bytes};
    }
    pthread_mutex_unlock(&g_mu);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    for (int i = 0; i < n; i++) {
        uint64_t nz = 0;
        for (uint64_t k = 0; k < it[i].n; k++) nz += it[i].px[k] != 0;
        static uint64_t last[16];
        if (nz != last[i]) VLOG("[rtlog] present %llu: image %llu has %llu non-zero bytes of %llu", (unsigned long long)vkb_frame(),
                                (unsigned long long)it[i].im->serial, (unsigned long long)nz, (unsigned long long)it[i].n);
        last[i] = nz;
    }
}

// TOTK_B2B_LOG=1 (development): GPU buffer copies that read or write the memory of GPU-written images. Their contents
// are only in the images here, not in game memory (as they are on the Switch), so such a copy moves other bytes.
void vkb_tex_b2b_log(uint64_t src, uint64_t dst, uint64_t n) {
    static int on = -1, said;
    if (on < 0) on = getenv("TOTK_B2B_LOG") != NULL;
    if (!on || said >= 200) return;
    vkb_lock(&g_mu, LK_IMAGES);
    for (entry *e = g_all; e && said < 200; e = e->all_next) {
        if (!e->im.gpu_written || !e->im.storage) continue;
        nvn_texlayout L;
        nvn_tex_layout(&e->im.d, &L);
        uint64_t lo = (uint64_t)(uintptr_t)e->im.storage, hi = lo + L.size;
        int rd = src < hi && lo < src + n, wr = dst < hi && lo < dst + n;
        if (!rd && !wr) continue;
        said++;
        VLOG("[b2b] copy %#llx -> %#llx +%#llx %s GPU-written image %llu (%s, format %#x %ux%u levels %u at %#llx+%#llx), present %llu",
             (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n, rd && wr ? "reads and writes" : rd ? "reads" : "writes",
             (unsigned long long)e->im.serial, e->refs ? "live" : "aside", e->im.d.format, e->im.width, e->im.height, e->im.d.levels,
             (unsigned long long)lo, (unsigned long long)L.size, (unsigned long long)vkb_frame());
    }
    pthread_mutex_unlock(&g_mu);
}

// F9 texture check (development): the textures the traced draws sample that the CPU fills (vkb_tex_vcheck_add), compared
// once the GPU is idle with what an upload would make of game memory now. A difference with no CPU write since the
// upload means the image missed a change: draws read other contents than the game holds (terrain heights from another
// tile stretch the ground into huge wedges). Level 0 of 2D (array) textures; ASTC and converted formats are left out.
static uint64_t *g_vt;
static int g_nvt, g_capvt;
static pthread_mutex_t g_vt_mu = PTHREAD_MUTEX_INITIALIZER;
void vkb_tex_vcheck_add(const vkb_image *im) {
    if (!im || (im->gpu_written && !im->cpu_visible) || !im->storage) return;
    pthread_mutex_lock(&g_vt_mu);
    int dup = 0;
    for (int k = g_nvt - 1; k >= 0 && k >= g_nvt - 64 && !dup; k--) dup = g_vt[k] == im->serial;
    if (!dup && g_nvt < 100000) {
        if (g_nvt == g_capvt && !(g_vt = realloc(g_vt, sizeof *g_vt * (size_t)(g_capvt = g_capvt ? g_capvt * 2 : 1024)))) die("texture check");
        g_vt[g_nvt++] = im->serial;
    }
    pthread_mutex_unlock(&g_vt_mu);
}
static int by_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
void vkb_tex_vcheck_run(void) {
    pthread_mutex_lock(&g_vt_mu);
    if (g_nvt) qsort(g_vt, (size_t)g_nvt, sizeof *g_vt, by_u64);
    typedef struct { vkb_image *im; uint8_t *px; uint32_t pitch, by, layers, bpb; int gw; } job;
    enum { MAXJ = 8192 };
    static job jobs[MAXJ];
    int n = 0, skipped = 0;
    VkDeviceSize total = 0;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    vkb_lock(&g_mu, LK_IMAGES);
    for (entry *e = g_all; e && n < MAXJ; e = e->all_next) {
        vkb_image *im = &e->im;
        if (!e->refs || !g_nvt || !bsearch(&im->serial, g_vt, (size_t)g_nvt, sizeof *g_vt, by_u64)) continue;
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(im->d.format, &bw, &bh);
        // GPU-written images in game memory the CPU can access: once written back (and not drawn since), game memory
        // should hold the same; a difference is the CPU writing over GPU contents (which never reach the image)
        int gw = im->gpu_written, wb_busy = 0;
        for (int j = 0; gw && j < g_nwbj; j++) wb_busy |= g_wbj[j].e == e;
        if ((gw ? im->wb_seq != im->write_seq || im->wb_pending || wb_busy : !im->upload_stamp || im->not_uploadable) || im->astc || !im->storage ||
            !bpb || im->depth > 1 || im->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (im->d.format >= 0x19 && im->d.format <= 0x24)) { skipped++; continue; }
        uint32_t bx = (im->width + bw - 1) / bw, by = (im->height + bh - 1) / bh, pitch = bx * (uint32_t)bpb, layers = im->layers > 64 ? 64 : im->layers;
        VkDeviceSize bytes = (VkDeviceSize)pitch * by * layers;
        if (total + bytes > (1024u << 20)) { skipped++; continue; }
        total += bytes;
        VkBuffer b;
        VkDeviceSize off;
        uint8_t *px = vkb_ring_alloc(bytes, 16, &b, &off);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = bx * (uint32_t)bw, .bufferImageHeight = by * (uint32_t)bh,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}, .imageExtent = {im->width, im->height, 1}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
        jobs[n++] = (job){im, px, pitch, by, layers, (uint32_t)bpb, gw};
    }
    pthread_mutex_unlock(&g_mu);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    int bad = 0, cpu = 0, said = 0, gwdiff = 0, ngw = 0;
    for (int i = 0; i < n; i++) {
        vkb_image *im = jobs[i].im;
        nvn_texlayout L;
        nvn_tex_layout(&im->d, &L);
        const uint8_t *src = im->storage;
        uint8_t *gathered = NULL;
        int vp = totk_vpool_gather((uintptr_t)im->storage, 0, NULL);
        if (vp && (gathered = malloc(L.size ? L.size : 1))) { totk_vpool_gather((uintptr_t)im->storage, L.size, gathered); src = gathered; }
        size_t slice = (size_t)jobs[i].pitch * jobs[i].by;
        uint8_t *lin = malloc(slice ? slice : 1);
        uint64_t diff = 0, first = 0;
        for (uint32_t layer = 0; lin && layer < jobs[i].layers; layer++) {
            if (nvn_deswizzle(&im->d, 0, layer, src, lin, jobs[i].pitch)) break;
            const uint8_t *g = jobs[i].px + slice * layer;
            for (size_t b = 0; b < slice; b++) if (g[b] != lin[b]) { if (!diff++) first = slice * layer + b; }
        }
        free(lin);
        free(gathered);
        ngw += jobs[i].gw;
        if (!diff) continue;
        if (jobs[i].gw) {
            if (gwdiff++ < 60)
                VLOG("[vcheck] GPU-written texture %llu format %#x %ux%u at %p: game memory differs from it after the write-back in %llu of %llu bytes "
                     "(first at +%llu): the CPU wrote over GPU contents", (unsigned long long)im->serial, im->d.format, im->width, im->height,
                     (const void *)im->storage, (unsigned long long)diff, (unsigned long long)(slice * jobs[i].layers), (unsigned long long)first);
            continue;
        }
        int tracked = vp || nvn_dirty_tracked(im->storage, L.size);
        int wrote = vp ? totk_vpool_dirty_since((uintptr_t)im->storage, L.size, im->upload_stamp) : tracked && nvn_dirty_since(im->storage, L.size, im->upload_stamp);
        if (wrote > 0) { cpu++; continue; }
        bad++;
        if (said++ < 200)
            VLOG("[vcheck] texture %llu format %#x %ux%u layers %u at %p: the image differs from game memory in %llu of %llu bytes (first at +%llu), "
                 "no CPU write seen since its upload (%s%s, mapping generation %llu now %llu)", (unsigned long long)im->serial, im->d.format,
                 im->width, im->height, im->layers, (const void *)im->storage, (unsigned long long)diff, (unsigned long long)(slice * jobs[i].layers),
                 (unsigned long long)first, vp ? "virtual pool" : tracked ? "write-watched" : "NOT write-watched", wrote < 0 ? ", pages unknown" : "",
                 (unsigned long long)im->vpool_gen, (unsigned long long)totk_vpool_generation());
    }
    VLOG("[vcheck] %d textures of the traced draws compared (%d left out): %d differ with no CPU write seen, %d differ where the CPU wrote since; "
         "%d GPU-written ones in CPU-visible memory, %d differ from it", n - ngw, skipped, bad, cpu, ngw, gwdiff);
    // Textures read from game memory that GPU-written images overlap: the GPU may have drawn their contents through
    // another texture object over the same memory, kept as a separate image (terrain tiles dark and without grass,
    // the Depths' ground black, 2026-10-27). Both shapes, and whether the game memory holds anything at all.
    int said2 = 0, zero = 0;
    vkb_lock(&g_mu, LK_IMAGES);
    for (int i = 0; i < n && said2 < 80; i++) {
        if (jobs[i].gw) continue;
        vkb_image *im = jobs[i].im;
        nvn_texlayout L;
        nvn_tex_layout(&im->d, &L);
        const uint8_t *m = im->storage;
        int vp = totk_vpool_gather((uintptr_t)m, 0, NULL), empty = !vp;
        for (uint64_t b = 0; empty && b < L.size; b += 8) { uint64_t v; memcpy(&v, m + b, 8); empty = !v; }
        zero += empty;
        int first = 1;
        for (entry *e = g_all; e && said2 < 80; e = e->all_next) {
            if (&e->im == im || !e->im.gpu_written || !e->im.storage) continue;
            nvn_texlayout L2;
            nvn_tex_layout(&e->im.d, &L2);
            if (!(e->im.storage < m + L.size && m < e->im.storage + L2.size)) continue;
            const nvn_texdesc *a = &im->d, *b = &e->im.d;
            if (first) {
                said2++;
                VLOG("[vcheck] texture %llu read from game memory (%s): target %u format %#x %ux%ux%u levels %u flags %#x stride %u block_h %u at %p+%#llx",
                     (unsigned long long)im->serial, empty ? "all zero" : "not zero", a->target, a->format, a->width, a->height, a->depth, a->levels,
                     a->flags, a->stride, a->pkg_block_h, (const void *)m, (unsigned long long)L.size);
                first = 0;
            }
            said2++;
            VLOG("[vcheck]   GPU-written image %llu over it (%s): target %u format %#x %ux%ux%u levels %u flags %#x stride %u block_h %u at %p+%#llx",
                 (unsigned long long)e->im.serial, e->refs ? "live" : "aside", b->target, b->format, b->width, b->height, b->depth, b->levels,
                 b->flags, b->stride, b->pkg_block_h, (const void *)e->im.storage, (unsigned long long)L2.size);
        }
        if (first && empty && said2 < 80) {   // nothing drew over it here either: who fills it on the Switch?
            said2++;
            VLOG("[vcheck] texture %llu read from game memory, all zero, no GPU-written image over it: format %#x %ux%u levels %u flags %#x at %p+%#llx",
                 (unsigned long long)im->serial, im->d.format, im->width, im->height, im->d.levels, im->d.flags, (const void *)m, (unsigned long long)L.size);
        }
    }
    pthread_mutex_unlock(&g_mu);
    VLOG("[vcheck] %d of those textures have all-zero game memory", zero);
    g_nvt = 0;
    pthread_mutex_unlock(&g_vt_mu);
}

void vkb_tex_dump(const char *dir) {
    typedef struct { vkb_image *im; uint8_t *px; uint32_t bpp; } job;
    job jobs[512];
    int n = 0;
    mtl_sub_begin();
    VkCommandBuffer cb = vkb_cmd();
    vkb_lock(&g_mu, LK_IMAGES);
    for (entry *e = g_all; e && n < 512; e = e->all_next) {
        // TOTK_DUMP_ALL=1: textures uploaded from game memory too (tables, LUTs)
        if (!e->refs || (!e->im.gpu_written && !(e->im.upload_stamp && getenv("TOTK_DUMP_ALL")))) continue;
        uint32_t bpp;
        uint8_t *px = dump_copy(cb, &e->im, 0, &bpp);
        if (px) jobs[n++] = (job){&e->im, px, bpp};
    }
    // 3D volumes in R8 or BC4 (the noise TOTK makes at startup, L21): level 0 whole, as <dir>/vol_<serial>_<format>.bin
    job vols[32];
    int nvol = 0;
    for (entry *e = g_all; e && nvol < 32; e = e->all_next) {
        vkb_image *im = &e->im;
        if (!e->refs || im->depth <= 1 || (im->format != VK_FORMAT_R8_UNORM && im->format != VK_FORMAT_BC4_UNORM_BLOCK)) continue;
        uint32_t bytes = im->format == VK_FORMAT_R8_UNORM ? im->width * im->height * im->depth : (im->width / 4) * (im->height / 4) * 8 * im->depth;
        VkBuffer b;
        VkDeviceSize off;
        uint8_t *px = vkb_ring_alloc(bytes, 16, &b, &off);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy r = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {im->width, im->height, im->depth}};
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
        vols[nvol++] = (job){im, px, bytes};
    }
    pthread_mutex_unlock(&g_mu);
    mtl_wait(mtl_sub_commit(), UINT64_MAX);
    _mkdir(dir);   // one level: the parent must exist
    for (int k = 0; k < n; k++) dump_save(dir, "", jobs[k].im, jobs[k].px, jobs[k].bpp);
    for (int k = 0; k < nvol; k++) {
        const vkb_image *im = vols[k].im;
        uint32_t zero = 0;
        for (uint32_t i = 0; i < vols[k].bpp; i++) zero += !vols[k].px[i];
        VLOG("[dump] volume texture %llu format %#x %ux%ux%u%s: %u bytes, %u zero", (unsigned long long)im->serial, im->d.format,
             im->width, im->height, im->depth, im->gpu_written ? ", GPU-written" : "", vols[k].bpp, zero);
        char path[1024];
        snprintf(path, sizeof path, "%s/vol_%llu_%02x.bin", dir, (unsigned long long)im->serial, im->d.format);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(vols[k].px, 1, vols[k].bpp, f); fclose(f); }
    }
    VLOG("dumped %d render targets to %s", n, dir);
    {   // GPU-written textures that share memory (aliasing): each is a separate image here
        int said = 0;
        vkb_lock(&g_mu, LK_IMAGES);
        for (entry *a = g_all; a && said < 40; a = a->all_next) {
            if (!a->refs || !a->im.gpu_written || !a->im.storage) continue;
            nvn_texlayout La;
            nvn_tex_layout(&a->im.d, &La);
            for (entry *b = a->all_next; b && said < 40; b = b->all_next) {
                if (!b->refs || !b->im.gpu_written || !b->im.storage) continue;
                nvn_texlayout Lb;
                nvn_tex_layout(&b->im.d, &Lb);
                if (a->im.storage < b->im.storage + Lb.size && b->im.storage < a->im.storage + La.size) {
                    said++;
                    VLOG("[alias] GPU-written textures %llu (format %#x %ux%u at %p) and %llu (format %#x %ux%u at %p) share memory",
                         (unsigned long long)a->im.serial, a->im.d.format, a->im.width, a->im.height, (const void *)a->im.storage,
                         (unsigned long long)b->im.serial, b->im.d.format, b->im.width, b->im.height, (const void *)b->im.storage);
                }
            }
        }
        pthread_mutex_unlock(&g_mu);
    }
    // TOTK_DUMP_MEM=<guest address>:<bytes>[,...]: raw guest memory (GPU-written buffers) as <dir>/mem_<address>.bin
    for (const char *p = getenv("TOTK_DUMP_MEM"); p && *p;) {
        char *end;
        unsigned long long addr = strtoull(p, &end, 0), size = *end == ':' ? strtoull(end + 1, &end, 0) : 0;
        if (addr && size && size < (64u << 20)) {
            char path[1024];
            snprintf(path, sizeof path, "%s/mem_%llx.bin", dir, addr);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite((const void *)(uintptr_t)addr, 1, size, f); fclose(f); }
        }
        p = strchr(end, ',');
        if (p) p++;
    }
}
