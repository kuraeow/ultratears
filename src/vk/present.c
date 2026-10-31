// Vulkan presentation (see present.h). The functions are loaded through SDL's vkGetInstanceProcAddr, so the exe does
// not link vulkan-1 and starts (without a picture) on a machine with no Vulkan driver.
// The present thread waits for a frame from the game, acquires a swapchain image, fits the newest game frame into it
// (the replayer's output image) and presents it. The game never waits on the display here; its pacing comes from the vsync events (front_win.c).
// Mailbox when the surface offers it (the newest frame wins, no tearing), FIFO otherwise.
#include "present.h"
#include "vk.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t runtime_now_ns(void);          // host/guest.c
void plat_sleep_until_ns(uint64_t t);  // runtime/platform

#define LOG(...) (fprintf(stderr, "[totk-vk] " __VA_ARGS__), fputc('\n', stderr))
#define FRAMES 2          // frames in flight
#define MAX_IMAGES 8

static SDL_Window *g_window;
static VkSwapchainKHR swapchain;
static VkFormat format;
static VkExtent2D extent;
static VkImage images[MAX_IMAGES];
static uint32_t nimages;
static VkSemaphore image_ready[FRAMES], render_done[MAX_IMAGES];
static VkFence in_flight[FRAMES];
static VkCommandPool pool;
static VkCommandBuffer cmd[FRAMES];
static VkPresentModeKHR mode;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint64_t frames_in;           // under mu
static _Atomic uint64_t presented;
static _Atomic int resized;

// Command buffers and sync objects of the present thread (the device is vk.c's).
static int create_present_objects(void) {
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = vkc.family};
    if (!vk_check(vk.vkCreateCommandPool(vkc.device, &pi, NULL, &pool), "vkCreateCommandPool")) return 0;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
    if (!vk_check(vk.vkAllocateCommandBuffers(vkc.device, &ai, cmd), "vkAllocateCommandBuffers")) return 0;
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < FRAMES; i++)
        if (!vk_check(vk.vkCreateSemaphore(vkc.device, &si, NULL, &image_ready[i]), "vkCreateSemaphore") ||
            !vk_check(vk.vkCreateFence(vkc.device, &fi, NULL, &in_flight[i]), "vkCreateFence")) return 0;
    return 1;
}

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < nimages; i++) vk.vkDestroySemaphore(vkc.device, render_done[i], NULL);
    if (swapchain) vk.vkDestroySwapchainKHR(vkc.device, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    nimages = 0;
}

// 0: the window has no area now (minimised) — try again later
static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (!vk_check(vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vkc.gpu, vkc.surface, &caps), "surface capabilities")) return 0;
    extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return 0;
    uint32_t nf = 0;
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vkc.gpu, vkc.surface, &nf, NULL);
    VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof *fm);
    vk.vkGetPhysicalDeviceSurfaceFormatsKHR(vkc.gpu, vkc.surface, &nf, fm);
    VkSurfaceFormatKHR pick = fm[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { pick = fm[i]; break; }
    free(fm);
    format = pick.format;
    uint32_t nm = 0;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vkc.gpu, vkc.surface, &nm, NULL);
    VkPresentModeKHR modes[16];
    if (nm > 16) nm = 16;
    vk.vkGetPhysicalDeviceSurfacePresentModesKHR(vkc.gpu, vkc.surface, &nm, modes);
    // vsync= in ultratears.cfg (TOTK_VSYNC): on (default) shows whole frames only, mailbox where the driver has it
    // (the game is not held to the display) else fifo; off presents at once (immediate: lowest latency, may tear)
    const char *vs = getenv("TOTK_VSYNC");
    int vsync_off = vs && (!strcmp(vs, "0") || !strcmp(vs, "off"));
    mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = VK_PRESENT_MODE_MAILBOX_KHR;
    if (vsync_off)
        for (uint32_t i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vkc.surface,
        .minImageCount = count, .imageFormat = format, .imageColorSpace = pick.colorSpace, .imageExtent = extent,
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = VK_TRUE};
    if (!vk_check(vk.vkCreateSwapchainKHR(vkc.device, &ci, NULL, &swapchain), "vkCreateSwapchainKHR")) return 0;
    nimages = MAX_IMAGES;
    vk.vkGetSwapchainImagesKHR(vkc.device, swapchain, &nimages, images);
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < nimages; i++) vk.vkCreateSemaphore(vkc.device, &si, NULL, &render_done[i]);
    LOG("swapchain %ux%u, %u images, %s", extent.width, extent.height, nimages,
        mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst, VkPipelineStageFlags s0, VkPipelineStageFlags s1) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vk.vkCmdPipelineBarrier(cb, s0, s1, 0, 0, NULL, 0, NULL, 1, &b);
}

