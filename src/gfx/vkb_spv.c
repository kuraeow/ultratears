// SPIR-V rewrites of translated shaders for the bind state (milestone 3.3 leftovers, the first part of the variant
// service). Vertex attributes of integer formats: Maxwell hands a shader the attribute's raw bits, and TOTK's skinned
// vertex shaders read bone indices that way (floatBitsToInt(attr) * 48). The default translation declares every input
// vec4 float, so the attribute had to be given as a scaled format (5 -> 5.0f) and the bone index was garbage. Here the
// input becomes uvec4 / ivec4 (the attribute is then given in its own integer format) and every load from it is
// bitcast back to float: the rest of the shader is unchanged and sees the raw bits, as on the hardware.
#include <stdlib.h>
#include <string.h>

#include "vkb_spv.h"

enum { OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_LOAD = 61,
       OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71, OP_BITCAST = 124 };
enum { DEC_LOCATION = 30, SC_INPUT = 1 };

typedef struct { uint32_t *w; size_t n, cap; } words;
static void put(words *o, uint32_t v) {
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 1024;
        o->w = realloc(o->w, o->cap * sizeof *o->w);
        if (!o->w) abort();
    }
    o->w[o->n++] = v;
}
static void put_ins(words *o, uint32_t op, int n, const uint32_t *ops) {
    put(o, (uint32_t)(n + 1) << 16 | op);
    for (int i = 0; i < n; i++) put(o, ops[i]);
}

int vkb_spv_int_inputs(const uint32_t *in, size_t nw, uint32_t locmask, uint32_t signedmask, uint32_t **out, size_t *outw) {
    if (nw < 5 || !locmask) return 0;
    uint32_t bound = in[3];
    // ids by kind (0: absent)
    uint32_t f32 = 0, vec4f = 0, i32[2] = {0, 0}, ivec4[2] = {0, 0};   // [0] unsigned, [1] signed
    uint32_t *loc_of = calloc(bound, sizeof *loc_of);                  // id -> location + 1 (decorated)
    uint32_t *ptr_pointee = calloc(bound, sizeof *ptr_pointee);        // Input pointer type id -> pointee
    uint8_t *var = calloc(bound, 1);                                   // id -> 1 + signedness of a patched input
    uint8_t *chain = calloc(bound, 1);                                 // access chain result -> 1 + signedness
    if (!loc_of || !ptr_pointee || !var || !chain) abort();
    int ok = 1, patched = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) { ok = 0; break; }
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_TYPE_FLOAT && a[1] == 32) f32 = a[0];
        else if (op == OP_TYPE_INT && a[1] == 32) i32[a[2] ? 1 : 0] = a[0];
        else if (op == OP_TYPE_VECTOR && a[2] == 4) {
            if (a[1] == f32) vec4f = a[0];
            else if (a[1] == i32[0] && i32[0]) ivec4[0] = a[0];
            else if (a[1] == i32[1] && i32[1]) ivec4[1] = a[0];
        } else if (op == OP_TYPE_POINTER && a[1] == SC_INPUT && a[0] < bound) ptr_pointee[a[0]] = a[2];
        else if (op == OP_VARIABLE && a[2] == SC_INPUT && a[1] < bound && loc_of[a[1]]) {
            uint32_t loc = loc_of[a[1]] - 1;
            if (loc < 32 && (locmask >> loc & 1)) {
                if (a[0] >= bound || ptr_pointee[a[0]] != vec4f || !vec4f) ok = 0;   // only vec4 float inputs
                else { var[a[1]] = (uint8_t)(1 + (signedmask >> loc & 1)); patched++; }
            }
        }
        i += wc;
    }
    if (!ok || !patched) { free(loc_of); free(ptr_pointee); free(var); free(chain); return 0; }
    // new ids: missing integer scalar / vector types, Input pointers to them
    uint32_t next = bound, new_i32[2] = {0, 0}, new_ivec4[2] = {0, 0}, p_vec[2], p_scalar[2];
    for (int s = 0; s < 2; s++) {
        if (!i32[s]) i32[s] = new_i32[s] = next++;
        if (!ivec4[s]) ivec4[s] = new_ivec4[s] = next++;
        p_vec[s] = next++;
        p_scalar[s] = next++;
    }
    words o = {0};
    for (int k = 0; k < 5; k++) put(&o, in[k]);
    int types_done = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *a = in + i + 1;
        if (op == OP_VARIABLE && a[1] < bound && var[a[1]]) {
            if (!types_done) {   // the new types, before the first patched input (all they refer to is declared above)
                for (int s = 0; s < 2; s++) {
                    if (new_i32[s]) put_ins(&o, OP_TYPE_INT, 3, (uint32_t[]){i32[s], 32, (uint32_t)s});
                    if (new_ivec4[s]) put_ins(&o, OP_TYPE_VECTOR, 3, (uint32_t[]){ivec4[s], i32[s], 4});
                    put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){p_vec[s], SC_INPUT, ivec4[s]});
                    put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){p_scalar[s], SC_INPUT, i32[s]});
                }
                types_done = 1;
            }
            int s = var[a[1]] - 1;
            put(&o, in[i]);
            put(&o, p_vec[s]);
            for (uint32_t k = 2; k < wc; k++) put(&o, in[i + k]);
        } else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && a[2] < bound && var[a[2]]) {
            int s = var[a[2]] - 1;
            if (a[1] < bound) chain[a[1]] = (uint8_t)(1 + s);
            if (wc != 5) ok = 0;   // a component of the vector: one index
            put(&o, in[i]);
            put(&o, p_scalar[s]);
            for (uint32_t k = 2; k < wc; k++) put(&o, in[i + k]);
        } else if (op == OP_LOAD && a[2] < bound && (var[a[2]] || chain[a[2]])) {
            int whole = var[a[2]] != 0, s = (whole ? var[a[2]] : chain[a[2]]) - 1;
            uint32_t t = whole ? ivec4[s] : i32[s], raw = next++;
            put(&o, in[i]);
            put(&o, t);
            put(&o, raw);
            for (uint32_t k = 3; k < wc; k++) put(&o, in[i + k]);
            put_ins(&o, OP_BITCAST, 3, (uint32_t[]){a[0], a[1], raw});   // the original result id, as float again
        } else {
            for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
        }
        i += wc;
    }
    free(loc_of); free(ptr_pointee); free(var); free(chain);
    if (!ok) { free(o.w); return 0; }
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return patched;
}

