// Spatial anti-aliasing in place of the game's FXAA (antialias= in ultratears.cfg, TOTK_AA): SMAA 1x (antialias=smaa;
// Jimenez et al., third_party/smaa, MIT) or none (antialias=off: the FXAA draw becomes a copy of its source). Unset, the
// game's FXAA draws; with FSR 3.1 (antialias=fsr3, vkb_tsr.c) FSR anti-aliases and this file does nothing.
//
// TotK's FXAA (fs 22a61cb1ec73) reads the tonemapped scene (one texture) and writes a target of its size, before the
// game's last scaling pass and the HUD (at scale=100 too: the "scaling" is then 1:1). vkb_draw.c fxaa_replace finds the
// draw by its fragment shader while recording and calls vkb_smaa_run in its place, outside a pass: SMAA's three passes
// (src/gfx/shaders/smaa.comp, SMAA's HIGH preset) run as compute shaders on the replay's command buffer into images of
// this file, and the result is copied (a blit: the target is R11G11B10F, which a storage image need not take) into the
// FXAA's target. So the HUD, drawn later at the output size, is never touched.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "nvn/nvn_mtl.h"
#include "vkb.h"
#include "vkb_smaa_spv.h"
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SearchTex.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

// ---------------------------------------------------------------- settings
static int g_mode = -1;   // 0 the game's FXAA (or FSR 3.1, vkb_tsr.c), 1 SMAA, 2 none
int vkb_aa_mode(void) {
    if (g_mode < 0) {
        const char *e = getenv("TOTK_AA");
        g_mode = !e ? 0 : !strcmp(e, "smaa") ? 1 : !strcmp(e, "off") || !strcmp(e, "none") ? 2 : 0;
        if (g_mode) VLOG("antialiasing: %s in place of the game's FXAA", g_mode == 1 ? "SMAA 1x" : "none");
    }
    return g_mode;
}

// ---------------------------------------------------------------- GPU resources
enum { P_EDGES, P_WEIGHTS, P_BLEND, NP, NSETS = 2 };
enum { I_EDGES, I_WEIGHTS, I_OUT, I_AREA, I_SEARCH, NI };
static const struct { const uint32_t *code; size_t size; uint32_t nsamp; } PASS[NP] = {
    {smaa_edges_spv, sizeof smaa_edges_spv, 1}, {smaa_weights_spv, sizeof smaa_weights_spv, 3}, {smaa_blend_spv, sizeof smaa_blend_spv, 2}};
static struct {
    int tried, ok, have_tex, fresh;   // fresh: the frame-size images are new (UNDEFINED)
    VkDescriptorSetLayout dsl[NP];
    VkPipelineLayout pl[NP];
    VkShaderEXT sh[NP];
    VkDescriptorPool pool;
    VkDescriptorSet set[VKB_SLOTS][NSETS][NP];
    int set_next[VKB_SLOTS];
    uint64_t set_sub[VKB_SLOTS];
    VkSampler linear;
    vkb_gimg im[NI];
    uint32_t w, h;
    uint64_t runs;
    pthread_mutex_t mu;
} M = {.mu = PTHREAD_MUTEX_INITIALIZER};

