// Horizon host for the recompiled game: what MacNativeGames' runtime/runtime.h declares, for the parts ported here.
// The game's code is recompiled C working on a Cpu (src/runtime/cpu.h), so the host reaches guest registers through
// Cpu and calls guest functions through guest_call (gcall below), never as native function pointers.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#include "../runtime/rt.h"

_Noreturn void runtime_fail(const char *message);
uint64_t runtime_now_ns(void);
void *runtime_symbol(const char *name);  // guest address of an exported symbol (as a pointer), NULL if none

// ---- the calling guest thread (guest.c)
Cpu *guest_cpu(void);                    // this thread's guest CPU state, NULL on a pure host thread
void guest_cpu_set(Cpu *c);
Cpu *guest_cpu_new(uint64_t stack_top, uint64_t tls);
uint64_t guest_tls_alloc(void);          // a thread's TLS block (tpidrro_el0): IPC message buffer at +0, 0x200 bytes
// Call a guest function with integer/pointer arguments from host code running on a guest thread. The caller's guest
// registers are saved and restored around the call (host code may run inside an SVC or a host replacement).
uint64_t guest_call_args(uint64_t fn, int n, const uint64_t *args);
// Before the OS itself writes guest memory (file reads into a game buffer): guest pages are committed on the first
// CPU access through a fault, which the kernel's own writes do not take, so the range is committed first.
static inline void guest_prepare_write(void *addr, uint64_t size) {
    uint64_t a = (uint64_t)(uintptr_t)addr;
    if (size && a >= GUEST_LO && a < GUEST_HI) plat_commit(a, size);
}
#define GCALL(fn, ...) guest_call_args((uint64_t)(uintptr_t)(fn), \
    (int)(sizeof((uint64_t[]){0, __VA_ARGS__}) / sizeof(uint64_t)) - 1, (uint64_t[]){0, __VA_ARGS__} + 1)
#define GCALL0(fn) guest_call_args((uint64_t)(uintptr_t)(fn), 0, NULL)

// ---- svc.c
uint64_t runtime_svc(unsigned num, uint64_t *regs, uint64_t pc);
void runtime_memory_init(void);
uint64_t runtime_image_bytes(void);
uint64_t runtime_main_stack(uint64_t size);  // the main thread's guest stack (top), from the stack region

// ---- threads.c
uint32_t svc_thread_main_init(void);
int svc_thread(int num, uint64_t *regs);
void runtime_trace_guest(uint64_t fp, uint64_t lr);
void host_services_register(void);  // services_*.c: IPC services the SDK opens
void hle_bind_system_event(void *event, int autoclear);
void hle_signal_system_event(void *event);
uint32_t runtime_thread_current_handle(void);

// ---- loader (src/runtime/loader.c): module memory for QueryMemory / SetMemoryPermission
int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission,
                         unsigned *attribute);

// ---- input (overrides_input.c, mouse_camera.c), fed by the window (window_sdl.c)
#include "mouse_camera.h"
typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_set(const RuntimeGamepadState *s);
void runtime_input_key(unsigned code, int down);   // Mac virtual key codes (MacNativeGames' key layout)
void runtime_input_clear(void);
void runtime_input_mouse_config(int enabled, float sensitivity, int invert_y);
void runtime_input_mouse_motion(float dx, float dy);
void runtime_input_mouse_clear(void);
void runtime_input_mouse_frame(uint64_t now_ns);

// ---- window (window_sdl.c): SDL3 on the main thread
int window_init(void);              // 1 = window up (presentation may still be missing: logged)
_Noreturn void window_loop(void);

// ---- rumble and motion (hid_feedback.c <- window_sdl.c, motion_fusion.c; MacNativeGames runtime.h)
#define RUNTIME_MOTION_MAX 32
typedef struct { uint64_t sampling, delta_ns; float acc[3], gyro[3], angle[3], dir[9]; } RuntimeMotionState;   // G, rev/s, rev, 3 axes
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high);   // side 0 left, 1 right
int runtime_motion_read(RuntimeMotionState *out, int max);   // newest first; 0 = no motion now
void runtime_motion_start(void);   // the game (re)started its sensor: angle and direction start over
// One sample in the Switch six-axis frame (x right, y toward the top edge, z out of the face): acceleration in G with
// gravity (face up reads z = -1), rotation in rad/s.
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int runtime_motion_axes(const char *spec);   // TOTK_GYRO_AXES ("-x,y,-z"): 1 set, 0 unset, -1 malformed
void runtime_motion_deadzone_default(double deg_s);
void runtime_motion_live(int on);   // a controller with motion attached (1) / detached (0)
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads);

// ---- profiler.c: sampling CPU profiler for development (TOTK_PROF=<start s>:<seconds>[:<report path>])
void runtime_profiler_start(void);
uint64_t runtime_symbolize(uint64_t rip, char *out, size_t n);   // host address -> function (f_<guest> + guest symbol)
void runtime_watch_tick(uint64_t present);   // TOTK_WATCH: arms / reports the memory write watch (watch.c)
