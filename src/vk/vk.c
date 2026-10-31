// The shared Vulkan instance and device (see vk.h).
#include "vk.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))

VkFunctions vk;
VkContext vkc;
static pthread_mutex_t queue_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_queue_lock(void) { pthread_mutex_lock(&queue_mu); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&queue_mu); }

void vk_device_fault_report(void);
int vk_check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    LOG("%s failed (%d)", what, (int)r);
    static int reported;
    if (r == VK_ERROR_DEVICE_LOST && !reported++) vk_device_fault_report();   // once, whichever call saw it first
    return 0;
}

static int has_layer(const char *name) {
    uint32_t n = 0;
    vk.vkEnumerateInstanceLayerProperties(&n, NULL);
    VkLayerProperties *p = calloc(n ? n : 1, sizeof *p);
    vk.vkEnumerateInstanceLayerProperties(&n, p);
    int found = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].layerName, name)) found = 1;
    free(p);
    return found;
}

// TOTK_VK_VALIDATION: the layer's errors and warnings into the game's log ("[validation] ..."). Without a messenger the
// layer prints to the process's own output, which a player's log (ultratears.log) does not get (2026-10-29: a tester's
// validation run with the output redirected came back empty).
static VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types; (void)user;
    static int said;
    if (said++ < 5000)
        LOG("[validation] %s %s", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
            data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static int create_instance(SDL_Window *window) {
    PFN_vkGetInstanceProcAddr gipa = NULL;
    if (window) gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    else {   // no window (TOTK_HEADLESS): SDL's video subsystem is not up, so the loader is loaded here
        HMODULE m = LoadLibraryA("vulkan-1.dll");
        if (m) gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(m, "vkGetInstanceProcAddr");
    }
    if (!gipa) { LOG("no Vulkan loader (vulkan-1.dll)"); return 0; }
#define LOAD(f) vk.f = (PFN_##f)gipa(NULL, #f);
    VK_FN_GLOBAL(LOAD)
#undef LOAD
    Uint32 next = 0;
    const char *const *exts = window ? SDL_Vulkan_GetInstanceExtensions(&next) : NULL;
    // TOTK_VK_VALIDATION=1: the Khronos validation layer (Vulkan SDK), for development
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *v = getenv("TOTK_VK_VALIDATION");
    int validate = v && strcmp(v, "0") && has_layer(layer);
    if (v && strcmp(v, "0") && !validate) LOG("TOTK_VK_VALIDATION: %s not installed", layer);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "UltraTears",
                             .apiVersion = VK_API_VERSION_1_3};
    const char *with_debug[64];   // with validation: the window's extensions and VK_EXT_debug_utils (its messenger)
    if (validate && next < 63) {
        for (Uint32 i = 0; i < next; i++) with_debug[i] = exts[i];
        with_debug[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        exts = with_debug;
    }
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = exts,
                               .enabledLayerCount = validate ? 1u : 0u, .ppEnabledLayerNames = &layer};
    if (!vk_check(vk.vkCreateInstance(&ci, NULL, &vkc.instance), "vkCreateInstance")) return 0;
#define LOAD(f) vk.f = (PFN_##f)gipa(vkc.instance, #f);
    VK_FN_INSTANCE(LOAD)
#undef LOAD
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)gipa(vkc.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = validation_message};
        VkDebugUtilsMessengerEXT messenger;
        int ok = create && create(vkc.instance, &mi, NULL, &messenger) == VK_SUCCESS;
        LOG("validation layer on%s", ok ? ": its messages go to this log ([validation])" : " (no messenger: messages to the console)");
    }
    if (window && !SDL_Vulkan_CreateSurface(window, vkc.instance, NULL, &vkc.surface)) {
        LOG("window surface: %s", SDL_GetError());
        vkc.surface = VK_NULL_HANDLE;
    }
    return 1;
}

typedef struct { VkExtensionProperties *e; uint32_t n; } ExtList;
static ExtList device_extensions(VkPhysicalDevice p) {
    ExtList l = {0};
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, NULL);
    l.e = calloc(l.n ? l.n : 1, sizeof *l.e);
    vk.vkEnumerateDeviceExtensionProperties(p, NULL, &l.n, l.e);
    return l;
}
static int ext_in(const ExtList *l, const char *name) {
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->e[i].extensionName, name)) return 1;
    return 0;
}

