// Internal interface of the Vulkan NVN replayer (backend_vk.c, vkb_tex.c).
#pragma once
#include <stdint.h>

#include "nvn/nvn_mtl.h"
#include "nvn/nvn_obj.h"
#include "../vk/vk.h"

#define VKB_SLOTS 8                // command buffers in flight (backend_vk.c)

// ---- backend_vk.c
const p_set_rt *vkb_rt(void);    // current render targets
uint64_t vkb_frame(void);         // presents so far
int vkb_rt_count(void);           // colour targets of the current pass
VkExtent2D vkb_rt_extent(void);   // the current pass's render area
int vkb_slot(void);               // the command slot of the submission being replayed (per-slot rings / pools)
void vkb_gprof_report(void);          // TOTK_GPU_PROF: GPU time per shader of one frame (development)
// Begins the render pass on the current targets if needed (0: not drawable); with no targets at all: a pass without
// attachments over `bare`. fb: the targets the draws also sample (bit i: colour i, VKB_FB_DEPTH: depth), which get the
// attachment feedback loop layout.
enum { VKB_FB_DEPTH = 1u << 8 };
int vkb_pass_begin(VkExtent2D bare, uint32_t fb);
uint32_t vkb_pass_fb(void);   // the current pass's feedback targets
uint64_t vkb_pass_serial(void);   // changes with every pass begun
void vkb_pass_end(void);
enum { PE_OTHER, PE_SET_RT, PE_BARRIER, PE_COPY, PE_UPDATE, PE_CLEAR_TEX, PE_SUBMIT, PE_STORES, PE_SNAP, PE_FEEDBACK, PE_IMAGES,
       PE_MIRROR, PE_DISPATCH, PE_TEXCOPY, PE_SHADOW, PE_COUNT };   // why a pass ended (counted, the per-frame log line)
