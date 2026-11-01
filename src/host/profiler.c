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

static DWORD WINAPI profiler(void *unused) {
    (void)unused;
    timeBeginPeriod(1);
    Sleep((DWORD)(start_s * 1000));
    fprintf(stderr, "[prof] sampling for %.0f s\n", duration_s);
    DWORD self = GetCurrentThreadId();
    uint64_t t0 = runtime_now_ns(), end = t0 + (uint64_t)(duration_s * 1e9);
    uint64_t listed = 0, passes = 0;
    for (uint64_t now; (now = runtime_now_ns()) < end; passes++) {
        if (now - listed >= 500000000ull) { list_threads(self); listed = now; }
        sample_all();
        Sleep(1);
    }
    fprintf(stderr, "[prof] %.0f passes a second\n", passes / duration_s);
    report(out_path, (runtime_now_ns() - t0) / 1e9);
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
#include "host.h"

void runtime_profiler_start(void) {}
#endif