// A Vulkan 1.3 GPU with a queue family for graphics and compute (and presentation, with a window); discrete first.
static int pick_device(void) {
    uint32_t n = 0;
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, NULL);
    if (!n) { LOG("no Vulkan GPU"); return 0; }
    VkPhysicalDevice *all = calloc(n, sizeof *all);
    vk.vkEnumeratePhysicalDevices(vkc.instance, &n, all);
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        vk.vkGetPhysicalDeviceProperties(all[i], &pr);
        ExtList ex = device_extensions(all[i]);
        int ok = pr.apiVersion >= VK_API_VERSION_1_3 && ext_in(&ex, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
                 (!vkc.surface || ext_in(&ex, VK_KHR_SWAPCHAIN_EXTENSION_NAME));
        free(ex.e);
        if (!ok) continue;
        uint32_t nq = 0;
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
        vk.vkGetPhysicalDeviceQueueFamilyProperties(all[i], &nq, q);
        for (uint32_t k = 0; k < nq; k++) {
            VkBool32 present = VK_TRUE;
            if (vkc.surface) vk.vkGetPhysicalDeviceSurfaceSupportKHR(all[i], k, vkc.surface, &present);
            if ((q[k].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) || !present)
                continue;
            int score = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (best < score) { best = score; vkc.gpu = all[i]; vkc.family = k; }
            break;
        }
        free(q);
    }
    free(all);
    if (best < 0) { LOG("no Vulkan 1.3 GPU with VK_EXT_external_memory_host%s", vkc.surface ? " that presents to the window" : ""); return 0; }
    vk.vkGetPhysicalDeviceProperties(vkc.gpu, &vkc.props);
    vk.vkGetPhysicalDeviceMemoryProperties(vkc.gpu, &vkc.memory);
    vkc.so = (VkPhysicalDeviceShaderObjectPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT};
    vkc.db = (VkPhysicalDeviceDescriptorBufferPropertiesEXT){VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT, &vkc.so};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, &vkc.db};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &host};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p13};
    vk.vkGetPhysicalDeviceProperties2(vkc.gpu, &p2);
    vkc.host_pointer_alignment = host.minImportedHostPointerAlignment;
    // The shader translator (Ryujinx) assumes Maxwell's 32-lane warps for SHFL, VOTE and the lane id; a driver that
    // picks the subgroup size per shader (AMD RDNA: 32 or 64) is told 32 per shader object (vkb_shader.c make_object).
    // Where the size is fixed (NVIDIA: 32) nothing is pinned. TOTK_VK_SUBGROUP_PIN=0 turns it off (A/B).
    const char *pin = getenv("TOTK_VK_SUBGROUP_PIN");
    if (p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32 && p13.minSubgroupSize != p13.maxSubgroupSize && !(pin && *pin == '0')) {
        vkc.subgroup_pin = 32;
        vkc.subgroup_pin_stages = p13.requiredSubgroupSizeStages;
        // a compute workgroup must fit in maxComputeWorkgroupSubgroups subgroups of 32: where the largest would not,
        // compute shaders keep the driver's size
        if ((uint64_t)p13.maxComputeWorkgroupSubgroups * 32 < vkc.props.limits.maxComputeWorkGroupInvocations)
            vkc.subgroup_pin_stages &= ~(VkShaderStageFlags)VK_SHADER_STAGE_COMPUTE_BIT;
    }
    LOG("subgroup size %u..%u; pinned to %u for stages %#x", p13.minSubgroupSize, p13.maxSubgroupSize, vkc.subgroup_pin,
        (unsigned)vkc.subgroup_pin_stages);
    LOG("GPU: %s (Vulkan %u.%u.%u, driver %#x)", vkc.props.deviceName, VK_API_VERSION_MAJOR(vkc.props.apiVersion),
        VK_API_VERSION_MINOR(vkc.props.apiVersion), VK_API_VERSION_PATCH(vkc.props.apiVersion), vkc.props.driverVersion);
    return 1;
}

