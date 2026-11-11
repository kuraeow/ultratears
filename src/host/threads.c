/* Horizon kernel threads and synchronization for the recompiled game.
 * Ported from MacNativeGames games/totk/runtime/threads.c @b75be8c (MIT, "Adapted from pinned D2R Mobile
 * host/hle_thread.c, MIT"). Changes: each guest thread runs the recompiled code on its own host thread with a Cpu
 * (guest_call) instead of entering ARM code on the guest stack; Darwin QoS / Mach diagnostics removed; host -> guest
 * SDK calls go through GCALL. */
/* Original header: Adapted from pinned D2R Mobile host/hle_thread.c, MIT.
 * Game-specific diagnostics and optional timing accounting removed.
 * Core affinity is virtual bookkeeping; Darwin schedules pthreads. */
// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   // glibc's adaptive mutex (G, below)
#endif
#include "host.h"
#include "ipc.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#undef LOG
#define LOG(...) do {fprintf(stderr,"[totk-thread] " __VA_ARGS__);fputc('\n',stderr);}while(0)
#define die(...) do {LOG(__VA_ARGS__);runtime_fail("thread runtime failure");}while(0)
#define now_ns runtime_now_ns

// The global lock G and the per-thread condition variables. Windows: a slim reader/writer lock and condition variables
// (user-space fast paths, a spin before sleeping), not winpthreads' mutex (no spin) and condition variable (built on
// semaphores and locks of its own: several kernel transitions per wait or signal), which ~50 guest threads convoyed on.
// Linux: glibc's adaptive mutex, which spins briefly before the futex.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CONDITION_VARIABLE gcond_t;
static SRWLOCK G = SRWLOCK_INIT;
static inline void g_lock(void) { AcquireSRWLockExclusive(&G); }
static inline void g_unlock(void) { ReleaseSRWLockExclusive(&G); }
static inline void gc_init(gcond_t *c) { InitializeConditionVariable(c); }
static inline void gc_destroy(gcond_t *c) { (void)c; }
static inline void gc_signal(gcond_t *c) { WakeConditionVariable(c); }
static inline int gc_wait(gcond_t *c) { SleepConditionVariableSRW(c, &G, INFINITE, 0); return 0; }
static inline int gc_timedwait(gcond_t *c, const struct timespec *dl) {   // dl: CLOCK_REALTIME, as pthread_cond_timedwait
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int64_t left = (int64_t)(dl->tv_sec - now.tv_sec) * 1000000000 + (dl->tv_nsec - now.tv_nsec);
    if (left <= 0) return ETIMEDOUT;
    SleepConditionVariableSRW(c, &G, (DWORD)((left + 999999) / 1000000), 0);   // whole ms, rounded up
    clock_gettime(CLOCK_REALTIME, &now);
    left = (int64_t)(dl->tv_sec - now.tv_sec) * 1000000000 + (dl->tv_nsec - now.tv_nsec);
    return left <= 0 ? ETIMEDOUT : 0;   // early (woken, or spurious): the caller looks again
}
#else
typedef pthread_cond_t gcond_t;
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP   // glibc with _GNU_SOURCE
static pthread_mutex_t G = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#endif
static inline void g_lock(void) { pthread_mutex_lock(&G); }
static inline void g_unlock(void) { pthread_mutex_unlock(&G); }
static inline void gc_init(gcond_t *c) { pthread_cond_init(c, NULL); }
static inline void gc_destroy(gcond_t *c) { pthread_cond_destroy(c); }
static inline void gc_signal(gcond_t *c) { pthread_cond_signal(c); }
static inline int gc_wait(gcond_t *c) { return pthread_cond_wait(c, &G); }
static inline int gc_timedwait(gcond_t *c, const struct timespec *dl) { return pthread_cond_timedwait(c, &G, dl); }
#endif
#define guest_ns runtime_now_ns
static inline int dm_detclock_enabled(void) {return 0;}

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   // O_SESSION / O_TMEM: IPC objects owned by ipc.c
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; uint64_t host_context[2]; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; gcond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr, last_fp;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    uint32_t qos;
    int cur_core;              // virtual core held while running guest code, -1 while blocked / not started
    int preempt;               // a higher-priority thread waits for this thread's core
    uint64_t cseq, core_since; struct thr *cnext;
    uint32_t wh[4]; int nwh;   // handles of the current WaitSynchronization (state dump)
    int last_hle;              // 1 + wrapper index of the last host call made while "hletrace" was on (state dump)
    uint64_t yrun_ns, yrun_svc;   // the current run of yields: when it started, and nsvc at its last yield (yield_backoff_ns)
    // UltraTears (runtime_threads_report, TOTK_STATS): time blocked by wait kind (1 lock 2 condition 3 address 4 sync),
    // waiting for a virtual core, and from a wake to running again (the host's scheduling latency)
    uint64_t st_block_ns[5], st_blocks, st_core_ns, st_core_waits, st_wake_ns, st_wakes, woke_ns, st_hold_ns;
    uint64_t blk_t0, blk_cred, core_t0, core_cred; int blk_kind;   // a wait in progress: since when, counted up to when
    unsigned long os_tid;      // the host's id of this thread (os_times), set by the thread itself
    uint64_t st_inv_ns, st_inv_max;   // core waits while a worse-priority thread held a core this one may use: total, longest
    // asynchronous preemption (below): this thread's host-call depth and Cpu (TLR), interruption state
    volatile int *depth; Cpu *cpu; int ap_ok, ap_pending; uint64_t ap_rip, ap_last;
#ifdef _WIN32
    HANDLE ap_h; CONTEXT *ap_ctx;
#else
    pthread_t ap_pt; int ap_state; uint64_t ap_srip, ap_to;
#endif
} thr_t;

/* The host thread's CPU time and, on Linux, its time runnable but not running (waiting for a processor: taken by other
   threads), in ns since it started (runtime_threads_report). Linux: /proc/self/task/<tid>/schedstat ("run delay
   slices"); Windows: GetThreadTimes, no delay. 0 when not readable. */
