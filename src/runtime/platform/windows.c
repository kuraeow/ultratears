// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>
#include <malloc.h>

#include "../platform.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return false;
    // Another thread faulted on the same chunk and committed it first: retry the access.
    if (mi.State == MEM_COMMIT) return mi.Protect == PAGE_READWRITE;
    if (mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The window keeps write tracking (MEM_WRITE_WATCH: the Vulkan renderer's vertex / index cache re-copies only pages the
// CPU wrote, runtime/nvn/vulkan/exec.c). Without it (refused): no tracking, the renderer reads guest memory in place.
static bool g_write_watch;

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p == (void *)(uintptr_t)lo) g_write_watch = true;
    else p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    if (!g_write_watch || addr < g_lo || addr + size > g_hi || max <= 0) return -1;
    ULONG_PTR n = (ULONG_PTR)max;
    DWORD gran = 4096;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)addr, (SIZE_T)size, (PVOID *)pages, &n, &gran) != 0) return -1;
    *page = gran;
    // Only the pages returned are reset. With max of them there may be more: the caller asks again from after the last
    // one. (Resetting the whole range here lost every written page past the first max: a mesh streamed in several MB at
    // once kept its old contents in the GPU's copy - the "giant planes", L13 in dev-local/docs/issues.md.)
    return (int)n;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

// PROCESS_MEMORY_COUNTERS_EX and its kernel32 entry (psapi.h is not in the trimmed toolchain)
typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
           QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
} mem_counters;
__declspec(dllimport) BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, mem_counters *counters, DWORD cb);

void plat_memory_map(void) {
    mem_counters pc = {.cb = sizeof pc};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof pc);
    // the heaps' committed regions (HeapWalk region entries), to tell them apart from other private memory
    enum { MAXR = 4096, TOP = 12 };
    static struct { uint64_t lo, hi; } hr[MAXR];
    int nhr = 0;
    uint64_t heap_commit = 0, heap_used = 0;
    // The process heap and the C runtime's (malloc): they live as long as the process. Other heaps (the driver's) may be
    // destroyed between GetProcessHeaps and HeapLock - walking them crashed (2026-10-28); their memory counts as other.
    HANDLE heaps[2] = {GetProcessHeap(), (HANDLE)_get_heap_handle()};
    DWORD nh = heaps[1] && heaps[1] != heaps[0] ? 2 : 1;
    for (DWORD h = 0; h < nh; h++) {
        if (!HeapLock(heaps[h])) continue;
        PROCESS_HEAP_ENTRY e = {0};
        while (HeapWalk(heaps[h], &e)) {
            if (e.wFlags & PROCESS_HEAP_REGION) {
                heap_commit += e.Region.dwCommittedSize;
                if (nhr < MAXR) { hr[nhr].lo = (uint64_t)(uintptr_t)e.lpData; hr[nhr].hi = (uint64_t)(uintptr_t)e.Region.lpLastBlock; nhr++; }
            } else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) heap_used += e.cbData;
        }
        HeapUnlock(heaps[h]);
    }
    uint64_t guest = 0, priv = 0, mapped = 0, image = 0, in_heap = 0;
    struct { uint64_t base, size; } top[TOP] = {{0}};
    static const uint64_t edge[] = {64 << 10, 1 << 20, 4 << 20, 16 << 20, 63 << 20, 65 << 20, UINT64_MAX};   // size classes
    enum { NB = sizeof edge / sizeof edge[0] };
    uint64_t bn[NB] = {0}, bb[NB] = {0};
    for (uint64_t a = 0x10000; a < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t base = (uint64_t)(uintptr_t)mi.BaseAddress, size = mi.RegionSize;
        if (mi.State == MEM_COMMIT) {
            if (base >= g_lo && base < g_hi) guest += size;
            else if (mi.Type == MEM_IMAGE) image += size;
            else if (mi.Type == MEM_MAPPED) mapped += size;
            else {
                int heap = 0;
                for (int i = 0; i < nhr && !heap; i++) heap = base >= hr[i].lo && base < hr[i].hi + 1;
                if (heap) in_heap += size;
                else {
                    priv += size;
                    int c = 0;
                    while (size >= edge[c]) c++;
                    bn[c]++; bb[c] += size;
                    int k = TOP - 1;
                    if (size > top[k].size) {
                        while (k > 0 && size > top[k - 1].size) { top[k] = top[k - 1]; k--; }
                        top[k].base = base; top[k].size = size;
                    }
                }
            }
        }
        a = base + size;
    }
    const double M = 1048576.0;
    static DWORD faults_before;   // page faults since the last map (with a simulated small PC: pages trimmed and back)
    fprintf(stderr, "[mem] commit charge %.0f MiB (peak %.0f), in RAM %.0f MiB (peak %.0f), page faults %lu since the last; "
            "committed: guest window %.0f, heaps %.0f (in use %.0f, %d regions; %.0f found by the walk), other private %.0f, "
            "mapped %.0f, images %.0f MiB\n",
            pc.PrivateUsage / M, pc.PeakPagefileUsage / M, pc.WorkingSetSize / M, pc.PeakWorkingSetSize / M,
            pc.PageFaultCount - faults_before, guest / M, heap_commit / M, heap_used / M, nhr, in_heap / M, priv / M,
            mapped / M, image / M);
    faults_before = pc.PageFaultCount;
    static const char *const cls[NB] = {"<64K", "64K-1M", "1-4M", "4-16M", "16-63M", "~64M", ">65M"};
    char line[512];
    int at = 0;
    for (int c = 0; c < NB; c++)
        if (bn[c]) at += snprintf(line + at, sizeof line - (size_t)at, " %s: %llu = %.0f MiB;", cls[c], (unsigned long long)bn[c], bb[c] / M);
    fprintf(stderr, "[mem]   other private by region size:%s\n", at ? line : " -");
    for (int k = 0; k < TOP && top[k].size; k++)
        fprintf(stderr, "[mem]   private %016llx +%.1f MiB\n", (unsigned long long)top[k].base, top[k].size / M);
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