// Feature structs: queried with everything the GPU supports and passed back unchanged to vkCreateDevice.
static VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
static VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
static VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
static VkPhysicalDeviceExtendedDynamicState3FeaturesEXT feds3 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
static VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT fvids = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
static VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT};
static VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
static VkPhysicalDeviceDepthClipControlFeaturesEXT fdcc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
static VkPhysicalDeviceDepthClipEnableFeaturesEXT fdce = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
static VkPhysicalDeviceCustomBorderColorFeaturesEXT fcbc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
static VkPhysicalDeviceFaultFeaturesEXT ffault = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
static VkPhysicalDeviceProvokingVertexFeaturesEXT fpv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
static VkPhysicalDeviceConditionalRenderingFeaturesEXT fcr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT};
static VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
static VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT fplr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT};
static VkPhysicalDeviceShaderObjectFeaturesEXT fso = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
static VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT ffsi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT ffl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT};
static VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT ffld = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT};
static int has_fl, has_fld;
static VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
static int has_lr, has_cbc;

static int create_device(void) {
    ExtList ex = device_extensions(vkc.gpu);
    const char *names[32];
    uint32_t nnames = 0;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    f11.pNext = &f12;
    f12.pNext = &f13;
    void **tail = &f13.pNext;
    // extension name, its feature struct (NULL: none), the flag in vkc
    struct { const char *name; void *features; int *flag; } want[] = {
        {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, NULL, NULL},
        {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, &fdb, &vkc.has_descriptor_buffer},
        {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, NULL, NULL},
        {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, &fgpl, &vkc.has_gpl},
        {VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, &feds3, &vkc.has_eds3},
        {VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, &fvids, &vkc.has_vertex_input_ds},
        {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, &fdcc, &vkc.has_depth_clip_control},
        {VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, &fdce, NULL},
        {VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, &fcbc, &has_cbc},
        {VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, &fpv, NULL},
        {VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME, &fcr, NULL},
        {VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, &fu8, NULL},
        {VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME, &fplr, NULL},
        {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, &fso, &vkc.has_shader_object},
        {VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME, &ffsi, &vkc.has_interlock},
        // TOTK samples textures that are attachments of the same pass (G-buffer decals, depth-reading effects)
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME, &ffl, &has_fl},
        {VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME, &ffld, &has_fld},
        // ...and a draw that samples a target earlier draws of the pass wrote waits for them inside the pass
        {VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, &flr, &has_lr},
        {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, NULL, &vkc.has_memory_budget},   // the memory log line
        // development (TOTK_VK_CHECKPOINTS=1): the markers a lost device reached
        {VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, NULL, &vkc.has_checkpoints},
        // ...and written by the GPU to host memory, which outlives the device (the NVIDIA driver gave 0 checkpoints, 2026-10-27)
        {VK_AMD_BUFFER_MARKER_EXTENSION_NAME, NULL, &vkc.has_buffer_marker},
        // the driver's own report of a lost device: what failed and where (AMD, Mesa; as totk-pc-recomp 112fb2d)
        {VK_EXT_DEVICE_FAULT_EXTENSION_NAME, &ffault, &vkc.has_device_fault},
    };
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
        if (!strcmp(want[i].name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && !vkc.surface) continue;
        if ((want[i].flag == &vkc.has_checkpoints || want[i].flag == &vkc.has_buffer_marker) && !getenv("TOTK_VK_CHECKPOINTS")) continue;
        if (!ext_in(&ex, want[i].name)) { LOG("extension %s not available", want[i].name); continue; }
        names[nnames++] = want[i].name;
        if (want[i].flag) *want[i].flag = 1;
        if (want[i].features) { *tail = want[i].features; tail = (void **)&((VkBaseOutStructure *)want[i].features)->pNext; }
    }
    free(ex.e);
    vk.vkGetPhysicalDeviceFeatures2(vkc.gpu, &f2);
    vkc.has_shader_object = vkc.has_shader_object && fso.shaderObject;
    if (!vkc.has_shader_object) {
        LOG("VK_EXT_shader_object is required but is not supported by this GPU or driver");
        return 0;
    }
    // the shaders' interlock is the pixel one (vkb_spv_drop_interlock removes it where this is missing)
    vkc.has_interlock = vkc.has_interlock && ffsi.fragmentShaderPixelInterlock;
    if (!vkc.has_interlock) LOG("no fragment shader pixel interlock: shaders that use it run without it");
    // Without the dynamic state (AMD's Windows driver) the shader objects cannot declare the loop: the layout alone is
    // used, which the Vulkan spec leaves undefined there but AMD's driver samples right (PR #36 by Moran45, Radeon 780M).
    vkc.has_feedback_loop = has_fl && ffl.attachmentFeedbackLoopLayout;
    vkc.has_feedback_dynamic = vkc.has_feedback_loop && has_fld && ffld.attachmentFeedbackLoopDynamicState;
    vkc.has_local_read = has_lr && flr.dynamicRenderingLocalRead;
    // TOTK_VK_NO_FEEDBACK (development): =1 as AMD's Windows driver (the layout without the dynamic state, no barriers
    // inside a pass); =2 without feedback loops at all (what is done then: TOTK_VK_FB_FALLBACK, vkb_draw.c)
    const char *nf = getenv("TOTK_VK_NO_FEEDBACK");
    if (nf && *nf && *nf != '0') {
        vkc.has_feedback_dynamic = vkc.has_local_read = 0;
        if (*nf == '2') vkc.has_feedback_loop = 0;
    }
    ffl.attachmentFeedbackLoopLayout = (VkBool32)vkc.has_feedback_loop;   // the features enabled are the ones used
    ffld.attachmentFeedbackLoopDynamicState = (VkBool32)vkc.has_feedback_dynamic;
    flr.dynamicRenderingLocalRead = (VkBool32)vkc.has_local_read;
    if (!vkc.has_feedback_loop) LOG("no attachment feedback loops: a target sampled by its own draw is attached in GENERAL (TOTK_VK_FB_FALLBACK)");
    else if (!vkc.has_feedback_dynamic) LOG("attachment feedback loops without the dynamic state: the layout alone");
    if (!vkc.has_local_read) LOG("no barriers inside a pass: a pass that drew ends before a draw sampling its target");
    if (!f13.subgroupSizeControl && vkc.subgroup_pin) {
        LOG("no subgroup size control: shaders run in the driver's subgroup size");
        vkc.subgroup_pin = 0;
    }
    // NVN samplers take any border colour (TOTK clamps the grass interaction map to 0.5 grey); the samplers are made
    // before the texture is known, so the colour must work without a format
    vkc.has_custom_border = has_cbc && fcbc.customBorderColors && fcbc.customBorderColorWithoutFormat;
    vkc.has_device_fault = vkc.has_device_fault && ffault.deviceFault;
    ffault.deviceFaultVendorBinary = VK_FALSE;   // the counts and addresses are enough for the log
    if (vkc.has_custom_border) {
        VkPhysicalDeviceCustomBorderColorPropertiesEXT cbp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &cbp};
        vk.vkGetPhysicalDeviceProperties2(vkc.gpu, &p2);
        vkc.max_custom_border = cbp.maxCustomBorderColorSamplers;
        LOG("custom sampler border colours: up to %u samplers", vkc.max_custom_border);
    } else LOG("no custom sampler border colours: the nearest standard colour is used");
    fcbc.customBorderColors = (VkBool32)vkc.has_custom_border;   // the features enabled are the ones used
    fcbc.customBorderColorWithoutFormat = (VkBool32)vkc.has_custom_border;
    // TOTK_VK_ROBUST=1 (development): bounds-checked buffer access, to tell a shader's out-of-range access from other
    // causes of a lost device
    if (!getenv("TOTK_VK_ROBUST")) f2.features.robustBufferAccess = VK_FALSE;
    else LOG("robust buffer access %s (TOTK_VK_ROBUST)", f2.features.robustBufferAccess ? "on" : "not supported");
    vkc.has_descriptor_buffer = vkc.has_descriptor_buffer && fdb.descriptorBuffer;
    if (vkc.has_descriptor_buffer) fdb.descriptorBufferCaptureReplay = VK_FALSE;   // tools only, costs driver work
    if (vkc.has_descriptor_buffer)
        LOG("descriptor buffer: uniform %zu, storage %zu, combined image sampler %zu, storage image %zu bytes; offset alignment %llu; "
            "ranges resource %llu / sampler %llu", vkc.db.uniformBufferDescriptorSize, vkc.db.storageBufferDescriptorSize,
            vkc.db.combinedImageSamplerDescriptorSize, vkc.db.storageImageDescriptorSize, (unsigned long long)vkc.db.descriptorBufferOffsetAlignment,
            (unsigned long long)vkc.db.maxResourceDescriptorBufferRange, (unsigned long long)vkc.db.maxSamplerDescriptorBufferRange);
    fdb.descriptorBufferImageLayoutIgnored = fdb.descriptorBufferImageLayoutIgnored && vkc.has_descriptor_buffer;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vkc.family,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = nnames, .ppEnabledExtensionNames = names};
    if (!vk_check(vk.vkCreateDevice(vkc.gpu, &ci, NULL, &vkc.device), "vkCreateDevice")) return 0;
