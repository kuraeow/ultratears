#ifdef _WIN32
// Sampling CPU profiler for development (TOTK_PROF=<start s>:<seconds>[:<report path>]). A host thread wakes about
// 1000 times a second, suspends every other thread of the process in turn, reads its instruction pointer and resumes
// it. At the end the samples are mapped to functions through the PDB (dbghelp) and a report is written:
//   - self samples per function (recompiled game functions f_<guest address> also get the guest symbol, if any);
//   - per thread: busy share and its top functions.
// A thread that is blocked in the kernel (waits, sleeps) reports an ntdll address: those count as waiting.
// Suspending threads costs a few microseconds each; at 1 kHz it slows the game by about a percent.
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define MAX_THREADS 128
#define TABLE (1u << 20)   // open addressing: rip -> count (per thread)

typedef struct { uint64_t rip; uint32_t count; uint16_t thread; } Slot;
static Slot *table;
static uint64_t samples_total, samples_lost;
static DWORD tids[MAX_THREADS];
static uint64_t thread_samples[MAX_THREADS], thread_wait[MAX_THREADS];
static unsigned nthreads;
static HMODULE ntdll, win32u;
static uint64_t ntdll_lo, ntdll_hi, win32u_lo, win32u_hi, exe_lo, exe_hi;
#define VIA (1ull << 63)   // a table key with this bit: the game executable's function that called into a DLL
#define WAIT (1ull << 62)  // ... or into a kernel wait
#define WAIT4 (1ull << 61) // ... the fourth such function up the stack of a wait
#define FLAGS (VIA | WAIT | WAIT4)

static void module_range(HMODULE m, uint64_t *lo, uint64_t *hi) {
    if (!m) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)m + ((IMAGE_DOS_HEADER *)m)->e_lfanew);
    *lo = (uint64_t)(uintptr_t)m;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
}

static unsigned thread_index(DWORD tid) {
    for (unsigned i = 0; i < nthreads; i++) if (tids[i] == tid) return i;
    if (nthreads == MAX_THREADS) return MAX_THREADS;
    tids[nthreads] = tid;
    return nthreads++;
}

static void add(uint64_t rip, unsigned t) {
    uint64_t h = (rip * 0x9E3779B97F4A7C15ull ^ t) >> 44;
    for (unsigned k = 0; k < TABLE; k++) {
        Slot *s = &table[(h + k) & (TABLE - 1)];
        if (s->count && s->rip == rip && s->thread == t) { s->count++; return; }
        if (!s->count) { *s = (Slot){rip, 1, (uint16_t)t}; return; }
    }
    samples_lost++;
}

// The process's threads, re-listed every 500 ms (a thread snapshot costs milliseconds: listing them on every pass held
// the sampler to ~20 passes a second). Handles stay open between passes.
static HANDLE handles[MAX_THREADS];
static unsigned slot_of[MAX_THREADS], nhandles;
static void list_threads(DWORD self) {
    for (unsigned i = 0; i < nhandles; i++) CloseHandle(handles[i]);
    nhandles = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {sizeof te};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok && nhandles < MAX_THREADS; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        unsigned t = thread_index(te.th32ThreadID);
        if (t >= MAX_THREADS) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        handles[nhandles] = h;
        slot_of[nhandles++] = t;
    }
    CloseHandle(snap);
}

static void sample_all(void) {
    for (unsigned i = 0; i < nhandles; i++) {
        HANDLE h = handles[i];
        if (SuspendThread(h) == (DWORD)-1) continue;   // exited
        CONTEXT ctx = {.ContextFlags = CONTEXT_FULL};
        if (GetThreadContext(h, &ctx)) {
            unsigned t = slot_of[i];
            uint64_t rip = ctx.Rip;
            thread_samples[t]++;
            int waiting = (rip >= ntdll_lo && rip < ntdll_hi) || (rip >= win32u_lo && rip < win32u_hi);
            if (waiting) thread_wait[t]++;
            else add(rip, t);
            // outside the game executable (a DLL: the GPU driver, the C runtime; or a wait in the kernel): the game
            // executable's functions on the stack, by unwinding - the first (VIA) and, for waits, the fourth (WAIT4:
            // past the pthread wrappers, the code that waits)
            int outside = !(rip >= exe_lo && rip < exe_hi), seen = 0;
            for (int d = 0; outside && d < 48 && rip; d++) {
                DWORD64 base;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &base, NULL);
                if (fe) {
                    void *hd; DWORD64 fr;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, rip, fe, &ctx, &hd, &fr, NULL);
                } else {   // a leaf function: the return address is on top of the stack
                    ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
                    ctx.Rsp += 8;
                }
                rip = ctx.Rip;
                if (!(rip >= exe_lo && rip < exe_hi)) continue;
                seen++;
                if (seen == 1) add(rip | VIA | (waiting ? WAIT : 0), t);
                if (!waiting || seen == 4) { if (waiting) add(rip | VIA | WAIT | WAIT4, t); break; }
            }
            samples_total++;
        }
        ResumeThread(h);
    }
}