void vkb_pass_end_r(int why);
int vkb_stores_take(void);
int vkb_scissor(VkRect2D *r);
int vkb_viewport0(float v[4]);   // the scissor set by the game (0: none)
uint64_t vkb_draw_count(void);   // draws and dispatches recorded so far   // vkb_draw.c: storage writes since the last pass began (and forgets them)
// Destroys a view / an image and its memory once the GPU has finished every submission that may use them: the one being
// recorded now included (its earlier commands may already refer to them).
void vkb_destroy_later(VkImageView view, VkImage img, VkDeviceMemory mem);
void vkb_passprof_note(const char *key);   // TOTK_GPU_PASSPROF: names the open pass by its first shader
void vkb_checkpoint(const char *what, const char *k1, const char *k2);   // TOTK_VK_CHECKPOINTS: names the next GPU command
int vkb_in_pass(void);
// Host-visible upload memory of the current submission (reused after its fence): n bytes aligned to `align`.
void *vkb_ring_alloc(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
void vkb_ring_stats(uint64_t *chunks, uint64_t *bytes, uint64_t *pairs);   // upload memory now: chunks, bytes, slot x thread pairs
void vkb_ring_counts(uint64_t *peak, uint64_t *made, uint64_t *freed);   // its peak bytes; chunks made and given back so far
uint64_t vkb_ring_reclaim(void);   // out of memory: idle slots' upload memory back (bytes freed)
int vkb_slot_idle(int slot);   // the GPU has finished the slot's submission and it does not record now (backend_vk.c)
// What the next vkAllocateMemory of this thread is for (the memory log counts by it; reset by the allocation).
enum { MT_OTHER, MT_IMAGE, MT_RING, MT_DESC, MT_POOL_COPY, MT_MIRROR, MT_UCAP, MT_WRITEBACK, MT_COPY_BACK, MT_N };
extern __thread int vkb_mtag;
VkCommandBuffer vkb_cmd(void);    // the command buffer of the submission being replayed
int vkb_render_suppressed(void);   // rendering disabled or conditional (recorded state; audit)
int vkb_ready(void);              // 1 when replaying on a Vulkan device

// ---- vkb_tex.c: NVN textures as Vulkan images (one per texture serial: the game copies texture objects by value)
typedef struct vkb_image {
    uint64_t serial;
    nvn_texdesc d;
    VkImage img;
    VkDeviceMemory mem;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;          // the whole image's current layout (replay thread)
    uint32_t layers, levels, width, height, depth;
    int astc;                      // ASTC in the game: decoded to RGBA8 on upload, or BC7 from the cache (bc7)
    int bc7;                       // an ASTC texture held as BC7 (1) or BC1 (2) from the cache (vkb_bc7.c); 0: RGBA8
    int uploading;                 // an upload is being recorded (several threads: the others wait for it, vkb_tex_upload)
    VkImageUsageFlags usage;
    int compressed;                // block-compressed format: no attachment / storage use
    int gpu_written;               // rendered / cleared / copied to by the GPU: never overwritten from game memory ([temporary])
    uint64_t upload_stamp;         // nvn_dirty generation of the last upload from game memory (0 = never)
    uint64_t dirty_checked;        // the submission in which the game memory was last checked for CPU writes
    int stale;                     // that check found CPU writes: upload before the next use
    int cpu_written;               // WriteTexels changed its game memory: upload before the next use (any pool)
    int not_uploadable;            // contents in game memory that the upload cannot take (depth / 3-component formats)
    uint64_t vpool_gen;            // in a virtual pool: the mapping generation of the last upload (0: not in one)
    uint64_t content_hash;         // game memory without a write watch: its contents at the last upload (0: not hashed)
    uint64_t hash_sub;             // the submission in which content_hash was last compared
    int hash_hot;                  // its contents changed before (the font atlas): compared every submission, budget or not
    const uint8_t *storage;        // the texture's storage in game memory (block-linear / linear)
    uint64_t write_seq;            // vkb_image_written: when the GPU last wrote it (0 = never)
    uint64_t write_frame;          // ...and in which present
    int cpu_visible;               // its game memory is in a pool the CPU can access: GPU contents are written back to it
    int wb_pending;                // in the write-back queue (vkb_tex.c vkb_tex_writeback_frame)
    uint64_t wb_seq;               // write_seq of the contents last written back (or being)
    struct vkb_image *refresh_from;   // newer contents at the same memory in another size: scaled in before use (vkb_tex.c)
} vkb_image;
void vkb_image_written(vkb_image *im);   // the GPU writes it (render target, clear, copy, storage)
// TOTK_DEPTH_AUDIT=1 (development, vkb_tex.c): depth is D32F here, D16/D24 in the game. Logs colour textures sampled
// (how "sample") or read back (how "ReadTexels") from memory the GPU drew depth into after they were last written, and
// depth bias by the depth target's format (vkb_draw.c).
int vkb_depth_audit_on(void);
void vkb_depth_audit_read(const vkb_image *im, const char *how);
void vkb_depth_audit_bias(const vkb_image *depth, float units, float factor);
int vkb_tex_refresh(VkCommandBuffer cb, vkb_image *im);   // outside a pass: the pending refresh_from copy; 1 if recorded

vkb_image *vkb_tex_feedback_copy(VkCommandBuffer cb, vkb_image *im);   // outside a pass: a copy to sample im from (vkb_tex.c)
// A compressed texture as a render target through an uncompressed view of its blocks (L21): outside a pass, the view of
// an image of the level's size in blocks (w, h) that the pass draws into; vkb_tex_block_flush (after the pass) copies it
// into the texture. VK_NULL_HANDLE when the view is not one of blocks.
VkImageView vkb_tex_block_target(VkCommandBuffer cb, vkb_image *im, const view_t *v, uint32_t *w, uint32_t *h);
void vkb_tex_block_flush(VkCommandBuffer cb);
uint64_t vkb_tex_feedback_copies(void);
int vkb_fb_general(void);   // no feedback loops: sampled targets attached in GENERAL and read in place (vkb_draw.c)
vkb_image *vkb_image_of(const tex_t *t);   // creates it on first use; NULL for textures that cannot be images yet
// Moves the image to `layout` before work of `stage`/`access` (records a barrier on the current command buffer).
void vkb_image_use(VkCommandBuffer cb, vkb_image *im, VkImageLayout layout, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
// A view of the image as NVN's view (NULL = the whole texture); cached per image.
// attachment: 0 sampled, 1 render target, 2 storage image. view_type: a VkImageViewType the shader samples it as, or -1
// (from the texture / view target). VK_NULL_HANDLE when the image cannot be used so.
VkImageView vkb_view(vkb_image *im, const view_t *v, int attachment, int view_type);
// Brings the image's contents up to date from game memory when the CPU wrote them (records copies on cb; outside a pass).
// 1 when it recorded an upload.
int vkb_tex_upload(VkCommandBuffer cb, vkb_image *im);
void vkb_tex_image_stats(uint64_t *made, uint64_t *shared, uint64_t *taken_back);   // images by memory and shape

// ---- vkb_draw.c: device-local mirrors of written game memory for the current submission
int vkb_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // the mirror, else the imported pool
void vkb_mirrors_evict(uint64_t addr, uint64_t len);   // mirrors straddling the range go back (outside a pass)
void vkb_mirrors_submit(VkCommandBuffer cb);           // end of a submission: all back to game memory

// ---- backend_vk.c / vkb_shadow.c: copies of the memory pools in device memory (discrete GPUs)
void vkb_pool_use(uint64_t addr, uint64_t len, int write);   // before a GPU access is recorded (write: may end the pass)
int vkb_pool_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // the copy when current, else game memory
int vkb_shadow_on(void);
uint32_t vkb_shadow_alloc(VkDeviceSize size, VkBuffer *buf, VkDeviceSize *off);   // 0: none
void vkb_shadow_free(uint32_t h);   // reused after the submission being recorded
uint64_t vkb_shadow_bytes(void);

// ---- vkb_ucap.c: uniform contents as of the submit call
int vkb_ucap_on(void);
uint64_t mtl_done(void);   // backend_vk.c: the last submission the GPU completed
int vkb_ucap_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);   // the copy of a bound uniform range
void vkb_ucap_read(uint64_t addr, void *out, size_t n);   // CPU read of uniform memory, from the copy where there is one
void vkb_ucap_drop(uint64_t addr, uint64_t len);
void vkb_ucap_inline(uint64_t addr, uint64_t n, const uint8_t *data);   // an inline uniform update, for later CPU reads
int vkb_ucap_why(uint64_t addr, uint64_t size);          // a GPU write: copies it overlaps are no longer used
void vkb_ucap_stats(uint64_t *caps, uint64_t *bytes, uint64_t *ns, uint64_t *hits, uint64_t *drops);

// ---- vkb_bc7.c: ASTC textures as BC7, from a cache by the contents of the ASTC blocks
typedef struct { uint64_t a, b; } vkb_bc7_key;
int vkb_bc7_on(void);   // TOTK_BC7=0: ASTC decoded to RGBA8 (A/B)
// The key of one surface: its ASTC blocks in rows (n bytes), its size in texels (depth: slices of a 3D level), block size, sRGB.
vkb_bc7_key vkb_bc7_key_of(const uint8_t *blocks, size_t n, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb);
enum { VKB_BC_7 = 0, VKB_BC_1 = 1 };                         // the format of a cache entry
int vkb_bc7_lookup(vkb_bc7_key k, int *fmt, size_t *len);    // in the cache: its format and length
int vkb_bc7_has(vkb_bc7_key k, size_t n);                    // n bytes of BC7 blocks for it in the cache
int vkb_bc7_read(vkb_bc7_key k, uint8_t *out, size_t n);
void vkb_bc7_store(vkb_bc7_key k, const uint8_t *data, size_t n);
void vkb_bc_store(vkb_bc7_key k, const uint8_t *data, size_t n, int fmt);
void vkb_bc7_queue(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb);   // encoded in the background
void vkb_bc7_encode_now(vkb_bc7_key k, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t d, int srgb, int fmt, uint8_t *out);
void vkb_bc7_stats(uint64_t *hits, uint64_t *misses, uint64_t *encoded, uint64_t *dropped, uint64_t *encode_ns, size_t *queued);

// ---- vkb_desc.c: descriptors in buffers (VK_EXT_descriptor_buffer)
int vkb_db_on(void);
void vkb_db_layouts(const VkDescriptorSetLayout layout[2]);
void vkb_db_slot_reset(int slot);
uint64_t vkb_db_gen(void);   // changes when offsets made before are void (descriptor memory replaced)
VkDeviceSize vkb_db_set(VkCommandBuffer cb, int slot, uint64_t rec, const VkWriteDescriptorSet *w, int n, int compute);
void vkb_db_bind(VkCommandBuffer cb, int slot, uint64_t rec);   // the memory offsets refer to, bound to this command buffer
void vkb_db_stats(uint64_t *get_calls, uint64_t *cache_hits);

// ---- vkb_copy.c: NVN copies between game memory and textures (outside a pass; they end one)
void vkb_copy_stride(int64_t stride);                       // SetCopyRowStride
int64_t vkb_copy_stride_get(void);
int vkb_copy_t2b_range(const p_copy_b2t *c, uint64_t *addr, uint64_t *len);   // the buffer range CopyTextureToBuffer writes
void vkb_copy_buf_tex(const p_copy_b2t *c, int to_tex);     // CopyBufferToTexture (1) / CopyTextureToBuffer (0)
void vkb_copy_tex_tex(const p_copy_t2t *c);                 // CopyTextureToTexture
int vkb_tex_stale(vkb_image *im);          // the CPU changed its game memory since the upload (checked once per submission)
VkFormat vkb_format(int nvn_format, VkImageAspectFlags *aspect, int *astc);
void vkb_tex_release(const tex_t *t);
int vkb_tex_snap_wanted(const char *vkey, const char *fkey, uint64_t frame);   // TOTK_SNAP (development)
void vkb_tex_snap(VkCommandBuffer cb, vkb_image *im, const char *what);
void vkb_tex_snap_flush(void);
void vkb_tex_lumlog(const tex_t *t, uint64_t present);   // TOTK_LUMLOG (development)
void vkb_dump_request(void);                // TOTK_DUMP_KEY=<dir>: F9 dumps a frame and traces its draws (development)
void vkb_draw_trace_arm(uint64_t frame, long long n);   // TOTK_VK_DRAWTRACE from a frame on, armed while running
void vkb_vcheck_add(uint64_t addr, uint64_t len, uint32_t draw, int kind);   // F9: a traced draw's index 0 / vertex 1 / constant 2 / storage 3 range
void vkb_tex_vcheck_add(const vkb_image *im);   // F9: a texture a traced draw samples (vkb_tex.c)
void vkb_tex_vcheck_run(void);              // F9: those textures compared with game memory
void vkb_tex_b2b_log(uint64_t src, uint64_t dst, uint64_t n);   // TOTK_B2B_LOG: buffer copies over GPU-written images
int vkb_rt_log_match(const tex_t *t);       // TOTK_RT_LOG: the format and size being logged (backend_vk.c)
int vkb_rt_log_d(const nvn_texdesc *d);
uint64_t vkb_pool_room(uint64_t addr);       // bytes from addr to the end of its imported pool (0: none)
int vkb_span_addr(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // a range across pools, laid out as the GPU sees it
uint64_t nvn_va2cpu(uint64_t va);           // a GPU address the game gave: the CPU address of that byte (hle_nvn.c)
void vkb_tex_dump(const char *dir);         // TOTK_DUMP: the GPU-written images as BMP (development)       // the game finalized the texture: image freed after the GPU is done

// ---- vkb_draw.c: render state, texture / sampler pools, draws
void vkb_slot_reset(int slot);                  // the slot's previous submission has completed: reuse its memory
void vkb_state_reset(void);
void vkb_state(uint32_t op, const uint8_t *payload);   // state / binding commands
void vkb_draw(const uint8_t *payload, int indexed);    // OP_DRAW / OP_DRAW_IDX
void vkb_dispatch(const uint8_t *payload);             // OP_DISPATCH
void vkb_draw_stats(uint64_t *draws, uint64_t *skipped);
size_t vkb_state_size(void);                    // the render state as bytes (a chunk's starting state, vkb_mt.c)
void vkb_state_save(void *dst);
void vkb_state_load(const void *src);
void vkb_rec_begin(void);                       // this thread starts a command buffer: its recording caches start empty
void vkb_draw_mt_init(void);                    // objects made on first use, made before threads record (prologue)
void vkb_draw_prescan(const uint8_t *payload, int indexed, int dispatch);   // the GPU writes a draw / dispatch makes (ucap)

// ---- vkb_mt.c: submissions recorded by several threads (2026-10-24)
// The translation thread walks a submission once in order (vkb_mt_submit): render state, events and counters,
// conditional rendering, the GPU writes that void captured uniforms, and cuts it into chunks at render target changes,
// each with its starting state. Recording threads (the translation thread too, once the walk is done) record the chunks
// into command buffers of their own. Work that must come before every chunk (texture uploads, pool copies) goes into the
// submission's prologue (vkb_pro_begin / vkb_pro_end, under a lock). Each chunk tracks the layouts its images need first
// and leave last (vkb_trk); between chunks a small command buffer moves them (and waits for all earlier work).
int vkb_mt_on(void);             // TOTK_VK_RECORD_THREADS (default: by the number of processors; 1: single thread)
int vkb_mt_recording(void);      // this thread is recording a chunk or the prologue
uint64_t vkb_seq(void);          // the position of the command being replayed within its submission
void vkb_seq_set(uint64_t seq);
void vkb_pro_begin(void);        // records into the prologue from here (a no-op on one thread)
void vkb_pro_end(void);
void vkb_cmd_set(VkCommandBuffer cb);   // this thread's command buffer (VK_NULL_HANDLE: the submission's own)
void vkb_mt_slot_reset(int slot);       // the slot's previous submission has completed (backend_vk.c mtl_sub_begin)
void vkb_mt_submit(const uint64_t *h, int n);   // records the submission's command handles (after mtl_sub_begin)
// Image layouts per chunk (vkb_tex.c): the first layout each image needs in it and the one it leaves it in.
typedef struct { vkb_image *im; VkImageLayout first, cur; } vkb_trk_e;
typedef struct { vkb_trk_e *e; int n, cap; int *tab; int tcap; } vkb_trk;
void vkb_trk_set(vkb_trk *t);    // this thread records with this tracking (NULL: images' own layouts, one thread)
vkb_trk *vkb_trk_get(void);
void vkb_trk_reset(vkb_trk *t);
int vkb_layout_is(vkb_image *im, VkImageLayout l);   // the image is in l as recorded so far (a first use: it will be)
VkImageLayout vkb_layout(vkb_image *im);              // its layout as recorded so far
