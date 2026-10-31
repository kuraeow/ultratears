// Shaders of the NVN replayer (milestone 3.3 step 4): the game's Maxwell shaders, translated to SPIR-V by Ryujinx's
// translator (MacNativeGames' totk_xlate), become Vulkan shader objects (VK_EXT_shader_object): no pipelines to build
// at draw time, every other state is dynamic.
//  - A shader is identified as MacNativeGames does: SHA-256 of its control section (0x880 bytes) and its code (length
//    at control+0x704); the stage is at control+0x714 (0 vertex, 1 fragment, 5 compute).
//  - [temporary] The translation is the default variant from the translator comparison (work/shaders/v143/ryujinx: every
//    texture 2D-typed by its own reflection, attributes and outputs float). The runtime translation service with
//    variants per bound state (MacNativeGames' totk_shader_rt) replaces it before the open world.
//  - Descriptors: one universal layout. Each stage owns 64 bindings of set 0 (vertex 0-63, fragment 64-127; compute its
//    own layout): +0 Ryujinx support buffer, +1..17 constant buffers c0-c16, +18..33 storage buffers, +34..57 combined
//    textures, +58..61 images. The translator numbers bindings densely per shader; they are rewritten in the SPIR-V
//    when it is loaded, so any vertex shader combines with any fragment shader under one pipeline layout.
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>   // after windows.h (its types)
#endif
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "compat/gfx_compat.h"
#include "host/hle.h"
#include "vkb.h"
#include "vkb_shader.h"
#include "vkb_spv.h"
#include "vkb_shpack.h"
#include "vkb_shbin.h"

#define VLOG(...) LOG("[vk] " __VA_ARGS__)

static VkDescriptorSetLayout g_layout[2];   // 0 graphics, 1 compute
static VkPipelineLayout g_playout[2];

static void make_layouts(void) {
    for (int compute = 0; compute < 2; compute++) {
        VkDescriptorSetLayoutBinding b[128];
        VkDescriptorBindingFlags fl[128];
        uint32_t n = 0;
        for (int st = 0; st < (compute ? 1 : 2); st++) {
            VkShaderStageFlags sf = compute ? VK_SHADER_STAGE_COMPUTE_BIT : st ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            uint32_t base = (uint32_t)st * VKB_STAGE_BINDINGS;
            struct { uint32_t off, count; VkDescriptorType type; } r[] = {
                {VKB_B_SUPPORT, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, {VKB_B_CBUF, VKB_NCBUF, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
                {VKB_B_SBUF, VKB_NSBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {VKB_B_TEX, VKB_NTEX, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
                {VKB_B_IMG, VKB_NIMG, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}};
            for (unsigned k = 0; k < sizeof r / sizeof *r; k++)
                for (uint32_t i = 0; i < r[k].count; i++) {
                    b[n] = (VkDescriptorSetLayoutBinding){base + r[k].off + i, r[k].type, 1, sf, NULL};
                    fl[n++] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;   // only what the shaders use is written
                }
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                          .bindingCount = n, .pBindingFlags = fl};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi, .bindingCount = n, .pBindings = b};
        if (vkb_db_on()) { ci.pNext = NULL; ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT; }   // vkb_desc.c
        if (!vk_check(vk.vkCreateDescriptorSetLayout(vkc.device, &ci, NULL, &g_layout[compute]), "descriptor set layout")) die("layout");
        VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &g_layout[compute]};
        if (!vk_check(vk.vkCreatePipelineLayout(vkc.device, &pi, NULL, &g_playout[compute]), "pipeline layout")) die("layout");
    }
    if (vkb_db_on()) vkb_db_layouts(g_layout);
}
VkDescriptorSetLayout vkb_set_layout(int compute) { if (!g_layout[0]) make_layouts(); return g_layout[compute]; }
VkPipelineLayout vkb_pipeline_layout(int compute) { if (!g_layout[0]) make_layouts(); return g_playout[compute]; }

// ---------------- the reflection (<key>.res.json): a small reader for its fixed shape ----------------
static const char *json_array(const char *j, const char *name) {   // just after the '[' of "name": [
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), '[');
    return p ? p + 1 : NULL;
}
// next {...} object of an array: its bounds; NULL at the closing ']'
static const char *json_next(const char *p, const char **end) {
    while (*p && *p != '{' && *p != ']') p++;
    if (*p != '{') return NULL;
    int depth = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '{') depth++;
        else if (*q == '}' && !--depth) { *end = q + 1; return p; }
    }
    return NULL;
}
static long json_int(const char *obj, const char *end, const char *key, long dflt) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return dflt;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return strtol(p, NULL, 10);
}
static long json_top_int(const char *j, const char *key, long dflt) { return json_int(j, j + strlen(j), key, dflt); }

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)len + 1);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    if (b) b[len] = 0;
    fclose(f);
    if (n) *n = (size_t)len;
    return b;
}

// ---------------- shaders by key (the entries: below, with the objects' lifetime) ----------------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   // the entries, their objects and variants, the programs
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;     // an entry no longer busy

#ifdef _WIN32
static BCRYPT_ALG_HANDLE g_sha;   // opened once (opening it per hash cost more than the hash)
static void sha_open(void) { if (BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0)) die("SHA-256 provider"); }
static void sha256_hex(const void *a, size_t na, const void *b, size_t nb, char out[65]) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, sha_open);
    BCRYPT_ALG_HANDLE alg = g_sha;
    BCRYPT_HASH_HANDLE h;
    uint8_t md[32];
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0)) die("SHA-256 provider");
    BCryptHashData(h, (PUCHAR)a, (ULONG)na, 0);
    BCryptHashData(h, (PUCHAR)b, (ULONG)nb, 0);
    BCryptFinishHash(h, md, sizeof md, 0);
    BCryptDestroyHash(h);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[md[i] >> 4]; out[2 * i + 1] = hex[md[i] & 15]; }
    out[64] = 0;
}
#else
typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx;