typedef struct { uint64_t fn; uint32_t count; uint16_t thread; char *name; } Fn;
static int by_fn(const void *a, const void *b) {
    const Fn *x = a, *y = b;
    return x->fn < y->fn ? -1 : x->fn > y->fn ? 1 : 0;
}
static int by_thread_fn(const void *a, const void *b) {
    const Fn *x = a, *y = b;
    if (x->thread != y->thread) return x->thread < y->thread ? -1 : 1;
    return by_fn(a, b);
}
static int by_count(const void *a, const void *b) {
    uint64_t x = ((const Fn *)a)->count, y = ((const Fn *)b)->count;
    return x < y ? 1 : x > y ? -1 : 0;
}
// Adds up runs of equal function (and thread, when per_thread) in a sorted array, in place; returns the new length.
static unsigned fold(Fn *e, unsigned n, int per_thread) {
    unsigned out = 0;
    for (unsigned i = 0; i < n; i++) {
        if (out && e[out - 1].fn == e[i].fn && (!per_thread || e[out - 1].thread == e[i].thread)) e[out - 1].count += e[i].count;
        else e[out++] = e[i];
    }
    return out;
}


// Function start and name of a host address; recompiled functions f_<hex> get the guest symbol as well.
static uint64_t describe(HANDLE proc, uint64_t rip, char *out, size_t n) {
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 off = 0;
    if (!SymFromAddr(proc, rip, &off, si)) {
        HMODULE m = NULL;
        char mod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)(uintptr_t)rip, &m))
            GetModuleFileNameA(m, mod, sizeof mod);
        const char *base = strrchr(mod, '\\');
        snprintf(out, n, "[%s]", base ? base + 1 : mod);
        return (uint64_t)(uintptr_t)m;
    }
    uint64_t guest = 0;
    const char *g = NULL;
    uint64_t goff = 0;
    if (sscanf(si->Name, "f_%llx", (unsigned long long *)&guest) == 1) g = loader_symbol_at(guest, &goff);
    if (g && !goff) snprintf(out, n, "%s  (%s)", si->Name, g);
    else if (g) snprintf(out, n, "%s  (%s+%#llx)", si->Name, g, (unsigned long long)goff);
    else snprintf(out, n, "%s", si->Name);
    HMODULE m = NULL;   // outside the game's executable: the module too (a DLL's nearest export may not be the function)
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &m) &&
        m != GetModuleHandleA(NULL)) {
        char mod[MAX_PATH] = "?";
        GetModuleFileNameA(m, mod, sizeof mod);
        const char *base = strrchr(mod, '\\');
        size_t l = strlen(out);
        snprintf(out + l, n - l, "  [%s]", base ? base + 1 : mod);
    }
    return si->Address;
}


static void sym_init(HANDLE proc) {
    static int done;
    if (done++) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(proc, NULL, TRUE);
}
// For other development tools (watch.c): the function a host address belongs to, as in the profile report.
uint64_t runtime_symbolize(uint64_t rip, char *out, size_t n) {
    HANDLE proc = GetCurrentProcess();
    sym_init(proc);
    return describe(proc, rip, out, n);
}

