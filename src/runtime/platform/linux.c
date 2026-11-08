// Linux host layer (runtime/platform.h). x86-64 and aarch64.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
// The write-watch ABI of Linux 6.7 (uapi linux/fs.h and linux/userfaultfd.h) for older headers: the game is built
// against the Steam Runtime 3 sysroot's 5.10 headers and runs on newer kernels (SteamOS 3.8+). A kernel without it
// refuses the calls at run time and the write watch is off.
#ifndef PAGEMAP_SCAN
struct page_region { __u64 start, end, categories; };
struct pm_scan_arg {
    __u64 size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask, category_anyof_mask,
        return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

// ---- guest address space
static void watch_init(uint64_t lo, uint64_t hi);
static void watch_register(uint64_t lo, uint64_t hi);
static void fz_reset(void);
static void fz_thaw_range(uint64_t lo, uint64_t hi, int hold);
static void fz_never(uint64_t lo, uint64_t hi);
static void fz_init(void);
static int fz_fault(uint64_t fa);

// Everything readable/writable from the start, like macOS: the guest maps at 4K granularity, and untouched pages
// cost nothing (MAP_NORESERVE: the 12 GB window doesn't count against overcommit).
bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    if (lo >= 0x10000) {
        mmap((void *)(uintptr_t)(lo - 0x10000), 0x10000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    }
    void *p = mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != (void *)(uintptr_t)lo) { munmap(p, hi - lo); return false; }  // (kernels before 4.17: just a hint)
    watch_init(lo, hi);
    return true;
}

// Fresh zero pages over the whole window (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    if (mmap((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
             -1, 0) != (void *)(uintptr_t)lo) return false;
    fz_reset();               // the fresh mapping is writable everywhere
    watch_register(lo, hi);   // the new mapping is not registered with userfaultfd yet
    return true;
}

// The whole window is backed from the start; before the kernel itself writes guest memory (file reads into a game
// buffer, host/fs.c) the frozen regions there become writable again and stay so for a while (a write by the kernel
// into a read-only page fails with EFAULT instead of faulting).
void plat_commit(uint64_t addr, uint64_t size) { if (size) fz_thaw_range(addr, addr + size, 1); }
void plat_memory_map(void) {}

// A file mapped read-only, its first len bytes (0: all of it); *size: the bytes mapped. NULL: not there. (The graphics
// code reads the shader pack and the BC7 cache this way; mmap is not reachable from there.)
const void *plat_map_file(const char *path, uint64_t len, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (!len) len = fstat(fd, &st) ? 0 : (uint64_t)st.st_size;
    void *p = len ? mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);   // (the mapping stays)
    if (p == MAP_FAILED) return NULL;
    *size = len;
    return p;
}
void plat_unmap_file(const void *p, uint64_t size) { munmap((void *)p, size); }

// Write watch: what MEM_WRITE_WATCH / GetWriteWatch are on Windows, done as Wine does it. The window is registered with
// userfaultfd for asynchronous write protection (the kernel lifts a page's protection on its first write, without
// stopping anyone), and PAGEMAP_SCAN reports the pages written since their last scan and protects them again in the
// same call. The kernel's own writes (file reads into game memory) count too; GPU writes do not, as on Windows.
// On by default (Linux 6.7): the memory pools are copies in Vulkan memory
// on Linux (gfx/backend_vk.c, "pool copies"), so nothing imports game memory. Imported (TOTK_POOL_IMPORT=1,
// development) it cannot work: amdgpu re-maps imported memory for writing after every protection change, so every
// page reads as written on every scan (4 fps in the title menu); then it is off, nvn_dirty treats pools as untracked
// and vkb_tex.c compares texture contents instead. TOTK_WRITE_WATCH=0 turns it off.
static int g_uffd = -1, g_pagemap = -1;
static uint64_t g_watch_lo, g_watch_hi;

static void watch_register(uint64_t lo, uint64_t hi) {
    if (g_uffd < 0 || lo < g_watch_lo || hi > g_watch_hi) return;
    // transparent huge pages would populate (and count as written) 2 MiB at the first write of one page
    madvise((void *)(uintptr_t)lo, hi - lo, MADV_NOHUGEPAGE);
    struct uffdio_register reg = {.range = {.start = lo, .len = hi - lo}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ioctl(g_uffd, UFFDIO_REGISTER, &reg)) {
        fprintf(stderr, "[totk] write watch: registering %llx-%llx failed (%s); writes there are not tracked\n",
                (unsigned long long)lo, (unsigned long long)hi, strerror(errno));
        g_watch_hi = g_watch_lo;   // plat_written_pages: -1 from now on
    }
}