// Storage buffers the shader reads: bindings first..first+n-1, bit i when a load or an atomic (they read too) goes through a
// pointer into binding first+i. The translator's reflection only tells writes; a draw that only writes a buffer need not
// wait for earlier draws' stores to it (vkb_draw.c). Pointers are followed through access chains and copies, which in
// the translator's SPIR-V come before their uses.
uint32_t vkb_spv_sbuf_reads(const uint32_t *in, size_t nw, uint32_t first, int n) {
    if (nw < 5 || n <= 0) return 0;
    uint32_t bound = in[3], mask = 0;
    int8_t *of = malloc(bound);   // id -> storage buffer index, -1: none
    uint32_t *bind = calloc(bound, sizeof *bind);
    if (!of || !bind) abort();
    memset(of, -1, bound);
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == 33 && a[0] < bound) bind[a[0]] = a[2] + 1;   // Binding
        else if (op == OP_VARIABLE && wc >= 4 && a[1] < bound && bind[a[1]] > first && bind[a[1]] - 1 - first < (uint32_t)n)
            of[a[1]] = (int8_t)(bind[a[1]] - 1 - first);
        else if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == 67 || op == 83) && wc >= 4 && a[1] < bound && a[2] < bound)
            of[a[1]] = of[a[2]];   // OpPtrAccessChain 67, OpCopyObject 83
        else if ((op == OP_LOAD || (op >= 227 && op <= 242 && op != 228)) && wc >= 4 && a[2] < bound && of[a[2]] >= 0)
            mask |= 1u << of[a[2]];   // OpLoad, OpAtomic* except OpAtomicStore
        i += wc;
    }
    free(of);
    free(bind);
    return mask;
}
// Outputs a vertex shader does not have, for the inputs of the fragment shader it is drawn with. With shader objects
// the stages are translated one by one, so a fragment shader can read a location the vertex shader never declares:
// undefined in Vulkan (VUID-RuntimeSpirv-OpEntryPoint-08743); NVIDIA happens to give something harmless, AMD does
// not. Ryujinx, translating the stages together, declares such outputs in the vertex shader and writes (0, 0, 0, 1)
// to them first (Translator.cs, InitializeOutputComponent); this does the same on the translated module.
int vkb_spv_add_outputs(const uint32_t *in, size_t nw, uint32_t locmask, uint32_t umask, uint32_t imask, uint32_t **out,
                        size_t *outw) {
    if (nw < 5 || !locmask) return 0;
    uint32_t bound = in[3], next = bound;
    uint32_t f32 = 0, s32[2] = {0, 0}, vec[3] = {0, 0, 0};   // vec: [0] float, [1] uint, [2] int (vec4)
    uint32_t entry_fn = 0;
    size_t entry_at = 0, first_dec = 0, first_fn = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) return 0;
        const uint32_t *a = in + i + 1;
        if (op == 15 && a[0] == 0 && !entry_at) { entry_at = i; entry_fn = a[1]; }   // OpEntryPoint Vertex
        else if (op == OP_DECORATE && !first_dec) first_dec = i;
        else if (op == OP_TYPE_FLOAT && a[1] == 32) f32 = a[0];
        else if (op == OP_TYPE_INT && a[1] == 32) s32[a[2] ? 1 : 0] = a[0];
        else if (op == OP_TYPE_VECTOR && a[2] == 4) {
            if (a[1] == f32 && f32) vec[0] = a[0];
            else if (a[1] == s32[0] && s32[0]) vec[1] = a[0];
            else if (a[1] == s32[1] && s32[1]) vec[2] = a[0];
        } else if (op == 54 && !first_fn) first_fn = i;   // OpFunction
        i += wc;
    }
    if (!entry_at || !first_dec || !first_fn) return 0;
    // what is missing gets a new id; scalar types before the vectors that use them
    uint32_t new_f32 = 0, new_s32[2] = {0, 0}, new_vec[3] = {0, 0, 0}, ptr[3], one[3], zero[3], val[3], var[32];
    int need[3] = {0, 0, 0}, nvar = 0;
    for (int l = 0; l < 32; l++)
        if (locmask >> l & 1) need[imask >> l & 1 ? 2 : umask >> l & 1 ? 1 : 0] = 1;
    if (need[0] && !f32) f32 = new_f32 = next++;
    for (int s = 0; s < 2; s++) if (need[1 + s] && !s32[s]) s32[s] = new_s32[s] = next++;
    for (int k = 0; k < 3; k++) {
        if (!need[k]) continue;
        if (!vec[k]) vec[k] = new_vec[k] = next++;
        ptr[k] = next++; zero[k] = next++; one[k] = next++; val[k] = next++;
    }
    for (int l = 0; l < 32; l++) if (locmask >> l & 1) var[l] = next++, nvar++;
    words o = {0};
    for (int k = 0; k < 5; k++) put(&o, in[k]);
    int stored = 0, in_entry = 0, seen_label = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        const uint32_t *a = in + i + 1;
        if (i == entry_at) {   // the entry point lists the new outputs as part of its interface
            put(&o, (uint32_t)(wc + nvar) << 16 | op);
            for (uint32_t k = 1; k < wc; k++) put(&o, in[i + k]);
            for (int l = 0; l < 32; l++) if (locmask >> l & 1) put(&o, var[l]);
            i += wc;
            continue;
        }
        if (i == first_dec)
            for (int l = 0; l < 32; l++) if (locmask >> l & 1) put_ins(&o, OP_DECORATE, 3, (uint32_t[]){var[l], DEC_LOCATION, (uint32_t)l});
        if (i == first_fn) {   // types, constants and variables end here
            if (new_f32) put_ins(&o, OP_TYPE_FLOAT, 2, (uint32_t[]){f32, 32});
            for (int s = 0; s < 2; s++) if (new_s32[s]) put_ins(&o, OP_TYPE_INT, 3, (uint32_t[]){s32[s], 32, (uint32_t)s});
            for (int k = 0; k < 3; k++) {
                if (!need[k]) continue;
                uint32_t scalar = k ? s32[k - 1] : f32;
                if (new_vec[k]) put_ins(&o, OP_TYPE_VECTOR, 3, (uint32_t[]){vec[k], scalar, 4});
                put_ins(&o, OP_TYPE_POINTER, 3, (uint32_t[]){ptr[k], 3, vec[k]});   // StorageClass Output
                put_ins(&o, 43, 3, (uint32_t[]){scalar, zero[k], 0});                      // OpConstant 0 / 0.0
                put_ins(&o, 43, 3, (uint32_t[]){scalar, one[k], k ? 1u : 0x3f800000u});    // OpConstant 1 / 1.0
                put_ins(&o, 44, 6, (uint32_t[]){vec[k], val[k], zero[k], zero[k], zero[k], one[k]});   // OpConstantComposite
            }
            for (int l = 0; l < 32; l++)
                if (locmask >> l & 1) put_ins(&o, OP_VARIABLE, 3, (uint32_t[]){ptr[imask >> l & 1 ? 2 : umask >> l & 1 ? 1 : 0], var[l], 3});
        }
        if (op == 54) in_entry = a[1] == entry_fn;
        // in the entry function, after its first label and the function variables that must open the block
        if (in_entry && !stored && seen_label && op != OP_VARIABLE && op != 8 && op != 317) {   // OpLine, OpNoLine
            for (int l = 0; l < 32; l++)
                if (locmask >> l & 1) put_ins(&o, 62, 2, (uint32_t[]){var[l], val[imask >> l & 1 ? 2 : umask >> l & 1 ? 1 : 0]});   // OpStore
            stored = 1;
        }
        if (in_entry && op == 248) seen_label = 1;   // OpLabel
        for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
        i += wc;
    }
    if (!stored) { free(o.w); return 0; }
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return nvar;
}