static void report(const char *path, double seconds) {
    HANDLE proc = GetCurrentProcess();
    sym_init(proc);
    CreateDirectoryA("diag", NULL);   // the default's folder (development output in the game folder)
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[prof] cannot write %s\n", path); return; }
    unsigned n = 0;
    for (unsigned i = 0; i < TABLE; i++) n += table[i].count != 0;
    Fn *e = calloc(n ? n : 1, sizeof *e), *t = calloc(n ? n : 1, sizeof *t);
    if (!e || !t) runtime_fail("profiler report memory");
    // one entry per (rip, thread): its function start; names only for the functions that make the report
    unsigned k = 0;
    for (unsigned i = 0; i < TABLE; i++) {
        Slot *s = &table[i];
        if (!s->count) continue;
        char name[160];
        e[k++] = (Fn){describe(proc, s->rip & ~FLAGS, name, sizeof name) | (s->rip & FLAGS), s->count, s->thread, NULL};
    }
    memcpy(t, e, n * sizeof *e);
    qsort(e, n, sizeof *e, by_fn);
    unsigned nall = fold(e, n, 0);
    qsort(e, nall, sizeof *e, by_count);
    qsort(t, n, sizeof *t, by_thread_fn);
    unsigned nthr = fold(t, n, 1);
    uint64_t busy = 0;
    for (unsigned i = 0; i < nthreads; i++) busy += thread_samples[i] - thread_wait[i];
    fprintf(f, "TOTK CPU profile: %.1f s, %llu samples (%llu running, %llu waiting in the kernel), %u threads, %llu lost\n",
            seconds, (unsigned long long)samples_total, (unsigned long long)busy,
            (unsigned long long)(samples_total - busy), nthreads, (unsigned long long)samples_lost);
    fprintf(f, "One sample per thread per ~1 ms: a thread busy all the time collects ~1000 samples a second.\n\n");
    fprintf(f, "== Running samples by function (all threads) ==\n");
    for (unsigned i = 0, shown = 0; i < nall && shown < 100; i++) {
        if (e[i].fn & FLAGS) continue;
        shown++;
        char name[160];
        describe(proc, e[i].fn, name, sizeof name);
        fprintf(f, "%6.2f%%  %8u  %s\n", 100.0 * e[i].count / (busy ? busy : 1), e[i].count, name);
    }
    fprintf(f, "\n== Threads (busy = share of the thread's samples outside kernel waits) ==\n");
    for (unsigned th = 0; th < nthreads; th++) {
        uint64_t run = thread_samples[th] - thread_wait[th];
        if (run < 50) continue;
        fprintf(f, "thread %5lu: busy %5.1f%% (%llu running samples)\n", tids[th], 100.0 * run / thread_samples[th],
                (unsigned long long)run);
        unsigned a = 0;
        while (a < nthr && t[a].thread != th) a++;
        unsigned b = a;
        while (b < nthr && t[b].thread == th) b++;
        qsort(t + a, b - a, sizeof *t, by_count);
        for (unsigned i = a, shown = 0; i < b && shown < 24; i++) {
            if (t[i].fn & FLAGS) continue;
            shown++;
            char name[160];
            describe(proc, t[i].fn, name, sizeof name);
            fprintf(f, "        %5.1f%%  %s\n", 100.0 * t[i].count / run, name);
        }
        fprintf(f, "      time in DLLs, by the game executable's function that called them:\n");
        for (unsigned i = a, shown = 0; i < b && shown < 16; i++) {
            if ((t[i].fn & FLAGS) != VIA) continue;
            shown++;
            char name[160];
            describe(proc, t[i].fn & ~FLAGS, name, sizeof name);
            fprintf(f, "        %5.1f%%  %s\n", 100.0 * t[i].count / run, name);
        }
        for (int w = 0; w < 2; w++) {
            fprintf(f, "      waits (share of all its samples), by the %s game executable's function on the stack:\n", w ? "fourth" : "first");
            for (unsigned i = a, shown = 0; i < b && shown < 12; i++) {
                if ((t[i].fn & FLAGS) != (w ? FLAGS : (VIA | WAIT))) continue;
                shown++;
                char name[160];
                describe(proc, t[i].fn & ~FLAGS, name, sizeof name);
                fprintf(f, "        %5.1f%%  %s\n", 100.0 * t[i].count / thread_samples[th], name);
            }
        }
    }
    fclose(f);
    free(e);
    free(t);
    fprintf(stderr, "[prof] report written to %s\n", path);
}