static inline uint32_t ror32_s(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t data[64]) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t W[64];
    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)data[i*4] << 24) | ((uint32_t)data[i*4 + 1] << 16) |
               ((uint32_t)data[i*4 + 2] << 8) | ((uint32_t)data[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32_s(W[i-15], 7) ^ ror32_s(W[i-15], 18) ^ (W[i-15] >> 3);
        uint32_t s1 = ror32_s(W[i-2], 17) ^ ror32_s(W[i-2], 19) ^ (W[i-2] >> 10);
        W[i] = W[i-16] + s0 + W[i-7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32_s(e, 6) ^ ror32_s(e, 11) ^ ror32_s(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K[i] + W[i];
        uint32_t S0 = ror32_s(a, 2) ^ ror32_s(a, 13) ^ ror32_s(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init_s(sha256_ctx *ctx) {
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

static void sha256_update_s(sha256_ctx *ctx, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    while (len > 0) {
        size_t take = 64 - idx;
        if (take > len) take = len;
        memcpy(&ctx->buffer[idx], p, take);
        p += take;
        len -= take;
        idx += take;
        if (idx == 64) {
            sha256_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha256_final_s(sha256_ctx *ctx, uint8_t hash[32]) {
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(&ctx->buffer[idx], 0, 64 - idx);
        sha256_transform(ctx->state, ctx->buffer);
        idx = 0;
    }
    memset(&ctx->buffer[idx], 0, 56 - idx);
    uint64_t bits = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        ctx->buffer[56 + i] = (uint8_t)(bits >> ((7 - i) * 8));
    }
    sha256_transform(ctx->state, ctx->buffer);
    for (int i = 0; i < 8; i++) {
        hash[i*4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void sha256_hex(const void *a, size_t na, const void *b, size_t nb, char out[65]) {
    sha256_ctx ctx;
    sha256_init_s(&ctx);
    if (a && na) sha256_update_s(&ctx, a, na);
    if (b && nb) sha256_update_s(&ctx, b, nb);
    uint8_t md[32];
    sha256_final_s(&ctx, md);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[md[i] >> 4]; out[2 * i + 1] = hex[md[i] & 15]; }
    out[64] = 0;
}
#endif

// The translated shaders: data/shaders/translated.pack (vkb_shpack.c); without it, a folder of <key>.res.json + <key>.spv
// (TOTK_SHADER_DIR, development: the translator's output before it is packed).
static const char *cache_dir(void) {
    static const char *d;
    if (!d) { d = getenv("TOTK_SHADER_DIR"); if (!d) d = vkb_shpack_on() ? "data/shaders/translated.pack" : "data/shaders/translated"; }
    return d;
}
static int from_pack(void) {   // a folder only when TOTK_SHADER_DIR names one that is there (an older ultratears.cfg may name a removed one)
    static int v = -1;
    if (v < 0) {
        const char *d = getenv("TOTK_SHADER_DIR");
#ifdef _WIN32
        DWORD a = d ? GetFileAttributesA(d) : INVALID_FILE_ATTRIBUTES;
        v = vkb_shpack_on() && !(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY));
#else
        struct stat st;
        v = vkb_shpack_on() && !(d && stat(d, &st) == 0 && S_ISDIR(st.st_mode));
#endif
    }
    return v;
}

// The translator declares gl_PerVertex with ClipDistance[8] and CullDistance[1]: 9 > maxCombinedClipAndCullDistances (8)
// on NVIDIA, though no shader writes the cull distance. The CullDistance member (the block's last) is dropped: its
// OpTypeStruct operand, OpMemberName and OpMemberDecorate. Returns the new word count.
static size_t drop_cull_distance(uint32_t *w, size_t n) {
    uint32_t st = 0, mem = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return n;
        if (op == 72 && wc == 5 && w[i + 3] == 11 && w[i + 4] == 4) { st = w[i + 1]; mem = w[i + 2]; }   // BuiltIn CullDistance
        i += wc;
    }
    if (!st) return n;
    size_t o = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        int drop = (op == 72 || op == 6) && wc >= 3 && w[i + 1] == st && w[i + 2] == mem;   // OpMemberDecorate / OpMemberName
        if (op == 30 && w[i + 1] == st && wc - 2 == mem + 1) {   // OpTypeStruct: the member is the last operand
            memmove(&w[o], &w[i], (wc - 1) * 4);
            w[o] = ((wc - 1) << 16) | 30;
            o += wc - 1;
        } else if (!drop) {
            memmove(&w[o], &w[i], wc * 4);
            o += wc;
        }
        i += wc;
    }
    return o;
}

// ---- shader objects: from the driver's binary on disk when there is one (vkb_shbin.c), else compiled from SPIR-V
// and its binary kept
static _Atomic uint64_t g_from_bin, g_from_spv, g_bin_refused, g_obj_live, g_obj_destroyed;
static VkResult make_object(const char *key, const vkb_shbin_variant *v, VkShaderStageFlagBits stage, const uint32_t *spv,
                            size_t bytes, VkShaderEXT *obj) {
    VkDescriptorSetLayout layout = vkb_set_layout(stage == VK_SHADER_STAGE_COMPUTE_BIT);
    VkShaderCreateInfoEXT ci = {VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage,
        .nextStage = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0,
        .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT, .codeSize = bytes, .pCode = spv, .pName = "main",
        .setLayoutCount = 1, .pSetLayouts = &layout};
    // 32 lanes, as the translator assumes, where the driver would pick (vk.c: vkc.subgroup_pin); the binaries on disk
    // were made the same way (their file header holds the pin, vkb_shbin.c)
    VkShaderRequiredSubgroupSizeCreateInfoEXT lanes = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
                                                       .requiredSubgroupSize = vkc.subgroup_pin};
    if (vkc.subgroup_pin && (vkc.subgroup_pin_stages & stage)) ci.pNext = &lanes;
    void *bin;
    size_t nbin;
    if (key && vkb_shbin_get(key, v, &bin, &nbin)) {
        VkShaderCreateInfoEXT bi = ci;
        bi.codeType = VK_SHADER_CODE_TYPE_BINARY_EXT; bi.codeSize = nbin; bi.pCode = bin;
        VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &bi, NULL, obj);
        free(bin);
        if (r == VK_SUCCESS) { atomic_fetch_add(&g_from_bin, 1); atomic_fetch_add(&g_obj_live, 1); return r; }
        vkb_shbin_drop(key, v);   // refused (VK_INCOMPATIBLE_SHADER_BINARY_EXT): compiled again below and kept anew
        atomic_fetch_add(&g_bin_refused, 1);
    }
    VkResult r = vk.vkCreateShadersEXT(vkc.device, 1, &ci, NULL, obj);
    if (r != VK_SUCCESS) return r;
    atomic_fetch_add(&g_from_spv, 1);
    atomic_fetch_add(&g_obj_live, 1);
    size_t n = 0;
    if (key && vk.vkGetShaderBinaryDataEXT(vkc.device, *obj, &n, NULL) == VK_SUCCESS && n) {
        void *d = malloc(n);
        if (d && vk.vkGetShaderBinaryDataEXT(vkc.device, *obj, &n, d) == VK_SUCCESS) vkb_shbin_put(key, v, d, n);
        free(d);
    }
    return r;
}
static void drop_object(VkShaderEXT o) {   // no submission uses it any more once the next one is done
    void vkb_destroy_shader_later(VkShaderEXT sh);   // backend_vk.c
    vkb_destroy_shader_later(o);
    atomic_fetch_sub(&g_obj_live, 1);
    atomic_fetch_add(&g_obj_destroyed, 1);
}

// TOTK_VK_FILL_OUTPUTS=0: no outputs added for fragment inputs the vertex shader does not write (A/B).
static int fill_outputs_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_FILL_OUTPUTS"); v = !(e && *e == '0'); }
    return v;
}

// A vertex shader for integer attributes at the locations in intmask (signed where sintmask has the bit): those
// inputs read the raw bits; and, drawn with fs, with outputs (0, 0, 0, 1) at the fragment inputs it does not write
// (vkb_spv.c). Made on first use (from its binary when there is one) and kept while the shader lives; nothing to change
// gives the shader itself.
VkShaderEXT vkb_shader_variant(const vkb_shader *cs, uint32_t intmask, uint32_t sintmask, const vkb_shader *fs) {
    vkb_shader *s = (vkb_shader *)cs;
    intmask &= s->in_mask;
    sintmask &= intmask;
    uint32_t outmask = fs && fill_outputs_on() ? fs->in_mask & ~s->out_mask : 0;
    uint32_t out_umask = fs ? fs->in_umask & outmask : 0, out_imask = fs ? fs->in_imask & outmask : 0;
    if ((!intmask && !outmask) || !s->spv) return s->obj;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < s->nvariants; i++) {
        const vkb_variant *v = &s->variants[i];
        if (v->intmask == intmask && v->sintmask == sintmask && v->outmask == outmask && v->out_umask == out_umask &&
            v->out_imask == out_imask) { pthread_mutex_unlock(&g_mu); return v->obj; }
    }
    int full = s->nvariants == VKB_VARIANTS;
    pthread_mutex_unlock(&g_mu);
    if (full) {   // not made again on every draw
        static int said;
        if (said++ < 8) VLOG("[temporary] shader %.12s: more than %d variants: the shader itself is used", s->key ? s->key : "?", VKB_VARIANTS);
        return s->obj;
    }
    uint32_t *a = NULL, *b = NULL;
    size_t na = 0, nb = 0;
    const uint32_t *spv = s->spv;
    size_t nw = s->nspv_words;
    int ints_ok = !intmask, outs_ok = !outmask;
    if (intmask && vkb_spv_int_inputs(spv, nw, intmask, sintmask, &a, &na)) { spv = a; nw = na; ints_ok = 1; }
    if (outmask && vkb_spv_add_outputs(spv, nw, outmask, out_umask, out_imask, &b, &nb)) { spv = b; nw = nb; outs_ok = 1; }
    VkShaderEXT obj = VK_NULL_HANDLE;
    if (spv != s->spv && ints_ok) {   // (the caller takes any variant as one with the integer inputs)
        if (getenv("TOTK_SPV_DUMP")) {   // development: the variant's SPIR-V for spirv-val / spirv-dis
            char path[256];
            snprintf(path, sizeof path, "%s/%.16s-i%x-s%x-o%x.spv", getenv("TOTK_SPV_DUMP"), s->key ? s->key : "x", intmask, sintmask, outmask);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(spv, 4, nw, f); fclose(f); }
        }
        vkb_shbin_variant id = {{intmask, sintmask, outmask, out_umask, out_imask}, s->stamp};
        if (make_object(s->key, &id, VK_SHADER_STAGE_VERTEX_BIT, spv, nw * 4, &obj) != VK_SUCCESS) obj = VK_NULL_HANDLE;
    }
    free(a);
    free(b);
    static int said, made;
    if (!obj) {
        obj = s->obj;
        ints_ok = !intmask;
        outs_ok = !outmask;
    } else if (made++ < 16)
        VLOG("shader %.12s: variant for integer inputs %#x (signed %#x), outputs added %#x", s->key ? s->key : "?", intmask, sintmask,
             outs_ok ? outmask : 0);
    if (!ints_ok && said++ < 8) VLOG("[temporary] shader %.12s: no variant for integer inputs %#x: the float reading stays", s->key ? s->key : "?", intmask);
    if (!outs_ok && said++ < 8) VLOG("[temporary] shader %.12s: outputs %#x not added: fragment inputs read undefined values", s->key ? s->key : "?", outmask);
    pthread_mutex_lock(&g_mu);
    if (s->obj && s->nvariants < VKB_VARIANTS) s->variants[s->nvariants++] = (vkb_variant){intmask, sintmask, outmask, out_umask, out_imask, obj};
    else if (obj != s->obj) { pthread_mutex_unlock(&g_mu); drop_object(obj); return s->obj; }   // released meanwhile
    pthread_mutex_unlock(&g_mu);
    return obj;
}

// Depth compares with the reference clamped to [0, 1] (vkb_spv_clamp_dref, GitHub #34); TOTK_VK_DREF_CLAMP=0: as before
// (A/B). The rewrites at load are part of what a compiled binary was made from (stamp_of).
static int dref_clamp(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_VK_DREF_CLAMP"); v = !(e && *e == '0'); }
    return v;
}
// Which translation of a shader, as the compiled binaries are keyed (vkb_shbin_variant.stamp), and the rewrites
// load_module makes. From the pack: the hash of its .res.json and .spv (tools/pack_shaders.py). From a folder: the
// .res.json's last write (seconds) and size, taken from _stat64 here and from FindFirstFile in ahead_thread.
static uint32_t stamp_fold(uint64_t h) {
    if (dref_clamp()) h ^= 0x5BD1E995ull << 32;   // binaries made before the clamp are not used (the halves fold below)
    if (!vkc.has_interlock) h ^= 0x27D4EB2Full << 32;   // nor those of modules with the interlock (vkb_spv_drop_interlock)
    return (uint32_t)(h ^ h >> 32) | 1;   // never 0
}
static uint32_t stamp_of(uint64_t mtime_s, uint64_t size) {
    return stamp_fold((mtime_s * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full));
}
static uint32_t stamp_packed(uint32_t content) { return stamp_fold((uint64_t)content * 0x9E3779B97F4A7C15ull); }

// Reads the reflection into s and rewrites the SPIR-V bindings into the universal layout: *spv_out (malloc) of
// *bytes. The shader object is made by the caller.
static int load_module(vkb_shader *s, const char *key, int nvn_stage, uint32_t **spv_out, size_t *bytes) {
    char *j = NULL;
    uint32_t *spv = NULL, content;
    size_t nspv = 0;
    if (from_pack()) {
        s->stamp = vkb_shpack_get(key, &j, &spv, &nspv, &content) ? stamp_packed(content) : 0;
    } else {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.res.json", cache_dir(), key);
        j = read_file(path, NULL);
#ifdef _WIN32
        struct _stat64 st;
        s->stamp = _stat64(path, &st) == 0 ? stamp_of((uint64_t)st.st_mtime, (uint64_t)st.st_size) : 0;
#else
        struct stat st;
        s->stamp = stat(path, &st) == 0 ? stamp_of((uint64_t)st.st_mtime, (uint64_t)st.st_size) : 0;
#endif
        snprintf(path, sizeof path, "%s/%s.spv", cache_dir(), key);
        spv = (uint32_t *)read_file(path, &nspv);
    }
    if (!j || !spv || nspv < 20 || (nspv & 3) || spv[0] != 0x07230203) { free(j); free(spv); return 0; }
    s->stage = nvn_stage;
    uint32_t base = nvn_stage == 1 ? VKB_STAGE_BINDINGS : 0;
    uint32_t remap[256];   // translator binding -> universal binding (UINT32_MAX: unknown)
    for (int i = 0; i < 256; i++) remap[i] = UINT32_MAX;
    remap[0] = base + VKB_B_SUPPORT;
    const char *p, *e;
    // constant buffers: slot = NVN constant buffer index
    if ((p = json_array(j, "cbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), slot = json_int(p, e, "slot", -1);
            if (b < 0 || b > 255 || slot < 0 || slot >= VKB_NCBUF) continue;
            remap[b] = base + VKB_B_CBUF + (uint32_t)slot;
            s->cbuf_mask |= 1u << slot;
        }
    // storage buffers: the address comes from the NVN driver bank c0, 16 bytes per slot: the stage's storage buffer
    // table (vertex 0x110, fragment 0x510, compute 0x310) or, for compute, its uniform buffer table (0x210): TOTK's
    // compute shaders also use uniform buffers as storage (MacNativeGames' "ubo_ptr")
    int sb_base = nvn_stage == 0 ? 0x110 : nvn_stage == 1 ? 0x510 : 0x310;
    if ((p = json_array(j, "sbufs")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), off = json_int(p, e, "cb_offset", -1) * 4;
            if (b < 0 || b > 255 || s->nsbuf >= VKB_NSBUF) { s->unsupported = 1; continue; }
            long cb = json_int(p, e, "cb_slot", 0);
            int write = (json_int(p, e, "flags", 0) & 1) != 0;   // Ryujinx BufferUsageFlags.Write
            if (write) s->stores = 1;
            int slot = -1, kind = VKB_SB_TABLE;
            if (cb == 0 && off >= sb_base && off < sb_base + 0x100 && !(off & 15)) slot = (int)(off - sb_base) / 16;
            else if (cb == 0 && nvn_stage == 5 && off >= 0x210 && off < 0x310 && !(off & 15)) { slot = (int)(off - 0x210) / 16; kind = VKB_SB_UBO; }
            else if (cb >= 3 && cb <= 16 && off >= 0 && off < 0x10000) { slot = (int)cb - 3; kind = VKB_SB_UBO_PTR; }
            if (slot < 0 || slot >= VKB_NSBUF) {
                static int said; if (said++ < 8) VLOG("shader %s: storage buffer address from c%ld+%#lx: unknown source", key, cb, off);
                slot = 0xFF;
            }
            remap[b] = base + VKB_B_SBUF + (uint32_t)s->nsbuf;
            s->sbuf[s->nsbuf].slot = (uint8_t)slot;
            s->sbuf[s->nsbuf].kind = (uint8_t)kind;
            s->sbuf[s->nsbuf].write = (uint8_t)write;
            s->sbuf[s->nsbuf++].off = (uint16_t)(kind == VKB_SB_UBO_PTR ? off : 0);
        }
    // textures: the handle is a word offset into the NVN driver bank c2 - combined textures at 0x20 (8 bytes per slot),
    // separate textures at 0x168 (MacNativeGames' totk_xlate Service.cs)
    if ((p = json_array(j, "textures")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->ntex >= VKB_NTEX || h < 0x20) { s->unsupported = 1; continue; }
            vkb_tex_ref *t = &s->tex[s->ntex];
            t->separate = h >= 0x168;
            t->slot = (uint8_t)((h - (t->separate ? 0x168 : 0x20)) / 8);
            t->type = (uint16_t)type;
            remap[b] = base + VKB_B_TEX + (uint32_t)s->ntex++;
        }
    if ((p = json_array(j, "images")))
        for (; (p = json_next(p, &e)); p = e) {
            long b = json_int(p, e, "binding", -1), h = json_int(p, e, "handle", -1) * 4, type = json_int(p, e, "type", 3);
            if (b < 0 || b > 255 || s->nimg >= VKB_NIMG || h < 0x120) { s->unsupported = 1; continue; }
            s->img[s->nimg] = (vkb_tex_ref){(uint8_t)((h - 0x120) / 8), 0, (uint16_t)type};
            s->stores = 1;   // [temporary] images are taken as written (the reflection has no read-only mark)
            remap[b] = base + VKB_B_IMG + (uint32_t)s->nimg++;
        }
    s->frag_outputs = (uint32_t)json_top_int(j, "frag_out_map", 0);
    free(j);
    // rewrite Binding decorations (OpDecorate %id Binding n: opcode 71, decoration 33)
    size_t nw = drop_cull_distance(spv, nspv / 4);
    uint32_t *clamped = NULL;
    size_t ncl = 0;
    if (dref_clamp() && vkb_spv_clamp_dref(spv, nw, &clamped, &ncl)) { free(spv); spv = clamped; nw = ncl; }
    if (!vkc.has_interlock) nw = vkb_spv_drop_interlock(spv, nw);   // a device without it refused these modules (RX 570)
    nspv = nw * 4;
    for (size_t i = 5; i < nw;) {
        uint32_t op = spv[i] & 0xffff, wc = spv[i] >> 16;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && spv[i + 2] == 33) {
            uint32_t old = spv[i + 3];
            if (old > 255 || remap[old] == UINT32_MAX) {
                VLOG("shader %s: binding %u not in its reflection", key, old);
                s->unsupported = 1;
            } else spv[i + 3] = remap[old];
        }
        i += wc;
    }
    // input / output locations: vertex variants; fragment inputs no vertex shader writes (audit)
    s->in_mask = vkb_spv_input_mask(spv, nw);
    s->out_mask = vkb_spv_output_mask(spv, nw);
    vkb_spv_input_int_masks(spv, nw, &s->in_umask, &s->in_imask);
    uint32_t reads = vkb_spv_sbuf_reads(spv, nw, base + VKB_B_SBUF, s->nsbuf);
    for (int i = 0; i < s->nsbuf; i++) s->sbuf[i].read = (uint8_t)(reads >> i & 1);
    s->vk_stage = nvn_stage == 0 ? VK_SHADER_STAGE_VERTEX_BIT : nvn_stage == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    *spv_out = spv;
    *bytes = nspv;
    return 1;
}

// ---- shader objects follow the game's programs. On the Switch a shader costs the game only its code, and TOTK keeps
// the programs of where Link is - ~6 000-7 000 shaders - and finalizes those of places left behind (a 20-minute session
// with teleports, 2026-10-28: 8 394 shaders seen, at most 6 946 held by live programs). A shader object costs the
// driver ~0.13 MiB, so objects kept for good (as the used.txt list made ahead did) grow with the time played, towards
// all 28 687 shaders (~3.5 GiB). Now:
//  - a program set (ProgramSetShaders, on the game's thread) holds its shaders; one not made yet is made by the creator
//    threads from its binary (~1 ms) - most programs are set seconds before their first draw;
//  - a program finalized or set again lets go of its shaders through the translation queue (vkb_shader_prog_drop ->
//    vkb_shader_release), after the submissions queued before it; a shader no program holds is destroyed once the
//    next submission is done (vkb_destroy_shader_later);
//  - a draw with a shader whose object is not there yet makes it at once (or waits for the creator making it);
//  - all of the game's translated shaders are compiled ahead, once per driver, into the binary file (ahead_thread).
// TOTK_SHADER_AHEAD=0: no compiling ahead.
typedef struct shentry {
    char key[65];
    int c1_set, stage;
    int loaded;   // 0 the reflection not read yet, 1 read, -1 no translation
    int busy;     // a thread is making the object (others wait on g_cv)
    int refs;     // live programs holding the shader
    int queued;   // waiting for a creator thread
    vkb_shader sh;
    struct shentry *next;
} shentry;
static shentry *g_tab[4096];
static uint64_t g_loaded, g_missing;
uint64_t vkb_shader_load_ns;   // objects made by draws (long submission log, backend_vk.c)

static shentry *entry(const char *key, int stage) {   // g_mu held: the entry of key, added if new
    uint32_t h = (uint32_t)strtoul(key + 56, NULL, 16) & 4095;
    shentry *e = g_tab[h];
    while (e && strcmp(e->key, key)) e = e->next;
    if (e) return e;
    if (!(e = calloc(1, sizeof *e))) die("shader entry");
    memcpy(e->key, key, 65);
    e->sh.key = e->key;
    e->stage = stage;
    e->next = g_tab[h];
    g_tab[h] = e;
    return e;
}

// g_mu held (released while working): the entry's object, made here or by another thread. 0: no translation.
static int ensure(shentry *e, int on_draw) {
    while (e->busy) pthread_cond_wait(&g_cv, &g_mu);
    if (e->sh.obj) return 1;
    if (e->loaded < 0) return 0;
    e->busy = 1;
    int first = !e->loaded;
    pthread_mutex_unlock(&g_mu);
    uint64_t t0 = runtime_now_ns();
    vkb_shader again;   // a shader made again: the reflection is kept, this read of it is dropped
    memset(&again, 0, sizeof again);
    uint32_t *spv = NULL;
    size_t bytes = 0;
    int ok = load_module(first ? &e->sh : &again, e->key, e->stage, &spv, &bytes);
    VkShaderEXT obj = VK_NULL_HANDLE;
    VkShaderStageFlagBits st = first ? e->sh.vk_stage : again.vk_stage;
    if (ok) {
        VkResult r = make_object(e->key, &(vkb_shbin_variant){{0}, first ? e->sh.stamp : again.stamp}, st, spv, bytes, &obj);
        if (r != VK_SUCCESS) { VLOG("shader %s: vkCreateShadersEXT %d", e->key, (int)r); ok = 0; }
    }
    if (ok && getenv("TOTK_VK_VALIDATION")) VLOG("shader %.12s = VkShaderEXT %#llx", e->key, (unsigned long long)(uintptr_t)obj);   // names validation messages
    pthread_mutex_lock(&g_mu);
    if (ok) {
        e->sh.obj = obj;
        e->loaded = 1;
        if (st == VK_SHADER_STAGE_VERTEX_BIT) { e->sh.spv = spv; e->sh.nspv_words = bytes / 4; spv = NULL; }   // for variants
        if (first) g_loaded++;
    } else if (first) {
        e->loaded = -1;
        if (g_missing++ < 16) VLOG("shader %s (stage %d): no translation in %s", e->key, e->stage, cache_dir());
    }
    free(spv);
    if (on_draw) vkb_shader_load_ns += runtime_now_ns() - t0;
    e->busy = 0;
    pthread_cond_broadcast(&g_cv);
    return ok;
}

// g_mu held: a program let go of the shader; with no program left, its objects go once the next submission is done.
static void unref(shentry *e) {
    if (--e->refs > 0 || e->busy || !e->sh.obj) return;
    VkShaderEXT obj = e->sh.obj;
    for (int i = 0; i < e->sh.nvariants; i++) if (e->sh.variants[i].obj != obj) drop_object(e->sh.variants[i].obj);
    e->sh.nvariants = 0;
    drop_object(obj);
    e->sh.obj = VK_NULL_HANDLE;
    free(e->sh.spv);
    e->sh.spv = NULL;
    e->sh.nspv_words = 0;
}

// ---- the creator threads: objects of programs just set, ahead of their first draw
enum { CREATORS = 2, CQ = 1 << 16 };
static shentry *g_cq[CQ];
static uint64_t g_cq_in, g_cq_out;
static pthread_cond_t g_cq_cv = PTHREAD_COND_INITIALIZER;
static void *creator_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-make");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    pthread_mutex_lock(&g_mu);
    for (;;) {
        while (g_cq_out == g_cq_in) pthread_cond_wait(&g_cq_cv, &g_mu);
        shentry *e = g_cq[g_cq_out++ % CQ];
        e->queued = 0;
        if (e->refs > 0) ensure(e, 0);   // (a program that went meanwhile: nothing to make)
    }
    return NULL;
}
static void creators_start(void) {
    for (int i = 0; i < CREATORS; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, creator_thread, NULL) == 0) pthread_detach(t);
    }
}

