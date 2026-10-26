
#ifndef O_BINARY
#define O_BINARY 0
#endif

#include <unistd.h>
static inline int _chsize_s(int fd, int64_t size) {
    return ftruncate(fd, (off_t)size);
}
// What glibc before 2.38 lacks (included via runtime/platform.h on Linux only): the AppImage is built against
// Ubuntu 22.04's glibc 2.35 (no strlcpy / strlcat until 2.38, no arc4random until 2.36).
#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/random.h>

static inline void *_aligned_malloc(size_t size, size_t alignment) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    return ptr;
}
static inline void _aligned_free(void *ptr) {
    free(ptr);
}

#if !__GLIBC_PREREQ(2, 38)
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t k = len < n - 1 ? len : n - 1;
        memcpy(d, s, k);
        d[k] = 0;
    }
    return len;
}
static inline size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}
#endif

#if !__GLIBC_PREREQ(2, 36)
static inline uint32_t arc4random(void) {
    uint32_t v = 0;
    while (getrandom(&v, sizeof v, 0) != sizeof v) {}
    return v;
}
static inline uint32_t arc4random_uniform(uint32_t n) {
    if (n < 2) return 0;
    uint32_t min = -n % n, r;  // reject the low values that would bias the modulo
    do r = arc4random(); while (r < min);
    return r % n;
}
#endif