static double start_s, duration_s;
static char out_path[512] = "diag/profile.txt";
static volatile int prof_stop, prof_state;   // runtime_profiler_stop; 1 sampling, 2 report written

static DWORD WINAPI profiler(void *unused) {
    (void)unused;
    timeBeginPeriod(1);
    Sleep((DWORD)(start_s * 1000));
    fprintf(stderr, "[prof] sampling for %.0f s\n", duration_s);
    DWORD self = GetCurrentThreadId();
    uint64_t t0 = runtime_now_ns(), end = t0 + (uint64_t)(duration_s * 1e9);
    uint64_t listed = 0, passes = 0;
    prof_state = 1;
    for (uint64_t now; (now = runtime_now_ns()) < end && !prof_stop; passes++) {
        if (now - listed >= 500000000ull) { list_threads(self); listed = now; }
        sample_all();
        Sleep(1);
    }
    double secs = (runtime_now_ns() - t0) / 1e9;
    fprintf(stderr, "[prof] %.0f passes a second\n", passes / secs);
    report(out_path, secs);
    prof_state = 2;
    return 0;
}

void runtime_profiler_start(void) {
    const char *e = getenv("TOTK_PROF");
    if (!e || !*e) return;
    char path[512] = "";
    if (sscanf(e, "%lf:%lf:%511s", &start_s, &duration_s, path) < 2 || start_s < 0 || duration_s <= 0) {
        fprintf(stderr, "[prof] TOTK_PROF=%s: want <start s>:<seconds>[:<report path>]\n", e);
        return;
    }
    if (*path) snprintf(out_path, sizeof out_path, "%s", path);
    table = calloc(TABLE, sizeof *table);
    if (!table) runtime_fail("profiler table");
    ntdll = GetModuleHandleA("ntdll.dll");
    win32u = GetModuleHandleA("win32u.dll");
    module_range(ntdll, &ntdll_lo, &ntdll_hi);
    module_range(win32u, &win32u_lo, &win32u_hi);
    module_range(GetModuleHandleA(NULL), &exe_lo, &exe_hi);
    HANDLE h = CreateThread(NULL, 0, profiler, NULL, 0, NULL);
    if (h) SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
    fprintf(stderr, "[prof] armed: %.0f s from now, for %.0f s, report %s\n", start_s, duration_s, out_path);
}
#else
// Linux (the Steam Deck): the same report from per-thread CPU-time timers. Every thread of the process gets a timer on
// its own CPU clock that sends it SIGPROF each millisecond it runs; the handler counts its instruction pointer. Threads
// made later are picked up every half second. The samples are all running time (a waiting thread spends no CPU), so a
// thread's busy share is its samples against the wall clock. Names come from the executable's symbol table (it keeps
// its symbols) and, for the libraries (the Vulkan driver, libc), from dladdr.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "host.h"

#define MAX_THREADS 256
#define TABLE (1u << 20)   // open addressing: (rip, thread) -> count
typedef struct { _Atomic uint64_t rip; _Atomic uint32_t count; uint16_t thread; } Slot;
static Slot *table;
static _Atomic int tids[MAX_THREADS];
static int timers[MAX_THREADS];   // kernel timer ids (raw syscalls: the build's sysroot has them in librt)
static _Atomic unsigned nthreads;
static _Atomic uint64_t samples_total, samples_lost;
static _Atomic int sampling;