// ---- programs: the shaders each live program holds
typedef struct { const void *prog; int n; shentry *e[6]; } progrec;   // prog: NULL free, (void *)1 removed
typedef struct vkb_shader_rel { int n; shentry *e[6]; } vkb_shader_rel;
enum { PROGS = 1 << 16 };
static progrec *g_progs;
static int g_nprog, g_ngone;
static uint64_t g_prog_sets, g_prog_drops;
static progrec *prog_slot(const void *p, int add) {   // g_mu held
    uint32_t h = (uint32_t)(((uint64_t)(uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 48) & (PROGS - 1);
    progrec *gone = NULL;
    for (uint32_t i = 0; i < PROGS; i++, h = (h + 1) & (PROGS - 1)) {
        progrec *r = &g_progs[h];
        if (r->prog == p) return r;
        if (r->prog == (const void *)1) { if (!gone) gone = r; continue; }
        if (!r->prog) {
            if (!add) return NULL;
            if (gone) { g_ngone--; r = gone; }
            g_nprog++;
            r->prog = p;
            r->n = 0;
            return r;
        }
    }
    return NULL;
}
static void progs_rebuild(void) {   // g_mu held: too many removed slots slow the probes down
    progrec *old = g_progs;
    if (!(g_progs = calloc(PROGS, sizeof *g_progs))) die("program table");
    g_nprog = g_ngone = 0;
    for (int i = 0; i < PROGS; i++)
        if ((uintptr_t)old[i].prog > 1) { progrec *r = prog_slot(old[i].prog, 1); r->n = old[i].n; memcpy(r->e, old[i].e, sizeof r->e); }
    free(old);
}
static vkb_shader_rel *prog_take(progrec *r) {   // g_mu held: what the program held, for vkb_shader_release
    if (!r || !r->n) return NULL;
    vkb_shader_rel *rel = malloc(sizeof *rel);
    if (!rel) die("shader release");
    rel->n = r->n;
    memcpy(rel->e, r->e, sizeof rel->e);
    r->n = 0;
    return rel;
}

// The shader's key (SHA-256 of its control section and code) and NVN stage; 0: not a shader we translate.
static int shader_key(const NVNshaderData *sd, char key[65], uint32_t *stage) {
    const uint8_t *control = sd->control;
    if (!control || !sd->data) return 0;
    uint32_t length;
    memcpy(&length, control + 0x704, 4);
    memcpy(stage, control + 0x714, 4);
    if (!length || length > (16u << 20) || (*stage != 0 && *stage != 1 && *stage != 5)) return 0;
    sha256_hex(control, 0x880, (const void *)(uintptr_t)sd->data, length, key);
    return 1;
}

static void ahead_start(void);
static void started(void) { creators_start(); ahead_start(); }
static pthread_once_t g_start = PTHREAD_ONCE_INIT;

vkb_shader_rel *vkb_shader_prog_set(const void *prog, int n, const NVNshaderData *sd) {
    pthread_once(&g_start, started);
    char keys[6][65];
    uint32_t stages[6];
    int nk = 0;
    for (int i = 0; i < n && i < 6; i++) if (shader_key(&sd[i], keys[nk], &stages[nk])) nk++;
    pthread_mutex_lock(&g_mu);
    if (!g_progs && !(g_progs = calloc(PROGS, sizeof *g_progs))) die("program table");
    if (g_nprog + g_ngone > PROGS / 4 * 3) progs_rebuild();
    progrec *r = prog_slot(prog, 1);
    vkb_shader_rel *rel = prog_take(r);   // its earlier shaders (let go of after the commands queued with them)
    for (int i = 0; i < nk && r; i++) {
        shentry *e = entry(keys[i], (int)stages[i]);
        r->e[r->n++] = e;
        if (e->refs++ == 0 && !e->sh.obj && !e->busy && !e->queued && e->loaded >= 0 && g_cq_in - g_cq_out < CQ) {
            e->queued = 1;
            g_cq[g_cq_in++ % CQ] = e;
            pthread_cond_signal(&g_cq_cv);
        }
    }
    g_prog_sets++;
    pthread_mutex_unlock(&g_mu);
    return rel;
}
vkb_shader_rel *vkb_shader_prog_drop(const void *prog) {
    pthread_mutex_lock(&g_mu);
    progrec *r = g_progs ? prog_slot(prog, 0) : NULL;
    vkb_shader_rel *rel = prog_take(r);
    if (r) { r->prog = (const void *)1; g_nprog--; g_ngone++; g_prog_drops++; }
    pthread_mutex_unlock(&g_mu);
    return rel;
}
void vkb_shader_release(vkb_shader_rel *rel) {   // the translation queue reached the program's release
    if (!rel) return;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < rel->n; i++) unref(rel->e[i]);
    pthread_mutex_unlock(&g_mu);
    free(rel);
}

// ---- compiling ahead: every translated shader once per driver, into the binary file, at the lowest priority
static _Atomic uint64_t g_ahead_done, g_ahead_made, g_ahead_total;
static int stage_of(const char *json) {   // "stage": "vs" / "ps" / "cs" -> NVN 0 / 1 / 5; -1 other
    const char *p = strstr(json, "\"stage\"");
    if (!p || !(p = strchr(p + 7, '"'))) return -1;
    return !strncmp(p, "\"vs\"", 4) ? 0 : !strncmp(p, "\"ps\"", 4) ? 1 : !strncmp(p, "\"cs\"", 4) ? 5 : -1;
}
// The pass over the pack (ahead_thread below: over a folder).
static void ahead_pack(void) {
    uint64_t t0 = runtime_now_ns();
    size_t n = vkb_shpack_count();
    for (size_t i = 0; i < n; i++) {
        char key[65];
        uint32_t content;
        vkb_shpack_key(i, key, &content);
        atomic_fetch_add(&g_ahead_total, 1);
        vkb_shbin_variant v0 = {{0}, stamp_packed(content)};
        vkb_shbin_mark(key, v0.stamp);
        if (vkb_shbin_has(key, &v0)) { atomic_fetch_add(&g_ahead_done, 1); continue; }
        char *j;
        uint32_t *raw;
        size_t nraw;
        int stage = vkb_shpack_get(key, &j, &raw, &nraw, &content) ? stage_of(j) : -1;
        if (stage >= 0) { free(j); free(raw); }
        if (stage < 0) continue;
        vkb_shader tmp;
        memset(&tmp, 0, sizeof tmp);
        uint32_t *spv;
        size_t bytes;
        VkShaderEXT obj;
        if (load_module(&tmp, key, stage, &spv, &bytes)) {
            if (!vkb_shbin_has(key, &v0) && make_object(key, &v0, tmp.vk_stage, spv, bytes, &obj) == VK_SUCCESS) {
                vk.vkDestroyShaderEXT(vkc.device, obj, NULL);   // only its binary was wanted; no command ever saw it
                atomic_fetch_sub(&g_obj_live, 1);
                atomic_fetch_add(&g_ahead_made, 1);
            }
            free(spv);
        }
        atomic_fetch_add(&g_ahead_done, 1);
    }
    VLOG("shaders compiled ahead: %llu new of %llu, %.0f s", (unsigned long long)g_ahead_made, (unsigned long long)g_ahead_total,
         (runtime_now_ns() - t0) / 1e9);
    vkb_shbin_compact();
}
static void *ahead_thread(void *unused) {
    (void)unused;
    pthread_setname_np("totk-shader-ahead");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    if (from_pack()) { ahead_pack(); return NULL; }
#ifndef _WIN32
    VLOG("shaders compiled ahead: only from the shader pack on this system (setup makes it)");
    return NULL;
#else
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.res.json", cache_dir());
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    uint64_t t0 = runtime_now_ns();
    do {
        if (strlen(fd.cFileName) != 64 + 9) continue;
        char key[65];
        memcpy(key, fd.cFileName, 64);
        key[64] = 0;
        atomic_fetch_add(&g_ahead_total, 1);
        uint64_t ft = (uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32 | fd.ftLastWriteTime.dwLowDateTime;
        uint64_t mtime = ft >= 116444736000000000ull ? (ft - 116444736000000000ull) / 10000000ull : 0;   // FILETIME -> Unix seconds
        vkb_shbin_variant v0 = {{0}, stamp_of(mtime, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow)};
        vkb_shbin_mark(key, v0.stamp);   // the binaries of other stamps are old (vkb_shbin_compact)
        if (vkb_shbin_has(key, &v0)) { atomic_fetch_add(&g_ahead_done, 1); continue; }
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", cache_dir(), fd.cFileName);
        char *j = read_file(path, NULL);
        int stage = j ? stage_of(j) : -1;
        free(j);
        if (stage < 0) continue;
        vkb_shader tmp;
        memset(&tmp, 0, sizeof tmp);
        uint32_t *spv;
        size_t bytes;
        VkShaderEXT obj;
        if (load_module(&tmp, key, stage, &spv, &bytes)) {
            vkb_shbin_variant v = {{0}, tmp.stamp};   // as the game will look it up (load_module)
            if (!vkb_shbin_has(key, &v) && make_object(key, &v, tmp.vk_stage, spv, bytes, &obj) == VK_SUCCESS) {
                vk.vkDestroyShaderEXT(vkc.device, obj, NULL);   // only its binary was wanted; no command ever saw it
                atomic_fetch_sub(&g_obj_live, 1);
                atomic_fetch_add(&g_ahead_made, 1);
            }
            free(spv);
        }
        atomic_fetch_add(&g_ahead_done, 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    VLOG("shaders compiled ahead: %llu new of %llu, %.0f s", (unsigned long long)g_ahead_made, (unsigned long long)g_ahead_total,
         (runtime_now_ns() - t0) / 1e9);
    vkb_shbin_compact();
    return NULL;
#endif
}
static void ahead_start(void) {
    const char *e = getenv("TOTK_SHADER_AHEAD");
    if (e && *e == '0') return;
    pthread_t t;
    if (pthread_create(&t, NULL, ahead_thread, NULL) == 0) pthread_detach(t);
}

const vkb_shader *vkb_shader_get(const NVNshaderData *sd) {
    char key[65];
    uint32_t stage;
    if (!shader_key(sd, key, &stage)) return NULL;
    pthread_once(&g_start, started);
    pthread_mutex_lock(&g_mu);
    shentry *e = entry(key, (int)stage);
    if (!ensure(e, 1)) { pthread_mutex_unlock(&g_mu); return NULL; }
    if (!e->c1_set) {   // the constants come with the game's program
        e->c1_set = 1;
        const uint8_t *control = sd->control;
        uint32_t c1off, length;
        memcpy(&c1off, control + 0x700, 4);   // the shader's constants: the code's tail (c1)
        memcpy(&length, control + 0x704, 4);
        if (c1off < length) {   // copied: the program's memory may be freed and reused while the key stays cached
            uint8_t *c = malloc(length - c1off);
            if (!c) die("shader constants");
            memcpy(c, (const uint8_t *)(uintptr_t)sd->data + c1off, length - c1off);
            e->sh.c1 = c;
            e->sh.c1_size = length - c1off;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return &e->sh;
}

void vkb_shader_stats(uint64_t *loaded, uint64_t *missing) { *loaded = g_loaded; *missing = g_missing; }
void vkb_shader_report(void) {   // the memory log
    uint64_t rec, bytes, hits, misses;
    vkb_shbin_stats(&rec, &bytes, &hits, &misses);
    pthread_mutex_lock(&g_mu);
    int progs = g_nprog;
    uint64_t sets = g_prog_sets, drops = g_prog_drops;
    pthread_mutex_unlock(&g_mu);
    VLOG("shaders: objects %llu (made from binaries %llu, compiled %llu, binaries refused %llu, destroyed %llu); programs live %d "
         "(set %llu, finalized %llu); binaries on disk %llu (%.0f MiB); compiled ahead %llu of %llu",
         (unsigned long long)g_obj_live, (unsigned long long)g_from_bin, (unsigned long long)g_from_spv,
         (unsigned long long)g_bin_refused, (unsigned long long)g_obj_destroyed, progs, (unsigned long long)sets,
         (unsigned long long)drops, (unsigned long long)rec, bytes / 1048576.0, (unsigned long long)g_ahead_done,
         (unsigned long long)g_ahead_total);
}