// Input locations whose variable is a vector or scalar of 32-bit unsigned (*umask) or signed (*imask) integers.
void vkb_spv_input_int_masks(const uint32_t *in, size_t nw, uint32_t *umask, uint32_t *imask) {
    *umask = *imask = 0;
    if (nw < 5) return;
    uint32_t bound = in[3];
    uint32_t *loc_of = calloc(bound, sizeof *loc_of), *pointee = calloc(bound, sizeof *pointee);
    uint8_t *kind = calloc(bound, 1);   // type id -> 1 unsigned, 2 signed (scalars and vectors of them)
    if (!loc_of || !pointee || !kind) abort();
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_TYPE_INT && a[0] < bound && a[1] == 32) kind[a[0]] = (uint8_t)(a[2] ? 2 : 1);
        else if (op == OP_TYPE_VECTOR && a[0] < bound && a[1] < bound) kind[a[0]] = kind[a[1]];
        else if (op == OP_TYPE_POINTER && a[0] < bound && a[1] == SC_INPUT) pointee[a[0]] = a[2];
        else if (op == OP_VARIABLE && a[2] == SC_INPUT && a[0] < bound && a[1] < bound && loc_of[a[1]] && loc_of[a[1]] <= 32 &&
                 pointee[a[0]] < bound) {
            uint32_t bit = 1u << (loc_of[a[1]] - 1);
            if (kind[pointee[a[0]]] == 1) *umask |= bit;
            else if (kind[pointee[a[0]]] == 2) *imask |= bit;
        }
        i += wc;
    }
    free(loc_of); free(pointee); free(kind);
}

