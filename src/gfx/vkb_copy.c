// GPU copies between game memory and textures: NVN CopyBufferToTexture, CopyTextureToBuffer, CopyTextureToTexture
// (with SetCopyRowStride), after MacNativeGames' nvn_mtl.m copy_b2t / copy_t2b / copy_t2t (MIT). These were not replayed
// at all: TOTK uploads every frame of a pre-rendered cutscene from its NV12 buffer with CopyBufferToTexture, and the
// movie texture stayed empty (a green picture: zero luma and chroma).
// A texture a copy writes holds GPU contents from then on (gpu_written): it is not uploaded from game memory again,
// whose bytes a GPU copy does not change here (as MacNativeGames' GPU-owned textures).
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "nvn/nvn_tex.h"
#include "vkb.h"
#include "vkb_audit.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static __thread int64_t g_stride;   // SetCopyRowStride: bytes between rows in the buffer, 0 = tightly packed (per recording thread)
void vkb_copy_stride(int64_t stride) { g_stride = stride; }
int64_t vkb_copy_stride_get(void) { return g_stride; }

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
}
static void unsupported(const char *what, const vkb_image *im) {
    vkb_audit(A_TEX_COPY_UNSUPPORTED, NULL);
    static int said;
    if (said++ < 8) VLOG("[temporary] %s: texture %llu format %#x not copied", what, im ? (unsigned long long)im->serial : 0ull, im ? im->d.format : 0);
}
// Colour formats stored here as NVN lays them out (what the upload copies byte for byte): not ASTC (decoded on the
// CPU), not depth (D16 / D24 are kept as D32F), not the 3-component formats (expanded to 4).
static int raw_layout(const vkb_image *im, int *bw, int *bh, int *bpb) {
    *bpb = nvn_fmt_bpb(im->d.format, bw, bh);
    return *bpb && !im->astc && im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !(im->d.format >= 0x19 && im->d.format <= 0x24);
}
static uint32_t level_extent(uint32_t v, unsigned level) { return v >> level ? v >> level : 1; }

// The buffer range a copy between a buffer and a texture covers, with its texture and region checked (0: not copied).
static vkb_image *copy_range(const p_copy_b2t *c, const char *what, uint64_t *len_out, int *bw, int *bh, int *bpb, int quiet) {
    vkb_image *im = c->tex ? vkb_image_of(c->tex) : NULL;
    if (!im || !raw_layout(im, bw, bh, bpb)) { if (!quiet) unsupported(what, im); return NULL; }
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (level >= im->levels) { if (!quiet) unsupported(what, im); return NULL; }
    int is3d = im->depth > 1, nz = c->r.depth > 0 ? c->r.depth : 1;
    uint32_t lw = level_extent(im->width, level), lh = level_extent(im->height, level), ld = is3d ? level_extent(im->depth, level) : 1;
    if (c->r.width <= 0 || c->r.height <= 0 || c->r.xoffset < 0 || c->r.yoffset < 0 || c->r.zoffset < 0) return NULL;
    if ((uint32_t)(c->r.xoffset + c->r.width) > lw || (uint32_t)(c->r.yoffset + c->r.height) > lh ||
        (is3d ? (uint32_t)(c->r.zoffset + nz) > ld : layer0 + c->r.zoffset + nz > im->layers)) { if (!quiet) unsupported(what, im); return NULL; }
    uint64_t wb = (uint64_t)((c->r.width + *bw - 1) / *bw) * *bpb, row = g_stride > 0 ? (uint64_t)g_stride : wb;
    uint64_t rows = (uint64_t)(c->r.height + *bh - 1) / *bh, img = row * rows;
    *len_out = img * (nz - 1) + row * (rows - 1) + wb;
    if (row < wb || row % *bpb) { if (!quiet) vkb_audit(A_COPY_SKIPPED, NULL); return NULL; }
    return im;
}
int vkb_copy_t2b_range(const p_copy_b2t *c, uint64_t *addr, uint64_t *len) {   // vkb_mt.c: the walk's GPU writes
    int bw, bh, bpb;
    if (!copy_range(c, "", len, &bw, &bh, &bpb, 1)) return 0;
    *addr = c->src;
    return 1;
}