// Until the first game frame (and for the null device): a colour that cycles with the frame number.
static VkClearColorValue frame_colour(uint64_t n) {
    float t = (float)(n % 240) / 240.0f * 6.2831853f;
    return (VkClearColorValue){{0.15f + 0.1f * sinf(t), 0.15f + 0.1f * sinf(t + 2.094f), 0.2f + 0.1f * sinf(t + 4.188f), 1}};
}

// ---- output images: written by the replay thread, shown by the present thread
#define OUTS 3
typedef struct {
    VkImage img; VkDeviceMemory mem; uint32_t w, h;
    int state;                   // 0 free, 1 written (pending), 2 being shown
    uint64_t seq;                // when it was written (newest pending wins)
    VkSemaphore timeline; uint64_t value; int crop[4];
    uint64_t not_before;         // shown no earlier than this (runtime_now_ns; the pacer's slot)
} out_t;
static out_t g_out[OUTS];
static uint64_t g_out_seq;       // under mu

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int vkp_active(void) { return swapchain != VK_NULL_HANDLE || g_window != NULL; }

int vkp_out_acquire(uint32_t w, uint32_t h, VkImage *image) {
    pthread_mutex_lock(&mu);
    int pick = -1;
    for (int i = 0; i < OUTS && pick < 0; i++) if (g_out[i].state == 0) pick = i;
    if (pick < 0)   // none free: overwrite the oldest pending one (it was never shown; newer frames win)
        for (int i = 0; i < OUTS; i++) if (g_out[i].state == 1 && (pick < 0 || g_out[i].seq < g_out[pick].seq)) pick = i;
    if (pick >= 0) g_out[pick].state = 3;   // being written
    pthread_mutex_unlock(&mu);
    if (pick < 0) return -1;
    out_t *o = &g_out[pick];
    if (o->img && (o->w != w || o->h != h)) {   // free: no present work uses it
        vk.vkDestroyImage(vkc.device, o->img, NULL);
        vk.vkFreeMemory(vkc.device, o->mem, NULL);
        o->img = VK_NULL_HANDLE;
    }
    if (!o->img) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,   // sampled: FSR (fsr.c)
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements req;
        if (!vk_check(vk.vkCreateImage(vkc.device, &ci, NULL, &o->img), "output image")) goto fail;
        vk.vkGetImageMemoryRequirements(vkc.device, o->img, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (!vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &o->mem), "output memory")) {
            vk.vkDestroyImage(vkc.device, o->img, NULL);
            o->img = VK_NULL_HANDLE;
            goto fail;
        }
        vk.vkBindImageMemory(vkc.device, o->img, o->mem, 0);
        o->w = w; o->h = h;
    }
    *image = o->img;
    return pick;
fail:
    pthread_mutex_lock(&mu);
    o->state = 0;
    pthread_mutex_unlock(&mu);
    return -1;
}

