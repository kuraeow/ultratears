// The vertex shader variants to make ahead (vkb_variants.c): data/shaders/variants.txt, written by
// tools/shader_variants.py from the MacNativeGames shared shader list.
#pragma once
#include <stdint.h>

// The integer attribute masks the list names for a vertex shader: up to max pairs (intmask, sintmask); the count.
int vkb_variants_ints(const char *vs_key, uint32_t (*out)[2], int max);
// The fragment shaders the list pairs a vertex shader with: up to max keys (65 bytes each, hex); the count.
int vkb_variants_pairs(const char *vs_key, char (*out)[65], int max);
// A number that changes with the file's contents (0: no file): part of what compiled ahead from it is marked with.
uint32_t vkb_variants_hash(void);