static unsigned thread_of(int tid) {
    unsigned n = atomic_load(&nthreads);
    for (unsigned i = 0; i < n; i++) if (atomic_load(&tids[i]) == tid) return i;
    return MAX_THREADS;
}
static void on_prof(int sig, siginfo_t *si, void *ctx) {
    (void)sig; (void)si;
    if (!atomic_load_explicit(&sampling, memory_order_relaxed)) return;
    uint64_t rip = (uint64_t)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
    unsigned t = thread_of((int)syscall(SYS_gettid));
    atomic_fetch_add_explicit(&samples_total, 1, memory_order_relaxed);
    if (t >= MAX_THREADS) { atomic_fetch_add_explicit(&samples_lost, 1, memory_order_relaxed); return; }
    uint64_t key = rip ^ ((uint64_t)t << 56);
    for (unsigned i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 44), k = 0; k < 64; i = (i + 1) & (TABLE - 1), k++) {
        Slot *s = &table[i];
        uint64_t have = atomic_load_explicit(&s->rip, memory_order_relaxed);
        if (!have) {
            uint64_t zero = 0;
            if (!atomic_compare_exchange_strong(&s->rip, &zero, key) && zero != key) continue;
            s->thread = (uint16_t)t;
        } else if (have != key) continue;
        atomic_fetch_add_explicit(&s->count, 1, memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&samples_lost, 1, memory_order_relaxed);
}
static void arm(int tid) {   // a timer on the thread's CPU clock, SIGPROF to that thread every ms of its running
    unsigned n = atomic_load(&nthreads);
    if (thread_of(tid) < MAX_THREADS || n >= MAX_THREADS) return;
    clockid_t clk = (clockid_t)((~(unsigned)tid << 3) | 6);   // CPUCLOCK_PERTHREAD_MASK | CPUCLOCK_SCHED
    struct sigevent ev = {0};
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = tid;
    if (syscall(SYS_timer_create, clk, &ev, &timers[n])) return;
    atomic_store(&tids[n], tid);
    atomic_store(&nthreads, n + 1);
    struct itimerspec it = {{0, 1000000}, {0, 1000000}};
    syscall(SYS_timer_settime, timers[n], 0, &it, NULL);
}
static void arm_all(void) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    int self = (int)syscall(SYS_gettid);
    for (struct dirent *e; (e = readdir(d));) { int tid = atoi(e->d_name); if (tid > 0 && tid != self) arm(tid); }
    closedir(d);
}

// ---- symbols: the executable's .symtab (functions), sorted by address
// The few ELF64 records read here and dl_iterate_phdr, declared from the ELF and glibc ABIs: <elf.h> and <link.h> are
// not in the Linux kit a player's build compiles against (deps/linux, tools/linux_used.py), which the build of
// 2026-11-04 found the hard way.
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version; uint64_t e_entry, e_phoff, e_shoff;
                 uint32_t e_flags; uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } elf64_ehdr;
typedef struct { uint32_t sh_name, sh_type; uint64_t sh_flags, sh_addr, sh_offset, sh_size; uint32_t sh_link, sh_info;
                 uint64_t sh_addralign, sh_entsize; } elf64_shdr;
typedef struct { uint32_t st_name; unsigned char st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } elf64_sym;
enum { ELF_SHT_SYMTAB = 2, ELF_STT_FUNC = 2 };
struct dl_phdr_info { uint64_t dlpi_addr; const char *dlpi_name; const void *dlpi_phdr; uint16_t dlpi_phnum; };   // its first members
int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *, size_t, void *), void *data);
typedef struct { uint64_t lo, hi; const char *name; } sym_t;
static sym_t *syms;
static size_t nsyms;
static uint64_t exe_base;
static int sym_cmp(const void *a, const void *b) { const sym_t *x = a, *y = b; return x->lo < y->lo ? -1 : x->lo > y->lo; }
static int find_base(struct dl_phdr_info *i, size_t n, void *out) { (void)n; if (!i->dlpi_name || !*i->dlpi_name) { *(uint64_t *)out = i->dlpi_addr; return 1; } return 0; }
static void sym_init(void) {
    static int done;
    if (done++) return;
    dl_iterate_phdr(find_base, &exe_base);
    int fd = open("/proc/self/exe", O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return; }
    const uint8_t *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return;
    const elf64_ehdr *eh = (const elf64_ehdr *)m;
    const elf64_shdr *sh = (const elf64_shdr *)(m + eh->e_shoff);
    for (unsigned i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != ELF_SHT_SYMTAB) continue;
        const elf64_sym *s = (const elf64_sym *)(m + sh[i].sh_offset);
        const char *str = (const char *)(m + sh[sh[i].sh_link].sh_offset);
        size_t n = sh[i].sh_size / sizeof *s;
        if (!(syms = malloc(sizeof *syms * n))) return;
        for (size_t k = 0; k < n; k++)
            if ((s[k].st_info & 0xf) == ELF_STT_FUNC && s[k].st_value)
                syms[nsyms++] = (sym_t){exe_base + s[k].st_value, exe_base + s[k].st_value + (s[k].st_size ? s[k].st_size : 1), str + s[k].st_name};
        qsort(syms, nsyms, sizeof *syms, sym_cmp);
        break;
    }
}
// The function rip is in (its start; the name in out), as the Windows report names it.
static uint64_t describe(uint64_t rip, char *out, size_t n) {
    size_t lo = 0, hi = nsyms;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (syms[mid].lo <= rip) lo = mid + 1; else hi = mid; }
    if (lo && rip < syms[lo - 1].hi) {
        const sym_t *s = &syms[lo - 1];
        uint64_t guest = 0, goff = 0;
        const char *g = sscanf(s->name, "f_%llx", (unsigned long long *)&guest) == 1 ? loader_symbol_at(guest, &goff) : NULL;
        if (g && !goff) snprintf(out, n, "%s  (%s)", s->name, g);
        else if (g) snprintf(out, n, "%s  (%s+%#llx)", s->name, g, (unsigned long long)goff);
        else snprintf(out, n, "%s", s->name);
        return s->lo;
    }
    Dl_info di;
    if (dladdr((void *)(uintptr_t)rip, &di) && di.dli_fname) {
        const char *base = strrchr(di.dli_fname, '/');
        if (di.dli_sname) { snprintf(out, n, "%s  [%s]", di.dli_sname, base ? base + 1 : di.dli_fname); return (uint64_t)(uintptr_t)di.dli_saddr; }
        snprintf(out, n, "[%s]", base ? base + 1 : di.dli_fname);
        return (uint64_t)(uintptr_t)di.dli_fbase;
    }
    snprintf(out, n, "?");
    return 0;
}
uint64_t runtime_symbolize(uint64_t rip, char *out, size_t n) { sym_init(); return describe(rip, out, n); }

