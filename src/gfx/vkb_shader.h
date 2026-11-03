// Shaders of the Vulkan NVN replayer (vkb_shader.c): translated SPIR-V as shader objects, universal descriptor layout.
#pragma once
#include <stdint.h>

#include "vkb.h"

// Universal layout: per stage VKB_STAGE_BINDINGS bindings of set 0 (vertex from 0, fragment from 64; compute: own layout).
enum {
    VKB_STAGE_BINDINGS = 64,
    VKB_B_SUPPORT = 0,                   // Ryujinx support buffer
    VKB_B_CBUF = 1, VKB_NCBUF = 17,      // c0..c16 at +1..17
    VKB_B_SBUF = 18, VKB_NSBUF = 16,     // storage buffers in the shader's order
    VKB_B_TEX = 34, VKB_NTEX = 24,       // combined textures in the shader's order
    VKB_B_IMG = 58, VKB_NIMG = 4,        // images in the shader's order
};

enum { VKB_SB_TABLE, VKB_SB_UBO, VKB_SB_UBO_PTR };
typedef struct { uint8_t slot, separate; uint16_t type; } vkb_tex_ref;   // NVN binding slot; Ryujinx SamplerType

enum { VKB_VARIANTS = 16 };
typedef struct { uint32_t intmask, sintmask, outmask, out_umask, out_imask; VkShaderEXT obj; } vkb_variant;

typedef struct {
    VkShaderEXT obj;
    VkShaderStageFlagBits vk_stage;
    int stage;                           // NVN: 0 vertex, 1 fragment, 5 compute
    uint32_t cbuf_mask;                  // hardware constant buffer slots read (binding +1+slot): c3..c16 = game uniform
                                         // buffer slot-3, c1 = the shader's own constants (c1 below), c0 = NVN driver bank
    const uint8_t *c1; uint32_t c1_size; // code[control+0x700 .. length): the shader's constants (a copy)
    uint64_t c1_rec; VkBuffer c1_buf; VkDeviceSize c1_off;   // its copy in the submission being recorded (vkb_draw.c)
    // storage buffers, binding +18+i. Where the address comes from (MacNativeGames' totk_xlate Service.cs):
    //  VKB_SB_TABLE: the NVN storage buffer slot (driver table in c0); VKB_SB_UBO: (compute) the uniform buffer slot, its
    //  range used as storage; VKB_SB_UBO_PTR: a 64-bit address stored in the game's uniform buffer `slot` at byte `off`;
    //  slot 0xFF: unknown (an empty buffer is bound)
    struct { uint8_t slot, kind, write, read; uint16_t off; } sbuf[VKB_NSBUF]; int nsbuf;   // write / read: stores to / loads from it
    vkb_tex_ref tex[VKB_NTEX]; int ntex; // binding +34+i
    vkb_tex_ref img[VKB_NIMG]; int nimg; // binding +58+i
    uint32_t frag_outputs;               // colour outputs written (bit per target)
    int stores;                          // writes storage buffers or images (later work must wait for it)
    int unsupported;                     // a resource outside the layout: draws with it are skipped
    const char *key;                     // SHA-256 key of the translation (traces)
    uint32_t *spv; size_t nspv_words;    // vertex shaders: the module as loaded (bindings rewritten), for variants
    uint32_t stamp;                      // which translation (vkb_shbin_variant.stamp)
    uint32_t in_mask, out_mask;          // input / output locations
    uint32_t in_umask, in_imask;         // inputs of unsigned / signed integer type (fragment shaders)
    vkb_variant variants[VKB_VARIANTS]; int nvariants;   // vertex variants (integer attributes, added outputs) made so far
} vkb_shader;

const vkb_shader *vkb_shader_get(const NVNshaderData *sd);   // NULL: no translation (logged)
// The vertex shader object for integer attributes at intmask (signed: sintmask) and drawn with the fragment shader fs
// (NULL: none): outputs added for the inputs of fs it does not write. See vkb_spv.c.
VkShaderEXT vkb_shader_variant(const vkb_shader *vs, uint32_t intmask, uint32_t sintmask, const vkb_shader *fs);
enum { VKB_LAYOUT_GRAPHICS, VKB_LAYOUT_COMPUTE, VKB_LAYOUT_BUFFERS, VKB_LAYOUTS };   // vkb_set_layout ids
VkDescriptorSetLayout vkb_set_layout(int id);   // with split sets VKB_LAYOUT_GRAPHICS holds the textures and images only
uint32_t vkb_set_layouts(int compute, VkDescriptorSetLayout out[2]);   // a pipeline's set layouts: their count
int vkb_split_sets(void);   // graphics descriptors in two sets: 0 textures and images, 1 buffers
VkPipelineLayout vkb_pipeline_layout(int compute);
void vkb_shader_stats(uint64_t *loaded, uint64_t *missing);
int vkb_prepare_shaders(void);   // UltraTears --prepare-shaders: every shader compiled by this driver; the exit code
void vkb_shader_report(void);   // the memory log line: shader objects, programs, binaries on disk, compiled ahead
// Shader objects follow the game's programs (vkb_shader.c): set holds the program's shaders and returns what it held
// before; drop (ProgramFinalize) returns what it held; release, called from the translation queue after the commands
// queued before it, lets go of them. NULL: nothing to release.
typedef struct vkb_shader_rel vkb_shader_rel;
vkb_shader_rel *vkb_shader_prog_set(const void *prog, int n, const NVNshaderData *sd);
vkb_shader_rel *vkb_shader_prog_drop(const void *prog);
void vkb_shader_release(vkb_shader_rel *rel);