#define LOAD(f) vk.f = (PFN_##f)vk.vkGetDeviceProcAddr(vkc.device, #f);
    VK_FN_DEVICE(LOAD)
#undef LOAD
    vk.vkGetDeviceQueue(vkc.device, vkc.family, 0, &vkc.queue);
    LOG("device: %u extensions; descriptor buffer %d, pipeline library %d, dynamic state 3 %d, vertex input %d, "
        "timeline semaphores %d, host pointer alignment %#llx", nnames, vkc.has_descriptor_buffer, vkc.has_gpl,
        vkc.has_eds3, vkc.has_vertex_input_ds, f12.timelineSemaphore, (unsigned long long)vkc.host_pointer_alignment);
    if (!f12.timelineSemaphore || !f13.dynamicRendering || !f13.synchronization2) { LOG("GPU lacks Vulkan 1.3 core features"); return 0; }
    return 1;
}

int vk_init(SDL_Window *window) {
    static int state;   // 0 not tried, 1 ready, -1 failed
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (!state) state = create_instance(window) && pick_device() && create_device() ? 1 : -1;
    pthread_mutex_unlock(&mu);
    return state == 1;
}

/* After VK_ERROR_DEVICE_LOST (VK_EXT_device_fault): the driver's description of the fault, the GPU addresses it involved
   and the vendor's codes, into the log. Nothing when the driver has no report. */
