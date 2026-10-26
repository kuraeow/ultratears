#define _GNU_SOURCE
#include <stdint.h>
#ifndef _WIN32
#include <sys/mman.h>
#define MEM_RESERVE 0x2000
#define MEM_COMMIT 0x1000
#define MEM_RELEASE 0x8000
#define PAGE_READWRITE 0x04
#define PAGE_NOACCESS 0x01
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
static inline void *VirtualAlloc(void *addr, size_t size, int type, int prot) {
    int p = (prot == PAGE_NOACCESS) ? PROT_NONE : (PROT_READ | PROT_WRITE);
    void *res = mmap(addr, size, p, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (res == MAP_FAILED) ? NULL : res;
}
static inline int VirtualFree(void *addr, size_t size, int type) {
    (void)size; (void)type;
    return munmap(addr, size);
}
static inline unsigned long GetLastError(void) { return 0; }
#endif
// [Windows port] NVN physical / virtual memory pools (the API of MacNativeGames' totk_vpool.h).
// MacNativeGames maps a virtual pool's 64 KiB pages onto physical pools' pages a second time (mach_vm_remap). Windows
// cannot alias ordinary private memory, and a virtual pool is a GPU address range: the game reaches the physical memory
// through the physical pool, the GPU through the virtual one. So here a virtual pool is
//  - a unique reserved, inaccessible address range (its GPU addresses; a CPU access faults visibly), and
//  - a page table: virtual 64 KiB page -> (physical pool, offset), which the GPU back end resolves (later: Vulkan
//    sparse binding of the physical pools' memory).
// Physical pools the CPU cannot access get host storage (as in MacNativeGames); CPU-visible ones keep the game storage.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "../host/hle.h"
#include "nvn_dirty.h"
#include "nvn_mtl.h"
#include "totk_vpool.h"

#define F_CPU_NO_ACCESS NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS
#define F_PHYSICAL NVN_MEMORY_POOL_FLAGS_PHYSICAL
#define F_VIRTUAL NVN_MEMORY_POOL_FLAGS_VIRTUAL
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')

typedef struct { const pool_t *phys; uint64_t off; } vpage;   // phys NULL: unmapped
typedef struct vtable { const pool_t *pool; uint64_t npages; vpage *pages; struct vtable *next; } vtable;
static vtable *g_tables;
static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_map_gen = 1;   // bumped by every MapVirtual: textures in virtual pools upload again

static vtable *table_of(const pool_t *v) {
    for (vtable *t = g_tables; t; t = t->next) if (t->pool == v) return t;
    return NULL;
}

int totk_vpool_init(pool_t *o) {
    int phys = (o->flags & F_PHYSICAL) != 0, virt = (o->flags & F_VIRTUAL) != 0;
    o->game_mem = o->mem; o->host = NULL; o->span = 0;
    if (phys == virt) { LOG("vpool: pool flags %#x must be exactly one of PHYSICAL/VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % TOTK_VPOOL_PAGE) { LOG("vpool: pool size %#llx not a multiple of 64 KiB", (unsigned long long)o->size); return 0; }
    if (virt && o->mem) { LOG("vpool: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { LOG("vpool: physical pool without storage"); return 0; }
    if (phys && !(o->flags & F_CPU_NO_ACCESS)) return 1;   // the game storage itself
    o->span = o->size;
    if (phys) {   // CPU_NO_ACCESS: our own storage
        o->host = VirtualAlloc(NULL, o->span, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    } else {      // virtual: addresses only
        o->host = VirtualAlloc(NULL, o->span, MEM_RESERVE, PAGE_NOACCESS);
        vtable *t = calloc(1, sizeof *t);
        if (!t) die("vpool: table allocation");
        t->pool = o; t->npages = o->size / TOTK_VPOOL_PAGE;
        t->pages = calloc(t->npages, sizeof *t->pages);
        if (!t->pages) die("vpool: page table allocation");
        pthread_mutex_lock(&g_map_mu); t->next = g_tables; g_tables = t; pthread_mutex_unlock(&g_map_mu);
    }
    if (!o->host) { LOG("vpool: VirtualAlloc(%#llx) failed (%lu)", (unsigned long long)o->span, GetLastError()); return 0; }
    o->mem = o->host;
    return 1;
}

int totk_vpool_fini(pool_t *o) {
    if (!o->host) return 0;
    pthread_mutex_lock(&g_map_mu);
    for (vtable **p = &g_tables; *p; p = &(*p)->next)
        if ((*p)->pool == o) { vtable *t = *p; *p = t->next; free(t->pages); free(t); break; }
    pthread_mutex_unlock(&g_map_mu);
    VirtualFree(o->host, 0, MEM_RELEASE);
    o->host = NULL; o->mem = o->game_mem; o->span = 0;
    return 1;
}

static int check_request(const pool_t *v, const totk_mapping_request *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) { LOG("vpool: request %d negative/empty range", i); return 0; }
    if ((r->virtual_offset | r->physical_offset | (int64_t)r->size) % TOTK_VPOOL_PAGE) { LOG("vpool: request %d not 64 KiB aligned", i); return 0; }
    if (end < r->size || end > v->size) { LOG("vpool: request %d exceeds virtual pool", i); return 0; }
    if (!r->physical_pool) return 1;   // unmap
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != POOL_MAGIC || !(p->flags & F_PHYSICAL)) { LOG("vpool: request %d source %p is not a physical pool", i, (const void *)p); return 0; }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) { LOG("vpool: request %d exceeds physical pool", i); return 0; }
    return 1;
}

int totk_vpool_map(pool_t *v, int count, const totk_mapping_request *r) {
    if (!v || v->magic != POOL_MAGIC || !(v->flags & F_VIRTUAL) || !v->mem) { LOG("vpool: MapVirtual target %p is not a virtual pool", (void *)v); return 0; }
    if (count < 0 || (count && !r)) { LOG("vpool: invalid request count %d", count); return 0; }
    for (int i = 0; i < count; i++) if (!check_request(v, &r[i], i)) return 0;
    pthread_mutex_lock(&g_map_mu);
    vtable *t = table_of(v);
    if (!t) { pthread_mutex_unlock(&g_map_mu); die("vpool: virtual pool %p has no page table", (void *)v); }
    for (int i = 0; i < count; i++) {
        uint64_t first = (uint64_t)r[i].virtual_offset / TOTK_VPOOL_PAGE, n = r[i].size / TOTK_VPOOL_PAGE;
        for (uint64_t k = 0; k < n; k++)
            t->pages[first + k] = (vpage){(const pool_t *)(const void *)r[i].physical_pool,
                                          r[i].physical_pool ? (uint64_t)r[i].physical_offset + k * TOTK_VPOOL_PAGE : 0};
    }
    pthread_mutex_unlock(&g_map_mu);
    atomic_fetch_add(&g_map_gen, 1);
    return 1;
}

uint64_t totk_vpool_generation(void) { return atomic_load(&g_map_gen); }

// [va, va+len) of a virtual pool gathered through its page table into dst (unmapped pages read as zeros).
// 0: va is not in a virtual pool (the caller reads it directly).
int totk_vpool_gather(uintptr_t va, uint64_t len, void *dst) {
    int found = 0;
    pthread_mutex_lock(&g_map_mu);
    for (vtable *t = g_tables; t; t = t->next) {
        uintptr_t base = (uintptr_t)t->pool->mem;
        if (va < base || va >= base + t->pool->size) continue;
        found = 1;
        for (uint64_t done = 0; done < len;) {
            uint64_t at = va + done - base, page = at / TOTK_VPOOL_PAGE, in = at % TOTK_VPOOL_PAGE;
            uint64_t n = TOTK_VPOOL_PAGE - in < len - done ? TOTK_VPOOL_PAGE - in : len - done;
            const vpage *p = page < t->npages ? &t->pages[page] : NULL;
            if (p && p->phys) memcpy((uint8_t *)dst + done, (const uint8_t *)p->phys->mem + p->off + in, n);
            else memset((uint8_t *)dst + done, 0, n);
            done += n;
        }
        break;
    }
    pthread_mutex_unlock(&g_map_mu);
    return found;
}

// Whether the physical memory behind [va, va+len) of a virtual pool was written since `stamp` (nvn_dirty generations).
// The virtual range itself is never written (no CPU access): the game writes the physical pools' memory, so a texture
// in a virtual pool is checked page by page there (TOTK's pre-rendered cutscenes: the movie frame, every frame).
// Untracked backing (CPU_NO_ACCESS pools: GPU data) counts as unchanged. -1: not in a virtual pool.
int totk_vpool_dirty_since(uintptr_t va, uint64_t len, uint64_t stamp) {
    int found = -1;
    pthread_mutex_lock(&g_map_mu);
    for (vtable *t = g_tables; t; t = t->next) {
        uintptr_t base = (uintptr_t)t->pool->mem;
        if (va < base || va >= base + t->pool->size) continue;
        found = 0;
        for (uint64_t done = 0; done < len && !found;) {
            uint64_t at = va + done - base, page = at / TOTK_VPOOL_PAGE, in = at % TOTK_VPOOL_PAGE;
            uint64_t n = TOTK_VPOOL_PAGE - in < len - done ? TOTK_VPOOL_PAGE - in : len - done;
            const vpage *p = page < t->npages ? &t->pages[page] : NULL;
            if (p && p->phys) {
                const uint8_t *cpu = (const uint8_t *)p->phys->mem + p->off + in;
                found = nvn_dirty_tracked(cpu, n) && nvn_dirty_since(cpu, n, stamp);
            }
            done += n;
        }
        break;
    }
    pthread_mutex_unlock(&g_map_mu);
    return found;
}

// The CPU address of the physical backing of [va, va+len) in a virtual pool (1 = mapped and contiguous).
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) {
    int ok = 0;
    pthread_mutex_lock(&g_map_mu);
    for (vtable *t = g_tables; t; t = t->next) {
        uintptr_t base = (uintptr_t)t->pool->mem;
        if (va < base || va + len > base + t->pool->size) continue;
        uint64_t page = (va - base) / TOTK_VPOOL_PAGE, last = (va + len - 1 - base) / TOTK_VPOOL_PAGE;
        const vpage *p = &t->pages[page];
        if (!p->phys) break;
        ok = 1;
        for (uint64_t k = page + 1; k <= last && ok; k++)
            ok = t->pages[k].phys == p->phys && t->pages[k].off == p->off + (k - page) * TOTK_VPOOL_PAGE;
        if (ok) *cpu = (uintptr_t)p->phys->mem + p->off + (va - base) % TOTK_VPOOL_PAGE;
        break;
    }
    pthread_mutex_unlock(&g_map_mu);
    return ok;
}
