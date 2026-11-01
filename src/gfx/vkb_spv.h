// SPIR-V rewrites of translated shaders for the bind state (vkb_spv.c).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Vertex inputs at the locations in locmask (vec4 float in the translation) become uvec4, or ivec4 where signedmask
// has the bit; loads from them are bitcast back to float. *out: a new module (malloc) of *outw words. Returns the
// number of inputs changed (0: nothing to do or a shape not handled - the caller keeps the original).
int vkb_spv_int_inputs(const uint32_t *in, size_t nwords, uint32_t locmask, uint32_t signedmask, uint32_t **out, size_t *outw);
// Locations of the module's Input variables (bit per location).
uint32_t vkb_spv_input_mask(const uint32_t *in, size_t nwords);
// Locations of the module's Output variables (bit per location).
uint32_t vkb_spv_sbuf_reads(const uint32_t *in, size_t nw, uint32_t first, int n);   // storage buffers read (bit per index)
uint32_t vkb_spv_output_mask(const uint32_t *in, size_t nwords);
// Input locations of 32-bit unsigned (*umask) / signed (*imask) integer type.
void vkb_spv_input_int_masks(const uint32_t *in, size_t nwords, uint32_t *umask, uint32_t *imask);
// A vertex shader with outputs added at the locations in locmask (vec4: float, uvec4 where umask has the bit, ivec4
// where imask has it), set to (0, 0, 0, 1) at the start of main: the fragment inputs it does not write. *out as
// above. Returns the number of outputs added (0: a shape not handled - the caller keeps the original).
int vkb_spv_add_outputs(const uint32_t *in, size_t nwords, uint32_t locmask, uint32_t umask, uint32_t imask, uint32_t **out,
                        size_t *outw);
// Depth compares with the reference clamped to [0, 1] (FClamp before every OpImage*Dref*): the Switch keeps a D16
// shadow map, whose compares clamp the reference; ours is D32F, whose compares do not (GitHub #34). *out as above.
// Returns the number of compares rewritten (0: none, or a shape not handled - the caller keeps the original).
int vkb_spv_clamp_dref(const uint32_t *in, size_t nwords, uint32_t **out, size_t *outw);
// Without VK_EXT_fragment_shader_interlock (AMD Polaris and Intel HD 600 on Windows): the interlock dropped - its begin /
// end, execution modes, capability and extension - in place. The section it guarded then runs unordered between
// overlapping fragments (a rare blending artefact) instead of the module being refused. Returns the new word count.
size_t vkb_spv_drop_interlock(uint32_t *w, size_t nwords);
