// Guest address -> recompiled function lookup, and the host (HLE) function registry.
// Based on LASRecomp runtime/dispatch.c (MIT); TOTK: three modules linked statically into the executable.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "rt.h"

// Tables emitted by tools/lifter/gen.py for each module (<name>_table.c).
#define GAME_TABLE(n) extern const uint64_t n##_fn_addrs[]; extern const GuestFn n##_fn_ptrs[]; extern const uint64_t n##_fn_count;
GAME_TABLE(main)
GAME_TABLE(subsdk0)
GAME_TABLE(sdk)

typedef struct { const uint64_t *addrs; const GuestFn *fns; uint64_t count, lo, hi; } FnTable;
static FnTable tables[MAX_MODULES];

// HLE stubs live at fake addresses HLE_BASE + 16*i
#define MAX_HLE 4096
typedef struct { const char *name; HostFn fn; void *native; char ret; } HleSlot;
static HleSlot hle_slots[MAX_HLE];
static int hle_count;

// Overrides of recompiled guest functions (by address)
#define MAX_OVERRIDE 1024
static struct { uint64_t addr; HostFn fn; const char *name; } overrides[MAX_OVERRIDE];
static int override_count;

// Host implementations registered by name (HLE_FUNC, hle.h)
#if defined(__linux__)
extern const HleImplEntry __start_hle_impls[] __attribute__((weak));
extern const HleImplEntry __stop_hle_impls[] __attribute__((weak));
#else
// PE/COFF: the linker sorts .hle$a < .hle$m (entries) < .hle$z by the text after '$'.
__attribute__((section(".hle$a"), used)) static const HleImplEntry __start_hle_impls[1];
__attribute__((section(".hle$z"), used)) static const HleImplEntry __stop_hle_impls[1];
#endif

static const HleImplEntry *hle_lookup_entry(const char *name) {
    const HleImplEntry *start = __start_hle_impls, *stop = __stop_hle_impls;
#if defined(__linux__)
    if (!start || !stop) return NULL;
#endif
    __asm__("" : "+r"(start), "+r"(stop));  // separate objects: keep the compiler from reasoning about them
    for (const HleImplEntry *p = start; p < stop; p++)
        if (p->name && !strcmp(p->name, name)) return p;
    return NULL;
}
const void *hle_lookup_entry_public(const char *name) { return hle_lookup_entry(name); }
HostFn hle_lookup_impl(const char *name) {
    const HleImplEntry *e = hle_lookup_entry(name);
    return e ? e->fn : NULL;
}

// HLE_NATIVE: the slot being called (set by dispatch_slow, which every host call passes through: host stubs are not
// cached in the dispatch cache).
static __thread const HleSlot *t_native;
void hle_native_thunk(Cpu *c) {
    const HleSlot *s = t_native;
    uint64_t (*f)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = s->native;
    uint64_t r = f(c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5], c->x[6], c->x[7]);
    switch (s->ret) {
    case 'v': break;
    case 'b': c->x[0] = (uint8_t)r; break;
    case 'w': c->x[0] = (uint32_t)r; break;
    default: c->x[0] = r; break;
    }
}