static uint32_t location_mask(const uint32_t *in, size_t nw, uint32_t storage);
uint32_t vkb_spv_input_mask(const uint32_t *in, size_t nw) { return location_mask(in, nw, SC_INPUT); }
uint32_t vkb_spv_output_mask(const uint32_t *in, size_t nw) { return location_mask(in, nw, 3); }   // StorageClass Output
static uint32_t location_mask(const uint32_t *in, size_t nw, uint32_t storage) {
    if (nw < 5) return 0;
    uint32_t bound = in[3], mask = 0;
    uint32_t *loc_of = calloc(bound, sizeof *loc_of);
    if (!loc_of) abort();
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) break;
        const uint32_t *a = in + i + 1;
        if (op == OP_DECORATE && wc >= 4 && a[1] == DEC_LOCATION && a[0] < bound) loc_of[a[0]] = a[2] + 1;
        else if (op == OP_VARIABLE && a[2] == storage && a[1] < bound && loc_of[a[1]] && loc_of[a[1]] <= 32)
            mask |= 1u << (loc_of[a[1]] - 1);
        i += wc;
    }
    free(loc_of);
    return mask;
}

// TOTK draws the sun's shadow map as D16; depth formats are D32F here (vkb_tex.c: the EQUAL-tested lighting tiles).
// Vulkan clamps a compare's reference to [0, 1] only for UNORM depth, so past the far end of shadow cascade 0, where
// the cascade-1 pixels look it up too, the reference is above 1 and the compare failed: a dark box on the ground ahead
// of Link (GitHub #34, A-FadedAway: the cause and this fix). The clamp covers the textures that are D32F on the Switch
// as well; it differs there only when the reference is outside [0, 1] and the stored depth is exactly 0 or 1.
int vkb_spv_clamp_dref(const uint32_t *in, size_t nw, uint32_t **out, size_t *outw) {
    enum { OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14, OP_CONSTANT = 43, OP_FUNCTION = 54, FCLAMP = 43 };
    static const char glsl[] = "GLSL.std.450";   // 13 bytes with the NUL: 4 words
    if (nw < 5) return 0;
    uint32_t bound = in[3], next = bound, f32 = 0, ext = 0, zero = 0, one = 0;
    int n = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (!wc || i + wc > nw) return 0;
        const uint32_t *a = in + i + 1;
        if (op == OP_EXT_INST_IMPORT && wc >= 6 && !memcmp(a + 1, glsl, sizeof glsl)) ext = a[0];
        else if (op == OP_TYPE_FLOAT && a[1] == 32) f32 = a[0];
        else if (op == OP_CONSTANT && wc == 4 && f32 && a[0] == f32) {
            if (a[2] == 0) zero = a[1];
            else if (a[2] == 0x3f800000u) one = a[1];
        } else if ((op == 89 || op == 90 || op == 97 || op == 307 || op == 308 || op == 315) && wc >= 6) n++;   // Dref at word 5
        i += wc;
    }
    if (!n || !f32) return 0;
    int add_ext = !ext, add_zero = !zero, add_one = !one;
    if (add_ext) ext = next++;
    if (add_zero) zero = next++;
    if (add_one) one = next++;
    words o = {0};
    for (int i = 0; i < 5; i++) put(&o, in[i]);
    for (size_t i = 5; i < nw;) {
        uint32_t op = in[i] & 0xffff, wc = in[i] >> 16;
        if (op == OP_MEMORY_MODEL && add_ext) {   // after the other imports, before the memory model
            uint32_t name[4] = {0};
            memcpy(name, glsl, sizeof glsl);
            put(&o, 6u << 16 | OP_EXT_INST_IMPORT); put(&o, ext);
            for (int k = 0; k < 4; k++) put(&o, name[k]);
            add_ext = 0;
        }
        if (op == OP_FUNCTION && (add_zero || add_one)) {   // the constants before the first function
            if (add_zero) { uint32_t c[3] = {f32, zero, 0}; put_ins(&o, OP_CONSTANT, 3, c); add_zero = 0; }
            if (add_one) { uint32_t c[3] = {f32, one, 0x3f800000u}; put_ins(&o, OP_CONSTANT, 3, c); add_one = 0; }
        }
        if ((op == 89 || op == 90 || op == 97 || op == 307 || op == 308 || op == 315) && wc >= 6) {
            uint32_t c = next++, x[7] = {f32, c, ext, FCLAMP, in[i + 5], zero, one};
            put_ins(&o, OP_EXT_INST, 7, x);
            for (uint32_t k = 0; k < wc; k++) put(&o, k == 5 ? c : in[i + k]);
        } else
            for (uint32_t k = 0; k < wc; k++) put(&o, in[i + k]);
        i += wc;
    }
    if (add_ext || add_zero || add_one) { free(o.w); return 0; }   // no memory model / function to place them by
    o.w[3] = next;
    *out = o.w;
    *outw = o.n;
    return n;
}

size_t vkb_spv_drop_interlock(uint32_t *w, size_t nw) {
    enum { OP_EXTENSION = 10, OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, OP_BEGIN = 5364, OP_END = 5365 };
    static const char ext[] = "SPV_EXT_fragment_shader_interlock";
    if (nw < 5) return nw;
    size_t o = 5;
    for (size_t i = 5; i < nw;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > nw) return nw;   // a shape not handled: as it is
        int drop = op == OP_BEGIN || op == OP_END ||
                   (op == OP_CAPABILITY && wc == 2 && (w[i + 1] == 5363 || w[i + 1] == 5372 || w[i + 1] == 5378)) ||
                   (op == OP_EXECUTION_MODE && wc >= 3 && w[i + 2] >= 5366 && w[i + 2] <= 5371) ||
                   (op == OP_EXTENSION && (wc - 1) * 4 >= sizeof ext && !memcmp(&w[i + 1], ext, sizeof ext));
        if (!drop) {
            memmove(&w[o], &w[i], wc * 4);
            o += wc;
        }
        i += wc;
    }
    return o;
}