static int pipelines(void) {
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    for (int p = 0; p < NP; p++) {   // bindings: its samplers, then its output
        VkDescriptorSetLayoutBinding b[4];
        uint32_t n = PASS[p].nsamp;
        for (uint32_t i = 0; i < n; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        b[n] = (VkDescriptorSetLayoutBinding){n, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n + 1, .pBindings = b};
        if (vk.vkCreateDescriptorSetLayout(vkc.device, &dci, NULL, &M.dsl[p]) != VK_SUCCESS) return 0;
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &M.dsl[p],
                                         .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        if (vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &M.pl[p]) != VK_SUCCESS) return 0;
        VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                    .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = PASS[p].size, .pCode = PASS[p].code,
                                    .pName = "main", .setLayoutCount = 1, .pSetLayouts = &M.dsl[p], .pushConstantRangeCount = 1,
                                    .pPushConstantRanges = &pr};
        if (vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, &M.sh[p]) != VK_SUCCESS) return 0;
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * VKB_SLOTS * NSETS},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NP * VKB_SLOTS * NSETS}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = VKB_SLOTS * NSETS * NP, .poolSizeCount = 2,
                                     .pPoolSizes = ps};
    if (vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &M.pool) != VK_SUCCESS) return 0;
    for (int s = 0; s < VKB_SLOTS; s++)
        for (int k = 0; k < NSETS; k++) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = M.pool,
                                              .descriptorSetCount = NP, .pSetLayouts = M.dsl};
            if (vk.vkAllocateDescriptorSets(vkc.device, &ai, M.set[s][k]) != VK_SUCCESS) return 0;
        }
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    if (vk.vkCreateSampler(vkc.device, &si, NULL, &M.linear) != VK_SUCCESS) return 0;
    // the lookup textures (made once, uploaded at the first run)
    return vkb_gimg_make(&M.im[I_AREA], VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, 1) &&
           vkb_gimg_make(&M.im[I_SEARCH], VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1);
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkPipelineStageFlags2 dst) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = dst, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &di);
}
static void to_general(VkCommandBuffer cb, int from, int to) {   // new images: undefined to GENERAL (kept so)
    VkImageMemoryBarrier2 ib[NI];
    uint32_t n = 0;
    for (int i = from; i < to; i++)
        ib[n++] = (VkImageMemoryBarrier2){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                          .image = M.im[i].img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vk.vkCmdPipelineBarrier2(cb, &di);
}
static void upload(VkCommandBuffer cb, int id, const unsigned char *bytes, size_t n) {
    VkBuffer buf;
    VkDeviceSize off;
    memcpy(vkb_ring_alloc(n, 16, &buf, &off), bytes, n);
    VkBufferImageCopy c = {.bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageExtent = {M.im[id].w, M.im[id].h, 1}};
    vk.vkCmdCopyBufferToImage(cb, buf, M.im[id].img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void dispatch(VkCommandBuffer cb, VkDescriptorSet set, int p, const VkImageView *in, VkImageView out, const float rt[4]) {
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet w[4];
    uint32_t n = PASS[p].nsamp;
    for (uint32_t i = 0; i <= n; i++) {
        ii[i] = (VkDescriptorImageInfo){i < n ? M.linear : VK_NULL_HANDLE, i < n ? in[i] : out, VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = i < n ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &ii[i]};
    }
    vk.vkUpdateDescriptorSets(vkc.device, n + 1, w, 0, NULL);
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
    vk.vkCmdBindShadersEXT(cb, 1, &st, &M.sh[p]);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.pl[p], 0, 1, &set, 0, NULL);
    vk.vkCmdPushConstants(cb, M.pl[p], VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, rt);
    vk.vkCmdDispatch(cb, ((uint32_t)rt[2] + 7) / 8, ((uint32_t)rt[3] + 7) / 8, 1);
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
}

// SMAA from src into dst (the FXAA draw's texture and target: the same size), on the current command buffer outside a
// pass. 0: not done (the FXAA draw goes ahead).
int vkb_smaa_run(vkb_image *src, vkb_image *dst) {
    pthread_mutex_lock(&M.mu);
    int ok = 0;
    if (!M.tried) {
        M.tried = 1;
        M.ok = pipelines();
        if (!M.ok) VLOG("antialiasing: SMAA could not be set up: the game's FXAA stays");
    }
    if (!M.ok || !src || !dst || src->width != dst->width || src->height != dst->height) goto out;
    {
        VkFormatProperties fp;
        vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, dst->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            VLOG("antialiasing: the FXAA target's format %d takes no blit: the game's FXAA stays", (int)dst->format);
            M.ok = 0;
            goto out;
        }
    }
    uint32_t w = src->width, h = src->height;
    VkCommandBuffer cb = vkb_cmd();
    if (M.w != w || M.h != h) {
        for (int i = I_EDGES; i <= I_OUT; i++) vkb_gimg_free(&M.im[i]);
        if (!vkb_gimg_make(&M.im[I_EDGES], VK_FORMAT_R8G8_UNORM, w, h, 1) || !vkb_gimg_make(&M.im[I_WEIGHTS], VK_FORMAT_R8G8B8A8_UNORM, w, h, 1) ||
            !vkb_gimg_make(&M.im[I_OUT], VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1)) {
            VLOG("antialiasing: SMAA's images (%ux%u) could not be made: the game's FXAA stays", w, h);
            for (int i = I_EDGES; i <= I_OUT; i++) vkb_gimg_free(&M.im[i]);
            M.w = M.h = 0, M.ok = 0;
            goto out;
        }
        M.w = w, M.h = h, M.fresh = 1;
        VLOG("antialiasing: SMAA images made for %ux%u", w, h);
    }
    VkImageView cv = vkb_view(src, NULL, 0, VK_IMAGE_VIEW_TYPE_2D);
    if (!cv) goto out;
    int slot = vkb_slot();
    if (M.set_sub[slot] != mtl_last_fence()) M.set_sub[slot] = mtl_last_fence(), M.set_next[slot] = 0;
    if (M.set_next[slot] >= NSETS) goto out;   // a third FXAA in one submission: the game's draws it
    VkDescriptorSet *sets = M.set[slot][M.set_next[slot]++];
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);   // our images' last use
    if (M.fresh) to_general(cb, I_EDGES, I_OUT + 1), M.fresh = 0;
    if (!M.have_tex) {
        to_general(cb, I_AREA, I_SEARCH + 1);
        upload(cb, I_AREA, areaTexBytes, sizeof areaTexBytes);
        upload(cb, I_SEARCH, searchTexBytes, sizeof searchTexBytes);
        barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        M.have_tex = 1;
    }
    vkb_image_use(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    const float rt[4] = {1.0f / (float)w, 1.0f / (float)h, (float)w, (float)h};
    dispatch(cb, sets[P_EDGES], P_EDGES, &cv, M.im[I_EDGES].view, rt);
    VkImageView wi[3] = {M.im[I_EDGES].view, M.im[I_AREA].view, M.im[I_SEARCH].view};
    dispatch(cb, sets[P_WEIGHTS], P_WEIGHTS, wi, M.im[I_WEIGHTS].view, rt);
    VkImageView bi[2] = {cv, M.im[I_WEIGHTS].view};
    dispatch(cb, sets[P_BLEND], P_BLEND, bi, M.im[I_OUT].view, rt);
    // the picture into the FXAA's target
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    vkb_image_use(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vk.vkCmdBlitImage(cb, M.im[I_OUT].img, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    vkb_image_written(dst);
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (M.runs++ == 120) VLOG("antialiasing: SMAA 1x %ux%u running", w, h);
    ok = 1;
out:
    pthread_mutex_unlock(&M.mu);
    return ok;
}
