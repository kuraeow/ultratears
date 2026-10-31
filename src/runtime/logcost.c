// The time the log costs: every write to stderr / stdout (the log: ultratears.log, unbuffered so that a crash loses
// nothing) goes through these wrappers (the link's --wrap, tools/build_game.py), which count the writes and the time
// they take. The long-frame line (gfx/nvn/hle_nvn.c) says how much of a long frame went to the log, so a stutter caused
// by writing it shows as such instead of as an unexplained frame. Writes to other files are passed through untimed.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "platform.h"

static _Atomic uint64_t g_writes, g_ns;

void runtime_log_cost(uint64_t *writes, uint64_t *ns) {
    *writes = atomic_load(&g_writes);
    *ns = atomic_load(&g_ns);
}

static int is_log(FILE *f) { return f == stderr || f == stdout; }

static void account(uint64_t t0) {
    atomic_fetch_add(&g_writes, 1);
    atomic_fetch_add(&g_ns, plat_now_ns() - t0);
}

int __real_vfprintf(FILE *f, const char *fmt, va_list ap);
int __real_fputs(const char *s, FILE *f);
int __real_fputc(int c, FILE *f);

int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!is_log(f)) return __real_vfprintf(f, fmt, ap);
    uint64_t t0 = plat_now_ns();
    int r = __real_vfprintf(f, fmt, ap);
    account(t0);
    return r;
}

int __wrap_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __wrap_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int __wrap_fputs(const char *s, FILE *f) {
    if (!is_log(f)) return __real_fputs(s, f);
    uint64_t t0 = plat_now_ns();
    int r = __real_fputs(s, f);
    account(t0);
    return r;
}

int __wrap_fputc(int c, FILE *f) {
    if (!is_log(f)) return __real_fputc(c, f);
    uint64_t t0 = plat_now_ns();
    int r = __real_fputc(c, f);
    account(t0);
    return r;
}
