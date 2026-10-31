/* Horizon memory and system SVCs for the recompiled game.
 * Ported from MacNativeGames games/totk/runtime/services.c (runtime_svc, memory_init, alias_map, stack_move) and
 * loader.c (module permissions / query) @b75be8c (MIT). Changes: guest regions live inside the window that
 * src/runtime reserves at startup (pages are committed when first touched); unmapping decommits pages, so a page
 * mapped again reads as zeros. Darwin's 16 KiB page handling is gone (Windows pages are 4 KiB, as on the Switch).
 * Unmapped guest pages are not made inaccessible yet: a stray access reads zeros instead of faulting. */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "host.h"
#include "ipc.h"

/* Guest address layout. The SDK encodes IPC buffer addresses with 39 bits, so all guest memory lies below 2^39.
   MacNativeGames' layout: a 64 GiB alias region (the real 39-bit Horizon size, which the game's allocators depend
   on) and 6 GiB for stacks and the heap below it. */
#define ALIAS_START   0x7000000000ULL
#define ALIAS_SIZE    (64ULL << 30)
#define LOW_START     0x1000000000ULL
#define LOW_SIZE      (6ULL << 30)
#define STACK_OFF     0x10000000ULL
#define STACK_SIZE    0x10000000ULL
#define STACK_START   (LOW_START + STACK_OFF)
#define HEAP_START    (LOW_START + STACK_OFF + STACK_SIZE + 0x10000000ULL)
#define HEAP_CAP      (4ULL << 30)   /* 1.4.3 sets a 2.2 GiB heap during nninitStartup */
#define ASLR_START    LOW_START
#define ASLR_SIZE     ((1ULL << 39) - LOW_START)
#define SYSTEM_RESOURCE_BUDGET (32ULL << 20)
#define APP_POOL_BASE 0xcd500000ULL  /* retail application pool, 3.2 GiB (MacNativeGames' default without 2K heaps) */
uint64_t totk_heap_extra_bytes(void);   /* render_res.c: a bigger game resolution grows the render-target heap by this */
#define APP_POOL (APP_POOL_BASE + totk_heap_extra_bytes())

static uint64_t heap_size;
static unsigned char alias_pages[ALIAS_SIZE / 4096];
static uint64_t alias_bytes;
static uint64_t main_stack_lo, main_stack_hi;
static uint8_t *user_exception_context;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;

static void decommit(uint64_t addr, uint64_t size) {
#ifdef _WIN32
    if (size && !VirtualFree((void *)(uintptr_t)addr, size, MEM_DECOMMIT)) runtime_fail("decommit guest pages");
#else
    (void)addr; (void)size;
#endif
}

void runtime_memory_init(void) {
    static int done;
    if (done) return;
    done = 1;
    user_exception_context = hle_alloc(0x1000);
    fprintf(stderr, "[totk] guest layout: alias %#llx+%#llx stack %#llx+%#llx heap %#llx (cap %#llx)\n",
            ALIAS_START, ALIAS_SIZE, STACK_START, STACK_SIZE, HEAP_START, HEAP_CAP);
}

uint64_t runtime_main_stack(uint64_t size) {
    /* The main thread's stack (main.npdm: 1 MiB) sits at the top of the stack region; the SDK maps further thread
       stacks below it with MapMemory. */
    main_stack_hi = STACK_START + STACK_SIZE;
    main_stack_lo = main_stack_hi - size;
    plat_commit(main_stack_lo, size);
    return main_stack_hi;
}

uint64_t runtime_image_bytes(void) {
    uint64_t total = 0;
    for (int i = 0; i < g_nmodules; i++) total += g_modules[i].size;
    return (total + 0xfff) & ~0xfffULL;
}

/* ---------- MapMemory / UnmapMemory into the stack region (MacNativeGames stack_move) ---------- */
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[4096];
static unsigned nstack_maps;
static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static uint64_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t size) {
    if (!size || ((dst | src | size) & 4095)) return 0xcc01;
    if (dst < STACK_START || size > STACK_SIZE || dst - STACK_START > STACK_SIZE - size) return 0xdc01;
    int inHeap = src >= HEAP_START && size <= heap_size && src - HEAP_START <= heap_size - size;
    int inAlias = src >= ALIAS_START && size <= ALIAS_SIZE && src - ALIAS_START <= ALIAS_SIZE - size;
    if (!inHeap && !inAlias) return 0xdc01;
    pthread_mutex_lock(&memory_lock);
    if (inAlias) {
        unsigned first = (unsigned)((src - ALIAS_START) / 4096);
        for (uint64_t i = 0; i < size / 4096; i++)
            if (!alias_pages[first + i]) { pthread_mutex_unlock(&memory_lock); return 0xdc01; }
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == size) found = i;
    if (unmap) {
        if (found == nstack_maps) { pthread_mutex_unlock(&memory_lock); return 0xd401; }
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, size);
        stack_maps[found] = stack_maps[--nstack_maps];
        decommit(dst, size);
    } else {
        if (nstack_maps == 4096) { pthread_mutex_unlock(&memory_lock); return 0xd401; }
        // The main thread's stack is mapped too (QueryMemory reports it): the SDK places thread stacks at random
        // addresses and retries when the kernel refuses one, as Horizon does for memory that is in use.
        if (overlaps(dst, size, main_stack_lo, main_stack_hi - main_stack_lo)) { pthread_mutex_unlock(&memory_lock); return 0xdc01; }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, size, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, size, stack_maps[i].src, stack_maps[i].size)) {
                pthread_mutex_unlock(&memory_lock);
                return 0xdc01;
            }
        plat_commit(dst, size);
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
        stack_maps[nstack_maps++] = (StackMap){dst, src, size};
    }
    pthread_mutex_unlock(&memory_lock);
    return 0;
}