#ifdef _WIN32
static unsigned long os_tid_self(void) { return GetCurrentThreadId(); }
static int os_times(unsigned long tid, uint64_t *run, uint64_t *delay) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)tid);
    FILETIME c, e, k, u;
    int ok = h && GetThreadTimes(h, &c, &e, &k, &u);
    if (h) CloseHandle(h);
    if (!ok) return 0;
    *run = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) * 100 + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime) * 100;
    *delay = 0;
    return 1;
}
#else
#include <sys/syscall.h>
#include <unistd.h>
static unsigned long os_tid_self(void) { return (unsigned long)syscall(SYS_gettid); }
static int os_times(unsigned long tid, uint64_t *run, uint64_t *delay) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%lu/schedstat", tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long r = 0, d = 0;
    int ok = fscanf(f, "%llu %llu", &r, &d) == 2;
    fclose(f);
    *run = r, *delay = d;
    return ok;
}
#endif
static thr_t *g_all;
static thr_t *g_main_thr;   // the guest main thread (svc_thread_main_init): its share of a long frame

#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
/* [port] the calling guest thread: a thread-local instead of a pthread key (winpthreads hands out key 0, which
   MacNativeGames' "g_keythr ? ... : NULL" checks took for "no key") */
static __thread thr_t *t_self;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return t_self;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = t_self;
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; t->woke_ns = now_ns(); gc_signal(&t->cv); }
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
// ---------- Virtual cores ----------
// Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
// GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they
// run guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking
// host services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a
// higher-priority waiter wants to preempt gives its core up at its next SVC. TOTK_FREE_CORES=1 disables this.
#define NCORE 3
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static int g_cores_on = -1;
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("TOTK_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }
/* Horizon's priority inheritance (UltraTears, 2026-11-04): a thread that owns a guest mutex counts at the priority of
   the best thread blocked on it, for handing out cores and for preemption. Without it a low-priority owner of a mutex
   the game's main thread waits for (the Steam Deck's log: 4.4 ms a frame blocked on locks) queued for a core behind
   workers of middling priority. The owner is read from the mutex word, so it follows hand-offs; one level only (an
   owner blocked on another mutex passes nothing on).
   TOTK_PRIO_INHERIT=1 turns it on (off by default: raw priorities, see pi_on). Holding G. */
static int ap_on(void);
static int pi_on(void) {
    static int on = -1;
    // Off by default (=1: on). On the PC, 2026-11-04 evening, the game itself stalled 65-420 ms ~8x as often with it
    // (and the spin below) on. It came back on with asynchronous preemption (2996fbb) without a measurement; the Steam
    // Deck, same route with and without (2026-11-06, both with direct texel reads): without it frames over 33 ms
    // 113 -> 80, over 50 ms 24 -> 20, waits behind a worse-priority core owner in long frames 21.2 -> 15.2 ms a frame,
    // nothing worse. (The preemption-disable check of runtime_core_checkpoint follows this switch too.)
    if (on < 0) { const char *e = getenv("TOTK_PRIO_INHERIT"); on = e && *e == '1'; (void)ap_on; }
    return on;
}
static int eff_prio(const thr_t *o) {
    int p = o->prio;
    if (!pi_on()) return p;
    for (const thr_t *w = g_wait; w; w = w->next)
        if (w->wkind == 1 && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}
static void core_grant(int c) {   // holding G: give free core c to the best waiter
    thr_t *b = NULL, **bp = NULL;
    int bprio = 0;
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int pr = eff_prio(*p);
        if (!b || pr < bprio || (pr == bprio && (*p)->cseq < b->cseq)) { b = *p; bp = p; bprio = pr; }
    }
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); g_core_owner[c] = b; gc_signal(&b->cv);
}
// t waits for a core: the owners of the cores it may run on that count worse give theirs up at their next checkpoint
// (holding G)
static void core_kick(const thr_t *t) {
    int tp = eff_prio(t);
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && eff_prio(g_core_owner[i]) > tp) g_core_owner[i]->preempt = 1;
}
static void core_put(thr_t *t) {   // holding G
    if (t->cur_core < 0) return;
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL;
    t->st_hold_ns += now_ns() - t->core_since;
    core_grant(c);
}
static void core_take(thr_t *t);
static int core_better_waiter(const thr_t *t, int c);
// ---------- Asynchronous preemption (TOTK_ASYNC_PREEMPT, on unless 0) ----------
// Horizon gives a core to a strictly better thread the moment that thread is ready. Here a better waiter flags the
// owner (core_kick), which gives the core up at its next SVC or indirect call (runtime_core_checkpoint); game code with
// neither keeps it for milliseconds, and while the world streams in the game's loaders (low priority) compute long
// stretches like that while its frame's workers wait. So a waiter that has waited AP_FIRST_NS interrupts the owner, then
// every AP_GAP_NS while it still waits: if the owner's host instruction pointer is in the lifted game code and it is in
// no host call (rt_host_depth 0: no host lock below it), it is sent to runtime_ap_stub, which keeps every register the
// game code may have live, gives the core up as at a checkpoint (runtime_ap_yield) and goes on where it was stopped.
// An owner inside an SDK spin lock (TLR+0x100 disable_count) is not preempted: TLR+0x102 (interrupt_flag) is set and
// the SDK calls SVC 0x36 once it leaves the lock (runtime_ap_sync). Priority inheritance (pi_on) comes with it: without
// it a better thread takes the core of a low-priority mutex owner it then waits for.
// The design is MacNativeGames' (games/totk/runtime/threads.c, 8bf0893, MIT, Copyright (c) 2026 MacNativeGames
// contributors); the code here is ours.
// TOTK_ASYNC_PREEMPT=0: the checkpoints only, as before (and no inheritance unless TOTK_PRIO_INHERIT=1).
#define AP_FIRST_NS 250000ull   // a waiter spins this long before the first interruption (most core waits end sooner)
#define AP_GAP_NS 1000000ull    // and interrupts the same owner at most once a millisecond
static int g_ap = -1;
static uintptr_t g_gen_lo, g_gen_hi;   // the lifted modules' host code: first and last function
static uint64_t g_ap_sent, g_ap_taken, g_ap_deferred, g_ap_host, g_ap_out, g_ap_fail, g_ap_sync;
extern __thread int rt_host_depth;     // runtime/dispatch.c
int rt_lifted_range(uintptr_t *lo, uintptr_t *hi);
void runtime_ap_stub(void);
void runtime_core_checkpoint(void);
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc);

