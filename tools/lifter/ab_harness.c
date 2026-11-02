// A/B check of the lifter (tools/lifter/ab_check.py): every picked guest function is compiled twice, as gen.py emitted
// it with registers and flags in the Cpu (o_<addr>) and with them in C locals (n_<addr>), and both run from the same
// random states. Development tool: the module image it maps is made from the player's own dump at run time.
//
// The module image is mapped at its base, writable, with a write watch: after each run the pages it wrote are hashed
// and put back. x0-x7 point into a scratch buffer (or hold small integers), the buffer holds pointers into itself and
// random data, SP sits near its top. Every call and indirect branch goes to one stub (ab_stub) on both sides instead of
// its callee: it folds the guest state at the call into a digest and sets the caller-saved registers from it, so a
// register the locals version did not store before the call, or did not load after it, shows. A fault or trap ends a
// run (vectored handler, longjmp); a run that does not finish in 2 s ends the process (the caller skips the function).
#include "cpu.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct { uint64_t addr; GuestFn old_fn, new_fn; } ab_fn;
extern const ab_fn ab_fns[];
extern const int ab_nfns;

volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) { (void)a; (void)lr; (void)x0; }

enum { BUF_SIZE = 0x40000 };
#define BUF 0x7200000000ull
#define PTR_LO (BUF + 0x8000)
#define PTR_HI (BUF + 0x28000)
#define STACK_TOP (BUF + 0x3F000)

static uint64_t mix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static jmp_buf *g_jb;
static volatile int g_status;   // 0 finished, 1 fault, 2 trap or SVC, 3 a loop around calls
static uint64_t g_digest;
static int g_calls;
void ab_stub(Cpu *c) {
    uint64_t h = g_digest ^ 0x5bd1e995u;
    for (int i = 0; i < 31; i++) h = mix(h ^ c->x[i]);
    h = mix(h ^ c->sp);
    for (int i = 0; i < 8; i++) h = mix(h ^ (uint64_t)c->v[i]);
    g_digest = h;
    if (++g_calls > 100000) { g_status = 3; longjmp(*g_jb, 1); }
    for (int i = 0; i <= 18; i++) c->x[i] = mix(h + (uint64_t)i);
    c->x[30] = mix(h + 30);
    for (int i = 0; i < 8; i++) c->v[i] = mix(h + 100 + (uint64_t)i);
    c->nzcv = h & 0xF0000000u;
}
void rt_dispatch(Cpu *c) { ab_stub(c); }

static volatile int g_in_run;
static volatile uint64_t g_fault_at;
void rt_udf(Cpu *c, uint64_t pc) { (void)c; g_status = 2; g_fault_at = pc; longjmp(*g_jb, 1); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; g_status = 2; g_fault_at = pc; longjmp(*g_jb, 1); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; g_status = 2; g_fault_at = pc; longjmp(*g_jb, 1); }
uint64_t rt_ticks(void) { return 0; }
void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; }