void vkp_out_ready(int slot, VkSemaphore timeline, uint64_t value, const int crop[4], uint64_t not_before) {
    pthread_mutex_lock(&mu);
    out_t *o = &g_out[slot];
    o->timeline = timeline; o->value = value; o->seq = ++g_out_seq; o->state = 1; o->not_before = not_before;
    if (crop) memcpy(o->crop, crop, sizeof o->crop); else memset(o->crop, 0, sizeof o->crop);
    for (int i = 0; i < OUTS; i++)   // older pending frames are never shown
        if (i != slot && g_out[i].state == 1 && g_out[i].seq < o->seq) g_out[i].state = 0;
    frames_in++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

// One window frame: the newest game frame (out, may be NULL: the cycling colour) fitted with its aspect ratio.
static int present_one(uint64_t n, unsigned slot, out_t *out) {
    if (atomic_exchange(&resized, 0) || !swapchain) {
        vk.vkWaitForFences(vkc.device, FRAMES, in_flight, VK_TRUE, UINT64_MAX);   // the present thread's own work only
        destroy_swapchain();
        if (!create_swapchain()) return 0;
    }
    vk.vkWaitForFences(vkc.device, 1, &in_flight[slot], VK_TRUE, UINT64_MAX);
    uint32_t idx;
    VkResult r = vk.vkAcquireNextImageKHR(vkc.device, swapchain, UINT64_MAX, image_ready[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { atomic_store(&resized, 1); return 0; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_check(r, "vkAcquireNextImageKHR"); return 0; }
    vk.vkResetFences(vkc.device, 1, &in_flight[slot]);
    VkCommandBuffer cb = cmd[slot];
    vk.vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vk.vkBeginCommandBuffer(cb, &bi);
    barrier(cb, images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue c = out ? (VkClearColorValue){{0, 0, 0, 1}} : frame_colour(n);   // bars around the game picture
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    if (out) {
        // source rectangle: the game's crop (NVN WindowCrop; 0 size = the whole texture)
        int sx = out->crop[0], sy = out->crop[1], sw = out->crop[2], sh = out->crop[3];
        if (sw <= 0 || sh <= 0 || sx + sw > (int)out->w || sy + sh > (int)out->h) { sx = sy = 0; sw = (int)out->w; sh = (int)out->h; }
        double scale = (double)extent.width / sw < (double)extent.height / sh ? (double)extent.width / sw : (double)extent.height / sh;
        int dw = (int)(sw * scale + 0.5), dh = (int)(sh * scale + 0.5), dx = ((int)extent.width - dw) / 2, dy = ((int)extent.height - dh) / 2;
        barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        // a window larger than the game's picture: FSR 1 (fsr.c), else (or when it is off) the bilinear blit
        int vk_fsr_record(VkCommandBuffer cb, VkImage src, uint32_t sw, uint32_t sh, const int crop[4], VkImage dst, int dx,
                          int dy, uint32_t dw, uint32_t dh);
        int rect[4] = {sx, sy, sw, sh};
        if (!vk_fsr_record(cb, out->img, out->w, out->h, rect, images[idx], dx, dy, (uint32_t)dw, (uint32_t)dh)) {
            VkImageBlit b = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{sx, sy, 0}, {sx + sw, sy + sh, 1}},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
            vk.vkCmdBlitImage(cb, out->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1, &b, VK_FILTER_LINEAR);
        }
    }
    barrier(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vk.vkEndCommandBuffer(cb);
    // wait for the swapchain image and, with a game frame, for the replayer's copy into the output image
    VkSemaphoreSubmitInfo waits[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = image_ready[slot], .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = out ? out->timeline : VK_NULL_HANDLE, .value = out ? out->value : 0,
         .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT}};   // FSR reads it in compute
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = render_done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = out ? 2u : 1u, .pWaitSemaphoreInfos = waits,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &render_done[idx], .swapchainCount = 1, .pSwapchains = &swapchain,
                           .pImageIndices = &idx};
    vk_queue_lock();   // the replayer submits on the same queue
    VkResult s = vk.vkQueueSubmit2(vkc.queue, 1, &si, in_flight[slot]);
    r = s == VK_SUCCESS ? vk.vkQueuePresentKHR(vkc.queue, &pi) : s;
    vk_queue_unlock();
    if (!vk_check(s, "vkQueueSubmit2")) return 0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) atomic_store(&resized, 1);
    else if (r != VK_SUCCESS) { vk_check(r, "vkQueuePresentKHR"); return 0; }
    if (out) vk.vkWaitForFences(vkc.device, 1, &in_flight[slot], VK_TRUE, UINT64_MAX);   // the output image is free again after this
    atomic_fetch_add(&presented, 1);
    return 1;
}

static void *present_thread(void *unused) {
    (void)unused;
    uint64_t shown = 0;
    unsigned slot = 0;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (frames_in == shown) pthread_cond_wait(&cv, &mu);
        shown = frames_in;   // frames that came while the last one was shown are skipped: only the newest counts
        int pick = -1;
        for (int i = 0; i < OUTS; i++) if (g_out[i].state == 1 && (pick < 0 || g_out[i].seq > g_out[pick].seq)) pick = i;
        if (pick >= 0) g_out[pick].state = 2;
        pthread_mutex_unlock(&mu);
        /* The steady rate (host/frame_rate.c): a frame drawn before its slot waits for it here, so frames reach the
           screen one period apart whatever their work; a frame more than half a second away is a clock change. */
        if (pick >= 0 && g_out[pick].not_before) {
            uint64_t at = g_out[pick].not_before, now = runtime_now_ns();
            if (at > now && at - now < 500000000ull) plat_sleep_until_ns(at);
        }
        if (present_one(shown, slot, pick >= 0 ? &g_out[pick] : NULL)) slot = (slot + 1) % FRAMES;
        if (pick >= 0) {
            pthread_mutex_lock(&mu);
            g_out[pick].state = 0;
            pthread_mutex_unlock(&mu);
        }
    }
    return NULL;
}

int vkp_init(SDL_Window *window) {
    g_window = window;
    if (!vk_init(window) || !vkc.surface || !create_present_objects()) return 0;
    if (!create_swapchain()) LOG("window has no area yet; swapchain deferred");
    pthread_t t;
    if (pthread_create(&t, NULL, present_thread, NULL)) { LOG("present thread"); return 0; }
    return 1;
}

void vkp_frame(void) {
    pthread_mutex_lock(&mu);
    frames_in++;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

void vkp_resized(void) { atomic_store(&resized, 1); }
uint64_t vkp_presented(void) { return atomic_load(&presented); }
uint64_t vkp_frames(void) {
    pthread_mutex_lock(&mu);
    uint64_t n = frames_in;
    pthread_mutex_unlock(&mu);
    return n;
}