static int ap_on(void) {
    if (g_ap >= 0) return g_ap;
    const char *e = getenv("TOTK_ASYNC_PREEMPT");
    int on = !(e && !strcmp(e, "0")) && cores_enabled() && !getenv("TOTK_NO_PREEMPT");
    uintptr_t lo = 0, hi = 0;
    const char *why = on ? NULL : "off";
    if (on && !rt_lifted_range(&lo, &hi)) on = 0, why = "no lifted code range";
    const uintptr_t host[] = {(uintptr_t)runtime_ap_stub, (uintptr_t)runtime_core_checkpoint, (uintptr_t)rt_svc,
                              (uintptr_t)rt_dispatch, (uintptr_t)guest_call};
    for (unsigned i = 0; on && i < sizeof host / sizeof *host; i++)
        if (host[i] >= lo && host[i] <= hi) on = 0, why = "host code inside the lifted code range";
    g_gen_lo = lo, g_gen_hi = hi, g_ap = on;
    fprintf(stderr, "[totk] virtual cores: asynchronous preemption %s%s%s\n", on ? "on" : "off", why ? ": " : "", why ? why : "");
    return on;
}
static int ap_disabled(const thr_t *t) {   // inside an SDK spin lock (TLR+0x100 disable_count)
    return t->cpu && t->cpu->tpidrro && *(volatile uint16_t *)(uintptr_t)(t->cpu->tpidrro + 0x100);
}