static LONG CALLBACK veh(EXCEPTION_POINTERS *e) {
    if (!g_in_run) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_DATATYPE_MISALIGNMENT) {
        g_status = 1;
        g_fault_at = code == EXCEPTION_ACCESS_VIOLATION ? (uint64_t)e->ExceptionRecord->ExceptionInformation[1] : code;
        g_in_run = 0;
        longjmp(*g_jb, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static uint8_t *g_img, *g_pristine;
static uint64_t g_base, g_size;
static volatile LONG64 g_run_started;

static DWORD WINAPI watchdog(void *p) {
    (void)p;
    for (;;) {
        Sleep(200);
        LONG64 t = g_run_started;
        if (t && GetTickCount64() - (ULONGLONG)t > 2000) { fprintf(stdout, "HANG\n"); fflush(stdout); ExitProcess(3); }
    }
}

typedef struct { uint64_t x[31], sp, v[32][2], digest, mem, buf; int calls, status; } result;

static void state(Cpu *c, uint64_t seed) {
    memset(c, 0, sizeof *c);
    uint8_t *b = (uint8_t *)(uintptr_t)BUF;
    uint64_t s = seed;
    for (size_t i = 0; i < BUF_SIZE; i += 8) {
        s = mix(s);
        uint64_t v = (s & 3) == 0 ? PTR_LO + ((s >> 8) % (PTR_HI - PTR_LO) & ~7ull) : (s & 3) == 1 ? (s >> 40) & 0xff : s;
        memcpy(b + i, &v, 8);
    }
    for (int i = 0; i < 31; i++) {
        s = mix(s);
        int k = (int)(s & 3);
        c->x[i] = k == 0 || (i < 8 && k == 1) ? PTR_LO + ((s >> 8) % (PTR_HI - PTR_LO) & ~7ull) : k == 2 ? (s >> 32) & 0xff : s;
    }
    c->x[30] = 0x7210000000ull;
    c->sp = STACK_TOP;
    for (int i = 0; i < 32; i++) { s = mix(s); c->v[i] = ((__uint128_t)mix(s) << 64) | s; }
    s = mix(s);
    c->nzcv = s & 0xF0000000u;
}

static void run(GuestFn f, uint64_t seed, result *r) {
    static Cpu cpu __attribute__((aligned(64)));
    Cpu *c = &cpu;
    state(c, seed);
    g_digest = 0; g_calls = 0; g_status = 0; g_fault_at = 0;
    jmp_buf jb;
    g_jb = &jb;
    g_run_started = (LONG64)GetTickCount64();
    if (!setjmp(jb)) {
        g_in_run = 1;
        f(c);
        g_in_run = 0;
    }
    g_in_run = 0;
    g_run_started = 0;
    memset(r, 0, sizeof *r);
    r->status = g_status;
    if (r->status) return;
    memcpy(r->x, c->x, sizeof r->x);
    r->sp = c->sp;
    memcpy(r->v, c->v, sizeof r->v);
    r->digest = g_digest; r->calls = g_calls;
    r->buf = fnv(0xcbf29ce484222325ull, (void *)(uintptr_t)BUF, BUF_SIZE);
    // module pages written: hashed, then put back
    static void *pages[1 << 16];
    ULONG_PTR n = sizeof pages / sizeof *pages; DWORD gran;
    uint64_t h = 0xcbf29ce484222325ull;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_img, g_size, pages, &n, &gran) == 0)
        for (ULONG_PTR i = 0; i < n; i++) {
            uint8_t *p = pages[i];
            h = fnv(h, &p, sizeof p); h = fnv(h, p, gran);
            memcpy(p, g_pristine + (p - g_img), gran);
        }
    r->mem = h;
    ResetWriteWatch(g_img, g_size);
}
static void restore_after_fault(void) {   // a run that faulted may have written module pages too
    static void *pages[1 << 16];
    ULONG_PTR n = sizeof pages / sizeof *pages; DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_img, g_size, pages, &n, &gran) == 0)
        for (ULONG_PTR i = 0; i < n; i++) memcpy(pages[i], g_pristine + ((uint8_t *)pages[i] - g_img), gran);
    ResetWriteWatch(g_img, g_size);   // the copies back are writes too
}

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s <image> <base> <from> <cases> <seed>\n", argv[0]); return 2; }
    g_base = strtoull(argv[2], NULL, 0);
    int from = atoi(argv[3]), cases = atoi(argv[4]);
    uint64_t seed = strtoull(argv[5], NULL, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); g_size = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
    g_img = VirtualAlloc((void *)(uintptr_t)g_base, g_size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    g_pristine = malloc(g_size);
    void *buf = VirtualAlloc((void *)(uintptr_t)BUF, BUF_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_img || !g_pristine || !buf || fread(g_pristine, 1, g_size, f) != g_size) { fprintf(stderr, "cannot map\n"); return 2; }
    fclose(f);
    memcpy(g_img, g_pristine, g_size);
    ResetWriteWatch(g_img, g_size);
    AddVectoredExceptionHandler(1, veh);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    int self_check = getenv("AB_SELF") != NULL;   // the Cpu version against itself: what the harness alone gives
    for (int i = from; i < ab_nfns; i++) {
        printf("FN %d %llx\n", i, (unsigned long long)ab_fns[i].addr);
        fflush(stdout);
        int ok = 0, bad = 0, faults = 0;
        for (int k = 0; k < cases; k++) {
            uint64_t s = mix(seed ^ ab_fns[i].addr ^ ((uint64_t)k << 48));
            result a, b;
            run(ab_fns[i].old_fn, s, &a);
            if (a.status) restore_after_fault();
            run(self_check ? ab_fns[i].old_fn : ab_fns[i].new_fn, s, &b);
            if (b.status) restore_after_fault();
            if (a.status && b.status) { faults++; continue; }
            if (a.status != b.status || memcmp(&a, &b, sizeof a)) {
                bad++;
                if (bad <= 2) {
                    printf("  case %d: status %d/%d calls %d/%d digest %d mem %d buf %d sp %d", k, a.status, b.status, a.calls, b.calls,
                           a.digest != b.digest, a.mem != b.mem, a.buf != b.buf, a.sp != b.sp);
                    for (int r = 0; r < 31; r++) if (a.x[r] != b.x[r]) printf(" x%d", r);
                    for (int r = 0; r < 32; r++) if (memcmp(a.v[r], b.v[r], 16)) printf(" v%d", r);
                    printf("\n");
                }
            } else ok++;
        }
        printf("RESULT %d ok %d bad %d faults %d\n", i, ok, bad, faults);
        fflush(stdout);
    }
    printf("DONE\n");
    return 0;
}
