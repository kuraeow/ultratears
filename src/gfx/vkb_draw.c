// Drawing for the Vulkan NVN replayer (milestone 3.3 step 5): the render state the command stream sets, the texture and
// sampler pools, descriptors, vertex input, draws and dispatches.
//  - State: what the frontend records (nvn_obj.h payloads), kept as NVN values and turned into Vulkan dynamic state at
//    each draw (shader objects: every state is dynamic). [temporary] All of it is set before every draw; skipping unchanged
//    state is a later optimisation measured against the profile.
//  - Descriptors: one set per draw from the submission's pool in the universal layout (vkb_shader.h). Constant buffers
//    c3..c16 are the game's uniform buffers (slot - 3), c1 the shader's own constants, c0 / c2 the NVN driver banks,
//    built here from the bound storage buffers and handles (the offsets of MacNativeGames' totk_xlate Service.cs).
//  - Textures and samplers: the game's texture / sampler pool registrations (mtl_reg_tex / mtl_reg_samp); a combined
//    handle is texture id | sampler id << 20.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb_shader.h"
#include "vkb_audit.h"
#include "vkb_lock.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

// ---------------- upload memory and descriptor pools, per command slot ----------------
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size, used; } chunk_t;
typedef struct { chunk_t c[32]; int n, cur; VkDescriptorPool pool[64]; int npool, curpool; } slot_res;
static slot_res g_res[VKB_SLOTS][VKB_MAXREC];   // per slot and recording thread (vkb_mt.c)
static struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize used; } g_mpool[VKB_SLOTS];   // device memory for mirrors
static uint32_t g_ring_type = UINT32_MAX;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vkc.memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vkc.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int chunk_new(chunk_t *c, VkDeviceSize size) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &c->buf), "upload buffer")) return 0;
    VkMemoryRequirements req;
    vk.vkGetBufferMemoryRequirements(vkc.device, c->buf, &req);
    if (g_ring_type == UINT32_MAX) {   // device-local and mapped where the GPU offers it (resizable BAR), else host memory
        g_ring_type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (g_ring_type != UINT32_MAX && vkc.memory.memoryHeaps[vkc.memory.memoryTypes[g_ring_type].heapIndex].size < (1ull << 30))
            g_ring_type = UINT32_MAX;   // the 256 MiB window without resizable BAR: keep it for the driver
        if (g_ring_type == UINT32_MAX) g_ring_type = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};   // descriptor buffers
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size, .memoryTypeIndex = g_ring_type};
    if (!vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &c->mem), "upload memory")) return 0;
    vk.vkBindBufferMemory(vkc.device, c->buf, c->mem, 0);
    void *m;
    if (!vk_check(vk.vkMapMemory(vkc.device, c->mem, 0, VK_WHOLE_SIZE, 0, &m), "map upload memory")) return 0;
    c->map = m; c->size = size; c->used = 0;
    return 1;
}

static __thread uint64_t g_rec;   // the command buffer this thread records (its caches below compare against it)
static _Atomic uint64_t g_rec_next;   // unique across threads
// Upload memory, descriptor memory and descriptor sets are this thread's for the whole submission: what refers to
// them is cached per submission (g_sub); what a command buffer has bound or set, per command buffer (g_rec).
static __thread uint64_t g_sub;
void vkb_rec_begin(void) { g_rec = atomic_fetch_add(&g_rec_next, 1) + 1; g_sub = mtl_last_fence() + 1; }
static int no_cache(void) {   // TOTK_VK_NO_RECORD_CACHE=1 (development): every draw records all its state again (A/B)
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_NO_RECORD_CACHE"); v = e && *e && *e != '0'; }
    return v;
}
void vkb_slot_reset(int slot) {   // the slot's previous submission has completed; a new one starts recording
    vkb_rec_begin();
    if (vkb_db_on()) vkb_db_slot_reset(slot);
    for (int w = 0; w < VKB_MAXREC; w++) {
        slot_res *r = &g_res[slot][w];
        for (int i = 0; i < r->n; i++) r->c[i].used = 0;
        r->cur = 0;
        for (int i = 0; i < r->npool; i++) vk.vkResetDescriptorPool(vkc.device, r->pool[i], 0);
        r->curpool = 0;
    }
    g_mpool[slot].used = 0;
}

void *vkb_ring_alloc(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    slot_res *r = &g_res[vkb_slot()][vkb_worker()];
    for (;;) {
        if (r->cur < r->n) {
            chunk_t *c = &r->c[r->cur];
            VkDeviceSize at = (c->used + align - 1) & ~(align - 1);
            if (at + n <= c->size) { c->used = at + n; *buf = c->buf; *off = at; return c->map + at; }
            r->cur++;
            continue;
        }
        if (r->n == 32) die("upload memory: 32 chunks (2 GiB) in one submission");
        if (!chunk_new(&r->c[r->n], n > (64u << 20) ? n : (64u << 20))) die("upload memory");
        r->n++;
    }
}

static VkDescriptorSet alloc_set(int compute) {
    slot_res *r = &g_res[vkb_slot()][vkb_worker()];
    VkDescriptorSetLayout layout = vkb_set_layout(compute);
    for (;;) {
        if (r->curpool < r->npool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = r->pool[r->curpool],
                                              .descriptorSetCount = 1, .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vk.vkAllocateDescriptorSets(vkc.device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            r->curpool++;
            continue;
        }
        if (r->npool == 64) die("descriptor pools: 64 in one submission");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048 * 2 * 18}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048 * 2 * 16},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048 * 2 * 24}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048 * 2 * 4}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2048, .poolSizeCount = 4, .pPoolSizes = sizes};
        if (!vk_check(vk.vkCreateDescriptorPool(vkc.device, &pi, NULL, &r->pool[r->npool]), "descriptor pool")) die("descriptor pool");
        r->npool++;
    }
}

// ---------------- the render state the command stream sets ----------------
static __thread struct {
    const prog_t *prog; uint64_t prog_serial; const vkb_shader *sh[6]; int prog_ok;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    int has_blend[8], has_color, has_chmask, has_ms, has_poly, has_ds;
    float vp[16][6]; int nvp; float dr[2]; int has_dr;
    p_scissor sc; int has_sc;
    struct { uint32_t value_mask, write_mask, ref; } stencil[2];   // front, back
    p_poly_offset po; float blend_color[4];
    uint32_t vattr[16]; int nvattr; vs_st vstream[16]; int nvstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][18], ssbo[6][16];
    uint64_t tex[6][32], img[6][8];
    int depth_clamp, raster_discard;
    const char *bad_key;   // the unsupported shader of a program that cannot be drawn (audit)
    int64_t copy_stride;
} S;   // this recording thread's (vkb_mt.c gives each chunk the state at its start)

void vkb_state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int f = 0; f < 2; f++) { S.stencil[f].value_mask = 0xFF; S.stencil[f].write_mask = 0xFF; }
    S.chmask.mask = 0xFFFFFFFF;
}
size_t vkb_state_size(void) { return sizeof S; }
void vkb_state_save(void *dst) { memcpy(dst, &S, sizeof S); }
void vkb_state_load(const void *src) { memcpy(&S, src, sizeof S); }

void vkb_state(uint32_t op, const uint8_t *a) {
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = (const p_bind_program *)a;
        S.prog = b->prog;
        S.prog_serial = 0;   // resolved at the next draw
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = (const p_bind_state *)a;
        switch (b->kind) {
        case 0: { blend_st s; memcpy(&s, b->bytes, sizeof s); S.blend[s.target & 7] = s; S.has_blend[s.target & 7] = 1; break; }
        case 1: memcpy(&S.color, b->bytes, sizeof S.color); S.has_color = 1; break;
        case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); S.has_chmask = 1; break;
        case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); S.has_ms = 1; break;
        case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); S.has_poly = 1; break;
        case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); S.has_ds = 1; break;
        }
        break;
    }
    case OP_VIEWPORTS: {
        const p_viewports *v = (const p_viewports *)a;
        for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], sizeof S.vp[0]);
        if (v->first + v->n > S.nvp) S.nvp = v->first + v->n > 16 ? 16 : v->first + v->n;
        break;
    }
    case OP_SCISSOR: memcpy(&S.sc, a, sizeof S.sc); S.has_sc = 1; break;
    case OP_DEPTH_RANGE: { const p_depth_range *d = (const p_depth_range *)a; S.dr[0] = d->n; S.dr[1] = d->f; S.has_dr = 1; break; }
    case OP_STENCIL: {
        const p_stencil *s = (const p_stencil *)a;
        for (int f = 0; f < 2; f++) {
            if (!(s->face & (1 << f))) continue;
            if (s->which == 0) S.stencil[f].value_mask = (uint32_t)s->value_mask;
            else if (s->which == 1) S.stencil[f].write_mask = (uint32_t)s->mask;
            else S.stencil[f].ref = (uint32_t)s->ref;
        }
        break;
    }
    case OP_POLY_OFFSET: memcpy(&S.po, a, sizeof S.po); break;
    case OP_BLEND_COLOR: memcpy(S.blend_color, a, sizeof S.blend_color); break;
    case OP_BIND_VATTR: {
        const p_bind_vattr *v = (const p_bind_vattr *)a;
        for (int i = 0; i < v->n && v->first + i < 16; i++) S.vattr[v->first + i] = v->st[i];
        S.nvattr = v->first + v->n > 16 ? 16 : v->first + v->n;
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *v = (const p_bind_vstream *)a;
        for (int i = 0; i < v->n && v->first + i < 16; i++) S.vstream[v->first + i] = v->st[i];
        S.nvstream = v->first + v->n > 16 ? 16 : v->first + v->n;
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *v = (const p_bind_vbuf *)a;
        for (int i = 0; i < v->n && v->first + i < 16; i++) S.vbuf[v->first + i] = v->r[i];
        break;
    }
    case OP_BIND_UBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index >= 0 && b->index < 18) S.ubo[b->stage][b->index] = *b; break; }
    case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
    case OP_BIND_TEX: case OP_BIND_IMG: {
        const p_bind_handles *b = (const p_bind_handles *)a;
        if (b->stage < 0 || b->stage >= 6) break;
        uint64_t *dst = op == OP_BIND_TEX ? S.tex[b->stage] : S.img[b->stage];
        int max = op == OP_BIND_TEX ? 32 : 8;
        for (int i = 0; i < b->n && b->first + i < max; i++) dst[b->first + i] = b->h[i];
        break;
    }
    case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value; break;
    case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value; break;
    case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
    default: break;
    }
}

// ---------------- texture and sampler pools ----------------
typedef struct { tex_t t; view_t v; uint8_t has_view, valid; } treg_t;
typedef struct { samp_par p; VkSampler s; uint8_t valid; } sreg_t;
static treg_t *g_treg;
static sreg_t *g_sreg;
static int g_ntreg, g_nsreg;
static vkb_rw g_reg_mu;   // game threads register while the recording threads read

static void *grow(void *a, int *n, int need, size_t elem) {
    if (need < *n) return a;
    int m = *n ? *n : 1024;
    while (m <= need) m *= 2;
    a = realloc(a, (size_t)m * elem);
    if (!a) die("pool registry");
    memset((uint8_t *)a + (size_t)*n * elem, 0, (size_t)(m - *n) * elem);
    *n = m;
    return a;
}
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    vkb_rw_write(&g_reg_mu, LK_TEXREG);
    g_treg = grow(g_treg, &g_ntreg, id, sizeof *g_treg);
    treg_t *r = &g_treg[id];
    r->valid = t != NULL;
    if (t) r->t = *t;
    r->has_view = v != NULL;
    if (v) r->v = *v;
    vkb_rw_write_end(&g_reg_mu);
}
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= (1 << 12)) return;
    vkb_rw_write(&g_reg_mu, LK_TEXREG);
    g_sreg = grow(g_sreg, &g_nsreg, id, sizeof *g_sreg);
    sreg_t *r = &g_sreg[id];
    VkSampler old = VK_NULL_HANDLE;
    if (r->s && (!p || memcmp(&r->p, p, sizeof *p))) { old = r->s; r->s = VK_NULL_HANDLE; }
    r->valid = p != NULL;
    if (p) r->p = *p;
    vkb_rw_write_end(&g_reg_mu);
    if (old) { mtl_finish(); vk.vkDestroySampler(vkc.device, old, NULL); }   // [temporary] waits for the GPU
}
void mtl_set_tex_pool_n(int n) { (void)n; }