#ifdef _WIN32
// Windows: the owner is suspended, its context read and, to move it, written back with another instruction pointer.
static void ap_open(thr_t *t) {
    HANDLE h = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, 0)) return;
    CONTEXT *ctx = _aligned_malloc(sizeof *ctx, 16);
    if (!ctx) { CloseHandle(h); return; }
    t->ap_h = h, t->ap_ctx = ctx, t->ap_ok = 1;
}
static int ap_stop(thr_t *o, uint64_t *rip) {
    if (SuspendThread(o->ap_h) == (DWORD)-1) return 0;
    memset(o->ap_ctx, 0, sizeof *o->ap_ctx);
    o->ap_ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(o->ap_h, o->ap_ctx)) { ResumeThread(o->ap_h); return 0; }
    *rip = o->ap_ctx->Rip;
    return 1;
}
static int ap_resume(thr_t *o, uint64_t to) {
    int ok = 1;
    if (to) o->ap_ctx->Rip = to, ok = SetThreadContext(o->ap_h, o->ap_ctx) != 0;
    ResumeThread(o->ap_h);
    return ok;
}
#else
// Linux: a real-time signal stops the owner in its handler, which reports the interrupted instruction pointer and
// waits until the waiter says where to go on (a state word: idle, asked, stopped, go).
#include <signal.h>
#include <ucontext.h>
enum { AP_IDLE, AP_REQ, AP_STOPPED, AP_GO };
static int g_ap_sig;
static void ap_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig, (void)si;
    thr_t *t = t_self;
    ucontext_t *u = ucv;
    if (!t) return;
    int req = AP_REQ;
    t->ap_srip = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    if (!__atomic_compare_exchange_n(&t->ap_state, &req, AP_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    while (__atomic_load_n(&t->ap_state, __ATOMIC_ACQUIRE) != AP_GO) __builtin_ia32_pause();
    if (t->ap_to) u->uc_mcontext.gregs[REG_RIP] = (greg_t)t->ap_to;
    __atomic_store_n(&t->ap_state, AP_IDLE, __ATOMIC_RELEASE);
}
static void ap_open(thr_t *t) {
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    if (!__atomic_load_n(&g_ap_sig, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&once);
        if (!g_ap_sig) {
            int sig = SIGRTMIN + 4;   // past the ones glibc keeps for itself
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = ap_handler;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            if (!sigaction(sig, &sa, NULL)) __atomic_store_n(&g_ap_sig, sig, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&once);
    }
    if (!g_ap_sig) return;
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, g_ap_sig);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    t->ap_pt = pthread_self(), t->ap_ok = 1;
}
static int ap_stop(thr_t *o, uint64_t *rip) {
    int idle = AP_IDLE;
    if (!__atomic_compare_exchange_n(&o->ap_state, &idle, AP_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (pthread_kill(o->ap_pt, g_ap_sig)) { __atomic_store_n(&o->ap_state, AP_IDLE, __ATOMIC_RELEASE); return 0; }
    uint64_t end = now_ns() + 2000000;   // a thread the host does not run now answers late: then not this time
    while (__atomic_load_n(&o->ap_state, __ATOMIC_ACQUIRE) != AP_STOPPED) {
        if (now_ns() < end) { __builtin_ia32_pause(); continue; }
        int req = AP_REQ;
        if (__atomic_compare_exchange_n(&o->ap_state, &req, AP_IDLE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    *rip = o->ap_srip;
    return 1;
}
static int ap_resume(thr_t *o, uint64_t to) {
    o->ap_to = to;
    __atomic_store_n(&o->ap_state, AP_GO, __ATOMIC_RELEASE);
    return 1;
}
#endif

// The calling guest thread, as it starts: interruptible if asynchronous preemption is on.
static void ap_thread_start(thr_t *t) {
    t->depth = &rt_host_depth;
    t->cpu = guest_cpu();
    if (ap_on()) ap_open(t);
}

// runtime_ap_stub (on the interrupted owner, game code at depth 0): where to go on, then the core given up to the
// better waiter as at a checkpoint (the request may be stale by now; then the thread just goes on).
uint64_t runtime_ap_rip(void) { return t_self->ap_rip; }
void runtime_ap_yield(void) {
    thr_t *t = t_self;
    g_lock();
    t->ap_pending = 0;
    if (t->preempt && t->cur_core >= 0 && !ap_disabled(t) && core_better_waiter(t, t->cur_core)) {
        g_ap_taken++;
        core_put(t);
        core_take(t);
    }
    g_unlock();
}
// SVC 0x36 SynchronizePreemptionState: the SDK leaves a spin lock with interrupt_flag set; a preemption it deferred
// happens here (svc.c).
void runtime_ap_sync(void) {
    thr_t *t = t_self;
    if (!t) return;
    if (t->cpu && t->cpu->tpidrro) *(volatile uint16_t *)(uintptr_t)(t->cpu->tpidrro + 0x102) = 0;
    g_lock();
    g_ap_sync++;
    if (t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) core_put(t), core_take(t);
    g_unlock();
}

// The register save around runtime_ap_yield. Entered in place of the interrupted instruction: it saves the flags, the
// volatile general registers, YMM0-15 (XMM0-15 in a build without AVX, whose CPUs have no VEX encoding) and MXCSR,
// calls runtime_ap_rip and stores the result in a slot it reserved at entry, calls runtime_ap_yield, restores
// everything and jumps through the slot. System V: below the 128-byte red zone the interrupted code may use; rsi and
// rdi are volatile there. Windows: no red zone, rsi and rdi kept by the C calls, 32 bytes of shadow space.
#ifdef __AVX__
#define AP_VMOV "vmovdqa"
#define AP_VR "%ymm"
#define AP_VZU "    vzeroupper\n"
#else
#define AP_VMOV "movdqa"
#define AP_VR "%xmm"
#define AP_VZU ""
#endif
#define AP_SAVE(n, off) "    " AP_VMOV " " AP_VR #n ", " #off "(%rsp)\n"
#define AP_LOAD(n, off) "    " AP_VMOV " " #off "(%rsp), " AP_VR #n "\n"
#define AP_VSAVE AP_SAVE(0, 64) AP_SAVE(1, 96) AP_SAVE(2, 128) AP_SAVE(3, 160) AP_SAVE(4, 192) AP_SAVE(5, 224) \
    AP_SAVE(6, 256) AP_SAVE(7, 288) AP_SAVE(8, 320) AP_SAVE(9, 352) AP_SAVE(10, 384) AP_SAVE(11, 416)            \
    AP_SAVE(12, 448) AP_SAVE(13, 480) AP_SAVE(14, 512) AP_SAVE(15, 544)
#define AP_VLOAD AP_LOAD(0, 64) AP_LOAD(1, 96) AP_LOAD(2, 128) AP_LOAD(3, 160) AP_LOAD(4, 192) AP_LOAD(5, 224) \
    AP_LOAD(6, 256) AP_LOAD(7, 288) AP_LOAD(8, 320) AP_LOAD(9, 352) AP_LOAD(10, 384) AP_LOAD(11, 416)            \
    AP_LOAD(12, 448) AP_LOAD(13, 480) AP_LOAD(14, 512) AP_LOAD(15, 544)
#ifdef _WIN32
__asm__(".text\n"
        ".globl runtime_ap_stub\n"
        ".def runtime_ap_stub; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "runtime_ap_stub:\n"
        ".seh_proc runtime_ap_stub\n"
        "    leaq -8(%rsp), %rsp\n"   // the slot; lea leaves the flags as the interrupted code had them
        ".seh_stackalloc 8\n"
        "    pushfq\n"
        ".seh_stackalloc 8\n"
        "    pushq %rax\n" ".seh_pushreg %rax\n"
        "    pushq %rcx\n" ".seh_pushreg %rcx\n"
        "    pushq %rdx\n" ".seh_pushreg %rdx\n"
        "    pushq %r8\n" ".seh_pushreg %r8\n"
        "    pushq %r9\n" ".seh_pushreg %r9\n"
        "    pushq %r10\n" ".seh_pushreg %r10\n"
        "    pushq %r11\n" ".seh_pushreg %r11\n"
        "    pushq %rbp\n" ".seh_pushreg %rbp\n"
        "    movq %rsp, %rbp\n"
        ".seh_setframe %rbp, 0\n"
        ".seh_endprologue\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 0 shadow space, 32 MXCSR, 36 the default MXCSR, 64 the vector registers
        AP_VSAVE
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        AP_VZU
        "    call runtime_ap_rip\n"
        "    movq %rax, 72(%rbp)\n"   // the slot: 8 pushes and the flags above rbp
        "    call runtime_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        AP_VLOAD
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 8(%rsp), %rsp\n"
        "    jmpq *-8(%rsp)\n"
        ".seh_endproc\n");
#else
__asm__(".text\n"
        ".globl runtime_ap_stub\n"
        ".type runtime_ap_stub, @function\n"
        ".p2align 4\n"
        "runtime_ap_stub:\n"
        "    .cfi_startproc\n"
        "    leaq -136(%rsp), %rsp\n"   // past the red zone, and the slot at the new rsp
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-32, %rsp\n"
        "    subq $576, %rsp\n"   // 32 MXCSR, 36 the default MXCSR, 64 the vector registers
        AP_VSAVE
        "    stmxcsr 32(%rsp)\n"
        "    movl $0x1f80, 36(%rsp)\n"
        "    ldmxcsr 36(%rsp)\n"
        "    cld\n"
        AP_VZU
        "    call runtime_ap_rip\n"
        "    movq %rax, 88(%rbp)\n"   // the slot: 10 pushes and the flags above rbp
        "    call runtime_ap_yield\n"
        "    ldmxcsr 32(%rsp)\n"
        AP_VLOAD
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdi\n"
        "    popq %rsi\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 136(%rsp), %rsp\n"
        "    jmpq *-136(%rsp)\n"
        "    .cfi_endproc\n"
        ".size runtime_ap_stub, .-runtime_ap_stub\n");
#endif

enum { AP_NONE, AP_RETRY, AP_SENT };
// The owner a waiter t would interrupt: the worst one, flagged to give its core up, among those of cores t may use
// that count worse than t (holding G).
static thr_t *ap_target(const thr_t *t) {
    int tp = eff_prio(t), wp = tp;
    thr_t *o = NULL;
    for (int i = 0; i < NCORE; i++) {
        thr_t *h = g_core_owner[i];
        if (!core_allowed(t, i) || !h || !h->preempt) continue;
        int p = eff_prio(h);
        if (p > wp) wp = p, o = h;
    }
    return o;
}
// Interrupt o if it runs top-level game code (holding G; o is stopped only between ap_stop and ap_resume, during
// which nothing here takes a lock, and it holds no host lock where it is moved).
static int ap_try(thr_t *o) {
    if (!o || o->cur_core < 0 || !o->preempt) return AP_NONE;
    if (!o->ap_ok || o->ap_pending) return AP_RETRY;
    if (o->depth && __atomic_load_n(o->depth, __ATOMIC_RELAXED) > 0) { g_ap_host++; return AP_RETRY; }
    if (ap_disabled(o)) {   // in an SDK spin lock: SVC 0x36 switches when it leaves
        *(volatile uint16_t *)(uintptr_t)(o->cpu->tpidrro + 0x102) = 1;
        g_ap_deferred++;
        return AP_RETRY;
    }
    uint64_t now = now_ns(), rip = 0, to = 0;
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return AP_RETRY;
    o->ap_last = now;
    if (!ap_stop(o, &rip)) { g_ap_fail++; return AP_RETRY; }
    if (rip < g_gen_lo || rip > g_gen_hi) g_ap_out++;
    else if (o->depth && __atomic_load_n(o->depth, __ATOMIC_RELAXED) > 0) g_ap_host++;
    else if (ap_disabled(o)) *(volatile uint16_t *)(uintptr_t)(o->cpu->tpidrro + 0x102) = 1, g_ap_deferred++;
    else o->ap_rip = rip, o->ap_pending = 1, to = (uint64_t)(uintptr_t)runtime_ap_stub;
    if (!ap_resume(o, to)) { o->ap_pending = 0; g_ap_fail++; return AP_RETRY; }
    if (to) g_ap_sent++;
    return to ? AP_SENT : AP_RETRY;
}
// The counters, for the thread report (TOTK_STATS)
static void ap_report(void) {
    if (g_ap > 0)
        fprintf(stderr, "[totk] virtual cores: asynchronous preemption since start: %llu interruptions sent, %llu cores "
                "handed over, deferred in a spin lock %llu (SVC 0x36 %llu), not now: in a host call %llu, outside game code %llu, failed %llu\n",
                (unsigned long long)g_ap_sent, (unsigned long long)g_ap_taken, (unsigned long long)g_ap_deferred,
                (unsigned long long)g_ap_sync, (unsigned long long)g_ap_host, (unsigned long long)g_ap_out,
                (unsigned long long)g_ap_fail);
}

static void core_take(thr_t *t) {   // holding G; waits until a core in t's mask is handed over
    if (!cores_enabled() || t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    if (c >= 0) { t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t; return; }
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t;
    core_kick(t);
    t->core_t0 = t->core_cred = now_ns();
    // a wait behind a worse-priority owner: Horizon would have given this thread the core at once; here the owner
    // gives it up only at its next SVC or indirect call (runtime_threads_totals, the long-frame log)
    int inv = 0, tp = eff_prio(t);
    for (int i = 0; i < NCORE && !inv; i++) inv = core_allowed(t, i) && g_core_owner[i] && eff_prio(g_core_owner[i]) > tp;
    uint64_t w0 = t->core_t0;
    // A short spin before sleeping (TOTK_CORE_SPIN_US, 20 by default; 0: none): a core often comes free within
    // microseconds (a worker's job ends, an SVC hands it over), and a sleep costs the host's wake latency on top
    // (15-20 us on the Steam Deck, 2026-11-04). core_grant sets cur_core under G; reading it here without G is a hint.
    static int64_t spin_ns = -1;
    if (spin_ns < 0) { const char *e = getenv("TOTK_CORE_SPIN_US"); spin_ns = (e && *e ? atoll(e) : 0) * 1000; }   // [temporary] 0 by default, see pi_on
    if (spin_ns > 0) {
        g_unlock();
        uint64_t s0 = now_ns();
        while (__atomic_load_n(&t->cur_core, __ATOMIC_ACQUIRE) < 0 && now_ns() - s0 < (uint64_t)spin_ns) __builtin_ia32_pause();
        g_lock();
    }
    if (g_ap > 0 && t->cur_core < 0 && ap_target(t)) {   // most waits end within microseconds: spin before interrupting
        g_unlock();
        uint64_t s0 = now_ns();
        while (__atomic_load_n(&t->cur_core, __ATOMIC_ACQUIRE) < 0 && now_ns() - s0 < AP_FIRST_NS) __builtin_ia32_pause();
        g_lock();
    }
    while (t->cur_core < 0) {
        if (g_ap > 0 && ap_try(ap_target(t)) != AP_NONE) {   // the owner keeps its core for now: again in a millisecond
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_nsec += 1000000;
            if (dl.tv_nsec >= 1000000000) dl.tv_sec++, dl.tv_nsec -= 1000000000;
            gc_timedwait(&t->cv, &dl);
        } else
            gc_wait(&t->cv);
    }
    if (inv) { uint64_t d = now_ns() - w0; t->st_inv_ns += d; if (d > t->st_inv_max) t->st_inv_max = d; }
    t->st_core_ns += now_ns() - t->core_cred, t->st_core_waits++;
    t->core_t0 = 0;
}
static thr_t *me_or_null(void) { return t_self; }
static int core_better_waiter(const thr_t *t, int c) {   // holding G: a waiter for core c better than t
    int tp = eff_prio(t);
    for (const thr_t *w = g_core_wait; w; w = w->cnext) if (core_allowed(w, c) && eff_prio(w) < tp) return 1;
    return 0;
}
// A thread blocks on the guest mutex at its waddr (holding G, linked into g_wait): the owner now counts at least at
// its priority, so if the owner is queued for a core, worse owners of the cores it may use give them up.
static void pi_lock_wait(const thr_t *t) {
    if (!pi_on() || !cores_enabled() || !t->waddr) return;
    uint32_t h = __atomic_load_n((const uint32_t *)(uintptr_t)t->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT;
    for (const thr_t *o = g_core_wait; o; o = o->cnext)
        if (o->handle == h) { core_kick(o); return; }
}
void runtime_thread_note_hle(unsigned i) { thr_t *t = me_or_null(); if (t) t->last_hle = (int)i + 1; }   // hle_stack.c, while tracing
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
// Host services that wait for other threads or the GPU give the core up around the wait (no host locks held).
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    g_lock(); core_put(t); g_unlock(); return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    g_lock(); core_take(t); g_unlock();
}
// Preemption point at SVC entry: a higher-priority thread waits for this core.
// Called at every SVC (guest.c rt_svc) and indirect call (runtime/dispatch.c rt_dispatch). The request may be stale:
// the waiter got another core meanwhile, and giving this one up would hand it to a worse thread.
// Horizon's preemption disable: the SDK's spin locks count TLR+0x100 (disable_count) while they try or hold the lock.
// Such a thread keeps its core (the request stays for its next checkpoint): taking the core of a spin lock's holder
// leaves a better thread spinning on that lock (as MacNativeGames does).
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null(); if (!t || !t->preempt) return;
    Cpu *c = guest_cpu();
    if (c && c->tpidrro && pi_on() && *(volatile uint16_t *)(uintptr_t)(c->tpidrro + 0x100)) return;
    g_lock();
    if (t->cur_core >= 0 && core_better_waiter(t, t->cur_core)) { core_put(t); core_take(t); }   // core_put clears the request
    else t->preempt = 0;
    g_unlock();
}
extern __thread volatile int *rt_preempt_flag;   // runtime/dispatch.c
extern void (*rt_preempt_fn)(void);
static void preempt_point_set(thr_t *t) {
    rt_preempt_fn = runtime_core_checkpoint;
    rt_preempt_flag = t && cores_enabled() && !getenv("TOTK_NO_PREEMPT") ? &t->preempt : NULL;   // TOTK_NO_PREEMPT=1: as before
}
// A pass through a spin wait: a guest loop that only another thread can end (tools/lifter/spin_waits_v143.txt; the
// lifter calls this on its back-edge). The loop holds a virtual core while it spins, and the thread that would end it
// may be waiting for that core: once one wait has lasted 50 us (TOTK_SPIN_HANDOFF_US) and any thread waits for a core,
// the core goes over for a yield and comes back. Passes of one loop at most 20 us apart are one wait. TOTK_SPIN=0: the loop keeps its core, as before.
static __thread uint64_t sw_head, sw_t0, sw_last;
void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    __builtin_ia32_pause();
    thr_t *t = t_self;
    if (!t) return;
    static int on = -1;
    static uint64_t handoff_ns;
    if (on < 0) {
        const char *e = getenv("TOTK_SPIN_HANDOFF_US");
        handoff_ns = (uint64_t)(e && *e ? atoll(e) : 50) * 1000;
        e = getenv("TOTK_SPIN");
        on = !(e && *e == '0');
    }
    uint64_t now = now_ns();
    if (head != sw_head || now - sw_last > 20000) { sw_head = head; sw_t0 = now; }   // a new wait
    sw_last = now;
    if (!on || t->cur_core < 0 || now - sw_t0 < handoff_ns || !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED)) return;
    int away = runtime_core_away();
    sched_yield();
    runtime_core_back(&away);
    sw_last = now_ns();
}
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }
// The host thread ids of the running guest threads (development: host/watch.c TOTK_BONES breakpoints).
int runtime_guest_tids(unsigned long *out, int max) {
    int n = 0;
    g_lock();
    for (thr_t *t = g_all; t && n < max; t = t->all)
        if (t->started && !t->exited && t->os_tid) out[n++] = t->os_tid;
    g_unlock();
    return n;
}

// UltraTears (TOTK_STATS, with the frame interval line): the guest threads that waited most over the last `frames`
// frames: blocked in a wait by kind, waiting for a virtual core, and the host's latency from a wake to running again.
// Where a frame's time goes when no thread is busy (the Steam Deck's profile of 2026-11-04: ~2.8 of 8 CPUs).
void runtime_threads_report(unsigned frames) {
    if (!frames) return;
    enum { N = 256 };
    // matched by thread id: a handle comes back for a new thread once the old one is gone, and its smaller totals went
    // negative (61489146967 ms in the Deck's log of 2026-11-04)
    static struct { uint64_t id; uint64_t b[5], core, cw, wake, wakes, blocks, hold, run, delay; } last[N];
    typedef struct { uint32_t h; int prio, k; unsigned long tid; double b[5], all, core, cw, wake, wakes, blocks, hold, cpu, dly; } row;
    row r[N];
    int n = 0;
    static uint64_t last_ns;   // the window's wall time per frame: what a thread did not wait, it ran (or waited on the host)
    uint64_t now = now_ns();
    double frame_ms = last_ns ? (double)(now - last_ns) / 1e6 / frames : 0;
    last_ns = now;
    g_lock();
    for (thr_t *t = g_all; t && n < N; t = t->all) {
        if (t->exited || !t->started) continue;
        if (t->blk_t0) t->st_block_ns[t->blk_kind] += now - t->blk_cred, t->blk_cred = now;   // waits in progress
        if (t->core_t0) t->st_core_ns += now - t->core_cred, t->core_cred = now;
        if (t->cur_core >= 0 && now > t->core_since) t->st_hold_ns += now - t->core_since, t->core_since = now;
        int k = 0;
        while (k < N && last[k].id && last[k].id != t->id) k++;
        if (k == N) {   // the table is full of threads seen before: start it over (one window of rows goes missing)
            memset(last, 0, sizeof last);
            k = 0;
        }
        if (!last[k].id) memset(&last[k], 0, sizeof last[k]), last[k].id = t->id;
        row *x = &r[n++];
        x->hold = (double)(t->st_hold_ns - last[k].hold) / 1e6 / frames, last[k].hold = t->st_hold_ns;
        x->h = t->handle, x->prio = t->prio, x->all = 0, x->k = k, x->tid = t->os_tid, x->cpu = x->dly = -1;
        for (int i = 0; i < 5; i++) x->b[i] = (double)(t->st_block_ns[i] - last[k].b[i]) / 1e6 / frames, x->all += x->b[i], last[k].b[i] = t->st_block_ns[i];
        x->core = (double)(t->st_core_ns - last[k].core) / 1e6 / frames, last[k].core = t->st_core_ns;
        x->cw = (double)(t->st_core_waits - last[k].cw) / frames, last[k].cw = t->st_core_waits;
        x->wake = (double)(t->st_wake_ns - last[k].wake), last[k].wake = t->st_wake_ns;
        x->wakes = (double)(t->st_wakes - last[k].wakes), last[k].wakes = t->st_wakes;
        x->blocks = (double)(t->st_blocks - last[k].blocks) / frames, last[k].blocks = t->st_blocks;
    }
    g_unlock();
    // the host's view (outside G: a file read per thread on Linux): CPU time the thread got, and time it was runnable
    // but the host ran other threads (Linux only) - a virtual core held while its thread waits for a processor
    double cpu_all = 0, dly_all = 0;
    for (int i = 0; i < n; i++) {
        uint64_t run, delay;
        if (!r[i].tid || !os_times(r[i].tid, &run, &delay)) continue;
        if (last[r[i].k].run && run >= last[r[i].k].run)
            r[i].cpu = (double)(run - last[r[i].k].run) / 1e6 / frames, r[i].dly = (double)(delay - last[r[i].k].delay) / 1e6 / frames;
        last[r[i].k].run = run, last[r[i].k].delay = delay;
        if (r[i].cpu >= 0) cpu_all += r[i].cpu, dly_all += r[i].dly;
    }
    double core_all = 0, wake_all = 0, wakes_all = 0, hold_all = 0;
    for (int i = 0; i < n; i++) core_all += r[i].core, wake_all += r[i].wake, wakes_all += r[i].wakes, hold_all += r[i].hold;
    for (int i = 0; i < n; i++)   // the rows: the threads that held the virtual cores longest first
        for (int j = i + 1; j < n; j++)
            if (r[j].hold > r[i].hold) { row t = r[i]; r[i] = r[j]; r[j] = t; }
    // "on a core": the time holding a virtual core (the three cores give at most 3x the frame); "away": neither on a
    // core nor in a guest wait - a host service that gave its core up (GPU, audio, files), or the host not running it
    fprintf(stderr, "[totk] guest threads per frame: virtual cores held %.2f of %.2f ms, waiting for a virtual core %.2f ms in all, "
            "wake latency %.0f us average (%.0f wakes a frame); host: CPU %.2f ms, waiting for a processor %.2f ms\n", hold_all, 3 * frame_ms, core_all,
            wakes_all ? wake_all / wakes_all / 1e3 : 0.0, wakes_all / frames, cpu_all, dly_all);
    for (int i = 0; i < n && i < 10; i++) {
        double away = frame_ms > 0 ? frame_ms - r[i].all - r[i].core - r[i].hold : 0.0;
        fprintf(stderr, "[totk]   thread %#x prio %d: on a core %.1f ms, core wait %.2f ms (%.1f), blocked %.1f ms (lock %.1f, cond %.1f, address %.1f, sync %.1f; %.1f waits), "
                "away %.1f ms, wake latency %.0f us; CPU %.1f ms, waiting for a processor %.1f ms\n", r[i].h, r[i].prio, r[i].hold, r[i].core, r[i].cw, r[i].all, r[i].b[1], r[i].b[2], r[i].b[3], r[i].b[4], r[i].blocks,
                away > 0 ? away : 0.0, r[i].wakes ? r[i].wake / r[i].wakes / 1e3 : 0.0, r[i].cpu > 0 ? r[i].cpu : 0.0, r[i].dly > 0 ? r[i].dly : 0.0);
    }
    ap_report();
}

/* UltraTears (the long-frame log, gfx/nvn/hle_nvn.c): running totals in ns - [0] all guest threads waiting for a virtual
   core, [1] the main thread holding one, [2] its core waits, [3..6] its blocked time by kind (lock, condition, address,
   sync), [7] all threads' core waits behind a worse-priority owner; [8] the longest such wait since the last call (not
   a running total). Waits in progress are counted up to now. Read once a frame; the caller takes differences. */
void runtime_threads_totals(uint64_t v[9]) {
    memset(v, 0, 9 * sizeof *v);
    uint64_t now = now_ns();
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited || !t->started) continue;
        if (t->core_t0) t->st_core_ns += now - t->core_cred, t->core_cred = now;
        v[0] += t->st_core_ns, v[7] += t->st_inv_ns;
        if (t->st_inv_max > v[8]) v[8] = t->st_inv_max;
        t->st_inv_max = 0;
    }
    thr_t *m = g_main_thr;
    if (m && !m->exited) {
        if (m->blk_t0) m->st_block_ns[m->blk_kind] += now - m->blk_cred, m->blk_cred = now;
        if (m->cur_core >= 0 && now > m->core_since) m->st_hold_ns += now - m->core_since, m->core_since = now;
        v[1] = m->st_hold_ns, v[2] = m->st_core_ns;
        for (int k = 1; k <= 4; k++) v[2 + k] = m->st_block_ns[k];
    }
    g_unlock();
}

static int64_t block(thr_t *t,int kind,int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t); int64_t r = block_inner(t, kind, ns); core_take(t); return r;
}
static int64_t block_inner2(thr_t *t, int kind, int64_t ns);
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {   // with the statistics (runtime_threads_report)
    int k = kind >= 1 && kind <= 4 ? kind : 0;
    if (ns) t->blk_kind = k, t->blk_t0 = t->blk_cred = now_ns();   // (under G: the report reads them)
    int64_t r = block_inner2(t, kind, ns);
    uint64_t t1 = now_ns();
    if (ns) {
        t->st_block_ns[k] += t1 - t->blk_cred, t->st_blocks++;
        t->blk_t0 = 0;
        if (t->woke && t->woke_ns && t1 > t->woke_ns) t->st_wake_ns += t1 - t->woke_ns, t->st_wakes++;
    }
    return r;
}
static int64_t block_inner2(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    if (kind == 1) pi_lock_wait(t);
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = gc_wait(&t->cv);
        } else rc = ns > 0 ? gc_timedwait(&t->cv, &dl) : gc_wait(&t->cv);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