// A thread every frame waits for (the replay of NVN command lists into Vulkan): above the game's ~50 worker threads,
// which otherwise preempt it on a busy CPU (MacNativeGames measured 6.6 -> 13 ms per frame on macOS; on the Great Sky
// Island the replay ran ~40 ms of a 42 ms frame with ~29 ms of CPU time, 2026-10-24). Not time-critical: Windows and the
// game's audio threads stay above it.
void plat_thread_boost(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

// ---- process
// stdout / stderr go to ultratears.log next to the exe unless they are already captured (a file or a pipe: "> x.log
// 2>&1"), and a fatal error shows a message box (plat_fatal_notice) instead of vanishing. UltraTears.exe is a console
// program: started from a .bat or a double click its log went to the console window, which closes with the game - a
// player whose game crashed had no log to send (2026-10-29). Processes started by this one (the start screen
// re-executing itself for the game) append to the same log.
static char g_log_path[MAX_PATH];

// TOTK_LOG_FILE=0: the log stays on the console (development); =1 or a path: always to that file. Under Wine (Proton)
// always to the file: its console closes with the game, and PROTON_LOG has only Wine's own lines.
// A constructor before all others: the first lines (the version, written first by main; the NVN registry's, from a
// constructor) went to the console while the file got the rest.
__attribute__((constructor(102))) static void log_to_file_without_console(void) {   // (101: boot.c reserves the guest window first)
    static bool done;
    if (done) return;
    done = true;
    const char *force = getenv("TOTK_LOG_FILE");
    if (force && !*force) force = NULL;
    if (force && !strcmp(force, "0")) return;
    if (!force && GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version")) force = "1";
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD type = err && err != INVALID_HANDLE_VALUE ? GetFileType(err) : FILE_TYPE_UNKNOWN;
    if (!force && (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)) return;   // captured by whoever started the game
    bool console = type == FILE_TYPE_CHAR;
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    if (force && strcmp(force, "1") != 0) snprintf(g_log_path, sizeof g_log_path, "%s", force);
    else snprintf(g_log_path, sizeof g_log_path, "%sultratears.log", exe);
    bool append = getenv("TOTK_LOG_APPEND") != NULL;
    if (!append && (!force || !strcmp(force, "1"))) {   // the last run's log stays as ultratears-previous.log: a player restarting after a crash keeps it
        char prev[MAX_PATH];
        snprintf(prev, sizeof prev, "%sultratears-previous.log", exe);
        MoveFileExA(g_log_path, prev, MOVEFILE_REPLACE_EXISTING);
    }
    if (console) fprintf(stderr, "UltraTears: the log is written to %s\n", g_log_path);
    if (!freopen(g_log_path, append ? "ab" : "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv("TOTK_LOG_APPEND=1");
}

// The console window only in Debug mode (the launcher's, debug=1 / --debug: TOTK_STATS; the user's choice 2026-10-31).
// Without it a console the game was given (a double click: UltraTears.exe is a console program) is let go; with it one
// is opened if there is none, and the log file is followed onto it a few times a second. The file stays the log: a
// crash loses nothing that the console had not shown yet.
static DWORD WINAPI follow_log(void *arg) {
    (void)arg;
    HANDLE con = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE log = CreateFileA(g_log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, 0, NULL);
    if (con == INVALID_HANDLE_VALUE || log == INVALID_HANDLE_VALUE) return 0;
    char buf[16384];
    for (;;) {
        DWORD n = 0, w;
        if (ReadFile(log, buf, sizeof buf, &n, NULL) && n) {
            WriteFile(con, buf, n, &w, NULL);
            continue;
        }
        Sleep(200);
    }
}

void plat_debug_console(int debug) {
    DWORD pids[2];
    bool own = GetConsoleWindow() && GetConsoleProcessList(pids, 2) == 1;   // ours alone (a double click), not a terminal's
    if (!debug) {
        if (own) FreeConsole();
        return;
    }
    if (!g_log_path[0]) return;   // the log is not in our file (captured, or kept on the console by TOTK_LOG_FILE=0)
    if (!GetConsoleWindow()) {
        if (!AllocConsole()) return;
        SetConsoleTitleA("UltraTears - log (Debug mode)");
        SetConsoleOutputCP(CP_UTF8);
    }
    CloseHandle(CreateThread(NULL, 0, follow_log, NULL, 0, NULL));
}

// The code is built for the CPU of the PC that built it (tools/build_game.py --arch: x86-64-v3 with AVX2, FMA and BMI2,
// or x86-64-v2 with SSE4.2 where those are missing, issue #19): say so instead of crashing with an illegal instruction
// when the folder moved to an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
#ifdef __AVX2__
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    MessageBoxA(NULL, "This copy of UltraTears was built for a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / "
                "Zen or newer). Prepare the game again on this PC (UltraTears Launcher): it builds the game for this CPU.", "UltraTears", MB_ICONERROR | MB_OK);
#else
    if (__builtin_cpu_supports("sse4.2") && __builtin_cpu_supports("popcnt") && __builtin_cpu_supports("ssse3")) return;
    MessageBoxA(NULL, "UltraTears needs a CPU with SSE4.2 (Intel Nehalem / 2008 or newer, AMD Bulldozer or newer).",
                "UltraTears", MB_ICONERROR | MB_OK);
#endif
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
// Most fatal errors after a failed allocation name only what was being allocated: say why when memory ran out (the
// system's commit limit nearly reached, or this process's own limit with TOTK_SIM_*).
static bool out_of_memory(void) {
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
    mem_counters pc = {.cb = sizeof pc};
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pc, sizeof pc);
    bool low = GlobalMemoryStatusEx(&ms) && ms.ullAvailPageFile < (512ull << 20);
    if (!low && QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &li, sizeof li, NULL) &&
        (li.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY))
        low = li.PeakProcessMemoryUsed + (256ull << 20) > li.ProcessMemoryLimit;
    if (low)
        fprintf(stderr, "[plat] out of memory: %llu MiB of commit left in the system; process %llu MiB (its limit %llu, used at most %llu)\n",
                (unsigned long long)(ms.ullAvailPageFile >> 20), (unsigned long long)(pc.PrivateUsage >> 20),
                (unsigned long long)(li.ProcessMemoryLimit >> 20), (unsigned long long)(li.PeakProcessMemoryUsed >> 20));
    return low;
}

void plat_fatal_notice(void) {
    bool low = out_of_memory();
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = "UltraTears stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "UltraTears stopped because of an error:\n\n%.600s", line);
            if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\nUltraTears requires Vulkan 1.3 and VK_EXT_shader_object. "
                         "Install the latest driver for your GPU. If the error remains, the GPU is not supported.");
            }
        }
    }
    size_t l = strlen(text);
    if (low)
        l += (size_t)snprintf(text + l, sizeof text - l, "\n\nThe PC ran out of memory (RAM and page file). Close other programs, "
                              "or let Windows manage the page file size (System > About > Advanced system settings > "
                              "Performance > Advanced > Virtual memory).");
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, "UltraTears", MB_ICONERROR | MB_OK);
}