/* ---------- MapPhysicalMemory / UnmapPhysicalMemory in the alias region (MacNativeGames alias_map) ---------- */
static uint64_t alias_map(int unmap, uint64_t addr, uint64_t size) {
    if (!size || ((addr | size) & 4095)) return 0xcc01;
    if (addr < ALIAS_START || size > ALIAS_SIZE || addr - ALIAS_START > ALIAS_SIZE - size) return 0xdc01;
    pthread_mutex_lock(&memory_lock);
    unsigned first = (unsigned)((addr - ALIAS_START) / 4096), count = (unsigned)(size / 4096);
    /* Horizon MapPhysicalMemory maps only the still-unmapped pages of the range and leaves mapped pages (and their
       contents) untouched; TOTK's physical-memory heaps grow by re-mapping ranges that overlap what they own. */
    uint64_t fresh = 0;
    if (!unmap)
        for (unsigned i = 0; i < count; i++) if (!alias_pages[first + i]) fresh += 4096;
    /* Out of the application pool is LimitReached (0x10801), which the SDK treats as recoverable out-of-memory. */
    if (!unmap && fresh + alias_bytes + heap_size + runtime_image_bytes() > APP_POOL) {
        pthread_mutex_unlock(&memory_lock);
        return 0x10801;
    }
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(addr, size, stack_maps[i].src, stack_maps[i].size)) { pthread_mutex_unlock(&memory_lock); return 0xdc01; }
        uint64_t released = 0;
        for (unsigned i = 0; i < count; i++)
            if (alias_pages[first + i]) {
                alias_pages[first + i] = 0;
                released += 4096;
                decommit(addr + (uint64_t)i * 4096, 4096);   // mapped again later: zero-filled like a fresh page
            }
        alias_bytes -= released;
    } else {
        for (unsigned i = 0; i < count; i++) alias_pages[first + i] = 1;
        alias_bytes += fresh;
    }
    pthread_mutex_unlock(&memory_lock);
    return 0;
}

/* ---------- module memory: permissions (RELRO) and QueryMemory (MacNativeGames loader.c) ---------- */
static pthread_mutex_t module_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *module_permissions[MAX_MODULES];
static unsigned module_page_permission(int i, uint64_t off) {
    Module *m = &g_modules[i];
    if (off < m->data_off) return off < ((m->text_off + m->text_size + 0xfff) & ~0xfffULL) ? 5 : 1;
    return module_permissions[i] ? module_permissions[i][(off - m->data_off) / 4096] : 3;
}
static unsigned char *permission_table(int i) {
    Module *m = &g_modules[i];
    if (!module_permissions[i]) {
        size_t count = (m->size - m->data_off + 4095) / 4096;
        module_permissions[i] = malloc(count);
        if (!module_permissions[i]) runtime_fail("module permission table");
        memset(module_permissions[i], 3, count);
    }
    return module_permissions[i];
}
static uint64_t module_permission(uint64_t address, uint64_t size, unsigned permission) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (permission != 0 && permission != 1 && permission != 3) return 0xd801;
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (address < m->base || address - m->base < m->data_off || address - m->base >= m->size ||
            size > m->size - (address - m->base)) continue;
        uint64_t off = address - m->base;
        pthread_mutex_lock(&module_permission_lock);
        unsigned char *t = permission_table(i);
        for (uint64_t p = 0; p < size / 4096; p++)
            if (t[(off - m->data_off) / 4096 + p] & 16) { pthread_mutex_unlock(&module_permission_lock); return 0xd401; }
        memset(t + (off - m->data_off) / 4096, permission, size / 4096);
        plat_protect(address, size, permission == 0 ? PLAT_PROT_NONE : permission == 1 ? PLAT_PROT_READ : PLAT_PROT_RW);
        pthread_mutex_unlock(&module_permission_lock);
        fprintf(stderr, "[totk] %s data permission +%#llx size=%#llx perm=%u\n", m->name, (unsigned long long)off,
                (unsigned long long)size, permission);
        return 0;
    }
    return 0xd401;
}
static uint64_t module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
    if (address & 4095) return 0xcc01;
    if (!size || (size & 4095)) return 0xca01;
    if (mask != 16 || attribute != 16) return 0xd801;   /* the SDK only uses the irreversible RELRO lock */
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (address < m->base || address - m->base < m->data_off || address - m->base >= m->size ||
            size > m->size - (address - m->base)) continue;
        pthread_mutex_lock(&module_permission_lock);
        unsigned char *t = permission_table(i);
        size_t first = (address - m->base - m->data_off) / 4096;
        for (uint64_t p = 0; p < size / 4096; p++) t[first + p] |= 16;
        pthread_mutex_unlock(&module_permission_lock);
        return 0;
    }
    return 0xd401;
}
int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission,
                         unsigned *attribute) {
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        if (address < m->base || address - m->base >= m->size) continue;
        uint64_t off = (address - m->base) & ~4095ULL, lo = off, hi = off + 4096;
        pthread_mutex_lock(&module_permission_lock);
        unsigned perm = module_page_permission(i, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_permission(i, lo - 4096) == perm) lo -= 4096;
        while (hi < end && module_page_permission(i, hi) == perm) hi += 4096;
        pthread_mutex_unlock(&module_permission_lock);
        *base = m->base + lo; *size = hi - lo; *state = data ? 4 : 3; *permission = perm & 7; *attribute = perm & 16;
        return 1;
    }
    return 0;
}