static VkSamplerAddressMode wrap(uint8_t w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // CLAMP (GL legacy), CLAMP_TO_EDGE
    }
}
static VkSampler sampler(int id) {
    static VkSampler fallback;   // made before threads record (vkb_draw_mt_init)
    vkb_rw_read(&g_reg_mu, LK_TEXREG);
    int valid = id >= 0 && id < g_nsreg && g_sreg[id].valid;
    VkSampler have = valid ? g_sreg[id].s : VK_NULL_HANDLE;
    samp_par par;
    if (valid) par = g_sreg[id].p;
    vkb_rw_read_end(&g_reg_mu);
    if (have) return have;
    if (!valid) {
        if (!fallback) {
            VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                                      .maxLod = VK_LOD_CLAMP_NONE};
            vk.vkCreateSampler(vkc.device, &ci, NULL, &fallback);
        }
        return fallback;
    }
    const samp_par *p = &par;
    int mip = p->minf >= 2;
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : p->reduction == 2 ? VK_SAMPLER_REDUCTION_MODE_MAX
                                                                                                : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    // [temporary] border colour: the nearest standard one (custom border colours: VK_EXT_custom_border_color later)
    {
        const float *b = p->border;
        int standard = (b[0] == 0 && b[1] == 0 && b[2] == 0 && (b[3] == 0 || b[3] == 1)) || (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1);
        if (!standard && (p->ws == 5 || p->wt == 5 || p->wr == 5 || p->ws == 4 || p->wt == 4 || p->wr == 4)) vkb_audit(A_BORDER_APPROX, NULL);
    }
    VkBorderColor border = p->border[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : p->border[0] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &red,
        .magFilter = p->magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = p->minf == 1 || p->minf == 3 || p->minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = p->minf == 4 || p->minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr),
        .mipLodBias = p->lod_bias, .anisotropyEnable = p->aniso > 1.0f, .maxAnisotropy = p->aniso > 16.0f ? 16.0f : p->aniso,
        .compareEnable = p->cmp_mode == 1, .compareOp = (VkCompareOp)(p->cmp_func ? p->cmp_func - 1 : 7),
        .minLod = p->lod_min, .maxLod = mip ? p->lod_max : 0.25f,   // no mipmap filter: the base level only (GL)
        .borderColor = border};
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    VkSampler s;
    if (!vk_check(vk.vkCreateSampler(vkc.device, &ci, NULL, &s), "sampler")) return sampler(-1);
    vkb_rw_write(&g_reg_mu, LK_TEXREG);
    int kept = id < g_nsreg && g_sreg[id].valid && !memcmp(&g_sreg[id].p, &par, sizeof par) && !g_sreg[id].s;
    if (kept) g_sreg[id].s = s;
    else if (id < g_nsreg && g_sreg[id].valid && !memcmp(&g_sreg[id].p, &par, sizeof par) && g_sreg[id].s) {   // another thread made one meanwhile
        VkSampler other = g_sreg[id].s;
        vkb_rw_write_end(&g_reg_mu);
        vk.vkDestroySampler(vkc.device, s, NULL);
        return other;
    }
    vkb_rw_write_end(&g_reg_mu);
    return s;   // [temporary] a sampler re-registered meanwhile leaks this one
}

// A 1x1 black image for unbound texture slots, one per view type. Created (cleared) outside a pass: dummy_init.
static VkImage g_dummy;
static void dummy_init(void) {
    VkImage *imgp = &g_dummy;
    if (*imgp) return;
    {
        VkImage img;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 6,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        vk.vkCreateImage(vkc.device, &ci, NULL, &img);
        VkMemoryRequirements req;
        vk.vkGetImageMemoryRequirements(vkc.device, img, &req);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory m;
        vk.vkAllocateMemory(vkc.device, &ai, NULL, &m);
        vk.vkBindImageMemory(vkc.device, img, m, 0);
        VkCommandBuffer cb = vkb_cmd();   // cleared to 0 and left in GENERAL (sampled and storage)
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
        vk.vkCmdPipelineBarrier2(cb, &di);
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
        vk.vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vk.vkCmdPipelineBarrier2(cb, &di);
        *imgp = img;
    }
}
static VkImageView g_dummy_views[8];   // made before threads record (vkb_draw_mt_init)
static VkImageView dummy_view(VkImageViewType type) {
    VkImageView *views = g_dummy_views;
    VkImage img = g_dummy;
    if (type > VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) type = VK_IMAGE_VIEW_TYPE_2D;
    if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // [temporary] a 2D image cannot have a 3D view
    if (!views[type]) {
        uint32_t layers = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : 1;
        VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = type, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
        vk.vkCreateImageView(vkc.device, &ci, NULL, &views[type]);
    }
    return views[type];
}

static VkImageViewType view_type_of(uint16_t t) {   // Ryujinx SamplerType
    int array = (t & 0x100) != 0;
    switch (t & 0xff) {
    case 1: return array ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return array ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

// The image a texture handle names (NULL: none / not an image yet); *view points to a copy of the pool entry's view.
static vkb_image *handle_image(uint64_t h, const view_t **view) {
    static __thread view_t views[64];
    static __thread unsigned next;
    int id = (int)(h & 0xFFFFF);
    vkb_rw_read(&g_reg_mu, LK_TEXREG);
    if (id <= 0 || id >= g_ntreg || !g_treg[id].valid) { vkb_rw_read_end(&g_reg_mu); *view = NULL; return NULL; }
    tex_t t = g_treg[id].t;
    view_t *v = NULL;
    if (g_treg[id].has_view) { v = &views[next++ & 63]; *v = g_treg[id].v; }
    vkb_rw_read_end(&g_reg_mu);
    *view = v;
    return vkb_image_of(&t);
}

// ---------------- sampled render targets ----------------
// TOTK samples textures bound as targets of the same draw (decals reading the G-buffer, effects reading depth). With
// VK_EXT_attachment_feedback_loop_layout such targets stay attached in the feedback loop layout and are sampled there;
// without it they would leave the pass, and the draw saw an empty texture instead (the audit's top substitution).
static __thread vkb_image *g_fb_rt[9];   // this draw's targets: colour 0-7, depth 8
static __thread uint32_t g_fb;           // the targets it samples (vkb.h VKB_FB_DEPTH)
extern int vk_feedback_loop_dynamic;     // vk.c: without it the loop is the layout alone (no state to set)

static int fb_index(const vkb_image *im) {   // the target bit of a sampled image, -1: not a target of this draw
    for (int i = 0; im && i < 9; i++) if (g_fb_rt[i] == im) return i;
    return -1;
}
static void feedback_scan(const vkb_shader *vs, const vkb_shader *fs) {
    g_fb = 0;
    memset(g_fb_rt, 0, sizeof g_fb_rt);
    if (!vkc.has_feedback_loop) return;
    const p_set_rt *rt = vkb_rt();
    int n = rt->n < 0 ? 0 : rt->n > 8 ? 8 : rt->n, any = 0;
    for (int i = 0; i < n; i++) if (rt->color[i]) any |= (g_fb_rt[i] = vkb_image_of(rt->color[i])) != NULL;
    if (rt->depth) any |= (g_fb_rt[8] = vkb_image_of(rt->depth)) != NULL;
    if (!any) return;
    for (int st = 0; st < 2; st++) {
        const vkb_shader *sh = st ? fs : vs;
        for (int i = 0; sh && i < sh->ntex; i++) {
            const view_t *v = NULL;
            int b = fb_index(sh->tex[i].separate ? NULL : handle_image(S.tex[st][sh->tex[i].slot], &v));
            if (b >= 0) g_fb |= 1u << b;
        }
    }
}
static int fb_sampled(const vkb_image *im) { int b = fb_index(im); return b >= 0 && ((g_fb >> b) & 1); }

// ---------------- buffers ----------------
static VkBuffer g_zero;   // 64 KiB of zeros for unbound buffers
static __thread const char *g_akey;   // the shader whose bindings are written (audit)
static VkBuffer g_support;
static void fixed_buffers(void) {
    if (g_zero) return;
    // Ryujinx SupportBuffer: alpha test off, no BGRA swaps, render scale 1 (73 entries from byte 192), 1392 bytes
    for (int k = 0; k < 2; k++) {
        VkBuffer *b = k ? &g_support : &g_zero;
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        vk.vkCreateBuffer(vkc.device, &bi, NULL, b);
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, *b, &req);
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory m;
        vk.vkAllocateMemory(vkc.device, &ai, NULL, &m);
        vk.vkBindBufferMemory(vkc.device, *b, m, 0);
        uint8_t *p;
        vk.vkMapMemory(vkc.device, m, 0, VK_WHOLE_SIZE, 0, (void **)&p);
        memset(p, 0, 65536);
        if (k) for (int i = 0; i < 73; i++) { float one = 1.0f; memcpy(p + 192 + 16 * i, &one, 4); }
    }
}

// ---------------- device-local mirrors of written storage buffers ----------------
// The game's memory pools are host memory imported into Vulkan (zero copy), so a shader that writes a storage buffer
// writes across PCIe; atomics to one address are serialised there (TOTK's sun-shadow statistics: 114 ms of GPU time for
// one 400x224 fullscreen draw, 2026-10-23). A written range gets a copy in device memory for the rest of the submission:
// copied in when a shader first writes it, used by every later GPU access to the range in that submission (storage and
// uniform buffers, vertex and index buffers, copies, updates), copied back once at its end. Before, each draw had a copy
// of its own, in and out: on the Great Sky Island ~360 draws a frame write one 128 KiB tile buffer, ~90 MB of copies a
// frame; the mirror memory ran out mid-frame and the rest wrote host memory (a 400x224 atomic pass took 59 ms: 6 FPS,
// 2026-10-24). An access that straddles a mirror's bounds sends the mirror back first (vkb_mirrors_evict).
// [temporary] The copy back writes the whole range: CPU writes to it while the submission is in flight are overwritten.
// Device-local pools with explicit CPU sync (NVN's Flush / InvalidateMappedRange) replace this (planned).
// The table is sorted by address; mirrors never overlap (a range that straddles one sends it back before another is made).
enum { MIRROR_POOL = 32u << 20, MIRROR_MAX = 4u << 20, MIRRORS = 1024 };
typedef struct { uint64_t addr, size; VkBuffer host, dev; VkDeviceSize hoff, doff; } mirror_t;
static mirror_t g_mir[MIRRORS];
static int g_nmir;
static uint64_t g_mirrors_made, g_mirrors_evicted;

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa, .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
}
// Mirror memory of the current submission's slot. The offset keeps the address's position within 256 bytes, so a
// range in the mirror meets the same offset alignment rules as in the imported pool (pools start on page boundaries).
static int mirror_alloc(uint64_t addr, VkDeviceSize n, VkBuffer *buf, VkDeviceSize *off) {
    int sl = vkb_slot();
    if (sl < 0) return 0;
    if (!g_mpool[sl].buf) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MIRROR_POOL,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        if (!vk_check(vk.vkCreateBuffer(vkc.device, &bi, NULL, &g_mpool[sl].buf), "mirror buffer")) return 0;
        VkMemoryRequirements req;
        vk.vkGetBufferMemoryRequirements(vkc.device, g_mpool[sl].buf, &req);
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (ai.memoryTypeIndex == UINT32_MAX || !vk_check(vk.vkAllocateMemory(vkc.device, &ai, NULL, &g_mpool[sl].mem), "mirror memory")) die("mirror memory");
        vk.vkBindBufferMemory(vkc.device, g_mpool[sl].buf, g_mpool[sl].mem, 0);
    }
    VkDeviceSize at = ((g_mpool[sl].used + 255) & ~(VkDeviceSize)255) + (addr & 255);
    if (at + n > MIRROR_POOL) return 0;
    g_mpool[sl].used = at + n;
    *buf = g_mpool[sl].buf;
    *off = at;
    return 1;
}
static int mirror_first(uint64_t addr) {   // index of the first mirror that ends after addr
    int lo = 0, hi = g_nmir;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_mir[m].addr + g_mir[m].size <= addr) lo = m + 1; else hi = m; }
    return lo;
}
static const mirror_t *mirror_find(uint64_t addr, uint64_t len) {   // the mirror that holds [addr, addr+len) entirely
    int k = mirror_first(addr);
    return k < g_nmir && addr >= g_mir[k].addr && addr + len <= g_mir[k].addr + g_mir[k].size ? &g_mir[k] : NULL;
}
static int mirrors_straddle(uint64_t addr, uint64_t len) {   // a mirror overlaps [addr, addr+len) without holding it
    int k = mirror_first(addr);
    return k < g_nmir && g_mir[k].addr < addr + len && !(addr >= g_mir[k].addr && addr + len <= g_mir[k].addr + g_mir[k].size);
}
static void mirror_add(const mirror_t *m) {
    int k = mirror_first(m->addr);
    memmove(&g_mir[k + 1], &g_mir[k], (size_t)(g_nmir - k) * sizeof g_mir[0]);
    g_mir[k] = *m;
    g_nmir++;
}
int vkb_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    const mirror_t *m = mirror_find(addr, len);
    if (m) { *buf = m->dev; *off = m->doff + (addr - m->addr); return 1; }
    return vkb_pool_addr(addr, len, buf, off);   // the pool's copy in device memory when current, else game memory
}
static void mirror_back(VkCommandBuffer cb, const mirror_t *m) {
    VkBufferCopy r = {m->doff, m->hoff, m->size};
    char at[32]; snprintf(at, sizeof at, "%#llx+%llu", (unsigned long long)m->addr, (unsigned long long)m->size);
    vkb_checkpoint("mirror out", at, NULL);
    vk.vkCmdCopyBuffer(cb, m->dev, m->host, 1, &r);
}
void vkb_mirrors_evict(uint64_t addr, uint64_t len) {
    VkCommandBuffer cb = vkb_cmd();
    int n = 0;
    for (int k = mirror_first(addr); k < g_nmir && g_mir[k].addr < addr + len;) {
        const mirror_t *m = &g_mir[k];
        if (addr >= m->addr && addr + len <= m->addr + m->size) { k++; continue; }
        if (!n++) barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        mirror_back(cb, m);
        memmove(&g_mir[k], &g_mir[k + 1], (size_t)(--g_nmir - k) * sizeof g_mir[0]);
        g_mirrors_evicted++;
        vkb_audit(A_MIRROR_EVICTED, NULL);
    }
    if (n) barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
void vkb_mirrors_submit(VkCommandBuffer cb) {
    if (!g_nmir) return;
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int k = 0; k < g_nmir; k++) mirror_back(cb, &g_mir[k]);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT);
    g_nmir = 0;
}