// Development: run as on a PC with less memory (L7, #31). TOTK_SIM_RAM_GB=8 or 16: the RAM a game gets there next to
// Windows and the usual programs (the total less 3 GB) as a hard working-set limit (pages over it are trimmed and
// fault back: from RAM here, so this shows the trimming, not a slow disk), and a commit limit of 1.5 times that
// (allocations over it fail as on a PC whose page file is full). TOTK_SIM_RAM_MB / TOTK_SIM_COMMIT_MB set them
// exactly. Video memory the driver keeps on the GPU is in the commit but not in the working set.
static void simulate_small_pc(void) {
    const char *gb = getenv("TOTK_SIM_RAM_GB"), *ram = getenv("TOTK_SIM_RAM_MB"), *com = getenv("TOTK_SIM_COMMIT_MB");
    uint64_t ws = 0, commit = 0;
    if (gb && atoi(gb) > 3) ws = (uint64_t)(atoi(gb) - 3) << 30, commit = ws * 3 / 2;
    if (ram && atoi(ram) > 0) ws = (uint64_t)atoi(ram) << 20;
    if (com && atoi(com) > 0) commit = (uint64_t)atoi(com) << 20;
    if (ws) {
        BOOL ok = SetProcessWorkingSetSizeEx(GetCurrentProcess(), 64 << 20, (SIZE_T)ws,
                                             QUOTA_LIMITS_HARDWS_MAX_ENABLE | QUOTA_LIMITS_HARDWS_MIN_DISABLE);
        fprintf(stderr, "[plat] simulated PC: working set at most %llu MiB%s (%lu)\n", (unsigned long long)(ws >> 20),
                ok ? "" : " - not set", ok ? 0ul : GetLastError());
    }
    if (commit) {
        HANDLE job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        li.ProcessMemoryLimit = (SIZE_T)commit;
        BOOL ok = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) &&
                  AssignProcessToJobObject(job, GetCurrentProcess());
        fprintf(stderr, "[plat] simulated PC: commit at most %llu MiB%s (%lu)\n", (unsigned long long)(commit >> 20),
                ok ? "" : " - not set", ok ? 0ul : GetLastError());
    }
}

