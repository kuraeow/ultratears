// AMD FidelityFX Super Resolution 1 on the present (present.c): when the window is larger than the game's output, EASU
// scales the game's picture to the window's size and RCAS sharpens it, in place of the bilinear blit; at the game's own
// size RCAS alone sharpens it (the game's FSR 1 scaled its scene there, unsharpened). As
// totk-pc-recomp's runtime/vk_fsr.c (MIT, Copyright (c) 2026 Valiantsin Dzerakh), whose shaders it uses
// (third_party/fsr1: fsr_easu.comp / fsr_rcas.comp around AMD's ffx_fsr1.h, MIT, and their SPIR-V); here the two passes
// are compute shader objects (as the rest of the backend), with one descriptor set each: the present thread waits for
// each frame's work before recording the next, so the sets are rewritten in turn.
//
// upscale= in ultratears.cfg (TOTK_UPSCALER): fsr (default) or bilinear; sharpness= (TOTK_SHARPNESS) 0..100, default
// 50 (0: no RCAS pass). Cost: two compute passes over the window's size and one copy per present; nothing on the game's.
#include "vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../third_party/fsr1/fsr_easu_spv.h"
#include "../../third_party/fsr1/fsr_rcas_spv.h"

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))

static struct {
    int tried, ok, logged, sharp, on;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkDescriptorPool dpool;
    VkDescriptorSet set[2];        // EASU, RCAS
    VkShaderEXT easu, rcas;
    VkSampler sampler;
    VkImage img[2];                // EASU output, RCAS output (RGBA16F, the window's size)
    VkDeviceMemory mem[2];
    VkImageView view[2];
    uint32_t w, h;
    VkImage src;                   // the source the view is of
    VkImageView src_view;
} F;

static uint32_t memory_type(uint32_t bits) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return i;
    return UINT32_MAX;
}

static VkShaderEXT make_shader(const uint32_t *code, size_t size) {
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = size, .pCode = code, .pName = "main",
                                .setLayoutCount = 1, .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1,
                                .pPushConstantRanges = &pr};
    VkShaderEXT s = VK_NULL_HANDLE;
    return vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, &s) == VK_SUCCESS ? s : VK_NULL_HANDLE;
}

static int init(void) {
    F.tried = 1;
    const char *u = getenv("TOTK_UPSCALER");
    F.on = !(u && !strcmp(u, "bilinear"));
    const char *s = getenv("TOTK_SHARPNESS");
    F.sharp = s && *s ? atoi(s) : 50;
    if (F.sharp < 0) F.sharp = 0;
    if (F.sharp > 100) F.sharp = 100;
    if (!F.on) return 0;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &F.dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2, .poolSizeCount = 2, .pPoolSizes = ps};
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vk.vkCreateDescriptorSetLayout(vkc.device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vk.vkCreatePipelineLayout(vkc.device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &F.dpool) != VK_SUCCESS ||
        vk.vkCreateSampler(vkc.device, &si, NULL, &F.sampler) != VK_SUCCESS) {
        LOG("FSR off: its objects could not be created; the present scales bilinearly");
        return 0;
    }
    VkDescriptorSetLayout l2[2] = {F.dsl, F.dsl};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = F.dpool,
                                      .descriptorSetCount = 2, .pSetLayouts = l2};
    if (vk.vkAllocateDescriptorSets(vkc.device, &ai, F.set) != VK_SUCCESS ||
        !(F.easu = make_shader(fsr_easu_spv, sizeof fsr_easu_spv)) || !(F.rcas = make_shader(fsr_rcas_spv, sizeof fsr_rcas_spv))) {
        LOG("FSR off: its shaders could not be created; the present scales bilinearly");
        return 0;
    }
    LOG("FSR 1 (EASU + RCAS, sharpness %d) ready for the present", F.sharp);
    return F.ok = 1;
}

static void free_images(void) {
    for (int i = 0; i < 2; i++) {
        if (F.view[i]) vk.vkDestroyImageView(vkc.device, F.view[i], NULL);
        if (F.img[i]) vk.vkDestroyImage(vkc.device, F.img[i], NULL);
        if (F.mem[i]) vk.vkFreeMemory(vkc.device, F.mem[i], NULL);
        F.view[i] = VK_NULL_HANDLE, F.img[i] = VK_NULL_HANDLE, F.mem[i] = VK_NULL_HANDLE;
    }
    F.w = F.h = 0;
}

static int ensure_images(uint32_t w, uint32_t h) {
    if (F.img[0] && F.w == w && F.h == h) return 1;
    free_images();   // the caller waited for the earlier presents
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VkMemoryRequirements req;
        if (vk.vkCreateImage(vkc.device, &ci, NULL, &F.img[i]) != VK_SUCCESS) { F.img[i] = VK_NULL_HANDLE; goto fail; }
        vk.vkGetImageMemoryRequirements(vkc.device, F.img[i], &req);
        VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits)};
        if (mi.memoryTypeIndex == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &mi, NULL, &F.mem[i]) != VK_SUCCESS) {
            F.mem[i] = VK_NULL_HANDLE;
            goto fail;
        }
        vk.vkBindImageMemory(vkc.device, F.img[i], F.mem[i], 0);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = F.img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vk.vkCreateImageView(vkc.device, &vi, NULL, &F.view[i]) != VK_SUCCESS) { F.view[i] = VK_NULL_HANDLE; goto fail; }
    }
    F.w = w, F.h = h;
    return 1;