uint32_t (*dm_thread_tick_fn)(void);   // the current game tick, when diagnostics register it
static void *thread_main(void *p) {
    thr_t *t = p;
    t_self = t;
    t->os_tid = os_tid_self();
    preempt_point_set(t);
    plat_thread_start();
    Cpu *c = guest_cpu_new(t->stack_top, guest_tls_alloc());
    guest_cpu_set(c);
    ap_thread_start(t);
    g_lock(); while (!t->started) gc_wait(&t->cv); core_take(t); g_unlock();
    c->x[0] = t->arg;
    guest_call(c, t->entry);   // does not return: the guest leaves through ExitThread (svc 0x0A)
    die("guest thread entry returned (Horizon requires ExitThread)");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    gc_destroy(&t->cv); free(t);
}

static _Noreturn void thread_exit_on_host(void *argument) {
    thr_t *t=argument;
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    g_lock();
    core_put(t);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    t_self = NULL;
    rt_preempt_flag = NULL;
    if (t->closed) thr_free(t);
    g_unlock();
    pthread_exit(NULL);
    __builtin_unreachable();   // winpthreads does not declare pthread_exit noreturn
}
static _Noreturn void thread_exit(thr_t *t) { thread_exit_on_host(t); }   // already on the thread's host stack

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    thr_t *t = calloc(1, sizeof *t);
    gc_init(&t->cv);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self();
    t->cur_core = -1; t->os_tid = os_tid_self();
    t_self = t;
    ap_thread_start(t);
    g_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; g_main_thr = t; core_take(t); g_unlock();
    t_self = t;
    preempt_point_set(t);
    return t->handle;
}