static void watch_init(uint64_t lo, uint64_t hi) {
    const char *e = getenv("TOTK_WRITE_WATCH"), *imp = getenv("TOTK_POOL_IMPORT");
    if (e && !strcmp(e, "0")) return;
    if (imp && !strcmp(imp, "1") && !(e && !strcmp(e, "1"))) return;
    // UFFD_USER_MODE_ONLY: allowed without privileges; asynchronous write faults never reach the userfaultfd anyway.
    // WP_UNPOPULATED: pages not touched yet are protected too, so their first write is seen like any other.
    int uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    int pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (uffd < 0 || pagemap < 0 || ioctl(uffd, UFFDIO_API, &api)) {
        fprintf(stderr, "[totk] write watch: not available (%s); CPU-written textures keep their first contents\n",
                strerror(errno));
        if (uffd >= 0) close(uffd);
        if (pagemap >= 0) close(pagemap);
        return;
    }
    g_uffd = uffd;
    g_pagemap = pagemap;
    g_watch_lo = lo;
    g_watch_hi = hi;
    fz_init();
    watch_register(lo, hi);
}

// The written pages of [at, end) into pages[n..max) (resetting them when wp); -1: the scan failed. *all: the whole
// range was looked at (not stopped by max).
static int scan_range(uint64_t at, uint64_t end, uint64_t *pages, int n, int max, int wp, int *all) {
    struct page_region r[64];
    *all = 0;
    while (at < end && n < max) {
        // max_pages: the pages past it stay unreported and unprotected, so the next call reports them
        // Written: protection lifted, on a populated page (present or swapped out) that is not the shared zero page.
        // A page never touched counts as written to the kernel and a read maps the zero page there: neither is a write,
        // as on Windows.
        struct pm_scan_arg a = {.size = sizeof a, .flags = (wp ? PM_SCAN_WP_MATCHING : 0) | PM_SCAN_CHECK_WPASYNC, .start = at,
                                .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 64, .max_pages = (uint64_t)(max - n),
                                .category_inverted = PAGE_IS_PFNZERO, .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                                .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED, .return_mask = PAGE_IS_WRITTEN};
        long k = ioctl(g_pagemap, PAGEMAP_SCAN, &a);
        if (k < 0) return -1;
        for (long i = 0; i < k; i++)
            for (uint64_t p = r[i].start; p < r[i].end && n < max; p += 4096) pages[n++] = p;
        if (a.walk_end <= at) break;
        at = a.walk_end;
    }
    *all = at >= end || n < max;
    return n;
}

/* Frozen regions (UltraTears, 2026-11-04). Every submission scans the pools it uses (about 1.2 GiB on the Steam Deck),
   and most of it - meshes, textures - is never written after loading: the scans took 1.8 ms of the translation thread
   per frame right before the submit (the Deck's log of 2026-11-04). A 2 MiB region of the window whose scans found
   nothing written FZ_IDLE times in a row is made read-only and not scanned any more. Its pages keep their write
   protection from the last scan, so nothing is lost: the first write faults (crash_handler -> fz_fault), the region
   becomes writable and thawed again, the write lifts the page's protection as before, and the next scan reports it.
   Before freezing, the whole region is checked for writes not yet reported (another pool's part of it). A region that
   thaws again and again waits longer each time (the threshold doubles, up to FZ_MAX). The kernel's own writes would not
   fault but fail (EFAULT), so plat_commit (called before file reads into guest memory) thaws and holds regions for
   2 s; a region the game changes the protection of (plat_protect) is never frozen. TOTK_WATCH_FREEZE=1: on (off by default, see fz_init). */
