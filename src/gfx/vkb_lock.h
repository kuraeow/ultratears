// Contention of the replayer's shared tables (development measurement, the per-frame log line): a lock that is taken
// at once costs nothing extra; a contended one adds its wait to its kind's total.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

enum { LK_TEXREG, LK_IMAGES, LK_POOLS, LK_COUNT };   // texture registry (vkb_draw.c), images (vkb_tex.c), memory pools (backend_vk.c)
extern _Atomic uint64_t vkb_lock_wait_ns[LK_COUNT], vkb_lock_waits[LK_COUNT];
uint64_t runtime_now_ns(void);
// Time spent in some replay steps (development measurement, the per-frame log line)
// new images (vkCreateImage + memory), uploads from game memory, queue submits; memory for the upload rings, descriptors
// and uniform copies; the uniform snapshot at a submission's start; pool copies brought up to date before a submit
enum { PK_CREATE, PK_UPLOAD, PK_SUBMIT, PK_ALLOC, PK_UCAP, PK_SYNC, PK_COUNT };
extern _Atomic uint64_t vkb_step_ns[PK_COUNT], vkb_step_n[PK_COUNT];
static inline void vkb_step_add(int kind, uint64_t t0) {
    atomic_fetch_add_explicit(&vkb_step_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_step_n[kind], 1, memory_order_relaxed);
}
static inline void vkb_lock(pthread_mutex_t *m, int kind) {
    if (!pthread_mutex_trylock(m)) return;
    uint64_t t0 = runtime_now_ns();
    pthread_mutex_lock(m);
    atomic_fetch_add_explicit(&vkb_lock_wait_ns[kind], runtime_now_ns() - t0, memory_order_relaxed);
    atomic_fetch_add_explicit(&vkb_lock_waits[kind], 1, memory_order_relaxed);
}

// A reader-writer lock for the tables the recording threads read for every draw (pools, the texture registry): readers
// do not exclude each other. Windows SRWLOCK (pointer-sized, zero = unlocked); backend_vk.c implements it.
typedef struct { void *opaque; } vkb_rw;
void vkb_rw_read(vkb_rw *l, int kind);
void vkb_rw_read_end(vkb_rw *l);
void vkb_rw_write(vkb_rw *l, int kind);
void vkb_rw_write_end(vkb_rw *l);

// Counters the recording threads bump per draw (the per-frame log lines): one cache line per recording thread, summed
// when read, so that threads do not contend for one counter.
#define VKB_MAXREC 8   // recording threads at most (vkb_mt.c), the translation thread included
typedef struct { uint64_t v; char pad[56]; } vkb_cnt[VKB_MAXREC];
int vkb_worker(void);   // vkb_mt.c: this thread's recording index (0: the translation thread, or any other thread)
static inline void vkb_cnt_add(vkb_cnt c, uint64_t n) { c[vkb_worker()].v += n; }
static inline uint64_t vkb_cnt_sum(const vkb_cnt c) { uint64_t s = 0; for (int i = 0; i < VKB_MAXREC; i++) s += c[i].v; return s; }