static __thread uint64_t unimpl_pc;
static void hle_unimplemented(Cpu *c) {
    int i = (int)((unimpl_pc - HLE_BASE) / 16);
    FATAL("unimplemented host function %s (x0=%llx x1=%llx lr=%llx)\n", hle_slots[i].name,
          (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
}

uint64_t hle_register(const char *name, HostFn fn) {
    for (int i = 0; i < hle_count; i++)
        if (!strcmp(hle_slots[i].name, name)) return HLE_BASE + 16ull * i;
    if (hle_count == MAX_HLE) FATAL("too many HLE stubs\n");
    hle_slots[hle_count].name = strdup(name);
    hle_slots[hle_count].fn = fn;
    return HLE_BASE + 16ull * hle_count++;
}

uint64_t hle_register_entry(const char *name, const void *entry) {
    const HleImplEntry *e = entry;
    uint64_t a = hle_register(name, e->fn);
    HleSlot *s = &hle_slots[(a - HLE_BASE) / 16];
    s->native = e->native;
    s->ret = e->ret;
    return a;
}

void hle_override(uint64_t addr, HostFn fn, const char *name) {
    if (override_count == MAX_OVERRIDE) FATAL("too many overrides\n");
    overrides[override_count].addr = addr;
    overrides[override_count].fn = fn;
    overrides[override_count].name = name;
    override_count++;
}

static uint64_t hle_next = HLE_HEAP;
void *hle_alloc(uint64_t size) {
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    uint64_t a = hle_next;
    hle_next += (size + 63) & ~63ull;
    if (hle_next > HLE_HEAP + HLE_HEAP_SIZE) FATAL("HLE heap exhausted\n");
    pthread_mutex_unlock(&m);
    plat_commit(a, size);
    memset((void *)a, 0, size);
    return (void *)a;
}

void dispatch_init(void) {
    const uint64_t *addrs[] = {main_fn_addrs, subsdk0_fn_addrs, sdk_fn_addrs};
    const GuestFn *fns[] = {main_fn_ptrs, subsdk0_fn_ptrs, sdk_fn_ptrs};
    uint64_t counts[] = {main_fn_count, subsdk0_fn_count, sdk_fn_count};
    for (int t = 0; t < MAX_MODULES; t++)
        tables[t] = (FnTable){addrs[t], fns[t], counts[t], addrs[t][0], addrs[t][counts[t] - 1]};
}

GuestFn dispatch_lookup(uint64_t addr) {
    for (int t = 0; t < MAX_MODULES; t++) {
        FnTable *ft = &tables[t];
        if (addr < ft->lo || addr > ft->hi) continue;
        uint64_t lo = 0, hi = ft->count;
        while (lo < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (ft->addrs[mid] < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < ft->count && ft->addrs[lo] == addr) return ft->fns[lo];
    }
    return NULL;
}

// Small direct-mapped cache in front of the binary search
#define DCACHE 4096
static __thread struct { uint64_t addr; GuestFn fn; } dcache[DCACHE];

static GuestFn dispatch_slow(Cpu *c) {
    uint64_t pc = c->pc;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        int i = (int)((pc - HLE_BASE) / 16);
        if (i < hle_count) {
            if (hle_slots[i].native) { t_native = &hle_slots[i]; return hle_native_thunk; }
            if (hle_slots[i].fn) return hle_slots[i].fn;
            unimpl_pc = pc;
            return hle_unimplemented;
        }
    }
    for (int i = 0; i < override_count; i++)
        if (overrides[i].addr == pc) return overrides[i].fn;
    GuestFn f = dispatch_lookup(pc);
    if (!f) {
        uint64_t off = 0;
        const char *s = loader_symbol_at(c->x[30], &off);
        FATAL("no function at %llx (lr=%llx %s+%llx)\n", (unsigned long long)pc, (unsigned long long)c->x[30],
              s ? s : "?", (unsigned long long)off);
    }
    unsigned h = (unsigned)(pc >> 2) & (DCACHE - 1);
    dcache[h].addr = pc;
    dcache[h].fn = f;
    return f;
}

// Always tail-calls, so chains of indirect jumps don't grow the host stack.
void rt_dispatch(Cpu *c) {
    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);
    GuestFn f = dcache[h].addr == c->pc ? dcache[h].fn : dispatch_slow(c);
    __attribute__((musttail)) return f(c);
}

void guest_call(Cpu *c, uint64_t addr) {
    uint64_t lr = c->x[30];
    c->x[30] = 0;
    c->pc = addr;
    rt_dispatch(c);
    c->x[30] = lr;
}

void rt_udf(Cpu *c, uint64_t pc) {
    (void)c;
    FATAL("undefined instruction / trap at %llx\n", (unsigned long long)pc);
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)c;
    FATAL("brk #%u at %llx\n", imm, (unsigned long long)pc);
}

void rt_fatal_trace(void) {
    plat_print_backtrace();
    fflush(stderr);
    plat_fatal_notice();
    abort();
}

// ---- crash reporting (platform/*.c calls these)

// Host pc -> recompiled guest function containing it ("f_<addr>").
static int cmp_host(const void *a, const void *b) {
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}
const char *rt_host_fn_name(uint64_t pc, uint64_t *offset) {
    static uint64_t (*byhost)[2];  // {host address, guest address}, sorted by host address
    static uint64_t n;
    static __thread char name[32];
    if (!byhost) {
        uint64_t total = 0, k = 0;
        for (int t = 0; t < MAX_MODULES; t++) total += tables[t].count;
        uint64_t (*v)[2] = malloc(total * sizeof *v);
        if (!v) return NULL;
        for (int t = 0; t < MAX_MODULES; t++)
            for (uint64_t i = 0; i < tables[t].count; i++) {
                v[k][0] = (uint64_t)(uintptr_t)tables[t].fns[i];
                v[k++][1] = tables[t].addrs[i];
            }
        qsort(v, k, sizeof *v, cmp_host);
        n = k;
        byhost = v;
    }
    uint64_t lo = 0, hi = n;  // last entry with host address <= pc
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (byhost[mid][0] <= pc) lo = mid + 1;
        else hi = mid;
    }
    if (!lo || pc - byhost[lo - 1][0] > 0x100000) return NULL;
    snprintf(name, sizeof name, "f_%llx", (unsigned long long)byhost[lo - 1][1]);
    if (offset) *offset = pc - byhost[lo - 1][0];
    return name;
}

bool rt_fault_watch(uint64_t fa, uint64_t pc) { (void)fa; (void)pc; return false; }

void (*rt_fault_extra)(void);   // the host's guest-state report (registers, guest call chain), set at startup

void rt_fault_report(const char *what, uint64_t fa, uint64_t pc, uint64_t lr) {
    uint64_t po = 0, lo = 0;
    const char *ps = rt_host_fn_name(pc, &po), *ls = rt_host_fn_name(lr, &lo);
    if (!ps) ps = plat_symbol(pc, &po);
    fprintf(stderr, "\n*** %s accessing %#llx\n    host pc %llx (%s+%llx)\n    host lr %llx (%s)\n", what,
            (unsigned long long)fa, (unsigned long long)pc, ps ? ps : "?", (unsigned long long)po,
            (unsigned long long)lr, ls ? ls : "?");
    if (rt_fault_extra) rt_fault_extra();
    rt_fatal_trace();
}

// ---- function-entry profiler hook (cpu.h FN_ENTER): off unless a profiler sets g_fnprof
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

// ---- CNTPCT_EL0: the Switch's 19.2 MHz system counter
uint64_t rt_ticks(void) { return (uint64_t)((unsigned __int128)plat_now_ns() * 12 / 625); }