void plat_init(void) {
    require_cpu();
    simulate_small_pc();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = plat_symbol(pc, &off);
        if (!s) s = rt_host_fn_name(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx UltraTears+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    MEMORY_BASIC_INFORMATION fm;
    if (er->NumberParameters >= 2 && VirtualQuery((void *)(uintptr_t)fa, &fm, sizeof fm))
        fprintf(stderr, "[plat] fault %s %#llx: region %p+%#llx state %#lx protect %#lx (allocation %p)\n",
                er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read",
                (unsigned long long)fa, fm.BaseAddress, (unsigned long long)fm.RegionSize, fm.State, fm.Protect,
                fm.AllocationBase);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
// The kernel does not take the lazy-commit fault: a guest buffer handed to ReadFile / WriteFile fails with
// ERROR_NOACCESS on reserved pages. Commit those first (only reserved ones: committed pages keep their protection).
static void commit_reserved(uint64_t a, uint64_t n) {
    if (!n || a >= g_hi || a + n <= g_lo) return;
    uint64_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return;
        uint64_t next = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_RESERVE) plat_commit(a, (next < end ? next : end) - a);
        a = next;
    }
}

static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    commit_reserved((uint64_t)(uintptr_t)buf, n);
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { TerminateProcess(GetCurrentProcess(), 0); }