enum { FZ_SHIFT = 21, FZ_IDLE = 8, FZ_MAX = 240, FZ_FREEZING = 254, FZ_FROZEN = 255 };
static _Atomic uint8_t *g_fz;          // per region: scans in a row with nothing written, or FZ_FREEZING / FZ_FROZEN
static _Atomic uint8_t *g_fz_strikes;  // per region: thaws by a write (255: never freeze)
static _Atomic uint64_t *g_fz_hold;    // per region: no freezing before this time (ns)
static uint64_t g_fz_n;
static int g_fz_on;
static _Atomic uint64_t g_fz_frozen_now, g_fz_thaws, g_fz_skipped;

static void fz_init(void) {
    // [temporary] off by default (=1: on): on the Steam Deck (2026-11-04) the main thread's write into a thawed region
    // faulted again and again (signal 11 at the title menu, 584 MiB frozen) - not understood yet
    const char *e = getenv("TOTK_WATCH_FREEZE");
    if (!(e && !strcmp(e, "1"))) return;
    g_fz_n = (g_watch_hi - g_watch_lo + (1ull << FZ_SHIFT) - 1) >> FZ_SHIFT;
    g_fz = calloc(g_fz_n, 1);
    g_fz_strikes = calloc(g_fz_n, 1);
    g_fz_hold = calloc(g_fz_n, sizeof *g_fz_hold);
    g_fz_on = g_fz && g_fz_strikes && g_fz_hold && !(g_watch_lo & ((1ull << FZ_SHIFT) - 1));
}
static void fz_reset(void) {
    if (!g_fz_on) return;
    for (uint64_t i = 0; i < g_fz_n; i++) atomic_store(&g_fz[i], 0);
    atomic_store(&g_fz_frozen_now, 0);
}
static uint64_t fz_lo(uint64_t i) { return g_watch_lo + (i << FZ_SHIFT); }
static uint64_t fz_hi(uint64_t i) { uint64_t h = fz_lo(i) + (1ull << FZ_SHIFT); return h < g_watch_hi ? h : g_watch_hi; }
// Region i writable again (from a fault or before a host write); 1 if it was frozen or freezing.
static int fz_thaw(uint64_t i) {
    uint8_t s = atomic_load(&g_fz[i]);
    if (s < FZ_FREEZING) return 0;
    if (!atomic_compare_exchange_strong(&g_fz[i], &s, 0)) return 1;   // another thread thawed it
    mprotect((void *)(uintptr_t)fz_lo(i), fz_hi(i) - fz_lo(i), PROT_READ | PROT_WRITE);
    if (s == FZ_FROZEN) atomic_fetch_sub(&g_fz_frozen_now, 1);
    return 1;
}
static volatile int g_fz_diag_rc, g_fz_diag_b;   // fz_report: the last unfrozen fault's mprotect errno, how many
// SIGSEGV at fa (crash_handler, any thread): a write into a frozen region. Async-signal-safe (atomics, mprotect).
static int fz_fault(uint64_t fa) {
    if (!g_fz_on || fa < g_watch_lo || fa >= g_watch_hi) return 0;
    uint64_t i = (fa - g_watch_lo) >> FZ_SHIFT;
    if (!fz_thaw(i)) {
        // Not frozen: a freeze that a thaw overtook may have left it read-only for a moment (fz_idle puts that right
        // too). Writable again, once per address in a row (a fault that writability does not cure is a real one), and
        // never where the game set the protection itself.
        static __thread uint64_t last;
        if (atomic_load(&g_fz_strikes[i]) == 255 || last == fa) { last = 0; return 0; }
        last = fa;
        g_fz_diag_rc = mprotect((void *)(uintptr_t)fz_lo(i), fz_hi(i) - fz_lo(i), PROT_READ | PROT_WRITE) ? errno : 0;
        g_fz_diag_b++;
        return 1;
    }
    uint8_t k = atomic_load(&g_fz_strikes[i]);
    if (k < 254) atomic_store(&g_fz_strikes[i], k + 1);
    atomic_fetch_add(&g_fz_thaws, 1);
    return 1;
}
// A fault in the guest window that fz_fault did not cure (crash_handler, the process ends after it): what the frozen
// regions knew of it and the kernel's mapping there, for the log (the Steam Deck's signal 11 of 2026-11-04).
#include <sys/utsname.h>
static void fz_report(uint64_t fa, int code, long err) {
    if (!g_fz_on || fa < g_watch_lo || fa >= g_watch_hi) return;
    uint64_t i = (fa - g_watch_lo) >> FZ_SHIFT;
    struct utsname u;
    if (uname(&u)) u.release[0] = 0;
    fprintf(stderr, "[fz] fault %#llx (si_code %d, error %#lx): region %llu state %u strikes %u hold %s; unfrozen faults %d, "
                    "last mprotect errno %d; frozen %llu MiB, thaws %llu; kernel %s\n",
            (unsigned long long)fa, code, err, (unsigned long long)i, (unsigned)atomic_load(&g_fz[i]),
            (unsigned)atomic_load(&g_fz_strikes[i]), atomic_load(&g_fz_hold[i]) > plat_now_ns() ? "on" : "off", g_fz_diag_b,
            g_fz_diag_rc, (unsigned long long)atomic_load(&g_fz_frozen_now) * 2, (unsigned long long)atomic_load(&g_fz_thaws),
            u.release);
    FILE *m = fopen("/proc/self/maps", "r");   // the mappings around it
    char line[512];
    while (m && fgets(line, sizeof line, m)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && hi > fz_lo(i) - (4ull << 20) && lo < fz_hi(i) + (4ull << 20))
            fprintf(stderr, "[fz]   %s", line);
    }
    if (m) fclose(m);
}
static void fz_thaw_range(uint64_t lo, uint64_t hi, int hold) {
    if (!g_fz_on || hi <= g_watch_lo || lo >= g_watch_hi) return;
    if (lo < g_watch_lo) lo = g_watch_lo;
    if (hi > g_watch_hi) hi = g_watch_hi;
    uint64_t until = hold ? plat_now_ns() + 2000000000ull : 0;
    for (uint64_t i = (lo - g_watch_lo) >> FZ_SHIFT; i <= (hi - 1 - g_watch_lo) >> FZ_SHIFT; i++) {
        if (hold && atomic_load(&g_fz_hold[i]) < until) atomic_store(&g_fz_hold[i], until);
        fz_thaw(i);
    }
}
static void fz_never(uint64_t lo, uint64_t hi) {
    if (!g_fz_on || hi <= g_watch_lo || lo >= g_watch_hi) return;
    if (lo < g_watch_lo) lo = g_watch_lo;
    if (hi > g_watch_hi) hi = g_watch_hi;
    for (uint64_t i = (lo - g_watch_lo) >> FZ_SHIFT; i <= (hi - 1 - g_watch_lo) >> FZ_SHIFT; i++) atomic_store(&g_fz_strikes[i], 255);
    fz_thaw_range(lo, hi, 0);
}
// Region i was scanned in part with nothing written: one more idle scan; at the threshold, frozen.
static void fz_idle(uint64_t i) {
    uint8_t s = atomic_load(&g_fz[i]), k = atomic_load(&g_fz_strikes[i]);
    if (s >= FZ_FREEZING || k == 255) return;
    unsigned need = k >= 5 ? FZ_MAX : (unsigned)FZ_IDLE << k;
    if (need > FZ_MAX) need = FZ_MAX;
    if (s + 1u < need) { atomic_compare_exchange_strong(&g_fz[i], &s, s + 1); return; }
    if (atomic_load(&g_fz_hold[i]) > plat_now_ns()) return;
    if (!atomic_compare_exchange_strong(&g_fz[i], &s, FZ_FREEZING)) return;   // one thread freezes it
    uint64_t lo = fz_lo(i), hi = fz_hi(i);
    if (mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ)) { uint8_t f = FZ_FREEZING; atomic_compare_exchange_strong(&g_fz[i], &f, 0); return; }
    if (atomic_load(&g_fz[i]) != FZ_FREEZING) {   // thawed meanwhile (a host write coming): its protection after ours undone
        mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE);
        return;
    }
    // writes before the protection that no scan has reported yet (the region's other pools): not frozen
    uint64_t one;
    int all, w = scan_range(lo, hi, &one, 0, 1, 0, &all);
    uint8_t f = FZ_FREEZING;
    if (w != 0) {
        if (atomic_compare_exchange_strong(&g_fz[i], &f, 0)) mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE);
        return;
    }
    if (atomic_compare_exchange_strong(&g_fz[i], &f, FZ_FROZEN)) atomic_fetch_add(&g_fz_frozen_now, 1);
}
void plat_watch_freeze_stats(uint64_t *frozen_mib, uint64_t *thaws, uint64_t *skipped_mib) {
    *frozen_mib = atomic_load(&g_fz_frozen_now) * 2;
    *thaws = atomic_load(&g_fz_thaws);
    *skipped_mib = atomic_load(&g_fz_skipped) >> 20;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (g_pagemap < 0 || addr < g_watch_lo || addr + size > g_watch_hi || max <= 0) return -1;
    *page = 4096;
    uint64_t at = addr, end = addr + size;
    int n = 0, all;
    if (!g_fz_on) return scan_range(at, end, pages, 0, max, 1, &all);
    while (at < end && n < max) {   // frozen regions are skipped; each run of the others is one scan
        uint64_t i = (at - g_watch_lo) >> FZ_SHIFT, j = i, e;
        if (atomic_load(&g_fz[i]) == FZ_FROZEN) {
            e = fz_hi(i) < end ? fz_hi(i) : end;
            atomic_fetch_add(&g_fz_skipped, e - at);
            at = e;
            continue;
        }
        while (fz_hi(j) < end && atomic_load(&g_fz[j + 1]) != FZ_FROZEN) j++;
        e = fz_hi(j) < end ? fz_hi(j) : end;
        int k = scan_range(at, e, pages, n, max, 1, &all);
        if (k < 0) return -1;
        // the run's regions: written ones start counting again, the others had one more idle scan (only where the scan
        // looked at all of it: past max the rest is unknown)
        int p = n;
        for (uint64_t r = i; r <= j; r++) {
            uint64_t rh = fz_hi(r);
            int written = 0;
            while (p < k && pages[p] < rh) written = 1, p++;
            if (!all && (p == k || rh > pages[k - 1])) break;
            if (written) { uint8_t s = atomic_load(&g_fz[r]); if (s < FZ_FREEZING) atomic_compare_exchange_strong(&g_fz[r], &s, 0); }
            else fz_idle(r);
        }
        n = k;
        if (!all) break;
        at = e;
    }
    return n;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    fz_never(addr, addr + size);   // the game's own protection: never frozen (a thaw would make it writable)
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000u), .tv_nsec = (long)(t % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// Real-time scheduling where the user may have it (rtkit / limits.conf), else just a higher priority: keeps the
// vsync thread on time while the guest threads load every core.
void plat_thread_boost(void) {}   // [temporary] Linux / Steam Deck: nice / SCHED_* for the replay thread - with the Deck port
void plat_thread_background(void) { setpriority(PRIO_PROCESS, (id_t)gettid(), 10); }   // (per thread on Linux)
void plat_thread_nice(int n) { if (n > 0) setpriority(PRIO_PROCESS, (id_t)gettid(), n > 19 ? 19 : n); }
// (a page the game protected - SetMemoryPermission - fails the copy instead of faulting)
#include <sys/uio.h>
int plat_safe_read(void *dst, const void *src, size_t n) {
    struct iovec l = {dst, n}, r = {(void *)src, n};
    return process_vm_readv(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n;
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    struct sched_param sp = {.sched_priority = 1};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) return;
    setpriority(PRIO_PROCESS, (id_t)gettid(), -10);  // (per thread on Linux; fails quietly without CAP_SYS_NICE)
}

// ---- process
// As on Windows (platform/windows.c): stdout / stderr go to ultratears.log next to the game, the last run's kept as
// ultratears-previous.log, even when stderr is a pipe: Steam starts a game with its output in a pipe (Game Mode on the
// Deck: no log at all while a pipe meant "already captured", 2026-11-01). A terminal keeps the lines too: the log is the
// player's to send, wherever the game was started from (the launcher, Steam, a terminal). TOTK_LOG_FILE=0: the log
// stays where stderr goes ("| tee x.log" in development); a path: that file. Processes started by this one append to
// the same log.
static char g_log_path[1024];

static void log_to_file(void) {
    const char *force = getenv("TOTK_LOG_FILE");
    if (force && !*force) force = NULL;
    if (force && !strcmp(force, "0")) return;
    bool tty = isatty(STDERR_FILENO);
    char dir[900];
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n <= 0) return;
    dir[n] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    // linux/ultratears beside the data (a build on a PC or with the AppImage; host/boot.c): the game folder above, where
    // ultratears.cfg is and where the log is on Windows
    struct stat cfg;
    char up[1100];
    if ((slash = strrchr(dir, '/')) && !strcmp(slash + 1, "linux")) {
        snprintf(up, sizeof up, "%.*s/ultratears.cfg", (int)(slash - dir), dir);
        if (stat(up, &cfg) == 0) *slash = 0;
    }
    if (force && strcmp(force, "1") != 0) snprintf(g_log_path, sizeof g_log_path, "%s", force);
    else snprintf(g_log_path, sizeof g_log_path, "%s/ultratears.log", dir);
    bool append = getenv("TOTK_LOG_APPEND") != NULL;
    if (!append && (!force || !strcmp(force, "1"))) {
        char prev[1024];
        snprintf(prev, sizeof prev, "%s/ultratears-previous.log", dir);
        rename(g_log_path, prev);
    }
    if (tty) fprintf(stderr, "UltraTears: the log is written to %s\n", g_log_path);
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("TOTK_LOG_APPEND", "1", 1);
}