// to_tex: CopyBufferToTexture (c->src is the buffer); else CopyTextureToBuffer (c->src is the destination buffer).
void vkb_copy_buf_tex(const p_copy_b2t *c, int to_tex) {
    const char *what = to_tex ? "buffer -> texture copy" : "texture -> buffer copy";
    int bw, bh, bpb;
    uint64_t len;
    vkb_image *im = copy_range(c, what, &len, &bw, &bh, &bpb, 0);
    if (c->tex && vkb_rt_log_match(c->tex))
        VLOG("[rtlog] %s texture %llu (%s) buffer %#llx+%llu region %dx%d at %d,%d, present %llu", what, (unsigned long long)c->tex->serial,
             im ? "copied" : "not copied", (unsigned long long)c->src, (unsigned long long)(im ? len : 0), c->r.width, c->r.height, c->r.xoffset,
             c->r.yoffset, (unsigned long long)vkb_frame());
    if (!im) return;
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    int is3d = im->depth > 1, nz = c->r.depth > 0 ? c->r.depth : 1;
    uint64_t wb = (uint64_t)((c->r.width + bw - 1) / bw) * bpb, row = g_stride > 0 ? (uint64_t)g_stride : wb;
    uint64_t rows = (uint64_t)(c->r.height + bh - 1) / bh;
    vkb_pass_end_r(PE_TEXCOPY);
    vkb_mirrors_evict(c->src, len);   // a range that straddles a mirror: game memory first
    vkb_pool_use(c->src, len, !to_tex);
    if (!to_tex) vkb_ucap_drop(c->src, len);
    VkBuffer buf;
    VkDeviceSize off;
    if (!vkb_gpu_addr(c->src, len, &buf, &off)) { vkb_audit(A_COPY_SKIPPED, NULL); return; }
    if (off % (bpb < 4 ? 4 : bpb)) { unsupported(what, im); return; }   // [temporary] Vulkan's buffer offset alignment
    VkCommandBuffer cb = vkb_cmd();
    vkb_tex_upload(cb, im);   // the texture's current contents (the source, or the rest of the destination)
    VkBufferImageCopy r = {.bufferOffset = off, .bufferRowLength = (uint32_t)(row / bpb * bw), .bufferImageHeight = (uint32_t)(rows * bh),
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, is3d ? 0 : layer0 + (uint32_t)c->r.zoffset, is3d ? 1 : (uint32_t)nz},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset : 0},
        .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, is3d ? (uint32_t)nz : 1}};
    if (to_tex) {   // the buffer may have been written by earlier GPU work
        barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkb_checkpoint("copy buffer -> texture", NULL, NULL);
        vk.vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        vkb_image_written(im);
    } else {
        barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkb_image_use(cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkb_checkpoint("copy texture -> buffer", NULL, NULL);
        vk.vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
        // later commands (and the CPU after the game's wait) read the buffer
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
    }
}

static VkFormatFeatureFlags features(VkFormat f) {
    VkFormatProperties p;
    vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &p);
    return p.optimalTilingFeatures;
}

