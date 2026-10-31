// The frame counter (F10, fps_overlay=1 in ultratears.cfg: TOTK_FPS_OVERLAY): the game's frames per second and their
// average time, over half a second, in the window's top right corner (the top left holds the game's hearts). The text is drawn on the CPU with a 5x7 pixel font
// of our own into a small host-visible buffer per frame in flight and copied into the swapchain image after the picture
// (present.c present_one): a copy of a few kilobytes, no pipeline.
#include "vk.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t runtime_now_ns(void);   // host/guest.c

enum { SLOTS = 2, MAX_W = 1024, MAX_H = 160 };
static _Atomic int g_on = -1;    // -1: TOTK_FPS_OVERLAY not read yet
static VkBuffer g_buf[SLOTS];
static VkDeviceMemory g_mem[SLOTS];
static uint8_t *g_map[SLOTS];
static uint64_t g_t0, g_frames;
static char g_text[32] = "-- FPS";

static int on(void) {
    int v = atomic_load(&g_on);
    if (v < 0) {
        const char *e = getenv("TOTK_FPS_OVERLAY");
        v = e && *e && strcmp(e, "0") != 0;
        atomic_store(&g_on, v);
    }
    return v;
}
void vk_overlay_toggle(void) {   // F10: also kept in ultratears.cfg (host/boot.c), as the launcher's setting shows it
    int v = !on();
    atomic_store(&g_on, v);
    void totk_cfg_set(const char *key, const char *value);
    totk_cfg_set("fps_overlay", v ? "1" : "");
}

// At every game frame the present thread shows.
void vk_overlay_frame(void) {
    uint64_t now = runtime_now_ns();
    if (!g_t0) { g_t0 = now; return; }
    g_frames++;
    if (now - g_t0 < 500000000ull) return;
    double s = (now - g_t0) * 1e-9;
    snprintf(g_text, sizeof g_text, "%.0f FPS  %.1f ms", g_frames / s, s * 1e3 / g_frames);
    g_t0 = now; g_frames = 0;
}

// 5x7 glyphs, a row a byte, bit 4 the leftmost pixel.
static const uint8_t *glyph(char c) {
    static const uint8_t digits[10][7] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
        {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0x0c, 0x0c}, dash[7] = {0, 0, 0, 0x1f, 0, 0, 0},
        F[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}, P[7] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
        S[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}, m[7] = {0, 0, 0x1a, 0x15, 0x15, 0x11, 0x11},
        s[7] = {0, 0, 0x0e, 0x10, 0x0e, 0x01, 0x1e}, none[7] = {0};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '.': return dot; case '-': return dash; case 'F': return F; case 'P': return P; case 'S': return S;
    case 'm': return m; case 's': return s; default: return none;
    }
}

static int buffers(void) {
    if (g_buf[0]) return 1;
    for (int i = 0; i < SLOTS; i++) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MAX_W * MAX_H * 4,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkMemoryRequirements req;
        if (vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_buf[i]) != VK_SUCCESS) return 0;
        vk.vkGetBufferMemoryRequirements(vkc.device, g_buf[i], &req);
        uint32_t type = UINT32_MAX;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t t = 0; t < vkc.memory.memoryTypeCount; t++)
            if ((req.memoryTypeBits & (1u << t)) && (vkc.memory.memoryTypes[t].propertyFlags & want) == want) { type = t; break; }
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        if (type == UINT32_MAX || vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_mem[i]) != VK_SUCCESS ||
            vk.vkBindBufferMemory(vkc.device, g_buf[i], g_mem[i], 0) != VK_SUCCESS ||
            vk.vkMapMemory(vkc.device, g_mem[i], 0, VK_WHOLE_SIZE, 0, (void **)&g_map[i]) != VK_SUCCESS) {
            g_buf[0] = VK_NULL_HANDLE;
            return 0;
        }
    }
    return 1;
}

/* After the picture, with dst in TRANSFER_DST_OPTIMAL (written by the blit or FSR before): the counter's box copied
   into its top right corner. slot: the present thread's frame in flight (its fence was waited for). */
void vk_overlay_record(VkCommandBuffer cb, VkImage dst, VkExtent2D ext, VkFormat fmt, unsigned slot) {
    if (!on() || slot >= SLOTS || (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_UNORM) || !buffers()) return;
    uint32_t px = ext.height >= 360 ? ext.height / 360 : 1;   // a font pixel: 4 screen pixels at 1440p, 2 at 720p
    size_t n = strlen(g_text);
    // the box at the right edge: its right side stays put as the digits change
    uint32_t pad = 3 * px, w = (uint32_t)n * 6 * px - px + 2 * pad, h = 7 * px + 2 * pad, y0 = 8 * px;
    uint32_t x0 = ext.width > w + 8 * px ? ext.width - w - 8 * px : 0;
    if (w > MAX_W || h > MAX_H || x0 + w > ext.width || y0 + h > ext.height) return;
    uint8_t *p = g_map[slot];
    int bgr = fmt == VK_FORMAT_B8G8R8A8_UNORM;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 4;
            int lit = 0;
            if (x >= pad && y >= pad && y < pad + 7 * px) {
                uint32_t cx = (x - pad) / px, cy = (y - pad) / px, ci = cx / 6, col = cx % 6;
                if (ci < n && col < 5) lit = glyph(g_text[ci])[cy] >> (4 - col) & 1;
            }
            uint8_t r = lit ? 0xf2 : 0x14, g = lit ? 0xf2 : 0x14, b = lit ? 0xf2 : 0x14;
            q[0] = bgr ? b : r; q[1] = g; q[2] = bgr ? r : b; q[3] = 0xff;
        }
    {   // TOTK_OVERLAY_DUMP=<file.ppm> (development): the box as drawn, once a counter is up
        static int done;
        const char *d = !done && strchr(g_text, 'F') && g_text[0] != '-' ? getenv("TOTK_OVERLAY_DUMP") : NULL;
        FILE *f = d ? fopen(d, "wb") : NULL;
        if (f) {
            fprintf(f, "P6 %u %u 255\n", w, h);
            for (uint32_t i = 0; i < w * h; i++) { uint8_t c[3] = {p[i * 4 + (bgr ? 2 : 0)], p[i * 4 + 1], p[i * 4 + (bgr ? 0 : 2)]}; fwrite(c, 1, 3, f); }
            fclose(f);
            done = 1;
        }
    }
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
    VkBufferImageCopy r = {.bufferRowLength = w, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .imageOffset = {(int32_t)x0, (int32_t)y0, 0}, .imageExtent = {w, h, 1}};
    vk.vkCmdCopyBufferToImage(cb, g_buf[slot], dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}
