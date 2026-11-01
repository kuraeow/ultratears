// The translated shaders in one file (vkb_shpack.c; tools/pack_shaders.py writes it).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The pack is there (TOTK_SHADER_PACK, `shaderpack=` in ultratears.cfg, default data/shaders/translated.pack): 1.
int vkb_shpack_on(void);
// A shader's .res.json (malloc, NUL-terminated) and .spv (malloc) and its translation stamp. 0: not in the pack.
int vkb_shpack_get(const char *key, char **json, uint32_t **spv, size_t *spv_bytes, uint32_t *stamp);
// The pack's shaders in key order: count, and the i-th key (hex) with its stamp.
size_t vkb_shpack_count(void);
void vkb_shpack_key(size_t i, char key[65], uint32_t *stamp);