typedef struct { uint64_t fn; uint32_t count; uint16_t thread; } Fn;
static int by_thread_fn(const void *a, const void *b) {
    const Fn *x = a, *y = b;
    if (x->thread != y->thread) return x->thread < y->thread ? -1 : 1;
    return x->fn < y->fn ? -1 : x->fn > y->fn;
}
static int by_fn(const void *a, const void *b) { const Fn *x = a, *y = b; return x->fn < y->fn ? -1 : x->fn > y->fn; }
static int by_count(const void *a, const void *b) { const Fn *x = a, *y = b; return x->count > y->count ? -1 : x->count < y->count; }
static unsigned fold(Fn *e, unsigned n, int per_thread) {   // sorted: equal functions (of one thread) merged
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (k && e[k - 1].fn == e[i].fn && (!per_thread || e[k - 1].thread == e[i].thread)) e[k - 1].count += e[i].count;
        else e[k++] = e[i];
    }
    return k;
}
static void report(const char *path, double seconds) {
    sym_init();
    mkdir("diag", 0755);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[prof] cannot write %s\n", path); return; }
    unsigned n = 0, nt = atomic_load(&nthreads);
    for (unsigned i = 0; i < TABLE; i++) n += table[i].count != 0;
    Fn *e = calloc(n ? n : 1, sizeof *e), *t = calloc(n ? n : 1, sizeof *t);
    uint64_t *per = calloc(MAX_THREADS, sizeof *per);
    if (!e || !t || !per) runtime_fail("profiler report memory");
    unsigned k = 0;
    uint64_t all = 0;
    char name[200];
    for (unsigned i = 0; i < TABLE; i++) {
        Slot *s = &table[i];
        if (!s->count) continue;
        uint64_t rip = s->rip & ((1ull << 56) - 1);
        e[k++] = (Fn){describe(rip, name, sizeof name), s->count, s->thread};
        per[s->thread] += s->count; all += s->count;
    }
    memcpy(t, e, n * sizeof *e);
    qsort(e, n, sizeof *e, by_fn);
    unsigned nall = fold(e, n, 0);
    qsort(e, nall, sizeof *e, by_count);
    qsort(t, n, sizeof *t, by_thread_fn);
    unsigned nthr = fold(t, n, 1);
    fprintf(f, "TOTK CPU profile: %.1f s, %llu samples (%llu running, 0 waiting in the kernel), %u threads, %llu lost\n", seconds,
            (unsigned long long)atomic_load(&samples_total), (unsigned long long)all, nt, (unsigned long long)atomic_load(&samples_lost));
    fprintf(f, "Linux: one sample per ms a thread runs (CPU-time timers); busy = its running time against the wall clock.\n\n");
    fprintf(f, "== Running samples by function (all threads) ==\n");
    for (unsigned i = 0; i < nall && i < 100; i++) {
        describe(e[i].fn, name, sizeof name);
        fprintf(f, "%6.2f%%  %8u  %s\n", 100.0 * e[i].count / (all ? all : 1), e[i].count, name);
    }
    fprintf(f, "\n== Threads (busy = share of the thread's samples outside kernel waits) ==\n");
    for (unsigned th = 0; th < nt; th++) {
        uint64_t run = per[th];
        if (run < 50) continue;
        fprintf(f, "thread %5d: busy %5.1f%% (%llu running samples)\n", atomic_load(&tids[th]), 100.0 * run / (seconds * 1000.0),
                (unsigned long long)run);
        unsigned a = 0;
        while (a < nthr && t[a].thread != th) a++;
        unsigned b = a;
        while (b < nthr && t[b].thread == th) b++;
        qsort(t + a, b - a, sizeof *t, by_count);
        for (unsigned i = a; i < b && i < a + 24; i++) {
            describe(t[i].fn, name, sizeof name);
            fprintf(f, "        %5.1f%%  %s\n", 100.0 * t[i].count / run, name);
        }
        fprintf(f, "      time in DLLs, by the game executable's function that called them:\n");   // (not on Linux)
        fprintf(f, "      waits (share of all its samples), by the first game executable's function on the stack:\n");
    }
    fclose(f);
    free(e); free(t); free(per);
    fprintf(stderr, "[prof] report written to %s\n", path);
}