#if defined(__x86_64__)
// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    fprintf(stderr, "FATAL: UltraTears needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or "
                    "newer).\n");
    exit(1);
}
#else
static void require_cpu(void) {}
#endif

void plat_debug_console(int debug) { (void)debug; }   // a terminal, if any, is the user's: nothing to open or hide

void plat_init(void) {
    log_to_file();
    require_cpu();
    // The timer slack (how late the kernel may wake a sleeping thread to batch wakeups, 50 us by default) down to 1 us:
    // the frame pacing sleeps to a deadline (plat_sleep_until_ns), as Windows' 1 ms timer resolution does. Threads
    // started afterwards inherit it.
    prctl(PR_SET_TIMERSLACK, 1000ul, 0, 0, 0);
}

// (inside an AppImage: the binary in its mount, which stays mounted while this process lives - re-exec included)
bool plat_exe_path(char *buf, size_t size) {
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
}

// Our own command line, from /proc/self/cmdline (NUL-separated).
static char **self_argv(void) {
    static char buf[32768];
    static char *argv[256];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return NULL;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return NULL;
    buf[n] = 0;
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) argv[k++] = p;
    argv[k] = NULL;
    return argv;
}

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    char **av = self_argv();
    if (av && plat_exe_path(path, sizeof path)) execv(path, av);
}