// A buffer range for a guest address: the imported pool (offset aligned as the descriptor type needs) or a copy now.
static VkDescriptorBufferInfo guest_range(uint64_t addr, uint64_t size, VkDeviceSize align, int copy_ok) {
    if (size > 65536 && align == vkc.props.limits.minUniformBufferOffsetAlignment) size = 65536;
    VkBuffer buf;
    VkDeviceSize off;
    if (addr && size && vkb_ucap_range(addr, size, &buf, &off) && !(off % align))   // contents as of the submit call (stores dropped it)
        return (VkDescriptorBufferInfo){buf, off, size};
    if (addr && size && vkb_gpu_addr(addr, size, &buf, &off)) {
        if (!(off % align)) return (VkDescriptorBufferInfo){buf, off, size};
        if (copy_ok) {   // [temporary] misaligned for this GPU: a copy of the current contents (later GPU-ordered updates missed)
            vkb_audit(A_UBO_COPIED, g_akey);
            VkBuffer rb;
            VkDeviceSize ro;
            void *p = vkb_ring_alloc(size, align, &rb, &ro);
            memcpy(p, (const void *)(uintptr_t)addr, size);
            return (VkDescriptorBufferInfo){rb, ro, size};
        }
    }
    return (VkDescriptorBufferInfo){g_zero, 0, 65536};
}

// The NVN driver bank c0 or c2 of a stage, as the shader reads it (bank 0: storage buffer table, compute uniform buffer
// table; bank 2: combined texture handles at 0x20, images at 0x120).
// Rebuilt only when what it holds changed since the last one of that bank and stage in this submission (four 4 KiB
// banks a draw were built and cleared before: ~96 MB a frame on the Great Sky Island).
typedef struct { uint64_t rec; VkBuffer b; VkDeviceSize off; uint8_t src[1024]; size_t n; } bank_t;
static __thread bank_t (*t_bank)[6];
static VkDescriptorBufferInfo driver_bank(int bank, int stage) {
    if (!t_bank && !(t_bank = calloc(2, sizeof *t_bank))) die("driver banks");
    bank_t (*last)[6] = t_bank;
    uint8_t src[1024];
    size_t n = 0;
    if (bank == 0) {
        memcpy(src, S.ssbo[stage], sizeof S.ssbo[stage]); n = sizeof S.ssbo[stage];
        if (stage == 5) { memcpy(src + n, S.ubo[5], sizeof S.ubo[5]); n += sizeof S.ubo[5]; }
    } else {
        memcpy(src, S.tex[stage], sizeof S.tex[stage]); n = sizeof S.tex[stage];
        memcpy(src + n, S.img[stage], sizeof S.img[stage]); n += sizeof S.img[stage];
    }
    int bi = bank ? 1 : 0;
    if (last[bi][stage].rec == g_sub && last[bi][stage].n == n && !memcmp(last[bi][stage].src, src, n) && !no_cache())
        return (VkDescriptorBufferInfo){last[bi][stage].b, last[bi][stage].off, 0x1000};
    VkBuffer b;
    VkDeviceSize off;
    uint8_t *p = vkb_ring_alloc(0x1000, vkc.props.limits.minUniformBufferOffsetAlignment, &b, &off);
    last[bi][stage].rec = g_sub; last[bi][stage].b = b; last[bi][stage].off = off; last[bi][stage].n = n;
    memcpy(last[bi][stage].src, src, n);
    memset(p, 0, 0x1000);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 1 ? 0x510 : 0x310;
        for (int r = 0; r < 16; r++) {
            uint32_t n = (uint32_t)S.ssbo[stage][r].size;
            memcpy(p + base + 16 * r, &S.ssbo[stage][r].addr, 8);
            memcpy(p + base + 16 * r + 8, &n, 4);
        }
        if (stage == 5)
            for (int r = 0; r < 16; r++) {
                uint32_t n = (uint32_t)S.ubo[5][r].size;
                memcpy(p + 0x210 + 16 * r, &S.ubo[5][r].addr, 8);
                memcpy(p + 0x210 + 16 * r + 8, &n, 4);
            }
    } else {
        for (int r = 0; r < 32; r++) memcpy(p + 0x20 + 8 * r, &S.tex[stage][r], 8);
        for (int r = 0; r < 8; r++) memcpy(p + 0x120 + 8 * r, &S.img[stage][r], 8);
    }
    return (VkDescriptorBufferInfo){b, off, 0x1000};
}

// ---------------- descriptors ----------------
typedef struct {
    VkWriteDescriptorSet w[128];
    VkDescriptorBufferInfo bi[128];
    VkDescriptorImageInfo ii[128];
    int n;
} writes_t;

// ---- the recording thread's own tables (heap: only recording threads have them)
#define SETS 8192
enum { PROGS = 8192 };
enum { D_VP, D_SC, D_RDISC, D_TOPO, D_RESTART, D_CULL, D_FRONT, D_POLY, D_BIAS_EN, D_BIAS, D_LINEW, D_CLAMP, D_CLIP, D_CLIPNEG,
       D_PROVOKE, D_SAMPLES, D_SMASK, D_A2C, D_A2ONE, D_DT, D_DW, D_DCMP, D_DBT, D_ST, D_SOP0, D_SOP1, D_SCM0, D_SCM1, D_SWM0,
       D_SWM1, D_SREF0, D_SREF1, D_BLEN, D_BEQ, D_WMASK, D_LOPEN, D_LOP, D_BCONST, D_FBLOOP, D_SHADERS, D_VINPUT, D_VB0,
       D_COUNT = D_VB0 + 16 };
typedef struct { const vkb_shader *sh; uint64_t rec; VkBuffer b; VkDeviceSize off; } c1_t;
typedef struct {
    struct { uint64_t rec, h1, h2; VkDescriptorSet set; } sets[2][SETS];
    // descriptor buffers (vkb_desc.c): the same reuse by contents, of offsets in the thread's descriptor memory
    struct { uint64_t rec, gen, h1, h2; VkDeviceSize off; } dsets[2][SETS];
    struct { uint64_t serial; const vkb_shader *sh[6]; const char *bad_key; int ok; } progs[PROGS];
    struct { uint64_t rec; uint8_t valid[D_COUNT]; uint16_t len[D_COUNT]; uint8_t v[D_COUNT][2048]; } dc;
    c1_t c1[1024];   // the shaders' own constants (c1) in the command buffer's upload memory
    writes_t writes;   // one draw at a time
} big_t;
static __thread big_t *B;
static void big_init(void) { if (!B && !(B = calloc(1, sizeof *B))) die("recording tables"); }

static void put_buf(writes_t *W, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkDescriptorBufferInfo info) {
    W->bi[W->n] = info;
    W->w[W->n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding,
                                         .descriptorCount = 1, .descriptorType = type, .pBufferInfo = &W->bi[W->n]};
    W->n++;
}
static void put_img(writes_t *W, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkDescriptorImageInfo info) {
    W->ii[W->n] = info;
    W->w[W->n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = binding,
                                         .descriptorCount = 1, .descriptorType = type, .pImageInfo = &W->ii[W->n]};
    W->n++;
}