static _Atomic uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    g_lock();
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) gc_signal(&t->cv);
    g_unlock();
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
/* Yield back-off (after nxnative e06a8ec / b703be2). On the Switch a yield with nothing else runnable costs nothing; here
   a guest loop that polls and yields becomes a host busy spin (on the iPhone the system logged about 1,300 wake-ups a
   second for the game). A run of yields with no other SVC between them stays plain sched_yield for its first 2 ms:
   shorter waits are handoffs between working threads, such as the main thread waiting for its ModuleSystem workers on
   the frame's critical path (nxnative started at 200 µs and 1.4.3 then dropped itself to 20 fps). After that each yield
   sleeps an eighth of the run so far, 10 to 250 µs, with the virtual core given up, so a wait ends at most an eighth late.
   Off by default: on the Mac at about 100 fps (1.2.1 field save, 300 s each) it saved 3% CPU (708 -> 685 s) but frames
   dropped steadily, 10-15 per 300 instead of 0-3. TOTK_YIELD_BACKOFF=on turns it on (250:2000); =<cap µs>[:<start µs>]
   sets the cap and the start. Returns the sleep for a yield whose run started run_ns ago, 0 for a plain yield. */
static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) {
        const char *e = getenv("TOTK_YIELD_BACKOFF"); int64_t c = 0;
        if (e && !strcmp(e, "on")) c = 250000;
        else if (e && *e) { c = atoll(e) * 1000; const char *colon = strchr(e, ':'); if (colon) start = atoll(colon + 1) * 1000; }
        if (c) LOG("TOTK_YIELD_BACKOFF: a run of yields backs off after %lld us, sleeping at most %lld us", (long long)(start / 1000), (long long)(c / 1000));
        cap = c;
    }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}