void vk_device_fault_report(void) {
    if (!vkc.has_device_fault || !vk.vkGetDeviceFaultInfoEXT) return;
    VkDeviceFaultCountsEXT n = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (vk.vkGetDeviceFaultInfoEXT(vkc.device, &n, NULL) != VK_SUCCESS) { LOG("device fault: no report from the driver"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT ven[16];
    if (n.addressInfoCount > 16) n.addressInfoCount = 16;
    if (n.vendorInfoCount > 16) n.vendorInfoCount = 16;
    n.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = ven};
    VkResult r = vk.vkGetDeviceFaultInfoEXT(vkc.device, &n, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) { LOG("device fault: no report from the driver (%d)", (int)r); return; }
    LOG("device fault: %s", info.description[0] ? info.description : "(no description)");
    static const char *const kind[] = {"none", "invalid read", "invalid write", "invalid execute", "instruction pointer unknown",
                                       "instruction pointer invalid", "instruction pointer fault"};
    for (uint32_t i = 0; i < n.addressInfoCount; i++)
        LOG("  address %#llx (+-%#llx): %s", (unsigned long long)addr[i].reportedAddress,
            (unsigned long long)addr[i].addressPrecision, addr[i].addressType < 7 ? kind[addr[i].addressType] : "?");
    for (uint32_t i = 0; i < n.vendorInfoCount; i++)
        LOG("  vendor: %s (code %#llx, data %#llx)", ven[i].description, (unsigned long long)ven[i].vendorFaultCode,
            (unsigned long long)ven[i].vendorFaultData);
}