// Textures and images a shader samples: uploads and layout changes (outside a pass). 1 = all set.
static void prepare_images(const vkb_shader *sh, int stage) {
    VkCommandBuffer cb = vkb_cmd();
    for (int i = 0; i < sh->ntex; i++) {
        const view_t *v = NULL;
        vkb_image *im = sh->tex[i].separate ? NULL : handle_image(S.tex[stage][sh->tex[i].slot], &v);
        if (im && im->refresh_from) vkb_tex_refresh(cb, im);
        if (!im || fb_sampled(im)) continue;   // a sampled target: the pass sets its layout
        vkb_tex_upload(cb, im);
        if (!vkb_layout_is(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL))
            vkb_image_use(cb, im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
    for (int i = 0; i < sh->nimg; i++) {
        const view_t *v = NULL;
        vkb_image *im = handle_image(S.img[stage][sh->img[i].slot], &v);
        if (im && (im->usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
            if (!vkb_layout_is(im, VK_IMAGE_LAYOUT_GENERAL))
                vkb_image_use(cb, im, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            vkb_image_written(im);
        }
    }
}
// Whether prepare_images would record anything (then a pass must end first). Threads recording: uploads go into the
// submission's prologue right here (vkb_tex_upload), so they do not end the pass.
static int images_need_work(const vkb_shader *sh, int stage) {
    int mt = vkb_mt_recording();
    for (int i = 0; i < sh->ntex; i++) {
        const view_t *v = NULL;
        vkb_image *im = sh->tex[i].separate ? NULL : handle_image(S.tex[stage][sh->tex[i].slot], &v);
        // render targets have no upload stamp: only their layout matters (this ended the pass at every draw before)
        if (!im || fb_sampled(im)) continue;
        if (mt) vkb_tex_upload(VK_NULL_HANDLE, im);
        if (im->refresh_from || !vkb_layout_is(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) return 1;
        if (!mt && ((!im->upload_stamp && !im->gpu_written) || vkb_tex_stale(im))) return 1;
    }
    for (int i = 0; i < sh->nimg; i++) {
        const view_t *v = NULL;
        vkb_image *im = handle_image(S.img[stage][sh->img[i].slot], &v);
        if (im && (im->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !vkb_layout_is(im, VK_IMAGE_LAYOUT_GENERAL)) return 1;
    }
    return 0;
}

// The guest range of a shader's storage buffer i (0 size: unknown / unbound).
static void sbuf_range(const vkb_shader *sh, int stage, int i, uint64_t *addr, uint64_t *size) {
    *addr = 0; *size = 0;
    uint8_t slot = sh->sbuf[i].slot;
    if (slot == 0xFF) return;
    if (sh->sbuf[i].kind == VKB_SB_TABLE) { *addr = S.ssbo[stage][slot].addr; *size = S.ssbo[stage][slot].size; }
    else if (sh->sbuf[i].kind == VKB_SB_UBO) { *addr = S.ubo[stage][slot].addr; *size = S.ubo[stage][slot].size; }
    else {   // a pointer inside the game's uniform buffer: {address, size}
        const p_bind_buf *u = &S.ubo[stage][slot];
        if (u->addr && sh->sbuf[i].off + 12 <= u->size) {
            uint32_t n;
            vkb_ucap_read(u->addr + sh->sbuf[i].off, addr, 8);
            vkb_ucap_read(u->addr + sh->sbuf[i].off + 8, &n, 4);
            *size = n;
        }
    }
}

// Before a draw / dispatch: accesses that straddle a mirror get game memory back, and the storage buffers the shader
// writes get a mirror for the rest of the submission (the pass ends only when commands are recorded here).
static void mirrors_prepare(const vkb_shader *sh, int stage) {
    static int off_env = -1;   // TOTK_VK_NO_MIRRORS=1 (development): written buffers stay in host memory
    if (off_env < 0) off_env = getenv("TOTK_VK_NO_MIRRORS") != NULL || vkb_shadow_on();   // pool copies take their place
    // [temporary] several threads recording: no mirrors (their order across chunks is not tracked); measure on the Deck
    int off = off_env || vkb_mt_recording();
    for (int w = 1; w >= 0; w--)   // pools in device memory: written ones first (they may end the pass)
        for (int i = 0; i < sh->nsbuf; i++) {
            uint64_t addr, size;
            if (sh->sbuf[i].write != w) continue;
            sbuf_range(sh, stage, i, &addr, &size);
            vkb_pool_use(addr, size, w);
            if (w && addr && size) vkb_ucap_drop(addr, size);   // a store: copied uniforms there are stale from now on
        }
    for (int slot = 3; slot < VKB_NCBUF; slot++)
        if (sh->cbuf_mask & (1u << slot)) vkb_pool_use(S.ubo[stage][slot - 3].addr, S.ubo[stage][slot - 3].size > 65536 ? 65536 : S.ubo[stage][slot - 3].size, 0);
    for (int i = 0; i < sh->nsbuf; i++) {
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        if (addr && size && mirrors_straddle(addr, size)) { vkb_pass_end_r(PE_MIRROR); vkb_mirrors_evict(addr, size); }
    }
    for (int slot = 3; slot < VKB_NCBUF; slot++) {
        const p_bind_buf *u = &S.ubo[stage][slot - 3];
        uint64_t size = u->size > 65536 ? 65536 : u->size;
        if ((sh->cbuf_mask & (1u << slot)) && u->addr && size && mirrors_straddle(u->addr, size)) { vkb_pass_end_r(PE_MIRROR); vkb_mirrors_evict(u->addr, size); }
    }
    int copied = 0;
    for (int i = 0; i < sh->nsbuf && !off; i++) {
        if (!sh->sbuf[i].write) continue;
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        if (!addr || !size || mirror_find(addr, size)) continue;
        int overlap = 0;   // two copies of one range would diverge: a range another of the shader's buffers reaches stays put
        for (int j = 0; j < sh->nsbuf && !overlap; j++) {
            uint64_t a2, s2;
            if (j == i) continue;
            sbuf_range(sh, stage, j, &a2, &s2);
            overlap = s2 && a2 < addr + size && addr < a2 + s2;
        }
        void *hb;
        uint64_t hoff;
        mirror_t m = {addr, size};
        const char *why = size > MIRROR_MAX ? "larger than a mirror" : overlap ? "overlaps another buffer of the shader"
                        : g_nmir >= MIRRORS ? "too many mirrors" : !mtl_addr(addr, size, &hb, &hoff) ? "not in an imported pool"
                        : !mirror_alloc(addr, size, &m.dev, &m.doff) ? "mirror memory full" : NULL;
        if (why) {   // the shader writes host memory across PCIe (atomics there are serialised: slow)
            vkb_audit(A_SSBO_HOST_WRITE, sh->key);
            static int said;
            if (said++ < 16) VLOG("[temporary] shader %.12s: written storage buffer %#llx+%llu stays in host memory: %s", sh->key ? sh->key : "?",
                                  (unsigned long long)addr, (unsigned long long)size, why);
            continue;
        }
        m.host = (VkBuffer)hb; m.hoff = hoff;
        VkCommandBuffer cb = vkb_cmd();
        if (!copied++) {
            vkb_pass_end_r(PE_MIRROR);
            barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        VkBufferCopy r = {m.hoff, m.doff, m.size};
        char at[32]; snprintf(at, sizeof at, "%#llx+%llu", (unsigned long long)addr, (unsigned long long)size);
        vkb_checkpoint("mirror in", at, sh->key);
        vk.vkCmdCopyBuffer(cb, m.host, m.dev, 1, &r);
        mirror_add(&m);
        g_mirrors_made++;
    }
    if (copied) barrier(vkb_cmd(), VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}
// Vertex and index buffers of a draw that straddle a mirror (outside a pass when it records).
static void mirrors_prepare_fixed(int indexed, const p_draw_idx *di) {
    for (uint32_t s = 0; s < 16; s++) vkb_pool_use(S.vbuf[s].address, S.vbuf[s].size, 0);
    if (indexed && di->ibuf) vkb_pool_use(di->ibuf, (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4), 0);
    for (uint32_t s = 0; s < 16; s++)
        if (S.vbuf[s].address && S.vbuf[s].size && mirrors_straddle(S.vbuf[s].address, S.vbuf[s].size)) {
            vkb_pass_end_r(PE_MIRROR); vkb_mirrors_evict(S.vbuf[s].address, S.vbuf[s].size);
        }
    if (indexed && di->ibuf) {
        uint64_t n = (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4);
        if (mirrors_straddle(di->ibuf, n)) { vkb_pass_end_r(PE_MIRROR); vkb_mirrors_evict(di->ibuf, n); }
    }
}

static void write_stage(writes_t *W, VkDescriptorSet set, const vkb_shader *sh, int stage) {
    uint32_t base = stage == 1 ? VKB_STAGE_BINDINGS : 0;
    VkDeviceSize ua = vkc.props.limits.minUniformBufferOffsetAlignment, sa = vkc.props.limits.minStorageBufferOffsetAlignment;
    g_akey = sh->key;
    put_buf(W, set, base + VKB_B_SUPPORT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (VkDescriptorBufferInfo){g_support, 0, 1392});
    for (int slot = 0; slot < VKB_NCBUF; slot++) {
        if (!(sh->cbuf_mask & (1u << slot))) continue;
        VkDescriptorBufferInfo info;
        if (slot == 1) {   // the shader's own constants: one copy per command buffer (they never change)
            uint32_t n = sh->c1_size ? (sh->c1_size + 15) & ~15u : 16;
            c1_t *c = &B->c1[((uintptr_t)sh * 0x9E3779B97F4A7C15ull) >> 54];
            if (c->sh != sh || c->rec != g_sub || no_cache()) {
                uint8_t *p = vkb_ring_alloc(n, ua, &c->b, &c->off);
                memset(p, 0, n);
                if (sh->c1_size) memcpy(p, sh->c1, sh->c1_size);
                c->sh = sh; c->rec = g_sub;
            }
            info = (VkDescriptorBufferInfo){c->b, c->off, n};
        } else if (slot == 0 || slot == 2) info = driver_bank(slot, stage);
        else {
            const p_bind_buf *u = &S.ubo[stage][slot - 3];
            info = guest_range(u->addr, u->size, ua, 1);
            if (info.buffer == g_zero && u->addr) {
                vkb_audit(A_UBO_MISSING, sh->key);
                static int said;
                if (said++ < 16) VLOG("[temporary] shader %.12s: uniform buffer c%d %#llx+%llu not in an imported pool: zeros are bound",
                                      sh->key ? sh->key : "?", slot, (unsigned long long)u->addr, (unsigned long long)u->size);
            }
        }
        put_buf(W, set, base + VKB_B_CBUF + (uint32_t)slot, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, info);
    }
    for (int i = 0; i < sh->nsbuf; i++) {
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        VkDescriptorBufferInfo bi = guest_range(addr, size, sa, 0);
        if (sh->sbuf[i].slot == 0xFF) vkb_audit(A_SSBO_UNKNOWN_SOURCE, sh->key);
        else if (bi.buffer == g_zero) vkb_audit(A_SSBO_MISSING, sh->key);
        if (bi.buffer == g_zero && addr) {
            static int said;
            if (said++ < 16) VLOG("[temporary] shader %.12s: storage buffer %#llx+%llu not in an imported pool (or misaligned): an empty one is bound",
                                  sh->key ? sh->key : "?", (unsigned long long)addr, (unsigned long long)size);
        }
        put_buf(W, set, base + VKB_B_SBUF + (uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bi);
    }
    for (int i = 0; i < sh->ntex; i++) {
        uint64_t h = sh->tex[i].separate ? 0 : S.tex[stage][sh->tex[i].slot];
        const view_t *v = NULL;
        vkb_image *im = h ? handle_image(h, &v) : NULL;
        VkImageViewType vt = view_type_of(sh->tex[i].type);
        int readable = im && (fb_sampled(im) ? vkb_layout_is(im, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT)
                                             : vkb_layout_is(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        VkImageView view = readable ? vkb_view(im, v, 0, (int)vt) : VK_NULL_HANDLE;
        if (sh->tex[i].separate) vkb_audit(A_TEX_SEPARATE, sh->key);
        else if (!(h & 0xFFFFF)) vkb_audit(A_TEX_UNBOUND, sh->key);
        else if (!im) vkb_audit(A_TEX_NO_IMAGE, sh->key);
        else if (!view) vkb_audit(A_TEX_NOT_READY, sh->key);
        else if (im->not_uploadable) vkb_audit(A_TEX_NOT_UPLOADED, sh->key);
        VkDescriptorImageInfo info = {sampler((int)(h >> 20) & 0xFFF), view ? view : dummy_view(vt),
                                      view ? vkb_layout(im) : VK_IMAGE_LAYOUT_GENERAL};
        put_img(W, set, base + VKB_B_TEX + (uint32_t)i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, info);
    }
    for (int i = 0; i < sh->nimg; i++) {
        const view_t *v = NULL;
        vkb_image *im = handle_image(S.img[stage][sh->img[i].slot], &v);
        VkImageView view = im && vkb_layout_is(im, VK_IMAGE_LAYOUT_GENERAL) ? vkb_view(im, v, 2, -1) : VK_NULL_HANDLE;
        if (!view) vkb_audit(A_IMG_MISSING, sh->key);
        VkDescriptorImageInfo info = {VK_NULL_HANDLE, view ? view : dummy_view(VK_IMAGE_VIEW_TYPE_2D), VK_IMAGE_LAYOUT_GENERAL};
        put_img(W, set, base + VKB_B_IMG + (uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, info);
    }
}

// A descriptor set with these writes: one made earlier in this submission with the same contents (two 64-bit hashes
// of every binding, buffer range, view, sampler and layout), else a new one. Each draw allocated and wrote a set of its
// own before (SavageWind measured ~45 % of them identical in BotW).
static vkb_cnt g_sets_new, g_sets_reused;
static __thread VkDescriptorSet g_bound[2];
static __thread uint64_t g_bound_rec[2];
static __thread struct { uint64_t rec, gen; VkDeviceSize off; } g_dbound[2];
static void hmix(uint64_t *h1, uint64_t *h2, uint64_t v) {
    *h1 = (*h1 ^ v) * 0x100000001b3ull;
    *h2 = (*h2 + v) * 0x9e3779b97f4a7c15ull;
    *h2 ^= *h2 >> 29;
}
static VkDescriptorSet resolve_set(writes_t *W, int compute) {
    uint64_t h1 = 0xcbf29ce484222325ull, h2 = 0x84222325cbf29ce4ull;
    for (int i = 0; i < W->n; i++) {
        const VkWriteDescriptorSet *w = &W->w[i];
        hmix(&h1, &h2, (uint64_t)w->dstBinding << 32 | (uint32_t)w->descriptorType);
        if (w->pBufferInfo) {
            hmix(&h1, &h2, (uint64_t)(uintptr_t)w->pBufferInfo->buffer);
            hmix(&h1, &h2, w->pBufferInfo->offset);
            hmix(&h1, &h2, w->pBufferInfo->range);
        } else {
            hmix(&h1, &h2, (uint64_t)(uintptr_t)w->pImageInfo->sampler);
            hmix(&h1, &h2, (uint64_t)(uintptr_t)w->pImageInfo->imageView);
            hmix(&h1, &h2, (uint64_t)w->pImageInfo->imageLayout);
        }
    }
    uint32_t at = (uint32_t)(h1 ^ h2 >> 32) & (SETS - 1);
    if (vkb_db_on()) {   // an offset in descriptor memory, returned in place of a set
        uint64_t gen = vkb_db_gen();
        for (int probe = 0; probe < 8 && !no_cache(); probe++) {
            uint32_t k = (at + (uint32_t)probe) & (SETS - 1);
            if (B->dsets[compute][k].rec == g_sub && B->dsets[compute][k].gen == gen && B->dsets[compute][k].h1 == h1 && B->dsets[compute][k].h2 == h2) {
                vkb_cnt_add(g_sets_reused, 1);
                return (VkDescriptorSet)(uintptr_t)(B->dsets[compute][k].off + 1);
            }
            if (B->dsets[compute][k].rec != g_sub || B->dsets[compute][k].gen != gen) { at = k; break; }
        }
        VkDeviceSize off = vkb_db_set(vkb_cmd(), vkb_slot(), g_rec, W->w, W->n, compute);
        B->dsets[compute][at].rec = g_sub; B->dsets[compute][at].gen = vkb_db_gen(); B->dsets[compute][at].h1 = h1; B->dsets[compute][at].h2 = h2;
        B->dsets[compute][at].off = off;
        vkb_cnt_add(g_sets_new, 1);
        return (VkDescriptorSet)(uintptr_t)(off + 1);
    }
    for (int probe = 0; probe < 8 && !no_cache(); probe++) {
        uint32_t k = (at + (uint32_t)probe) & (SETS - 1);
        if (B->sets[compute][k].rec == g_sub && B->sets[compute][k].h1 == h1 && B->sets[compute][k].h2 == h2) { vkb_cnt_add(g_sets_reused, 1); return B->sets[compute][k].set; }
        if (B->sets[compute][k].rec != g_sub) { at = k; break; }
    }
    VkDescriptorSet set = alloc_set(compute);
    for (int i = 0; i < W->n; i++) W->w[i].dstSet = set;
    vk.vkUpdateDescriptorSets(vkc.device, (uint32_t)W->n, W->w, 0, NULL);
    B->sets[compute][at].rec = g_sub; B->sets[compute][at].h1 = h1; B->sets[compute][at].h2 = h2; B->sets[compute][at].set = set;
    vkb_cnt_add(g_sets_new, 1);
    return set;
}
static void bind_set(VkCommandBuffer cb, VkDescriptorSet set, int compute) {
    if (vkb_db_on()) {   // `set` is offset + 1 in descriptor memory
        VkDeviceSize off = (VkDeviceSize)(uintptr_t)set - 1;
        uint64_t gen = vkb_db_gen();
        if (g_dbound[compute].rec == g_rec && g_dbound[compute].gen == gen && g_dbound[compute].off == off && !no_cache()) return;
        g_dbound[compute].rec = g_rec; g_dbound[compute].gen = gen; g_dbound[compute].off = off;
        vkb_db_bind(cb, vkb_slot(), g_rec);   // an offset made by an earlier command buffer of this thread: its memory bound here too
        uint32_t idx = 0;
        vk.vkCmdSetDescriptorBufferOffsetsEXT(cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              vkb_pipeline_layout(compute), 0, 1, &idx, &off);
        return;
    }
    if (g_bound_rec[compute] == g_rec && g_bound[compute] == set && !no_cache()) return;
    g_bound_rec[compute] = g_rec; g_bound[compute] = set;
    vk.vkCmdBindDescriptorSets(cb, compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS, vkb_pipeline_layout(compute), 0, 1, &set, 0, NULL);
}

// The program's shaders, resolved once per program serial.
// Programs resolved before, by serial (a new serial with every SetShaders): resolving hashes the shaders' code
// (vkb_shader_get, SHA-256 of tens of KiB), and TOTK switches programs hundreds of times a frame - with only the
// last one remembered, hashing took ~20% of the recording thread (Great Sky Island, 2026-10-24).
static int resolve_program(void) {
    if (!S.prog) return 0;
    if (S.prog_serial == S.prog->serial && S.prog_serial) return S.prog_ok;
    S.prog_serial = S.prog->serial;
    uint32_t at = (uint32_t)(S.prog_serial * 0x9E3779B97F4A7C15ull >> 51) & (PROGS - 1);
    if (S.prog_serial && B->progs[at].serial == S.prog_serial) {
        memcpy(S.sh, B->progs[at].sh, sizeof S.sh);
        S.bad_key = B->progs[at].bad_key;
        return S.prog_ok = B->progs[at].ok;
    }
    S.bad_key = NULL;
    memset(S.sh, 0, sizeof S.sh);
    S.prog_ok = 1;
    for (unsigned i = 0; i < S.prog->nstage && i < 6; i++) {
        const vkb_shader *sh = vkb_shader_get(&S.prog->sd[i]);
        if (!sh || sh->unsupported) { S.prog_ok = 0; S.bad_key = sh ? sh->key : NULL; continue; }
        S.sh[sh->stage] = sh;
    }
    B->progs[at].serial = S.prog_serial;
    memcpy(B->progs[at].sh, S.sh, sizeof S.sh);
    B->progs[at].bad_key = S.bad_key;
    B->progs[at].ok = S.prog_ok;
    return S.prog_ok;
}

// ---------------- dynamic state ----------------
static VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR; case 0xA: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0xB: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(uint32_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkCompareOp compare(uint32_t f, VkCompareOp dflt) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : dflt; }
static VkStencilOp stencil_op(uint32_t o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }

// ---------------- per-submission caches of recorded state ----------------
// Dynamic state, bound shaders, vertex input and vertex buffers stay set in a command buffer until set again (render
// passes do not reset them): a value equal to the one recorded last in this submission is not recorded again. Every draw
// used to record ~45 state commands, the shaders and the vertex input; on the Great Sky Island (~6000 draws a frame) the
// driver's share of recording was ~60 % of the replay thread (2026-10-24).
static vkb_cnt g_dc_hits, g_dc_calls, g_local_barriers;
static int dyn(int k, const void *v, size_t n) {   // 1: record it (changed since the last time in this command buffer)
    if (B->dc.rec != g_rec) { memset(B->dc.valid, 0, sizeof B->dc.valid); B->dc.rec = g_rec; }
    vkb_cnt_add(g_dc_calls, 1);
    if (n > sizeof B->dc.v[0] || no_cache()) return 1;
    if (B->dc.valid[k] && B->dc.len[k] == n && !memcmp(B->dc.v[k], v, n)) { vkb_cnt_add(g_dc_hits, 1); return 0; }
    memcpy(B->dc.v[k], v, n);
    B->dc.len[k] = (uint16_t)n;
    B->dc.valid[k] = 1;
    return 1;
}
#define DYN(k, T, val, call) do { T v_ = (val); if (dyn(k, &v_, sizeof v_)) call; } while (0)

static void set_dynamic_state(VkCommandBuffer cb, int ncolor, VkExtent2D ext, VkPrimitiveTopology topo) {
    // viewports (TOTK uses the upper-left window origin, as Vulkan) and the depth range
    VkViewport vps[16];
    int nvp = S.nvp ? S.nvp : 1;
    for (int i = 0; i < nvp; i++) {
        const float *v = S.vp[i];
        vps[i] = S.nvp ? (VkViewport){v[0], v[1], v[2], v[3], v[4], v[5]} : (VkViewport){0, 0, (float)ext.width, (float)ext.height, 0, 1};
        if (S.has_dr) { vps[i].minDepth = S.dr[0]; vps[i].maxDepth = S.dr[1]; }
        if (vps[i].width <= 0) vps[i].width = 1;
        if (vps[i].height == 0) vps[i].height = 1;
        // The translator writes clip space with y up (GL / Metal, MacNativeGames' target); Vulkan's points down.
        // A negative-height viewport flips it back (core since Vulkan 1.1), so the shaders stay as translated.
        vps[i].y += vps[i].height;
        vps[i].height = -vps[i].height;
    }
    if (dyn(D_VP, vps, sizeof vps[0] * (size_t)nvp)) vk.vkCmdSetViewportWithCount(cb, (uint32_t)nvp, vps);
    VkRect2D sc[16];
    memset(sc, 0, sizeof sc);
    for (int i = 0; i < nvp; i++) {
        sc[i] = S.has_sc ? (VkRect2D){{S.sc.x < 0 ? 0 : S.sc.x, S.sc.y < 0 ? 0 : S.sc.y}, {(uint32_t)(S.sc.w < 0 ? 0 : S.sc.w), (uint32_t)(S.sc.h < 0 ? 0 : S.sc.h)}}
                         : (VkRect2D){{0, 0}, ext};
    }
    if (dyn(D_SC, sc, sizeof sc[0] * (size_t)nvp)) vk.vkCmdSetScissorWithCount(cb, (uint32_t)nvp, sc);
    DYN(D_RDISC, VkBool32, S.raster_discard != 0, vk.vkCmdSetRasterizerDiscardEnable(cb, v_));
    DYN(D_TOPO, VkPrimitiveTopology, topo, vk.vkCmdSetPrimitiveTopology(cb, v_));
    DYN(D_RESTART, VkBool32, VK_FALSE, vk.vkCmdSetPrimitiveRestartEnable(cb, v_));   // [temporary] NVN primitive restart state is not recorded yet
    // polygon state
    uint32_t cull = S.has_poly ? S.poly.cull : 0, front = S.has_poly ? S.poly.front : 1, mode = S.has_poly ? S.poly.mode : 2;
    DYN(D_CULL, VkCullModeFlags, (VkCullModeFlags)cull, vk.vkCmdSetCullMode(cb, v_));
    DYN(D_FRONT, VkFrontFace, front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, vk.vkCmdSetFrontFace(cb, v_));
    DYN(D_POLY, VkPolygonMode, mode == 0 ? VK_POLYGON_MODE_POINT : mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL, vk.vkCmdSetPolygonModeEXT(cb, v_));
    int bias = S.has_poly && (S.poly.offset_en & 4);
    DYN(D_BIAS_EN, VkBool32, (VkBool32)bias, vk.vkCmdSetDepthBiasEnable(cb, v_));
    { float b[3] = {S.po.units, S.po.clamp, S.po.factor}; if (dyn(D_BIAS, b, sizeof b)) vk.vkCmdSetDepthBias(cb, b[0], b[1], b[2]); }
    DYN(D_LINEW, float, 1.0f, vk.vkCmdSetLineWidth(cb, v_));
    DYN(D_CLAMP, VkBool32, S.depth_clamp != 0, vk.vkCmdSetDepthClampEnableEXT(cb, v_));
    DYN(D_CLIP, VkBool32, !S.depth_clamp, vk.vkCmdSetDepthClipEnableEXT(cb, v_));
    DYN(D_CLIPNEG, VkBool32, VK_FALSE, vk.vkCmdSetDepthClipNegativeOneToOneEXT(cb, v_));   // the translator converts z = (z + w) / 2
    DYN(D_PROVOKE, VkProvokingVertexModeEXT, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, vk.vkCmdSetProvokingVertexModeEXT(cb, v_));
    // multisample
    DYN(D_SAMPLES, VkSampleCountFlagBits, VK_SAMPLE_COUNT_1_BIT, vk.vkCmdSetRasterizationSamplesEXT(cb, v_));   // [temporary] single-sampled targets
    DYN(D_SMASK, VkSampleMask, 0xFFFFFFFF, vk.vkCmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &v_));
    DYN(D_A2C, VkBool32, S.has_ms && S.ms.a2c, vk.vkCmdSetAlphaToCoverageEnableEXT(cb, v_));
    DYN(D_A2ONE, VkBool32, VK_FALSE, vk.vkCmdSetAlphaToOneEnableEXT(cb, v_));
    // depth / stencil
    const ds_st *d = &S.ds;
    DYN(D_DT, VkBool32, S.has_ds && d->dt, vk.vkCmdSetDepthTestEnable(cb, v_));
    DYN(D_DW, VkBool32, S.has_ds && d->dw, vk.vkCmdSetDepthWriteEnable(cb, v_));
    VkCompareOp dcmp = compare(d->df, VK_COMPARE_OP_LESS);
    {   // TOTK_VK_DEPTH_ALWAYS=<fragment shader key prefix>,...: those draws pass the depth test (development experiment)
        static const char *list;
        static int init;
        if (!init) { init = 1; list = getenv("TOTK_VK_DEPTH_ALWAYS"); }
        for (const char *p = list; p && *p && S.sh[1] && S.sh[1]->key;) {
            size_t n = strcspn(p, ",");
            if (n && !strncmp(S.sh[1]->key, p, n)) dcmp = VK_COMPARE_OP_ALWAYS;
            p += n + (p[n] == ',');
        }
    }
    DYN(D_DCMP, VkCompareOp, dcmp, vk.vkCmdSetDepthCompareOp(cb, v_));
    DYN(D_DBT, VkBool32, VK_FALSE, vk.vkCmdSetDepthBoundsTestEnable(cb, v_));
    DYN(D_ST, VkBool32, S.has_ds && d->st, vk.vkCmdSetStencilTestEnable(cb, v_));
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        int op[4] = {f ? d->fail_b : d->fail_f, f ? d->zpass_b : d->zpass_f, f ? d->zfail_b : d->zfail_f, f ? d->sf_b : d->sf_f};
        if (dyn(D_SOP0 + f, op, sizeof op))
            vk.vkCmdSetStencilOp(cb, face, stencil_op(op[0]), stencil_op(op[1]), stencil_op(op[2]), compare(op[3], VK_COMPARE_OP_ALWAYS));
        DYN(D_SCM0 + f, uint32_t, S.stencil[f].value_mask, vk.vkCmdSetStencilCompareMask(cb, face, v_));
        DYN(D_SWM0 + f, uint32_t, S.stencil[f].write_mask, vk.vkCmdSetStencilWriteMask(cb, face, v_));
        DYN(D_SREF0 + f, uint32_t, S.stencil[f].ref, vk.vkCmdSetStencilReference(cb, face, v_));
    }
    // colour: blend per target, write masks, logic op
    if (ncolor > 0) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        memset(eq, 0, sizeof eq);
        for (int i = 0; i < ncolor; i++) {
            en[i] = S.has_color && ((S.color.blend_en >> i) & 1);
            const blend_st *b = &S.blend[i];
            eq[i] = S.has_blend[i]
                ? (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb),
                                            blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)}
                : (VkColorBlendEquationEXT){VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD};
            wm[i] = (S.chmask.mask >> (4 * i)) & 0xF;
        }
        if (dyn(D_BLEN, en, sizeof en[0] * (size_t)ncolor)) vk.vkCmdSetColorBlendEnableEXT(cb, 0, (uint32_t)ncolor, en);
        if (dyn(D_BEQ, eq, sizeof eq[0] * (size_t)ncolor)) vk.vkCmdSetColorBlendEquationEXT(cb, 0, (uint32_t)ncolor, eq);
        if (dyn(D_WMASK, wm, sizeof wm[0] * (size_t)ncolor)) vk.vkCmdSetColorWriteMaskEXT(cb, 0, (uint32_t)ncolor, wm);
    }
    uint32_t lop = S.has_color ? S.color.logic_op : 3;
    DYN(D_LOPEN, VkBool32, lop != 3, vk.vkCmdSetLogicOpEnableEXT(cb, v_));
    DYN(D_LOP, VkLogicOp, (VkLogicOp)lop, vk.vkCmdSetLogicOpEXT(cb, v_));
    if (dyn(D_BCONST, S.blend_color, sizeof S.blend_color)) vk.vkCmdSetBlendConstants(cb, S.blend_color);
}

// NVN vertex attribute formats (NVNformat values) as vertex input formats.
static VkFormat vertex_format(int f) {
    switch (f) {
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;  case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;   case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT; case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;  case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;   case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;   case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x51: return VK_FORMAT_R8_USCALED;  case 0x52: return VK_FORMAT_R8_SSCALED;
    case 0x53: return VK_FORMAT_R16_USCALED; case 0x54: return VK_FORMAT_R16_SSCALED;
    case 0x57: return VK_FORMAT_R8G8_USCALED;  case 0x58: return VK_FORMAT_R8G8_SSCALED;
    case 0x59: return VK_FORMAT_R16G16_USCALED; case 0x5A: return VK_FORMAT_R16G16_SSCALED;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED;  case 0x5E: return VK_FORMAT_R8G8B8_SSCALED;
    case 0x5F: return VK_FORMAT_R16G16B16_USCALED; case 0x60: return VK_FORMAT_R16G16B16_SSCALED;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED;  case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED;
    case 0x65: return VK_FORMAT_R16G16B16A16_USCALED; case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;   case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    default: {
        VkFormat v = vkb_format(f, NULL, NULL);
        return f >= 0x79 ? VK_FORMAT_UNDEFINED : v;   // no compressed / ASTC vertex formats
    }
    }
}

// Integer vertex formats: 1 unsigned, 2 signed, 0 otherwise.
static int int_kind(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return 2;
    default: return 0;
    }
}

// Locations of the bound attributes in integer formats (signed ones also in *sint).
static uint32_t int_attributes(uint32_t *sint) {
    uint32_t m = 0;
    *sint = 0;
    for (int i = 0; i < S.nvattr; i++) {
        va_st a;
        memcpy(&a, &S.vattr[i], sizeof a);
        int k = a.fmt ? int_kind(vertex_format((int)a.fmt)) : 0;
        if (k) m |= 1u << i;
        if (k == 2) *sint |= 1u << i;
    }
    return m;
}

// Integer attributes the shader variant reads as integers (int_inputs) are given in their own format. Any other
// integer attribute still goes to a float input: as a scaled format ([temporary]: only if no variant could be made),
// 32-bit ones as float bits.
static VkFormat float_read(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED; case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
    case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED; case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED; case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
    case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED; case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
    case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED; case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
    case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED; case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED; case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
    case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED; case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_SFLOAT;   // bits reinterpreted (no 32-bit scaled)
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SFLOAT;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return VK_FORMAT_R32G32B32_SFLOAT;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return f;
    }
}

static void set_vertex_input(VkCommandBuffer cb, uint32_t int_inputs) {
    VkVertexInputBindingDescription2EXT bind[16];
    VkVertexInputAttributeDescription2EXT attr[16];
    memset(bind, 0, sizeof bind);
    memset(attr, 0, sizeof attr);
    uint32_t nb = 0, na = 0, used = 0;
    for (int i = 0; i < S.nvattr; i++) {
        va_st a;
        memcpy(&a, &S.vattr[i], sizeof a);
        VkFormat raw = a.fmt ? vertex_format((int)a.fmt) : VK_FORMAT_UNDEFINED;
        VkFormat f = int_inputs >> i & 1 ? raw : float_read(raw);
        if (f == VK_FORMAT_UNDEFINED) continue;
        {   // a vertex format the GPU cannot fetch reads garbage: say so once per format
            static uint8_t checked[256];
            if ((unsigned)f < 256 && !checked[f]) {
                checked[f] = 1;
                VkFormatProperties fp;
                vk.vkGetPhysicalDeviceFormatProperties(vkc.gpu, f, &fp);
                if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                    VLOG("[temporary] vertex format %d (NVN %#x) is not supported for vertex fetch on this GPU", (int)f, (unsigned)a.fmt);
            }
        }
        attr[na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = (uint32_t)i,
                                                             .binding = a.stream, .format = f, .offset = a.offset};
        used |= 1u << a.stream;
    }
    for (uint32_t s = 0; s < 16; s++) {
        if (!(used & (1u << s))) continue;
        bind[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = s,
            .stride = S.vstream[s].stride, .inputRate = S.vstream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = 1};
        VkBuffer buf;
        VkDeviceSize off;
        VkDeviceSize size = S.vbuf[s].size;
        if (S.vbuf[s].address && size && vkb_gpu_addr(S.vbuf[s].address, size, &buf, &off)) {
            VkDeviceSize key[3] = {(VkDeviceSize)(uintptr_t)buf, off, size};
            if (dyn(D_VB0 + (int)s, key, sizeof key)) vk.vkCmdBindVertexBuffers2(cb, s, 1, &buf, &off, &size, NULL);
        }
        else {
            if (S.vbuf[s].address) {   // [temporary] not in an imported pool (a virtual pool: sparse binding later): zeros
                vkb_audit(A_VBUF_MISSING, S.sh[0] ? S.sh[0]->key : NULL);
                static int said;
                if (said++ < 16) VLOG("[temporary] vertex buffer %#llx+%llu (stream %u) not in an imported pool: zeros are bound",
                                      (unsigned long long)S.vbuf[s].address, (unsigned long long)S.vbuf[s].size, s);
            }
            VkDeviceSize z = 0, zs = 65536, key[3] = {(VkDeviceSize)(uintptr_t)g_zero, 0, 65536};
            if (dyn(D_VB0 + (int)s, key, sizeof key)) vk.vkCmdBindVertexBuffers2(cb, s, 1, &g_zero, &z, &zs, NULL);
        }
    }
    struct { uint32_t nb, na; VkVertexInputBindingDescription2EXT b[16]; VkVertexInputAttributeDescription2EXT a[16]; } vi;
    memset(&vi, 0, sizeof vi);
    vi.nb = nb; vi.na = na;
    memcpy(vi.b, bind, sizeof bind[0] * nb);
    memcpy(vi.a, attr, sizeof attr[0] * na);
    if (dyn(D_VINPUT, &vi, sizeof vi)) vk.vkCmdSetVertexInputEXT(cb, nb, bind, na, attr);
}

static VkPrimitiveTopology topology(int prim, int *ok) {
    *ok = 1;
    switch (prim) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;   // POLYGON: a convex fan
    case 0xA: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 0xB: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 0xC: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 0xD: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 0xE: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: *ok = 0; return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // LINE_LOOP, QUADS, QUAD_STRIP: [temporary] not drawn
    }
}

// ---------------- draws ----------------
static vkb_cnt g_draws, g_skipped[4];   // skipped: no program / no shader translation, topology, targets, index buffer
static __thread int g_stores_pending;  // a recorded draw wrote storage buffers / images: barrier before the next pass
// Earlier draws wrote storage buffers / images: the next pass sees their writes (backend_vk.c pass_begin asks before any
// pass, a clear's too). TOTK's tiled lighting marks tiles from a fragment shader and the next pass's vertex shader reads
// the marks to drop tiles; without the barrier it read the cleared marks and lit every tile with every variant (the
// overexposed title screen, 2026-10-23). Draws within one pass do not wait for each other's stores (on the console that
// takes the game's own barrier, replayed as a full one): ending the pass after every storing draw ended ~590 passes a
// frame on the Great Sky Island (2026-10-24).
// Within a pass, a draw waits for earlier draws' stores only when it reads what they wrote: TOTK's tile marking draws
// (~360 a frame on the Great Sky Island) write one 128 KiB buffer and do not read it, the lighting draws read it.
enum { WRITTEN = 256 };
static __thread struct { uint64_t addr, size; } g_wr[WRITTEN];
static __thread int g_nwr, g_wr_all, g_wr_img;   // all: too many ranges (every read waits); img: storage images were written
int vkb_stores_take(void) { int p = g_stores_pending; g_stores_pending = 0; g_nwr = 0; g_wr_all = 0; g_wr_img = 0; return p; }
static void written_add(const vkb_shader *sh, int stage) {
    for (int i = 0; i < sh->nsbuf; i++) {
        if (!sh->sbuf[i].write) continue;
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        if (!addr || !size) { g_wr_all = 1; continue; }
        int k = 0;
        while (k < g_nwr && !(g_wr[k].addr == addr && g_wr[k].size == size)) k++;
        if (k < g_nwr) continue;
        if (g_nwr == WRITTEN) g_wr_all = 1; else g_wr[g_nwr].addr = addr, g_wr[g_nwr++].size = size;
    }
    if (sh->nimg) g_wr_img = 1;
}
static int written_overlaps(uint64_t addr, uint64_t size) {
    if (!addr || !size) return 0;
    if (g_wr_all) return 1;
    for (int k = 0; k < g_nwr; k++) if (addr < g_wr[k].addr + g_wr[k].size && g_wr[k].addr < addr + size) return 1;
    return 0;
}
static int reads_written(const vkb_shader *sh, int stage) {   // the shader reads what earlier stores wrote
    if (g_wr_img && (sh->ntex || sh->nimg)) return 1;
    for (int i = 0; i < sh->nsbuf; i++) {
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        if (sh->sbuf[i].read && written_overlaps(addr, size)) return 1;
    }
    for (int slot = 3; slot < VKB_NCBUF; slot++)
        if ((sh->cbuf_mask & (1u << slot)) && written_overlaps(S.ubo[stage][slot - 3].addr, S.ubo[stage][slot - 3].size)) return 1;
    return 0;
}
uint64_t vkb_draw_count(void) { return vkb_cnt_sum(g_draws); }
int vkb_viewport0(float v[4]) { if (!S.nvp) return 0; memcpy(v, S.vp[0], 4 * sizeof *v); return 1; }   // x y w h (trace)
int vkb_scissor(VkRect2D *r) {   // the game's scissor (clears are limited to it, as on NVN); 0: none set
    if (!S.has_sc) return 0;
    int32_t x = S.sc.x < 0 ? 0 : S.sc.x, y = S.sc.y < 0 ? 0 : S.sc.y;
    int32_t w = S.sc.w - (x - S.sc.x), h = S.sc.h - (y - S.sc.y);
    *r = (VkRect2D){{x, y}, {(uint32_t)(w < 0 ? 0 : w), (uint32_t)(h < 0 ? 0 : h)}};
    return 1;
}
void vkb_draw_stats(uint64_t *draws, uint64_t *skipped) {
    *draws = vkb_cnt_sum(g_draws);
    uint64_t sk[4] = {vkb_cnt_sum(g_skipped[0]), vkb_cnt_sum(g_skipped[1]), vkb_cnt_sum(g_skipped[2]), vkb_cnt_sum(g_skipped[3])};
    uint64_t dc_calls = vkb_cnt_sum(g_dc_calls), dc_hits = vkb_cnt_sum(g_dc_hits);
    uint64_t gets = 0, hits = 0;
    if (vkb_db_on()) vkb_db_stats(&gets, &hits);
    if (dc_calls) VLOG("recording caches: state %.0f %% not recorded again, descriptor sets %llu made / %llu reused (descriptor bytes: %llu from the driver, %llu cached), mirrors %llu made / %llu sent back early, barriers inside passes %llu",
                       100.0 * (double)dc_hits / (double)dc_calls, (unsigned long long)vkb_cnt_sum(g_sets_new), (unsigned long long)vkb_cnt_sum(g_sets_reused),
                       (unsigned long long)gets, (unsigned long long)hits, (unsigned long long)g_mirrors_made, (unsigned long long)g_mirrors_evicted,
                       (unsigned long long)vkb_cnt_sum(g_local_barriers));
    *skipped = sk[0] + sk[1] + sk[2] + sk[3];
    static uint64_t said;
    if (*skipped && *draws + *skipped >= said * 2) {
        said = *draws + *skipped;
        VLOG("skipped draws: no program / translation %llu, topology %llu, no targets %llu, index buffer %llu",
             (unsigned long long)sk[0], (unsigned long long)sk[1], (unsigned long long)sk[2], (unsigned long long)sk[3]);
    }
}

// TOTK_VK_DRAWTRACE=<frame>:<n>: n draws from that present on (development): targets, shaders, counts, outcome.
static int draw_trace(void) {
    static long long at = -1, left;
    if (at == -1) {
        const char *e = getenv("TOTK_VK_DRAWTRACE");
        at = e ? atoll(e) : 0;
        left = e && strchr(e, ':') ? atoll(strchr(e, ':') + 1) : 200;
    }
    return at > 0 && vkb_frame() >= (uint64_t)at && left-- > 0;
}
// ---------------- GPU profile (development) ----------------
// TOTK_GPU_PROF=<present>: a timestamp before and after every draw and dispatch of that frame; the next present prints
// the GPU time per shader (sorted) and the frame's span. Timestamps inside a pass overlap with neighbouring draws, so
// the per-draw times are approximate; the span is exact.
enum { GPROF_MAX = 8192 };
typedef struct { char key[13]; uint8_t compute; uint32_t count, inst, w, h; } gprof_info;
static VkQueryPool g_qp;
static int g_qn, g_qdone;
static gprof_info g_qinfo[GPROF_MAX];
static long long gprof_frame(void) {
    static long long at = -1;
    if (at == -1) { const char *e = getenv("TOTK_GPU_PROF"); at = e ? atoll(e) : 0; }
    return at;
}
static int gprof_begin(void) {
    long long at = gprof_frame();
    if (at <= 0 || g_qdone || vkb_frame() != (uint64_t)at || g_qn >= GPROF_MAX) return -1;
    if (!g_qp) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * GPROF_MAX};
        if (!vk_check(vk.vkCreateQueryPool(vkc.device, &ci, NULL, &g_qp), "query pool")) { g_qdone = 1; return -1; }
        vk.vkResetQueryPool(vkc.device, g_qp, 0, 2 * GPROF_MAX);
    }
    vk.vkCmdWriteTimestamp2(vkb_cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_qp, (uint32_t)(2 * g_qn));
    return g_qn++;
}
static void gprof_end(int q, const vkb_shader *sh, int compute, uint32_t count, uint32_t inst) {
    if (q < 0) return;
    vk.vkCmdWriteTimestamp2(vkb_cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_qp, (uint32_t)(2 * q + 1));
    gprof_info *i = &g_qinfo[q];
    snprintf(i->key, sizeof i->key, "%.12s", sh && sh->key ? sh->key : "?");
    VkExtent2D e = vkb_rt_extent();
    i->compute = (uint8_t)compute; i->count = count; i->inst = inst; i->w = e.width; i->h = e.height;
}
void vkb_gprof_report(void) {
    long long at = gprof_frame();
    if (at <= 0 || g_qdone || !g_qn || vkb_frame() <= (uint64_t)at) return;
    g_qdone = 1;
    mtl_finish();
    uint64_t *ts = malloc(sizeof *ts * 2 * (size_t)g_qn);
    if (!ts) return;
    vk.vkGetQueryPoolResults(vkc.device, g_qp, 0, (uint32_t)(2 * g_qn), sizeof *ts * 2 * (size_t)g_qn, ts, sizeof *ts,
                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    double ns = vkc.props.limits.timestampPeriod, sum = 0;
    typedef struct { char key[13]; double ms; int n; uint8_t compute; uint32_t w, h; uint64_t prims; } agg;
    agg *a = calloc((size_t)g_qn, sizeof *a);
    int na = 0;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int q = 0; q < g_qn; q++) {
        uint64_t t0 = ts[2 * q], t1 = ts[2 * q + 1];
        if (t0 < lo) lo = t0;
        if (t1 > hi) hi = t1;
        double ms = t1 > t0 ? (double)(t1 - t0) * ns / 1e6 : 0;
        sum += ms;
        int k = 0;
        while (k < na && strcmp(a[k].key, g_qinfo[q].key)) k++;
        if (k == na) { memcpy(a[na].key, g_qinfo[q].key, 13); a[na].compute = g_qinfo[q].compute; a[na].w = g_qinfo[q].w; a[na].h = g_qinfo[q].h; na++; }
        a[k].ms += ms; a[k].n++; a[k].prims += (uint64_t)g_qinfo[q].count * (g_qinfo[q].inst ? g_qinfo[q].inst : 1);
    }
    for (int i = 0; i < na; i++)   // by time, descending
        for (int j = i + 1; j < na; j++) if (a[j].ms > a[i].ms) { agg t = a[i]; a[i] = a[j]; a[j] = t; }
    VLOG("GPU profile of present %lld: %d draws/dispatches, %.2f ms summed, %.2f ms from first to last", at, g_qn, sum,
         hi > lo ? (double)(hi - lo) * ns / 1e6 : 0.0);
    for (int i = 0; i < na && i < 30; i++)
        VLOG("  %8.3f ms  %4d x %s %s  target %ux%u  vertices*instances %llu", a[i].ms, a[i].n, a[i].compute ? "cs" : "fs", a[i].key,
             a[i].w, a[i].h, (unsigned long long)a[i].prims);
    free(a);
    free(ts);
}

// What a traced shader reads and writes: textures, images (serial, format, size) and storage buffers (address, size).
static void trace_resources(const vkb_shader *sh, int stage) {
    char line[1024];
    int n = snprintf(line, sizeof line, "[draw]   %s %.12s:", stage == 0 ? "vs" : stage == 1 ? "fs" : "cs", sh->key ? sh->key : "?");
    for (int i = 0; i < sh->ntex && n < 900; i++) {
        const view_t *v = NULL;
        vkb_image *im = sh->tex[i].separate ? NULL : handle_image(S.tex[stage][sh->tex[i].slot], &v);
        n += im ? snprintf(line + n, sizeof line - n, " t%d=%llu/%#x/%ux%u%s@%p#%04llx", sh->tex[i].slot, (unsigned long long)im->serial, im->d.format,
                           im->width, im->height, im->gpu_written ? "/gpu" : "", (const void *)im->storage,
                           (unsigned long long)((uintptr_t)im->img & 0xffff))
                : snprintf(line + n, sizeof line - n, " t%d=-", sh->tex[i].slot);
        int sid = (int)(S.tex[stage][sh->tex[i].slot] >> 20) & 0xFFF;
        vkb_rw_read(&g_reg_mu, LK_TEXREG);
        if (sid < g_nsreg && g_sreg[sid].valid) {
            const samp_par *sp = &g_sreg[sid].p;
            if (sp->cmp_mode) n += snprintf(line + n, sizeof line - n, "(cmp %u func %u)", sp->cmp_mode, sp->cmp_func);
            if (getenv("TOTK_VK_TRACE_SAMPLERS"))   // NVN values: wrap 0 clamp .. 7 clamp to edge, filters 0 nearest 1 linear
                n += snprintf(line + n, sizeof line - n, "(s%d wrap %u %u %u min %u mag %u border %g %g %g %g)", sid, sp->ws, sp->wt,
                              sp->wr, sp->minf, sp->magf, sp->border[0], sp->border[1], sp->border[2], sp->border[3]);
        }
        vkb_rw_read_end(&g_reg_mu);
        if (sh->tex[i].type & 0x400) n += snprintf(line + n, sizeof line - n, "(shadow)");   // Ryujinx SamplerType.Shadow
    }
    for (int i = 0; i < sh->nimg && n < 900; i++) {
        const view_t *v = NULL;
        vkb_image *im = handle_image(S.img[stage][sh->img[i].slot], &v);
        n += im ? snprintf(line + n, sizeof line - n, " i%d=%llu/%#x/%ux%u", sh->img[i].slot, (unsigned long long)im->serial, im->d.format, im->width, im->height)
                : snprintf(line + n, sizeof line - n, " i%d=-", sh->img[i].slot);
    }
    for (int slot = 3; slot < VKB_NCBUF && n < 900; slot++)
        if (sh->cbuf_mask & (1u << slot))
            n += snprintf(line + n, sizeof line - n, " c%d=%#llx+%llu", slot, (unsigned long long)S.ubo[stage][slot - 3].addr,
                          (unsigned long long)S.ubo[stage][slot - 3].size);
    for (int i = 0; i < sh->nsbuf && n < 900; i++) {
        uint8_t slot = sh->sbuf[i].slot;
        const p_bind_buf *b = slot == 0xFF ? NULL : sh->sbuf[i].kind == VKB_SB_TABLE ? &S.ssbo[stage][slot] : &S.ubo[stage][slot];
        n += snprintf(line + n, sizeof line - n, " s%d(%c%d)=%#llx+%llu", i, "tup"[sh->sbuf[i].kind], slot,
                      b ? (unsigned long long)b->addr : 0ull, b ? (unsigned long long)b->size : 0ull);
    }
    VLOG("%s", line);
    // TOTK_VK_PEEK=<key prefix>:<cbuf slot>:<first vec4>-<last vec4>: those constants of traced draws with that shader
    const char *pk = getenv("TOTK_VK_PEEK");
    size_t kl = pk && strchr(pk, ':') ? (size_t)(strchr(pk, ':') - pk) : 0;
    if (kl && sh->key && !strncmp(sh->key, pk, kl)) {
        int slot = 0, a = 0, b = 0;
        if (sscanf(pk + kl, ":%d:%d-%d", &slot, &a, &b) == 3 && slot >= 3 && slot < VKB_NCBUF) {
            const p_bind_buf *u = &S.ubo[stage][slot - 3];
            for (int v = a; v <= b && u->addr && (uint64_t)(v + 1) * 16 <= u->size; v++) {
                float f[4];
                memcpy(f, (const void *)(uintptr_t)(u->addr + (uint64_t)v * 16), 16);
                VLOG("[draw]     c%d[%d] = %g %g %g %g", slot, v, f[0], f[1], f[2], f[3]);
            }
        }
    }
}

// TOTK_VK_SKIP=<key prefix>,...: draws with these vertex / fragment shaders are not drawn (development: bisecting)
static int skip_listed(void) {
    static const char *skip;
    static int init;
    if (!init) { skip = getenv("TOTK_VK_SKIP"); init = 1; }
    for (const char *p = skip; p && *p;) {
        size_t n = strcspn(p, ",");
        for (int s = 0; s < 2; s++)
            if (n && S.sh[s] && S.sh[s]->key && !strncmp(S.sh[s]->key, p, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

// The walk of a submission (vkb_mt.c): the storage buffers a draw or dispatch writes void the uniforms captured there
// from its position on (vkb_ucap_drop), as mirrors_prepare does when one thread records.
static void prescan_stores(const vkb_shader *sh, int stage) {
    for (int i = 0; i < sh->nsbuf; i++) {
        if (!sh->sbuf[i].write) continue;
        uint64_t addr, size;
        sbuf_range(sh, stage, i, &addr, &size);
        if (addr && size) vkb_ucap_drop(addr, size);
    }
}
void vkb_draw_prescan(const uint8_t *a, int indexed, int dispatch) {
    big_init();
    if (!resolve_program()) return;
    if (dispatch) { if (S.sh[5]) prescan_stores(S.sh[5], 5); return; }
    if (!S.sh[0] || skip_listed()) return;
    int ok;
    topology(indexed ? ((const p_draw_idx *)a)->prim : ((const p_draw *)a)->prim, &ok);
    if (!ok) return;
    prescan_stores(S.sh[0], 0);
    if (S.sh[1]) prescan_stores(S.sh[1], 1);
}

// Objects made on first use: made before threads record (the prologue is this thread's command buffer then).
void vkb_draw_mt_init(void) {
    big_init();
    fixed_buffers();
    if (!g_dummy) dummy_init();
    static const VkImageViewType types[] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
    for (unsigned t = 0; t < sizeof types / sizeof *types; t++) dummy_view(types[t]);   // (1D ones: as before, on first use)
    sampler(-1);
}

void vkb_draw(const uint8_t *a, int indexed) {
    big_init();
    fixed_buffers();
    vkb_audit_draw_begin();
    const p_draw *dr = (const p_draw *)a;
    const p_draw_idx *di = (const p_draw_idx *)a;
    int trace = draw_trace();
    if (trace) {
        const p_set_rt *rt = vkb_rt();
        const tex_t *c0 = rt->n > 0 ? rt->color[0] : NULL;
        resolve_program();
        VLOG("[draw] frame %llu %s prim %d count %d inst %d | rt %d: tex %llu fmt %#x %ux%u%s | prog %llu vs %s fs %s | %d attrs",
             (unsigned long long)vkb_frame(), indexed ? "indexed" : "arrays", indexed ? di->prim : dr->prim, indexed ? di->count : dr->count,
             indexed ? di->instances : dr->instances, rt->n, c0 ? (unsigned long long)c0->serial : 0ull, c0 ? c0->d.format : 0,
             c0 ? c0->d.width : 0, c0 ? c0->d.height : 0, rt->depth ? " +depth" : "",
             S.prog ? (unsigned long long)S.prog->serial : 0ull, S.sh[0] ? "ok" : "-", S.sh[1] ? "ok" : "-", S.nvattr);
        char tl[512];   // every target with its memory (two texture objects over the same memory: aliasing)
        int tn = 0;
        for (int i = 0; i < rt->n && i < 8; i++)
            if (rt->color[i]) {
                vkb_image *ti = vkb_image_of(rt->color[i]);
                tn += snprintf(tl + tn, sizeof tl - (size_t)tn, " c%d=%llu@%p#%04llx", i, (unsigned long long)rt->color[i]->serial, rt->color[i]->mem,
                               ti ? (unsigned long long)((uintptr_t)ti->img & 0xffff) : 0ull);
            }
        if (rt->depth) {
            vkb_image *ti = vkb_image_of(rt->depth);
            tn += snprintf(tl + tn, sizeof tl - (size_t)tn, " d=%llu@%p#%04llx", (unsigned long long)rt->depth->serial, rt->depth->mem,
                           ti ? (unsigned long long)((uintptr_t)ti->img & 0xffff) : 0ull);
        }
        VLOG("[draw]   targets:%s", tn ? tl : " -");
        const blend_st *b0 = &S.blend[0];
        VLOG("[draw]   state: blend en %#x t0 %s %u/%u op %u, a %u/%u op %u | chmask %#x | depth %s test %u write %u func %u stencil %u",
             S.has_color ? (unsigned)S.color.blend_en : 0u, S.has_blend[0] ? "set" : "-", (unsigned)b0->src_rgb, (unsigned)b0->dst_rgb, (unsigned)b0->eq_rgb,
             (unsigned)b0->src_a, (unsigned)b0->dst_a, (unsigned)b0->eq_a, S.chmask.mask, S.has_ds ? "set" : "-", (unsigned)S.ds.dt, (unsigned)S.ds.dw, (unsigned)S.ds.df, (unsigned)S.ds.st);
        {
            const p_set_rt *r = vkb_rt();
            VLOG("[draw]   depth: range %s %g..%g, viewport0 z %g..%g, offset %s (units %g factor %g clamp %g), clamp %d, target %s format %#x",
                 S.has_dr ? "set" : "-", S.dr[0], S.dr[1], S.nvp ? S.vp[0][4] : 0.0f, S.nvp ? S.vp[0][5] : 1.0f,
                 S.has_poly && (S.poly.offset_en & 4) ? "on" : "off", S.po.units, S.po.factor, S.po.clamp, S.depth_clamp,
                 r->depth ? "yes" : "no", r->depth ? r->depth->d.format : 0);
        }
        if (S.sh[0]) trace_resources(S.sh[0], 0);
        if (S.sh[1]) trace_resources(S.sh[1], 1);
    }
    if (!resolve_program() || !S.sh[0]) {
        vkb_cnt_add(g_skipped[0], 1);
        vkb_audit(S.bad_key ? A_SHADER_UNSUPPORTED : A_DRAW_NO_PROGRAM, S.bad_key);
        if (trace) VLOG("[draw]   skipped: no program / translation");
        return;
    }
    if (skip_listed()) return;
    int ok;
    VkPrimitiveTopology topo = topology(indexed ? di->prim : dr->prim, &ok);
    if (!ok) {
        vkb_cnt_add(g_skipped[1], 1);
        vkb_audit(A_DRAW_TOPOLOGY, S.sh[1] ? S.sh[1]->key : S.sh[0]->key);
        static int said; if (!said++) VLOG("[temporary] primitive %d not drawn (no Vulkan topology)", indexed ? di->prim : dr->prim);
        return;
    }
    const vkb_shader *vs = S.sh[0], *fs = S.sh[1];
    static int snap_after;   // TOTK_SNAP_AFTER=1: the draw's render targets after it as well (a_rt*)
    if (vkb_tex_snap_wanted(vs->key, fs ? fs->key : NULL, vkb_frame())) {   // TOTK_SNAP: what this draw sees
        snap_after = getenv("TOTK_SNAP_AFTER") != NULL;
        vkb_pass_end_r(PE_SNAP);
        char what[16];
        for (int st = 0; st < 2; st++) {
            const vkb_shader *sh = st ? fs : vs;
            for (int i = 0; sh && i < sh->ntex; i++) {
                const view_t *v = NULL;
                vkb_image *im = sh->tex[i].separate ? NULL : handle_image(S.tex[st][sh->tex[i].slot], &v);
                snprintf(what, sizeof what, "%ct%d", st ? 'f' : 'v', sh->tex[i].slot);
                if (im) vkb_tex_snap(vkb_cmd(), im, what);
            }
        }
        const p_set_rt *rt = vkb_rt();
        for (int i = 0; i < rt->n && i < 8; i++) {
            snprintf(what, sizeof what, "rt%d", i);
            if (rt->color[i]) vkb_tex_snap(vkb_cmd(), vkb_image_of(rt->color[i]), what);
        }
        if (rt->depth) vkb_tex_snap(vkb_cmd(), vkb_image_of(rt->depth), "rtdepth");
    }
    if (!g_dummy) { vkb_pass_end(); dummy_init(); }
    if (g_stores_pending) {   // the draw reads what earlier storing draws wrote: their writes first (outside the pass)
        static int every = -1;   // TOTK_VK_STORES_EVERY=1 (development A/B): every draw waits, as before 2026-10-24
        if (every < 0) { const char *e = getenv("TOTK_VK_STORES_EVERY"); every = e && *e && *e != '0'; }
        int reads = every || reads_written(vs, 0) || (fs && reads_written(fs, 1));
        for (uint32_t vb = 0; vb < 16 && !reads; vb++) reads = written_overlaps(S.vbuf[vb].address, S.vbuf[vb].size);
        if (!reads && indexed && di->ibuf) reads = written_overlaps(di->ibuf, (uint64_t)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4));
        if (reads) vkb_pass_end_r(PE_STORES);   // pass_begin records the barrier
        static long long log_at = -1, left = 40;   // TOTK_VK_STORES_LOG=<present>: these decisions (development)
        if (log_at < 0) { const char *e = getenv("TOTK_VK_STORES_LOG"); log_at = e ? atoll(e) : 0; }
        if (log_at && vkb_frame() >= (uint64_t)log_at && left > 0) {
            left--;
            VLOG("[stores] vs %.12s fs %.12s: reads written %d (ranges %d, all %d, images %d; first %#llx+%llu)", vs->key, fs ? fs->key : "-", reads,
                 g_nwr, g_wr_all, g_wr_img, g_nwr ? (unsigned long long)g_wr[0].addr : 0ull, g_nwr ? (unsigned long long)g_wr[0].size : 0ull);
            for (int st = 0; st < 2; st++) {
                const vkb_shader *sh = st ? fs : vs;
                for (int i = 0; sh && i < sh->nsbuf; i++) {
                    uint64_t addr, size;
                    sbuf_range(sh, st, i, &addr, &size);
                    VLOG("[stores]   %s sbuf %d %#llx+%llu read %d write %d", st ? "fs" : "vs", i, (unsigned long long)addr, (unsigned long long)size, sh->sbuf[i].read, sh->sbuf[i].write);
                }
                for (int slot = 3; sh && slot < VKB_NCBUF; slot++)
                    if (sh->cbuf_mask & (1u << slot))
                        VLOG("[stores]   %s c%d %#llx+%llu", st ? "fs" : "vs", slot, (unsigned long long)S.ubo[st][slot - 3].addr, (unsigned long long)S.ubo[st][slot - 3].size);
            }
        }
    }
    feedback_scan(vs, fs);
    if (vkb_in_pass() && vkb_pass_fb() != g_fb) vkb_pass_end_r(PE_FEEDBACK);   // [temporary] a pass per change of sampled targets
    if ((vkb_in_pass() && (images_need_work(vs, 0) || (fs && images_need_work(fs, 1))))) vkb_pass_end_r(PE_IMAGES);
    if (!vkb_in_pass()) {
        prepare_images(vs, 0);
        if (fs) prepare_images(fs, 1);
    }
    mirrors_prepare(vs, 0);   // written storage buffers: device-local copies for the submission
    if (fs) mirrors_prepare(fs, 1);
    mirrors_prepare_fixed(indexed, di);
    VkExtent2D bare = {0, 0};   // a draw without targets covers its viewport
    if (S.nvp && S.vp[0][2] > 0 && S.vp[0][3] > 0)
        bare = (VkExtent2D){(uint32_t)ceilf(S.vp[0][0] + S.vp[0][2]), (uint32_t)ceilf(S.vp[0][1] + S.vp[0][3])};
    if (!vkb_pass_begin(bare, g_fb)) { vkb_cnt_add(g_skipped[2], 1); vkb_audit(A_DRAW_NO_TARGETS, fs ? fs->key : vs->key); return; }
    VkCommandBuffer cb = vkb_cmd();
    writes_t *W = &B->writes;
    W->n = 0;
    write_stage(W, VK_NULL_HANDLE, vs, 0);
    if (fs) write_stage(W, VK_NULL_HANDLE, fs, 1);
    bind_set(cb, resolve_set(W, 0), 0);
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                       VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    uint32_t sint, ints = int_attributes(&sint) & vs->in_mask;   // integer attributes: the vertex shader's variant for them
    VkShaderEXT vobj = ints ? vkb_shader_variant(vs, ints, sint) : vs->obj;
    uint32_t int_inputs = vobj != vs->obj ? ints : 0;
    if (ints && !int_inputs) vkb_audit(A_ATTR_INT_AS_FLOAT, vs->key);
    if (fs && (fs->in_mask & ~vs->out_mask)) vkb_audit(A_FS_INPUT_UNWRITTEN, fs->key);
    if (S.has_ms && S.ms.enable && S.ms.samples > 1) vkb_audit(A_MULTISAMPLE, fs ? fs->key : vs->key);
    if (vkb_render_suppressed()) vkb_audit(A_RENDER_DISABLED, fs ? fs->key : vs->key);
    VkShaderEXT objs[5] = {vobj, fs ? fs->obj : VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    if (dyn(D_SHADERS, objs, sizeof objs)) vk.vkCmdBindShadersEXT(cb, 5, stages, objs);
    set_dynamic_state(cb, vkb_rt_count(), vkb_rt_extent(), topo);
    if (vkc.has_feedback_loop && vk_feedback_loop_dynamic) {   // required state with shader objects once the feature is enabled
        VkImageAspectFlags loop = (g_fb & 0xFF ? VK_IMAGE_ASPECT_COLOR_BIT : 0) | (g_fb & VKB_FB_DEPTH ? g_fb_rt[8]->aspect : 0);
        DYN(D_FBLOOP, VkImageAspectFlags, loop, vk.vkCmdSetAttachmentFeedbackLoopEnableEXT(cb, v_));
    }
    // A draw that samples a target of its pass sees what earlier draws of the pass wrote there only after a barrier
    // inside the pass (VK_KHR_dynamic_rendering_local_read, by region: each pixel waits for its own writes). Without it
    // the read may see old contents (as SavageWind found with BotW's terrain, which ORs bits into its G-buffer pixel).
    static __thread uint64_t wrote_pass;   // the pass in which a draw was recorded since the last such barrier
    static int no_local = -1;   // TOTK_VK_NO_LOCAL_BARRIER=1 (development A/B): no barrier inside the pass
    if (no_local < 0) { const char *e = getenv("TOTK_VK_NO_LOCAL_BARRIER"); no_local = e && *e == '1'; }
    if (g_fb && vkc.has_local_read && !no_local && wrote_pass == vkb_pass_serial()) {
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT};
        VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                                .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vk.vkCmdPipelineBarrier2(cb, &dep);
        vkb_cnt_add(g_local_barriers, 1);
    }
    wrote_pass = vkb_pass_serial();
    set_vertex_input(cb, int_inputs);
    uint32_t inst = indexed ? (uint32_t)(di->instances ? di->instances : 1) : (uint32_t)(dr->instances ? dr->instances : 1);
    vkb_checkpoint(indexed ? "draw indexed" : "draw", vs->key, fs ? fs->key : NULL);
    vkb_passprof_note(fs ? fs->key : vs->key);
    if (indexed) {
        VkBuffer buf;
        VkDeviceSize off;
        uint32_t isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
        if (!di->ibuf || !vkb_gpu_addr(di->ibuf, (uint64_t)di->count * isz, &buf, &off)) {
            vkb_cnt_add(g_skipped[3], 1);
            vkb_audit(A_DRAW_NO_INDEX, vs->key);
            return;
        }
        VkIndexType it = di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vk.vkCmdBindIndexBuffer(cb, buf, off, it);
        int gq = gprof_begin();
        vk.vkCmdDrawIndexed(cb, (uint32_t)di->count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        gprof_end(gq, fs ? fs : vs, 0, (uint32_t)di->count, inst);
    } else {
        int gq = gprof_begin();
        vk.vkCmdDraw(cb, (uint32_t)dr->count, inst, (uint32_t)dr->first, (uint32_t)dr->base_instance);
        gprof_end(gq, fs ? fs : vs, 0, (uint32_t)dr->count, inst);
    }
    if (vs->stores || (fs && fs->stores)) {
        g_stores_pending = 1;
        if (vs->stores) written_add(vs, 0);
        if (fs && fs->stores) written_add(fs, 1);
    }
    vkb_cnt_add(g_draws, 1);
    if (snap_after) {
        snap_after = 0;
        vkb_pass_end_r(PE_SNAP);
        const p_set_rt *rt = vkb_rt();
        char what[16];
        for (int i = 0; i < rt->n && i < 8; i++) {
            snprintf(what, sizeof what, "a_rt%d", i);
            if (rt->color[i]) vkb_tex_snap(vkb_cmd(), vkb_image_of(rt->color[i]), what);
        }
    }
}

void vkb_dispatch(const uint8_t *a) {
    big_init();
    fixed_buffers();
    const p_dispatch *d = (const p_dispatch *)a;
    if (draw_trace()) {
        VLOG("[draw] frame %llu dispatch %dx%dx%d | prog %llu cs %s", (unsigned long long)vkb_frame(), (int)d->x, (int)d->y, (int)d->z,
             S.prog ? (unsigned long long)S.prog->serial : 0ull, resolve_program() && S.sh[5] ? "ok" : "-");
        if (resolve_program() && S.sh[5]) trace_resources(S.sh[5], 5);
    }
    vkb_audit_draw_begin();
    if (!resolve_program() || !S.sh[5]) { vkb_cnt_add(g_skipped[0], 1); vkb_audit(S.bad_key ? A_SHADER_UNSUPPORTED : A_DRAW_NO_PROGRAM, S.bad_key); return; }
    vkb_pass_end_r(PE_DISPATCH);
    dummy_init();
    const vkb_shader *cs = S.sh[5];
    g_fb = 0;   // no targets: every sampled image leaves any feedback layout
    prepare_images(cs, 5);
    VkCommandBuffer cb = vkb_cmd();
    mirrors_prepare(cs, 5);
    writes_t *W = &B->writes;
    W->n = 0;
    write_stage(W, VK_NULL_HANDLE, cs, 5);
    VkDescriptorSet set = resolve_set(W, 1);
    // [temporary] compute sees all earlier work and later work sees it: full barriers
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk.vkCmdPipelineBarrier2(cb, &dep);
    g_stores_pending = 0;   // the full barrier covers the draws' stores too
    bind_set(cb, set, 1);
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT;
    vk.vkCmdBindShadersEXT(cb, 1, &st, &cs->obj);
    int gq = gprof_begin();
    vkb_checkpoint("dispatch", cs->key, NULL);
    vk.vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    gprof_end(gq, cs, 1, (uint32_t)(d->x * d->y * d->z), 1);
    vk.vkCmdPipelineBarrier2(cb, &dep);
    vkb_cnt_add(g_draws, 1);
}