int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = t_self; if (me) { me->last_svc = num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; } }

    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        if(!regs[1]||!regs[3]||(regs[3]&15)||(int)regs[4]<28||(int)regs[4]>59||(int)regs[5]<-3||(int)regs[5]>2){regs[0]=R_BAD_STATE;return 1;}
        thr_t *t = calloc(1, sizeof *t);
        if(!t)die("thread allocation");
        t->cur_core = -1;
        gc_init(&t->cv);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        g_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; g_unlock();
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);   // also the host-service stack of this guest thread
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx", t->handle,t->entry,t->stack_top);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; gc_signal(&t->cv); }
        g_unlock();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];

        thr_t *self = cur();
        if (ns > 0) { g_lock(); core_put(self); g_unlock(); }
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) {
#ifdef _WIN32   // winpthreads' nanosleep counts whole ms: the SDK's 20 us sleeps did not sleep at all (two takes of G
                // each), and 1 ms or more came up to 1 ms late. A high-resolution timer, spinning the last 0.2 ms,
                // with the core already given up.
            uint64_t plat_now_ns(void); void plat_sleep_until_ns(uint64_t t);
            plat_sleep_until_ns(plat_now_ns() + (uint64_t)ns);
#else            // exact on Linux (timer slack 1 us, platform/linux.c)
            struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0);