// CopyTextureToTexture: the same size as a plain copy (formats of one texel size: a reinterpretation, as on the
// Switch); scaling, mirroring or a different texel size as a blit (filtered with LINEAR_FILTER).
void vkb_copy_tex_tex(const p_copy_t2t *c) {
    vkb_image *s = c->src ? vkb_image_of(c->src) : NULL, *d = c->dst ? vkb_image_of(c->dst) : NULL;
    if ((c->src && vkb_rt_log_match(c->src)) || (c->dst && vkb_rt_log_match(c->dst)))
        VLOG("[rtlog] texture -> texture copy %llu (format %#x %ux%u) -> %llu (format %#x %ux%u), present %llu",
             c->src ? (unsigned long long)c->src->serial : 0ull, c->src ? c->src->d.format : 0, c->src ? c->src->d.width : 0, c->src ? c->src->d.height : 0,
             c->dst ? (unsigned long long)c->dst->serial : 0ull, c->dst ? c->dst->d.format : 0, c->dst ? c->dst->d.width : 0, c->dst ? c->dst->d.height : 0,
             (unsigned long long)vkb_frame());
    if (!s || !d) { unsupported("texture -> texture copy", s ? s : d); return; }
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    int s3 = s->depth > 1, d3 = d->depth > 1;
    int snz = c->sr.depth > 0 ? c->sr.depth : 1, dnz = c->dr.depth > 0 ? c->dr.depth : 1;
    if (sl >= s->levels || dl >= d->levels || s->aspect != d->aspect || s3 != d3 || s->astc != d->astc ||
        c->sr.width <= 0 || c->sr.height <= 0 || c->dr.width <= 0 || c->dr.height <= 0 ||
        (!s3 && (sz0 + c->sr.zoffset + snz > s->layers || dz0 + c->dr.zoffset + dnz > d->layers))) {
        unsupported("texture -> texture copy", s); return;
    }
    int sbw, sbh, dbw, dbh, sbpb = nvn_fmt_bpb(s->d.format, &sbw, &sbh), dbpb = nvn_fmt_bpb(d->d.format, &dbw, &dbh);
    int mirror = c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT);
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && snz == dnz && !mirror;
    int plain = same && (s->format == d->format || (sbpb && sbpb == dbpb && sbw == dbw && sbh == dbh && !s->astc));
    VkFilter filter = c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    if (!plain) {   // a blit: both formats must allow it (and filtering, when asked for)
        VkFormatFeatureFlags sf = features(s->format), df = features(d->format);
        if (!(sf & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(df & VK_FORMAT_FEATURE_BLIT_DST_BIT) || (mirror & NVN_COPY_FLAGS_MIRROR_Z_BIT) ||
            (s->aspect != VK_IMAGE_ASPECT_COLOR_BIT && (s->format != d->format || !same))) {
            unsupported("texture -> texture copy (scaled / converted)", s); return;
        }
        if (!(sf & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) filter = VK_FILTER_NEAREST;
    }
    vkb_pass_end_r(PE_TEXCOPY);
    VkCommandBuffer cb = vkb_cmd();
    vkb_tex_upload(cb, s);
    vkb_tex_upload(cb, d);   // the rest of the destination
    VkImageLayout sly = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dly = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (s == d) {   // within one image (another level or layer): one layout for both ends
        sly = dly = VK_IMAGE_LAYOUT_GENERAL;
        vkb_image_use(cb, s, sly, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    } else {
        vkb_image_use(cb, s, sly, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        vkb_image_use(cb, d, dly, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    VkImageSubresourceLayers ss = {s->aspect, sl, s3 ? 0 : sz0 + (uint32_t)c->sr.zoffset, s3 ? 1 : (uint32_t)snz};
    VkImageSubresourceLayers ds = {d->aspect, dl, d3 ? 0 : dz0 + (uint32_t)c->dr.zoffset, d3 ? 1 : (uint32_t)dnz};
    vkb_checkpoint(plain ? "copy texture -> texture" : "blit texture -> texture", NULL, NULL);
    if (plain) {
        VkImageCopy r = {ss, {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0}, ds, {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
                         {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 ? (uint32_t)snz : 1}};
        vk.vkCmdCopyImage(cb, s->img, sly, d->img, dly, 1, &r);
    } else {
        VkImageBlit r = {ss, {{c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
                              {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, s3 ? c->sr.zoffset + snz : 1}},
                         ds, {{c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
                              {c->dr.xoffset + c->dr.width, c->dr.yoffset + c->dr.height, d3 ? c->dr.zoffset + dnz : 1}}};
        if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int32_t t = r.dstOffsets[0].x; r.dstOffsets[0].x = r.dstOffsets[1].x; r.dstOffsets[1].x = t; }
        if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int32_t t = r.dstOffsets[0].y; r.dstOffsets[0].y = r.dstOffsets[1].y; r.dstOffsets[1].y = t; }
        vk.vkCmdBlitImage(cb, s->img, sly, d->img, dly, 1, &r, filter);
    }
    vkb_image_written(d);
}
