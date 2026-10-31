// [Windows port] The registry of NVN implementations (MacNativeGames' Mach-O sections swnvn / totknvnx) as PE sections,
// and the guest addresses the game receives for NVN names.
#include <string.h>

#include "../host/hle.h"
#include "nvn_impl.h"

#if defined(__linux__)
extern const nvn_impl __start_nvni[] __attribute__((weak));
extern const nvn_impl __stop_nvni[] __attribute__((weak));
extern const nvn_impl __start_nvnx[] __attribute__((weak));
extern const nvn_impl __stop_nvnx[] __attribute__((weak));

const nvn_impl *nvn_registry_impls(unsigned *n) {
    if (!__start_nvni || !__stop_nvni) { *n = 0; return NULL; }
    *n = (unsigned)(__stop_nvni - __start_nvni);
    return __start_nvni;
}
const nvn_impl *nvn_registry_extras(unsigned *n) {
    if (!__start_nvnx || !__stop_nvnx) { *n = 0; return NULL; }
    *n = (unsigned)(__stop_nvnx - __start_nvnx);
    return __start_nvnx;
}
#else
// The linker sorts .nvni$a < .nvni$m (NVN / NVN_NOPROTO entries) < .nvni$z by the text after '$'; likewise .nvnx.
__attribute__((section(".nvni$a"), used)) static const nvn_impl impls_start[1];
__attribute__((section(".nvni$z"), used)) static const nvn_impl impls_stop[1];
__attribute__((section(".nvnx$a"), used)) static const nvn_impl extras_start[1];
__attribute__((section(".nvnx$z"), used)) static const nvn_impl extras_stop[1];

static const nvn_impl *range(const nvn_impl *a, const nvn_impl *b, unsigned *n) {
    __asm__("" : "+r"(a), "+r"(b));   // separate objects: keep the compiler from reasoning about them
    const nvn_impl *first = a + 1;
    while (first < b && !first->name) first++;   // (section padding)
    *n = (unsigned)(b - first);
    return first;
}
const nvn_impl *nvn_registry_impls(unsigned *n) { return range(impls_start, impls_stop, n); }
const nvn_impl *nvn_registry_extras(unsigned *n) { return range(extras_start, extras_stop, n); }
#endif

// The native implementation of an NVN name (the thunks call it).
void *nvn_impl_fn(const char *name, int extra) {
    unsigned n;
    const nvn_impl *t = extra ? nvn_registry_extras(&n) : nvn_registry_impls(&n);
    for (unsigned i = 0; i < n; i++) if (t[i].name && !strcmp(t[i].name, name)) return t[i].fn;
    LOG("NVN implementation %s missing from the registry", name);
    runtime_fail("NVN registry");
}

extern const struct { const char *name; void (*fn)(Cpu *); } nvn_thunks[];   // nvn_thunks.gen.c
extern const unsigned nvn_thunk_count;

int nvn_nop_name(const char *name);   // hle_nvn.c g_nops
static void nvn_nop_thunk(Cpu *c) { (void)c; }

// The guest address for an NVN name: its thunk's, an empty function for the GPU-only commands the null device ignores
// (hle_nvn.c g_nops), or a host stub that stops the game naming it when it is called.
uint64_t nvn_registry_guest_address(const char *name) {
    for (unsigned i = 0; i < nvn_thunk_count; i++)
        if (!strcmp(nvn_thunks[i].name, name)) return hle_register(nvn_thunks[i].name, nvn_thunks[i].fn);
    if (nvn_nop_name(name)) return hle_register(name, nvn_nop_thunk);
    LOG("[nvn] stubbing unknown NVN symbol: %s", name);
    return hle_register(name, nvn_nop_thunk);
}