#endif
        }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = t_self;
            uint64_t y0 = now_ns();
            if (me && me->nsvc != me->yrun_svc + 1) me->yrun_ns = y0;   // another SVC since the last yield: a new run
            if (me) me->yrun_svc = me->nsvc;
            g_lock();   // Horizon yield: hand the core to a waiting thread of equal or higher priority
            int give = 0, mp = me->cur_core >= 0 ? eff_prio(me) : 0;
            for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && eff_prio(w) <= mp;
            if (give) { core_put(me); core_take(me); }
            g_unlock();
            uint64_t backoff = !give && me ? yield_backoff_ns(y0 - me->yrun_ns) : 0;
            if (backoff) {   // a long poll: sleep with the core given up, as SleepThread(ns) does
                g_lock(); core_put(me); g_unlock();
                struct timespec ts = {0, (long)backoff}; nanosleep(&ts, 0);
                g_lock(); core_take(me); g_unlock();
            } else if (!give) sched_yield();
            if (me && me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
        }
        if (ns > 0) { g_lock(); core_take(self); g_unlock(); }

        return 1;
    }
    case 0x0C: { g_lock();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; g_unlock();return 1; }
    case 0x0D: { if(regs[1]<28||regs[1]>59){regs[0]=R_BAD_STATE;return 1;}g_lock();thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE;g_unlock(); return 1; }
    case 0x0E: { g_lock();thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0;g_unlock(); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        g_lock();thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
        regs[0] = t ? 0 : R_BAD_HANDLE;g_unlock(); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }   // GetCurrentProcessorNumber
    case 0x25: { g_lock();thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0;g_unlock(); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        g_lock();
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: g_lock(); lock_handoff(regs[0]); g_unlock(); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        g_lock();
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        g_lock();
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        g_unlock();
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        g_lock();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        g_lock();
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;   // Horizon: value - 2 when it wakes all waiters
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        g_lock(); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); g_unlock();
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        g_lock();
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if(n<0||n>64||(!hs&&n)){regs[0]=R_BAD_STATE;return 1;}
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        g_lock();
        t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        int64_t r; int idx = -1;
        // Horizon's order (KSynchronizationObject::Wait): a signalled object, then timeout 0, then a pending cancel. A
        // poll or a wait on a signalled object leaves the cancel to the next wait that would block.
        for (;;) {
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns == 0 || (ns > 0 && left <= 0)) { r = R_TIMEOUT; break; }
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int64_t w = block(t, 4, left);
            if (w == R_TIMEOUT) { r = R_TIMEOUT; break; }
            if (w == (int64_t)R_CANCELLED) { t->cancel = 0; r = R_CANCELLED; break; }
        }
        g_unlock();
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, (int64_t)R_CANCELLED); }
        g_unlock();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        g_lock();
        unsigned i = (uint32_t)regs[0] - 0x1000;int valid=0;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            valid=1;event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid=1;void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            g_unlock();
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            g_lock();
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid=1;thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        g_unlock();
        regs[0] = valid?0:R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!t_self) die("hle_bind_system_event 须在客体线程上调用");
    static void *create, *getw;
    if (!create) { create = runtime_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = runtime_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = (uint32_t)GCALL(create, (uint64_t)(uintptr_t)ev, (uint64_t)autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = (uint32_t)GCALL(getw, (uint64_t)(uintptr_t)ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    g_lock();
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters();
    g_unlock();
}

// ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ----------
// Weak fallbacks keep standalone thread tests linkable; the runtime links the real ipc.c versions.
__attribute__((weak)) void runtime_ipc_session_closed(void *s) { (void)s; die("IPC session closed without ipc.c"); }
__attribute__((weak)) void runtime_ipc_tmem_closed(void *t) { (void)t; die("transfer memory closed without ipc.c"); }
uint32_t runtime_handle_new(int session, void *obj) { g_lock(); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj); g_unlock(); return h; }
void *runtime_handle_get(uint32_t h, int session) { g_lock(); void *o = h_peek(h, session ? O_SESSION : O_TMEM); g_unlock(); return o; }
// Service-side event: returns the writable handle kept by the service.
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    g_lock(); uint32_t hw = h_new(O_EVENT_W, e); g_unlock(); return hw;
}
// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t runtime_event_readable(uint32_t hw) {
    g_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e); g_unlock(); return hr;
}
void runtime_event_signal(uint32_t hw) {
    g_lock(); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->signaled = 1; wake_sync_waiters(); g_unlock();
}

// ---------- Diagnostics ----------
// MacNativeGames dumps every guest thread's wait state and guest return chain through Mach APIs (TOTK_DUMP_AFTER).
// TODO(port): a Windows version for hang diagnosis.
void runtime_dump_threads(void) { LOG("thread dump not ported yet"); }
