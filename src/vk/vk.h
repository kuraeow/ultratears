// The process's one Vulkan device, shared by the NVN replayer (src/gfx/backend_vk.c) and the window presentation
// (present.c). Functions are loaded at run time through SDL's vkGetInstanceProcAddr (no vulkan-1 link), into the table
// below; a function is called as vk.Name(...).
// The device enables every feature the GPU supports except robustBufferAccess (bounds checks the game does not need,
// at a cost on every buffer access), and the extensions of the replayer's design (2026-10-21).
#pragma once
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <vulkan/vulkan.h>

#define VK_FN_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties) X(vkEnumerateInstanceExtensionProperties)
#define VK_FN_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2)\
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR)
#define VK_FN_DEVICE(X) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2) \
    X(vkCmdClearColorImage) X(vkCmdCopyBuffer) X(vkCmdUpdateBuffer) X(vkCmdBlitImage) X(vkCmdFillBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkBindBufferMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetMemoryHostPointerPropertiesEXT) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdCopyImage) \
    X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShadersEXT) X(vkDestroyShaderEXT) X(vkGetShaderBinaryDataEXT) X(vkCmdBindShadersEXT) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDispatch) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdSetVertexInputEXT) X(vkCmdSetViewportWithCount) \
    X(vkCmdSetScissorWithCount) X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPolygonModeEXT) X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBoundsTestEnable) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdSetRasterizationSamplesEXT) X(vkCmdSetSampleMaskEXT) X(vkCmdSetAlphaToCoverageEnableEXT) \
    X(vkCmdSetAlphaToOneEnableEXT) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) X(vkCmdSetColorWriteMaskEXT) \
    X(vkCmdSetLogicOpEnableEXT) X(vkCmdSetLogicOpEXT) X(vkCmdSetBlendConstants) X(vkCmdSetDepthClampEnableEXT) \
    X(vkCmdSetLineWidth) X(vkCmdSetDepthClipNegativeOneToOneEXT) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdSetProvokingVertexModeEXT) X(vkCmdSetDepthClipEnableEXT) X(vkCmdSetDepthBounds) \
    X(vkCreateQueryPool) X(vkResetQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkCmdSetAttachmentFeedbackLoopEnableEXT) X(vkCmdSetCheckpointNV) X(vkGetQueueCheckpointDataNV) X(vkCmdWriteBufferMarkerAMD) X(vkInvalidateMappedMemoryRanges) \
    X(vkGetDescriptorSetLayoutSizeEXT) X(vkGetDescriptorSetLayoutBindingOffsetEXT) X(vkGetDescriptorEXT) \
    X(vkCmdBindDescriptorBuffersEXT) X(vkCmdSetDescriptorBufferOffsetsEXT)
#define VK_FN_DECLARE(f) PFN_##f f;
typedef struct { VK_FN_GLOBAL(VK_FN_DECLARE) VK_FN_INSTANCE(VK_FN_DECLARE) VK_FN_DEVICE(VK_FN_DECLARE) } VkFunctions;
#undef VK_FN_DECLARE
extern VkFunctions vk;

typedef struct {
    VkInstance instance;
    VkSurfaceKHR surface;       // the window's, VK_NULL_HANDLE without one
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;              // graphics + compute + transfer (+ present when there is a window); vk_queue_lock()
    uint32_t family;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memory;
    uint64_t host_pointer_alignment;   // VK_EXT_external_memory_host
    int has_descriptor_buffer, has_gpl, has_eds3, has_vertex_input_ds, has_depth_clip_control, has_shader_object, has_interlock;
    int has_feedback_loop;   // VK_EXT_attachment_feedback_loop_layout: sampling a bound attachment in its feedback loop layout
    int has_feedback_dynamic;   // ...+ _dynamic_state: the loop declared per draw (AMD's Windows driver has the layout only)
    int has_local_read;      // VK_KHR_dynamic_rendering_local_read: barriers inside a pass (draws reading what earlier draws wrote)
    VkPhysicalDeviceDescriptorBufferPropertiesEXT db;   // VK_EXT_descriptor_buffer: descriptor sizes, alignment, ranges
    VkPhysicalDeviceShaderObjectPropertiesEXT so;      // VK_EXT_shader_object: the driver's shader binary format (UUID, version)
    int has_checkpoints;     // TOTK_VK_CHECKPOINTS=1: VK_NV_device_diagnostic_checkpoints (where a lost device stopped)
    int has_memory_budget;   // VK_EXT_memory_budget: the memory this process uses per heap (the memory log line)
    int has_buffer_marker;   // TOTK_VK_CHECKPOINTS=1: VK_AMD_buffer_marker (the same, written by the GPU to host memory)
    int has_custom_border;   // VK_EXT_custom_border_color without a format: any sampler border colour, as on NVN
    uint32_t max_custom_border;   // ...at most this many such samplers at once (maxCustomBorderColorSamplers)
    uint32_t subgroup_pin;   // 32 where the driver may run shaders in other subgroup sizes (AMD: 32 or 64), else 0
    VkShaderStageFlags subgroup_pin_stages;   // ...the stages it can pin (requiredSubgroupSizeStages)
} VkContext;
extern VkContext vkc;

typedef struct SDL_Window SDL_Window;
// Creates the instance and device once (the first caller's window decides the instance extensions for presentation;
// NULL = no window, e.g. TOTK_HEADLESS). 1 = ready; 0 = no usable Vulkan 1.3 GPU (logged).
int vk_init(SDL_Window *window);
void vk_queue_lock(void);       // the queue is used by the replayer's submit thread and the present thread
void vk_queue_unlock(void);
int vk_check(VkResult r, const char *what);   // 1 on success, logs the failure otherwise