// ---- diagnostics
// Exported symbols only (the Makefile links with -rdynamic); frames in the generated code are left to the caller,
// which maps them to guest functions (runtime/dispatch.c). Static runtime functions come out as the nearest guest
// function, so backtrace lines also give the offset in the executable (addr2line -fe ultratears <offset>).
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: say so in a message box rather than vanishing (the details are in the log).
void plat_fatal_notice(void) {
    const char *headless = getenv("TOTK_HEADLESS");
    if (headless && *headless && strcmp(headless, "0") != 0) {
        _exit(1);
    }
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200];
    snprintf(text, sizeof text, "UltraTears stopped because of an error.\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UltraTears", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (UltraTears+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
#if defined(__aarch64__)
    uint64_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
#else
    // x86-64 has no link register: report the return address on top of the stack (valid at function entry)
    uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP], sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP], lr = 0;
    if (sig != SIGSEGV || (fa & ~0xFFFull) != (sp & ~0xFFFull)) lr = *(uint64_t *)(uintptr_t)sp;
#endif
    if (sig == SIGSEGV && fz_fault(fa)) return;   // a write into a frozen region of the write watch
#if defined(__x86_64__)
    if (sig == SIGSEGV) fz_report(fa, si->si_code, (long)uc->uc_mcontext.gregs[REG_ERR]);
#else
    if (sig == SIGSEGV) fz_report(fa, si->si_code, 0);
#endif
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
    const char *headless = getenv("TOTK_HEADLESS");
    if (headless && *headless && strcmp(headless, "0") != 0) {
        _exit(1);
    }
}

// A signal stack for the calling thread (a signal stack is per thread), so the fault and crash handlers still run when
// its own stack is exhausted. Never freed: threads that end are few, and a stack freed while a handler might run on it
// would be worse.
void plat_thread_start(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts, e.g. a compiler's own child)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown: as on Windows, the GPU driver's threads crashed in exit()
// while the game's threads still submitted work.
void plat_terminate(void) { _exit(0); }
