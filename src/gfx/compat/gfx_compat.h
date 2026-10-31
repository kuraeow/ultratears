// Windows stand-ins for the macOS APIs MacNativeGames' graphics front end (d2r_gfx/gfx/nvn) uses.
// - Mach time: mach_absolute_time() is the runtime's monotonic nanoseconds (timebase 1/1), mach_wait_until() sleeps
//   precisely until such a time.
// - Mach thread/task statistics and real-time thread policies: unavailable (KERN_FAILURE); MacNativeGames only logs
//   them, and every caller checks the result.
// - QoS classes: no-ops (Windows thread priorities are set where the port needs them).
// - sigsetjmp guards: plain setjmp (guest pages are committed on first touch, so guarded guest reads do not fault).
// - malloc_size: _msize.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef __linux__
int pthread_setname_np(pthread_t thread, const char *name);
#endif
#include <sched.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

uint64_t plat_now_ns(void);
void plat_sleep_until_ns(uint64_t t);

// ---- Mach time
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *tb) { tb->numer = 1; tb->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void) { return plat_now_ns(); }
static inline uint64_t mach_continuous_time(void) { return plat_now_ns(); }
static inline int mach_wait_until(uint64_t t) { plat_sleep_until_ns(t); return 0; }
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

// ---- Mach kernel calls MacNativeGames uses for statistics: always "failed"
typedef int kern_return_t;
typedef int boolean_t;
typedef unsigned mach_port_t;
typedef unsigned mach_msg_type_number_t;
typedef uintptr_t vm_address_t;
typedef size_t vm_size_t;
typedef int *thread_info_t, *task_info_t, *thread_policy_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
typedef struct { int seconds, microseconds; } time_value_t;
typedef struct { time_value_t user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int32_t pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
typedef struct { uint64_t virtual_size, resident_size, phys_footprint, compressed; } task_vm_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT 10
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT 28
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT 4
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT 80
static inline mach_port_t mach_thread_self(void) { return 0; }
static inline mach_port_t mach_task_self(void) { return 0; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
static inline kern_return_t thread_info(mach_port_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_info(mach_port_t t, int f, task_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
extern void plat_thread_realtime(uint64_t period_ns);
static inline kern_return_t thread_policy_set(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t n) {
    (void)t; (void)f; (void)n;
    if (p) {
        const thread_time_constraint_policy_data_t *tcp = (const thread_time_constraint_policy_data_t *)p;
        uint64_t period_ns = tcp->period ? (uint64_t)tcp->period : 16666666ULL;
        plat_thread_realtime(period_ns);
    } else {
        plat_thread_realtime(16666666ULL);
    }
    return KERN_SUCCESS;
}
static inline kern_return_t thread_policy_get(mach_port_t t, int f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *d) { (void)t; (void)f; (void)p; (void)n; (void)d; return KERN_FAILURE; }
static inline kern_return_t vm_read_overwrite(mach_port_t t, vm_address_t a, vm_size_t n, vm_address_t d, vm_size_t *got) { (void)t; (void)a; (void)n; (void)d; *got = 0; return KERN_FAILURE; }

// ---- QoS
typedef int qos_class_t;
#define QOS_CLASS_UNSPECIFIED 0
#define QOS_CLASS_USER_INTERACTIVE 0x21
#define QOS_CLASS_USER_INITIATED 0x19
static inline int pthread_attr_set_qos_class_np(pthread_attr_t *a, qos_class_t q, int r) { (void)a; (void)q; (void)r; return 0; }
static inline int pthread_set_qos_class_self_np(qos_class_t q, int r) { (void)q; (void)r; return 0; }
static inline int pthread_get_qos_class_np(pthread_t t, qos_class_t *q, int *r) { (void)t; *q = QOS_CLASS_UNSPECIFIED; *r = 0; return 0; }

// ---- signals and guarded reads
#ifndef SIGBUS
#define SIGBUS 10
#endif
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(b, m) setjmp(b)
#define siglongjmp(b, v) longjmp(b, v)

// ---- resource usage (statistics only)
struct rusage { struct { long tv_sec, tv_usec; } ru_utime, ru_stime; long ru_maxrss, ru_minflt, ru_majflt, ru_nvcsw, ru_nivcsw; };
#define RUSAGE_SELF 0
static inline int getrusage(int who, struct rusage *r) { (void)who; memset(r, 0, sizeof *r); return -1; }

// ---- memory
#ifdef __linux__
#define malloc_size(p) malloc_usable_size(p)
#else
#define malloc_size(p) _msize(p)
#endif
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define MAP_FAILED ((void *)-1)
static inline void *mmap(void *a, size_t n, int p, int f, int fd, long o) { (void)a; (void)n; (void)p; (void)f; (void)fd; (void)o; return MAP_FAILED; }
static inline int munmap(void *a, size_t n) { (void)a; (void)n; return -1; }
static inline int mprotect(void *a, size_t n, int p) { (void)a; (void)n; (void)p; return -1; }
#ifndef __linux__
static inline int getpagesize(void) { return 4096; }
#endif

// ---- threads: macOS names only the calling thread
static inline int gfx_setname(const char *n) { return pthread_setname_np(pthread_self(), n); }
#define pthread_setname_np(n) gfx_setname(n)

// ---- more Darwin calls: the thread id and per-clock nanoseconds
#ifdef __linux__
#include <sys/syscall.h>
static inline int pthread_threadid_np(void *t, uint64_t *id) { (void)t; *id = (uint64_t)syscall(SYS_gettid); return 0; }
#elif defined(_WIN32)
unsigned long __stdcall GetCurrentThreadId(void);
static inline int pthread_threadid_np(void *t, uint64_t *id) { (void)t; *id = GetCurrentThreadId(); return 0; }
#endif
static inline uint64_t clock_gettime_nsec_np(clockid_t clk) {
    struct timespec ts;
    if (clock_gettime(clk, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