fail:
    free_images();
    LOG("FSR: no %ux%u images; the present scales bilinearly", w, h);
    F.ok = 0;
    return 0;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                    VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa, .dstStageMask = ds,
                               .dstAccessMask = da, .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vk.vkCmdPipelineBarrier2(cb, &d);
}

static void pass(VkCommandBuffer cb, int which, VkShaderEXT sh, VkImageView in, VkImageLayout in_layout, VkImageView out,
                 const float pc[8], uint32_t gx, uint32_t gy) {
    VkDescriptorImageInfo ii = {F.sampler, in, in_layout}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F.set[which], .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = F.set[which], .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    vk.vkUpdateDescriptorSets(vkc.device, 2, w, 0, NULL);
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_COMPUTE_BIT;
    vk.vkCmdBindShadersEXT(cb, 1, &stage, &sh);
    vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 1, &F.set[which], 0, NULL);
    vk.vkCmdPushConstants(cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vk.vkCmdDispatch(cb, gx, gy, 1);
}

// Records the upscale of src (BGRA8, in TRANSFER_SRC_OPTIMAL; its crop rectangle crop = x, y, w, h) into dst (the
// swapchain image, in TRANSFER_DST_OPTIMAL) at dx, dy, size dw x dh. A window of the game's own size gets RCAS alone (the
// game's FSR 1 scaled its scene to that size without sharpening; the sharpness setting applies there too). 0: not done
// (off, smaller, or no resources) - the caller blits. The present waits for this frame's work before it records the next (present.c), so the source's
// view and the descriptor sets are free to replace.
int vk_fsr_record(VkCommandBuffer cb, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst, int dx, int dy,
                  uint32_t dw, uint32_t dh) {
    if (!F.tried) init();
    int up = dw > (uint32_t)crop[2] || dh > (uint32_t)crop[3];
    int sharpen_only = !up && dw == (uint32_t)crop[2] && dh == (uint32_t)crop[3] && !crop[0] && !crop[1] && F.sharp > 0;
    if (!F.ok || (!up && !sharpen_only) || !ensure_images(dw, dh)) return 0;
    if (F.src != src) {   // an output image of another size (the game changed it): a view of the new one
        if (F.src_view) vk.vkDestroyImageView(vkc.device, F.src_view, NULL);
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = VK_FORMAT_B8G8R8A8_UNORM,
                                    .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_ONE},
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vk.vkCreateImageView(vkc.device, &vi, NULL, &F.src_view) != VK_SUCCESS) {
            F.src_view = VK_NULL_HANDLE, F.src = VK_NULL_HANDLE;
            return 0;
        }
        F.src = src;
    }
    if (!F.logged) {
        F.logged = 1;
        if (up) LOG("FSR upscales %dx%d to %ux%u (sharpness %d)", crop[2], crop[3], dw, dh, F.sharp);
        else LOG("FSR sharpens the game's %ux%u picture (RCAS, sharpness %d)", dw, dh, F.sharp);
    }
    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, XF = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, CS, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (int i = 0; i < 2; i++)
        barrier(cb, F.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, CS | XF, 0, CS,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    uint32_t gx = (dw + 15) / 16, gy = (dh + 15) / 16;
    float rcas[8] = {(float)dw, (float)dh, (float)(100 - F.sharp) / 50.0f, 0, 0, 0, 0, 0};
    int last = 0;
    if (sharpen_only) {   // RCAS straight from the game's picture
        pass(cb, 1, F.rcas, F.src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, F.view[1], rcas, gx, gy);
        last = 1;
    } else {
        float easu[8] = {(float)crop[2], (float)crop[3], (float)sw, (float)sh, (float)dw, (float)dh, (float)crop[0], (float)crop[1]};
        pass(cb, 0, F.easu, F.src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, F.view[0], easu, gx, gy);
        if (F.sharp > 0) {
            barrier(cb, F.img[0], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            pass(cb, 1, F.rcas, F.view[0], VK_IMAGE_LAYOUT_GENERAL, F.view[1], rcas, gx, gy);
            last = 1;
        }
    }
    barrier(cb, F.img[last], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, XF,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, CS, 0,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);   // back as the replayer leaves it
    VkImageBlit region = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int)dw, (int)dh, 1}},
                          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + (int)dw, dy + (int)dh, 1}}};
    vk.vkCmdBlitImage(cb, F.img[last], VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                      VK_FILTER_NEAREST);
    return 1;
}