static double start_s, duration_s;
static char out_path[512] = "diag/profile.txt";
static volatile int prof_stop, prof_state;   // runtime_profiler_stop; 1 sampling, 2 report written
static void *profiler(void *unused) {
    (void)unused;
    struct timespec ts = {(time_t)start_s, (long)((start_s - (time_t)start_s) * 1e9)};
    nanosleep(&ts, NULL);
    fprintf(stderr, "[prof] sampling for %.0f s\n", duration_s);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    atomic_store(&sampling, 1);
    prof_state = 1;
    uint64_t t0 = runtime_now_ns(), end = t0 + (uint64_t)(duration_s * 1e9);
    for (int k = 0; runtime_now_ns() < end && !prof_stop; k++) {
        if (k % 10 == 0) arm_all();   // new threads every 0.5 s
        struct timespec step = {0, 50000000};
        nanosleep(&step, NULL);
    }
    atomic_store(&sampling, 0);
    unsigned n = atomic_load(&nthreads);
    for (unsigned i = 0; i < n; i++) syscall(SYS_timer_delete, timers[i]);
    report(out_path, (runtime_now_ns() - t0) / 1e9);
    prof_state = 2;
    return NULL;
}
void runtime_profiler_start(void) {
    const char *e = getenv("TOTK_PROF");
    if (!e || !*e) return;
    char path[512] = "";
    if (sscanf(e, "%lf:%lf:%511s", &start_s, &duration_s, path) < 2 || start_s < 0 || duration_s <= 0) {
        fprintf(stderr, "[prof] TOTK_PROF=%s: want <start s>:<seconds>[:<report path>]\n", e);
        return;
    }
    if (*path) snprintf(out_path, sizeof out_path, "%s", path);
    if (!(table = calloc(TABLE, sizeof *table))) runtime_fail("profiler table");
    pthread_t th;
    if (pthread_create(&th, NULL, profiler, NULL) == 0) pthread_detach(th);
    fprintf(stderr, "[prof] armed: %.0f s from now, for %.0f s, report %s\n", start_s, duration_s, out_path);
}
#endif

// The game is closing (window_sdl.c, before _Exit): a profile still sampling stops and writes what it has, rather than
// nothing (the Steam Deck's run of 2026-11-04 closed 18 s before its window ended). Waits up to 10 s for the report.
void runtime_profiler_stop(void) {
    if (prof_state != 1) return;
    prof_stop = 1;
    fprintf(stderr, "[prof] the game closes: writing the profile so far\n");
    for (int i = 0; i < 1000 && prof_state != 2; i++) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
#endif
    }
}