/* ---------- the SVC dispatcher (MacNativeGames runtime_svc) ---------- */
uint64_t runtime_svc(unsigned num, uint64_t *r, uint64_t pc) {
    static _Atomic int trace = -1;
    int enabled = atomic_load_explicit(&trace, memory_order_relaxed);
    if (enabled < 0) { enabled = getenv("TOTK_TRACE_SVC") != NULL; atomic_store_explicit(&trace, enabled, memory_order_relaxed); }
    if (enabled)
        fprintf(stderr, "[totk] SVC 0x%x at %#llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n", num, (unsigned long long)pc,
                (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
    if (svc_thread((int)num, r)) return r[0];
    if (runtime_ipc_svc(num, r)) return r[0];
    runtime_memory_init();
    switch (num) {
    case 0x02: r[0] = module_permission(r[0], r[1], (unsigned)r[2]); return r[0];
    case 0x03: r[0] = module_lock_permission(r[0], r[1], (unsigned)r[2], (unsigned)r[3]); return r[0];
    case 0x29: {   /* GetInfo */
        uint64_t value = 0;
        unsigned id = (unsigned)r[1];
        switch (id) {
        case 0: value = 7; break;
        case 1: value = 0x0ffffffff0000000ULL; break;
        case 2: value = ALIAS_START; break;
        case 3: value = ALIAS_SIZE; break;
        case 4: value = HEAP_START; break;
        case 5: value = HEAP_CAP; break;
        case 6: case 21: value = APP_POOL; break;
        case 7: case 22: value = runtime_image_bytes() + heap_size + alias_bytes; break;
        case 8: value = 0; break;
        case 10: value = 0; break;
        case 11: value = ((uint64_t)rand() << 32) ^ (uint64_t)rand() ^ runtime_now_ns(); break;
        case 12: value = ASLR_START; break;
        case 13: value = ASLR_SIZE; break;
        case 14: value = STACK_START; break;
        case 15: value = STACK_SIZE; break;
        case 16: value = SYSTEM_RESOURCE_BUDGET; break;
        case 17: value = sizeof(stack_maps) + sizeof(alias_pages); break;
        case 18: value = 0x0100f2c0115b6000ULL; break;
        case 20: value = (uintptr_t)user_exception_context; break;
        case 23: value = 1; break;
        case 28: value = 0; break;   /* AliasRegionExtraSize: no kernel-owned extension */
        default: fprintf(stderr, "unknown GetInfo id %u\n", id); runtime_fail("GetInfo id not implemented");
        }
        r[0] = 0; r[1] = value;
        return 0;
    }
    case 0x2c: case 0x2d: {
        uint64_t a = r[0], n = r[1];
        r[0] = alias_map(num == 0x2d, a, n);
        if (r[0])
            fprintf(stderr, "[totk] %sMapPhysicalMemory %#llx+%#llx failed %#llx (alias %#llx used, heap %#llx)\n",
                    num == 0x2d ? "Un" : "", (unsigned long long)a, (unsigned long long)n, (unsigned long long)r[0],
                    (unsigned long long)alias_bytes, (unsigned long long)heap_size);
        return r[0];
    }
    case 0x04: case 0x05: r[0] = stack_move(num == 5, r[0], r[1], r[2]); return r[0];
    case 0x06: {   /* QueryMemory(x0 info out, x2 address) */
        uint64_t addr = r[2], base = addr & ~0xfffULL, size = 0x1000;
        unsigned state = 0, perm = 0, attr = 0;
        uint64_t heap_end = HEAP_START + heap_size;
        if (addr >= main_stack_lo && addr < main_stack_hi) { base = main_stack_lo; size = main_stack_hi - main_stack_lo; state = 0xb; perm = 3; }
        else if (addr >= HEAP_START && addr < heap_end) { base = HEAP_START; size = heap_size; state = 5; perm = 3; }
        else if (addr >= LOW_START && addr < LOW_START + LOW_SIZE) {
            if (addr < HEAP_START) { base = LOW_START; size = HEAP_START - base; }
            else { base = heap_end; size = LOW_START + LOW_SIZE - base; }
        } else if (addr >= LOW_START + LOW_SIZE && addr < ALIAS_START) {
            base = LOW_START + LOW_SIZE; size = ALIAS_START - base; state = 0x10; perm = 0;   /* host-owned: Inaccessible */
        } else if (!addr) { base = 0; size = ASLR_START; state = 0; perm = 0; }
        else if (runtime_module_query(addr, &base, &size, &state, &perm, &attr)) {}
        else { state = 3; perm = 5; }
        pthread_mutex_lock(&memory_lock);
        if (addr >= ALIAS_START && addr < ALIAS_START + ALIAS_SIZE) {
            unsigned p = (unsigned)((addr - ALIAS_START) / 4096), low = p, high = p + 1;
            unsigned allocated = alias_pages[p];
            while (low > 0 && alias_pages[low - 1] == allocated) low--;
            while (high < sizeof(alias_pages) && alias_pages[high] == allocated) high++;
            base = ALIAS_START + (uint64_t)low * 4096; size = (uint64_t)(high - low) * 4096;
            state = allocated ? 5 : 0; perm = allocated ? 3 : 0;
        }
        for (unsigned i = 0; i < nstack_maps; i++) {
            StackMap *m = &stack_maps[i];
            uint64_t end = m->dst + m->size, send = m->src + m->size;
            if (addr >= m->dst && addr < end) { base = m->dst; size = m->size; state = 0xb; perm = 3; break; }
            if (addr >= m->src && addr < send) { base = m->src; size = m->size; state = 5; perm = 0; attr = 4; break; }
            if (state == 0) { uint64_t oldEnd = base + size; if (end <= addr && end > base) base = end; if (m->dst > addr && m->dst < oldEnd) oldEnd = m->dst; size = oldEnd - base; }
            else if (state == 5) { uint64_t oldEnd = base + size; if (send <= addr && send > base) base = send; if (m->src > addr && m->src < oldEnd) oldEnd = m->src; size = oldEnd - base; }
        }
        pthread_mutex_unlock(&memory_lock);
        struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info = {base, size, state, attr, perm, 0, 0, 0};
        if (r[0]) memcpy((void *)(uintptr_t)r[0], &info, sizeof(info));
        r[0] = 0; r[1] = 0;
        return 0;
    }
    case 0x01: {   /* SetHeapSize */
        uint64_t size = r[1];
        if (size > HEAP_CAP || size % 0x200000 || size + runtime_image_bytes() > APP_POOL) { r[0] = 0xd401; return r[0]; }
        if (size < heap_size) decommit(HEAP_START + size, heap_size - size);
        heap_size = size;
        r[0] = 0; r[1] = HEAP_START;
        return 0;
    }
    case 0x26: {   /* Break */
        uint32_t result = 0;
        if (r[1] && r[2] >= 4) memcpy(&result, (void *)(uintptr_t)r[1], 4);
        fprintf(stderr, "[totk] guest Break reason=%llx result=%x lr=%#llx\n", (unsigned long long)r[0], result,
                (unsigned long long)r[30]);
        if (r[0] & 0x80000000ULL) return 0;
        runtime_fail("original SDK Break");
    }
    case 0x27: {   /* OutputDebugString */
        size_t n = r[1];
        if (n > 65536) runtime_fail("debug string too long");
        fwrite((void *)(uintptr_t)r[0], 1, n, stderr);
        r[0] = 0;
        return 0;
    }
    default:
        fprintf(stderr, "[totk] unimplemented SVC 0x%x at %#llx lr=%#llx\n", num, (unsigned long long)pc, (unsigned long long)r[30]);
        if (num == 0x1f) fprintf(stderr, "[totk] named port %.12s\n", (const char *)(uintptr_t)r[1]);
        runtime_fail("system service not implemented");
    }
}
