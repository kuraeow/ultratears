/* Generic PC mouse camera layer: raw delta accumulation, look ownership state machine
 * (mouse vs right stick), angle conversion, FOV zoom scaling, optional exponential
 * target-angle smoothing, and single-consumption tick lifecycle.
 * Independent of TOTK guest memory addresses. */
#include "mouse_camera.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STICK_TAKEOVER_THRESHOLD 0.25f
#define STICK_REARM_THRESHOLD    0.15f
#define STICK_TAKEOVER_NS        50000000ULL   /* 50 ms above 25% radial magnitude */
#define STICK_REARM_NS           100000000ULL  /* 100 ms below 15% radial magnitude */
#define MOUSE_TAKEOVER_COUNTS    1.0           /* 1.0 count accumulated radial motion */

static pthread_mutex_t g_mc_lock = PTHREAD_MUTEX_INITIALIZER;

static MouseCameraConfig g_cfg = {
    .version = 2,
    .mouse_mode = 0,
    .backend = MOUSE_BACKEND_NATIVE,
    .look_deg_per_count = 0.04,
    .aim_multiplier = 1.0,
    .scope_multiplier = 1.0,
    .smooth_ms = 0,
    .invert_y = 0,
    .legacy_sensitivity = 1.0f,
    .trace_enabled = 0,
};

static LookOwner g_owner = LOOK_OWNER_MOUSE;
static bool g_captured = false;
static uint64_t g_epoch = 1;

/* Pending raw mouse deltas (double precision) */
static double g_pending_dx = 0.0;
static double g_pending_dy = 0.0;

/* Right-stick ownership handoff state */
static bool g_stick_armed = true;
static bool g_stick_in_neutral = true;
static uint64_t g_stick_neutral_since_ns = 0;
static bool g_stick_deflecting = false;
static uint64_t g_stick_deflect_since_ns = 0;
static float g_last_stick_mag = 0.0f;

/* Optional exponential smoothing state (target-angle remainder in radians) */
static double g_smooth_rem_yaw = 0.0;
static double g_smooth_rem_pitch = 0.0;

/* Tick tracking & single-consumption state */
static MouseCameraClockFn g_clock_fn = NULL;
static bool g_has_last_tick = false;
static uint64_t g_last_tick_id = 0;
static uint64_t g_last_camera_gen = 0;
static CameraContext g_last_context = CAMERA_CTX_UNCLASSIFIED;
static LookCommand g_current_cmd;
static bool g_cmd_needs_apply = false;
static bool g_cmd_applied = false;
static bool g_compat_sampled_by_hid = false;
static uint64_t g_unapplied_count = 0;
static uint64_t g_applied_count = 0;
static uint64_t g_unclassified_count = 0;

void mouse_camera_set_clock_fn(MouseCameraClockFn fn) {
    pthread_mutex_lock(&g_mc_lock);
    g_clock_fn = fn;
    pthread_mutex_unlock(&g_mc_lock);
}

uint64_t mouse_camera_now_ns(void) {
    MouseCameraClockFn fn;
    pthread_mutex_lock(&g_mc_lock);
    fn = g_clock_fn;
    pthread_mutex_unlock(&g_mc_lock);
    if (fn) return fn();
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * (1e9 / (double)freq.QuadPart));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

float mouse_camera_wrap_pi(double angle_rad) {
    if (!isfinite(angle_rad)) return 0.0f;
    double wrapped = atan2(sin(angle_rad), cos(angle_rad));
    return (float)wrapped;
}

float mouse_camera_clamp_pitch(double pitch_rad, float min_rad, float max_rad) {
    if (!isfinite(pitch_rad)) return 0.0f;
    double lo = (double)min_rad;
    double hi = (double)max_rad;
    if (lo > hi) {
        double tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (pitch_rad < lo) return (float)lo;
    if (pitch_rad > hi) return (float)hi;
    return (float)pitch_rad;
}

void mouse_camera_clamp_stick_vector(double vx, double vy, int32_t *out_rx, int32_t *out_ry) {
    if (!isfinite(vx) || !isfinite(vy)) {
        if (out_rx) *out_rx = 0;
        if (out_ry) *out_ry = 0;
        return;
    }
    double mag = hypot(vx, vy);
    if (mag > 32767.0) {
        double scale = 32767.0 / mag;
        vx *= scale;
        vy *= scale;
    }
    int32_t rx = (int32_t)llround(vx);
    int32_t ry = (int32_t)llround(vy);
    if (rx > 32767) rx = 32767;
    if (rx < -32767) rx = -32767;
    if (ry > 32767) ry = 32767;
    if (ry < -32767) ry = -32767;
    if (out_rx) *out_rx = rx;
    if (out_ry) *out_ry = ry;
}

static void clear_motion_locked(MouseResetReason reason) {
    (void)reason;
    g_pending_dx = 0.0;
    g_pending_dy = 0.0;
    g_smooth_rem_yaw = 0.0;
    g_smooth_rem_pitch = 0.0;
    g_cmd_needs_apply = false;
    g_cmd_applied = false;
    g_compat_sampled_by_hid = false;
    g_current_cmd.route = LOOK_ROUTE_NONE;
    g_current_cmd.yaw_delta_rad = 0.0f;
    g_current_cmd.pitch_delta_rad = 0.0f;
    g_current_cmd.compat_rx = 0;
    g_current_cmd.compat_ry = 0;
    g_epoch++;
}

static double parse_double_env(const char *name, double def_val, double min_val, double max_val) {
    const char *s = getenv(name);
    if (!s || !*s) return def_val;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v) || v < min_val || v > max_val) return def_val;
    return v;
}

void mouse_camera_init_from_env(void) {
    MouseCameraConfig cfg;
    cfg.version = 2;
    const char *ver = getenv("TOTK_MOUSE_CAMERA_VERSION");
    if (ver && atoi(ver) > 0) cfg.version = atoi(ver);

    const char *mode = getenv("TOTK_CAMERA_MODE");
    cfg.mouse_mode = (mode && !strcmp(mode, "mouse")) ? 1 : 0;

    const char *backend = getenv("TOTK_MOUSE_BACKEND");
    if (backend && !strcmp(backend, "legacy_stick"))
        cfg.backend = MOUSE_BACKEND_LEGACY_STICK;
    else
        cfg.backend = MOUSE_BACKEND_NATIVE;

    cfg.look_deg_per_count = parse_double_env("TOTK_MOUSE_LOOK_DEG_PER_COUNT", 0.04, 0.001, 1.0);
    cfg.aim_multiplier = parse_double_env("TOTK_MOUSE_AIM_MULTIPLIER", 1.0, 0.1, 2.0);
    cfg.scope_multiplier = parse_double_env("TOTK_MOUSE_SCOPE_MULTIPLIER", 1.0, 0.1, 2.0);

    int smooth = 0;
    const char *sm = getenv("TOTK_MOUSE_SMOOTH_MS");
    if (sm) {
        int val = atoi(sm);
        if (val == 0 || val == 5 || val == 10) smooth = val;
    }
    cfg.smooth_ms = smooth;

    const char *inv = getenv("TOTK_MOUSE_INVERT_Y");
    cfg.invert_y = (inv && !strcmp(inv, "1")) ? 1 : 0;

    cfg.legacy_sensitivity = (float)parse_double_env("TOTK_MOUSE_SENSITIVITY", 1.0, 0.1, 5.0);

    const char *tr = getenv("TOTK_MOUSE_TRACE");
    cfg.trace_enabled = (tr && !strcmp(tr, "1")) ? 1 : 0;

    mouse_camera_configure(&cfg);
}

void mouse_camera_configure(const MouseCameraConfig *cfg) {
    if (!cfg) return;
    pthread_mutex_lock(&g_mc_lock);
    g_cfg = *cfg;
    if (g_cfg.look_deg_per_count < 0.001 || g_cfg.look_deg_per_count > 1.0 || !isfinite(g_cfg.look_deg_per_count))
        g_cfg.look_deg_per_count = 0.04;
    if (g_cfg.aim_multiplier < 0.1 || g_cfg.aim_multiplier > 2.0 || !isfinite(g_cfg.aim_multiplier))
        g_cfg.aim_multiplier = 1.0;
    if (g_cfg.scope_multiplier < 0.1 || g_cfg.scope_multiplier > 2.0 || !isfinite(g_cfg.scope_multiplier))
        g_cfg.scope_multiplier = 1.0;
    if (g_cfg.smooth_ms != 0 && g_cfg.smooth_ms != 5 && g_cfg.smooth_ms != 10)
        g_cfg.smooth_ms = 0;
    if (g_cfg.legacy_sensitivity < 0.1f || g_cfg.legacy_sensitivity > 5.0f || !isfinite(g_cfg.legacy_sensitivity))
        g_cfg.legacy_sensitivity = 1.0f;

    g_owner = LOOK_OWNER_MOUSE;
    g_stick_armed = true;
    g_stick_in_neutral = true;
    g_stick_neutral_since_ns = 0;
    g_stick_deflecting = false;
    g_stick_deflect_since_ns = 0;
    g_last_stick_mag = 0.0f;
    g_has_last_tick = false;
    g_last_tick_id = 0;
    g_last_camera_gen = 0;
    g_last_context = CAMERA_CTX_UNCLASSIFIED;
    g_unapplied_count = 0;
    g_applied_count = 0;
    g_unclassified_count = 0;
    clear_motion_locked(MOUSE_RESET_CONFIG_CHANGED);
    memset(&g_current_cmd, 0, sizeof g_current_cmd);
    g_current_cmd.owner = g_owner;
    g_current_cmd.epoch = g_epoch;
    g_current_cmd.suppress_gyro = (g_cfg.mouse_mode != 0);
    g_current_cmd.suppress_gamepad_rstick = (g_cfg.mouse_mode != 0);
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_get_config(MouseCameraConfig *out) {
    if (!out) return;
    pthread_mutex_lock(&g_mc_lock);
    *out = g_cfg;
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_set_capture(bool captured) {
    pthread_mutex_lock(&g_mc_lock);
    bool effective = captured && (g_cfg.mouse_mode != 0);
    if (g_captured != effective) {
        g_captured = effective;
        clear_motion_locked(MOUSE_RESET_CAPTURE_CHANGED);
    } else if (!effective) {
        clear_motion_locked(MOUSE_RESET_CAPTURE_CHANGED);
    }
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_reset(MouseResetReason reason) {
    pthread_mutex_lock(&g_mc_lock);
    if (reason == MOUSE_RESET_INIT) {
        g_owner = LOOK_OWNER_MOUSE;
        g_stick_armed = true;
        g_stick_in_neutral = true;
        g_stick_neutral_since_ns = 0;
        g_stick_deflecting = false;
        g_stick_deflect_since_ns = 0;
        g_last_stick_mag = 0.0f;
        g_has_last_tick = false;
        g_last_tick_id = 0;
        g_last_camera_gen = 0;
        g_last_context = CAMERA_CTX_UNCLASSIFIED;
        g_unapplied_count = 0;
        g_applied_count = 0;
        g_unclassified_count = 0;
    }
    clear_motion_locked(reason);
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_push_motion(double dx, double dy, uint64_t event_ns) {
    (void)event_ns;
    if (!isfinite(dx) || !isfinite(dy)) return;
    pthread_mutex_lock(&g_mc_lock);
    if (!g_cfg.mouse_mode || !g_captured) {
        pthread_mutex_unlock(&g_mc_lock);
        return;
    }
    g_pending_dx += dx;
    g_pending_dy += dy;

    if (g_owner == LOOK_OWNER_GAMEPAD) {
        if (hypot(g_pending_dx, g_pending_dy) >= MOUSE_TAKEOVER_COUNTS) {
            g_owner = LOOK_OWNER_MOUSE;
            g_smooth_rem_yaw = 0.0;
            g_smooth_rem_pitch = 0.0;
            g_stick_armed = false;
            g_stick_in_neutral = false;
            g_stick_neutral_since_ns = 0;
            g_stick_deflecting = false;
            g_stick_deflect_since_ns = 0;
            g_epoch++;
        }
    } else {
        /* Mouse already owns look. If the right stick is currently tilted or attempting takeover,
         * disarm it so it must return to neutral (<15% for 100ms) before it can claim ownership. */
        if (g_last_stick_mag >= STICK_REARM_THRESHOLD || g_stick_deflecting) {
            g_stick_armed = false;
            g_stick_in_neutral = false;
            g_stick_neutral_since_ns = 0;
            g_stick_deflecting = false;
            g_stick_deflect_since_ns = 0;
        }
    }
    pthread_mutex_unlock(&g_mc_lock);
}

static void advance_stick_timers_locked(uint64_t now_ns) {
    if (!g_cfg.mouse_mode || g_owner != LOOK_OWNER_MOUSE || now_ns == 0) return;

    float mag = g_last_stick_mag;
    if (!g_stick_armed) {
        if (mag < STICK_REARM_THRESHOLD) {
            if (!g_stick_in_neutral) {
                g_stick_in_neutral = true;
                g_stick_neutral_since_ns = now_ns;
            } else if (now_ns >= g_stick_neutral_since_ns &&
                       (now_ns - g_stick_neutral_since_ns) >= STICK_REARM_NS) {
                g_stick_armed = true;
                g_stick_deflecting = false;
                g_stick_deflect_since_ns = 0;
            }
        } else {
            g_stick_in_neutral = false;
            g_stick_neutral_since_ns = 0;
        }
    }

    if (g_stick_armed) {
        if (mag > STICK_TAKEOVER_THRESHOLD) {
            if (!g_stick_deflecting) {
                g_stick_deflecting = true;
                g_stick_deflect_since_ns = now_ns;
            } else if (now_ns >= g_stick_deflect_since_ns &&
                       (now_ns - g_stick_deflect_since_ns) >= STICK_TAKEOVER_NS) {
                g_owner = LOOK_OWNER_GAMEPAD;
                g_stick_armed = false;
                g_stick_in_neutral = false;
                g_stick_neutral_since_ns = 0;
                g_stick_deflecting = false;
                g_stick_deflect_since_ns = 0;
                clear_motion_locked(MOUSE_RESET_OWNER_CHANGED);
            }
        } else {
            g_stick_deflecting = false;
            g_stick_deflect_since_ns = 0;
            if (mag < STICK_REARM_THRESHOLD) {
                g_stick_in_neutral = true;
                if (!g_stick_neutral_since_ns) g_stick_neutral_since_ns = now_ns;
            } else {
                g_stick_in_neutral = false;
                g_stick_neutral_since_ns = 0;
            }
        }
    }
}

void mouse_camera_advance_time(uint64_t now_ns) {
    if (now_ns == 0) return;
    pthread_mutex_lock(&g_mc_lock);
    advance_stick_timers_locked(now_ns);
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_sample_compat_stick(int32_t *out_rx, int32_t *out_ry) {
    int32_t rx = 0, ry = 0;
    pthread_mutex_lock(&g_mc_lock);
    if (g_cfg.mouse_mode && g_captured && g_owner == LOOK_OWNER_MOUSE &&
        g_current_cmd.route == LOOK_ROUTE_COMPAT_STICK) {
        rx = g_current_cmd.compat_rx;
        ry = g_current_cmd.compat_ry;
        g_compat_sampled_by_hid = true;
    }
    pthread_mutex_unlock(&g_mc_lock);
    if (out_rx) *out_rx = rx;
    if (out_ry) *out_ry = ry;
}

void mouse_camera_notify_frame_boundary(void) {
    pthread_mutex_lock(&g_mc_lock);
    if (g_compat_sampled_by_hid) {
        g_current_cmd.compat_rx = 0;
        g_current_cmd.compat_ry = 0;
        g_compat_sampled_by_hid = false;
    }
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_offer_stick(float rx, float ry, uint64_t event_ns) {
    if (!isfinite(rx) || !isfinite(ry)) return;
    float mag = hypotf(rx, ry);
    pthread_mutex_lock(&g_mc_lock);
    g_last_stick_mag = mag;
    advance_stick_timers_locked(event_ns);
    pthread_mutex_unlock(&g_mc_lock);
}

static LookRoute select_route_locked(const CameraFrame *frame) {
    if (!g_cfg.mouse_mode || !g_captured || g_owner != LOOK_OWNER_MOUSE)
        return LOOK_ROUTE_NONE;
    if (!frame)
        return LOOK_ROUTE_NONE;

    if (frame->context == CAMERA_CTX_DISABLED || !frame->allow_manual_look)
        return LOOK_ROUTE_NONE;

    if (frame->context == CAMERA_CTX_UNCLASSIFIED)
        return LOOK_ROUTE_NONE;

    if (frame->context == CAMERA_CTX_LOCK_ON)
        return LOOK_ROUTE_NONE;

    if (frame->context == CAMERA_CTX_MENU_WHEEL || frame->context == CAMERA_CTX_ULTRAHAND_OBJECT)
        return LOOK_ROUTE_COMPAT_STICK;

    if (g_cfg.backend == MOUSE_BACKEND_LEGACY_STICK)
        return LOOK_ROUTE_COMPAT_STICK;

    switch (frame->context) {
    case CAMERA_CTX_FREE_ROAM:
    case CAMERA_CTX_AIM:
    case CAMERA_CTX_SCOPE:
    case CAMERA_CTX_ULTRAHAND_LOOK:
    case CAMERA_CTX_VEHICLE:
        return LOOK_ROUTE_DIRECT_ANGLES;
    default:
        return LOOK_ROUTE_NONE;
    }
}

void mouse_camera_begin_tick(const CameraFrame *frame, LookCommand *out) {
    LookCommand local_out;
    memset(&local_out, 0, sizeof local_out);
    if (!frame) {
        if (out) *out = local_out;
        return;
    }

    pthread_mutex_lock(&g_mc_lock);

    /* Guard against duplicate calls within the exact same logical tick_id */
    if (g_has_last_tick && frame->tick_id == g_last_tick_id) {
        local_out = g_current_cmd;
        if (g_cmd_applied) {
            /* Already applied on this tick: return zero delta so it cannot be applied twice */
            local_out.route = LOOK_ROUTE_NONE;
            local_out.yaw_delta_rad = 0.0f;
            local_out.pitch_delta_rad = 0.0f;
        }
        pthread_mutex_unlock(&g_mc_lock);
        if (out) *out = local_out;
        return;
    }

    /* Check if the previous tick had direct look motion that was never marked applied */
    if (g_has_last_tick && g_cmd_needs_apply && !g_cmd_applied) {
        g_unapplied_count++;
        if (g_cfg.trace_enabled) {
            fprintf(stderr, "[totk-mouse-trace] WARNING: tick %llu (epoch %llu, ctx %d) had direct motion (yaw=%.5f, pitch=%.5f) that was not applied\n",
                    (unsigned long long)g_last_tick_id, (unsigned long long)g_current_cmd.epoch,
                    (int)g_current_cmd.context, g_current_cmd.yaw_delta_rad, g_current_cmd.pitch_delta_rad);
        }
    }

    /* Context or camera generation transitions re-anchor smoothing and invalidate old state */
    if (g_has_last_tick) {
        if (frame->context != g_last_context) {
            g_smooth_rem_yaw = 0.0;
            g_smooth_rem_pitch = 0.0;
            g_epoch++;
            if (frame->context == CAMERA_CTX_DISABLED || !frame->allow_manual_look) {
                g_pending_dx = 0.0;
                g_pending_dy = 0.0;
            }
        } else if (frame->camera_generation != g_last_camera_gen && frame->camera_generation != 0 && g_last_camera_gen != 0) {
            g_smooth_rem_yaw = 0.0;
            g_smooth_rem_pitch = 0.0;
            g_epoch++;
        }
    }

    g_has_last_tick = true;
    g_last_tick_id = frame->tick_id;
    g_last_camera_gen = frame->camera_generation;
    g_last_context = frame->context;
    g_cmd_needs_apply = false;
    g_cmd_applied = false;
    g_compat_sampled_by_hid = false;

    if (frame->now_ns != 0) {
        advance_stick_timers_locked(frame->now_ns);
    }

    if (frame->context == CAMERA_CTX_UNCLASSIFIED) {
        g_unclassified_count++;
        if (g_cfg.trace_enabled) {
            fprintf(stderr, "[totk-mouse-trace] tick %llu: unclassified camera context (gen=%#llx)\n",
                    (unsigned long long)frame->tick_id, (unsigned long long)frame->camera_generation);
        }
    }

    /* If disabled / loading / cutscene / no manual look, drop pending motion */
    if (frame->context == CAMERA_CTX_DISABLED || !frame->allow_manual_look) {
        g_pending_dx = 0.0;
        g_pending_dy = 0.0;
        g_smooth_rem_yaw = 0.0;
        g_smooth_rem_pitch = 0.0;
    }

    LookRoute route = select_route_locked(frame);
    double snap_dx = 0.0;
    double snap_dy = 0.0;

    if (g_owner == LOOK_OWNER_MOUSE && g_cfg.mouse_mode && g_captured) {
        snap_dx = g_pending_dx;
        snap_dy = g_pending_dy;
        g_pending_dx = 0.0;
        g_pending_dy = 0.0;
    }

    local_out.owner = g_owner;
    local_out.route = route;
    local_out.tick_id = frame->tick_id;
    local_out.epoch = g_epoch;
    local_out.camera_generation = frame->camera_generation;
    local_out.context = frame->context;
    local_out.consumed_dx = snap_dx;
    local_out.consumed_dy = snap_dy;
    local_out.target_yaw_rad = frame->current_yaw_rad;
    local_out.target_pitch_rad = frame->current_pitch_rad;
    local_out.suppress_gyro = (g_cfg.mouse_mode != 0 && g_owner == LOOK_OWNER_MOUSE);
    local_out.suppress_gamepad_rstick = (g_cfg.mouse_mode != 0 && g_owner == LOOK_OWNER_MOUSE);

    if (route == LOOK_ROUTE_DIRECT_ANGLES) {
        double scale_deg = g_cfg.look_deg_per_count;

        if (frame->context == CAMERA_CTX_AIM || frame->context == CAMERA_CTX_SCOPE) {
            double fov_scale = 1.0;
            if (frame->current_vfov_rad > 0.01f && frame->current_vfov_rad < 3.05f &&
                frame->neutral_vfov_rad > 0.01f && frame->neutral_vfov_rad < 3.05f) {
                double num = tan(0.5 * (double)frame->current_vfov_rad);
                double den = tan(0.5 * (double)frame->neutral_vfov_rad);
                if (den > 1e-6 && isfinite(num / den)) {
                    fov_scale = num / den;
                    if (fov_scale < 0.02) fov_scale = 0.02;
                    if (fov_scale > 2.5) fov_scale = 2.5;
                }
            }
            double mult = (frame->context == CAMERA_CTX_AIM) ? g_cfg.aim_multiplier : g_cfg.scope_multiplier;
            scale_deg *= fov_scale * mult;
        }

        double raw_yaw_deg = snap_dx * scale_deg;
        double raw_pitch_deg = (g_cfg.invert_y ? snap_dy : -snap_dy) * scale_deg;
        double raw_yaw_rad = raw_yaw_deg * (M_PI / 180.0);
        double raw_pitch_rad = raw_pitch_deg * (M_PI / 180.0);

        double step_yaw_rad = raw_yaw_rad;
        double step_pitch_rad = raw_pitch_rad;

        if (g_cfg.smooth_ms > 0) {
            g_smooth_rem_yaw += raw_yaw_rad;
            g_smooth_rem_pitch += raw_pitch_rad;
            double tau = (double)g_cfg.smooth_ms * 1e-3;
            double dt = (frame->dt_sec > 1e-5 && frame->dt_sec <= 0.25) ? frame->dt_sec : (1.0 / 60.0);
            double alpha = 1.0 - exp(-dt / tau);
            if (alpha < 0.0) alpha = 0.0;
            if (alpha > 1.0) alpha = 1.0;
            step_yaw_rad = g_smooth_rem_yaw * alpha;
            step_pitch_rad = g_smooth_rem_pitch * alpha;
            g_smooth_rem_yaw -= step_yaw_rad;
            g_smooth_rem_pitch -= step_pitch_rad;
            if (fabs(g_smooth_rem_yaw) < 1e-7 && fabs(g_smooth_rem_pitch) < 1e-7) {
                step_yaw_rad += g_smooth_rem_yaw;
                step_pitch_rad += g_smooth_rem_pitch;
                g_smooth_rem_yaw = 0.0;
                g_smooth_rem_pitch = 0.0;
            }
        } else {
            g_smooth_rem_yaw = 0.0;
            g_smooth_rem_pitch = 0.0;
        }

        local_out.yaw_delta_rad = (float)step_yaw_rad;
        local_out.pitch_delta_rad = (float)step_pitch_rad;
        local_out.target_yaw_rad = mouse_camera_wrap_pi((double)frame->current_yaw_rad + step_yaw_rad);
        local_out.target_pitch_rad = mouse_camera_clamp_pitch((double)frame->current_pitch_rad + step_pitch_rad,
                                                              frame->pitch_min_rad, frame->pitch_max_rad);
        if (fabsf(local_out.yaw_delta_rad) > 0.0f || fabsf(local_out.pitch_delta_rad) > 0.0f) {
            g_cmd_needs_apply = true;
        }
    } else if (route == LOOK_ROUTE_COMPAT_STICK) {
        double dt = frame->dt_sec;
        if (dt >= 0.001 && dt <= 0.25) {
            double scale = 32767.0 * (double)g_cfg.legacy_sensitivity / (1000.0 * dt);
            double vx = snap_dx * scale;
            double vy = snap_dy * scale * (g_cfg.invert_y ? 1.0 : -1.0);
            mouse_camera_clamp_stick_vector(vx, vy, &local_out.compat_rx, &local_out.compat_ry);
        }
    }

    g_current_cmd = local_out;

    if (g_cfg.trace_enabled) {
        fprintf(stderr, "[totk-mouse-trace] tick=%llu epoch=%llu owner=%d route=%d ctx=%d dx=%.3f dy=%.3f dyaw=%.5f dpitch=%.5f tyaw=%.4f tpitch=%.4f\n",
                (unsigned long long)local_out.tick_id, (unsigned long long)local_out.epoch,
                (int)local_out.owner, (int)local_out.route, (int)local_out.context,
                snap_dx, snap_dy, local_out.yaw_delta_rad, local_out.pitch_delta_rad,
                local_out.target_yaw_rad, local_out.target_pitch_rad);
    }

    pthread_mutex_unlock(&g_mc_lock);
    if (out) *out = local_out;
}

void mouse_camera_mark_applied(uint64_t tick_id, uint64_t camera_generation) {
    pthread_mutex_lock(&g_mc_lock);
    if (g_has_last_tick && tick_id == g_last_tick_id &&
        (camera_generation == 0 || g_last_camera_gen == 0 || camera_generation == g_last_camera_gen)) {
        if (!g_cmd_applied) {
            g_cmd_applied = true;
            g_applied_count++;
        }
    }
    pthread_mutex_unlock(&g_mc_lock);
}

LookOwner mouse_camera_current_owner(void) {
    pthread_mutex_lock(&g_mc_lock);
    LookOwner o = g_owner;
    pthread_mutex_unlock(&g_mc_lock);
    return o;
}

bool mouse_camera_is_captured(void) {
    pthread_mutex_lock(&g_mc_lock);
    bool c = g_captured;
    pthread_mutex_unlock(&g_mc_lock);
    return c;
}

bool mouse_camera_should_suppress_gyro(void) {
    pthread_mutex_lock(&g_mc_lock);
    bool suppress = (g_cfg.mouse_mode != 0 && g_owner == LOOK_OWNER_MOUSE);
    pthread_mutex_unlock(&g_mc_lock);
    return suppress;
}

void mouse_camera_resolve_npad_sticks(int32_t key_lx, int32_t key_ly,
                                      int32_t key_rx, int32_t key_ry,
                                      int32_t pad_lx, int32_t pad_ly,
                                      int32_t pad_rx, int32_t pad_ry,
                                      int32_t *out_lx, int32_t *out_ly,
                                      int32_t *out_rx, int32_t *out_ry) {
    /* Left stick: select stronger 2D vector as a unit, never mixing X and Y across devices */
    double key_l_mag = hypot((double)key_lx, (double)key_ly);
    double pad_l_mag = hypot((double)pad_lx, (double)pad_ly);
    if (pad_l_mag > key_l_mag) {
        if (out_lx) *out_lx = pad_lx;
        if (out_ly) *out_ly = pad_ly;
    } else {
        if (out_lx) *out_lx = key_lx;
        if (out_ly) *out_ly = key_ly;
    }

    pthread_mutex_lock(&g_mc_lock);
    if (!g_cfg.mouse_mode) {
        /* Keyboard camera mode: select stronger 2D vector as a unit */
        double key_r_mag = hypot((double)key_rx, (double)key_ry);
        double pad_r_mag = hypot((double)pad_rx, (double)pad_ry);
        if (pad_r_mag > key_r_mag) {
            if (out_rx) *out_rx = pad_rx;
            if (out_ry) *out_ry = pad_ry;
        } else {
            if (out_rx) *out_rx = key_rx;
            if (out_ry) *out_ry = key_ry;
        }
    } else {
        /* Mouse camera mode: single look owner */
        if (g_owner == LOOK_OWNER_GAMEPAD) {
            if (out_rx) *out_rx = pad_rx;
            if (out_ry) *out_ry = pad_ry;
        } else {
            if (g_current_cmd.route == LOOK_ROUTE_COMPAT_STICK) {
                if (out_rx) *out_rx = g_current_cmd.compat_rx;
                if (out_ry) *out_ry = g_current_cmd.compat_ry;
            } else {
                if (out_rx) *out_rx = 0;
                if (out_ry) *out_ry = 0;
            }
        }
    }
    pthread_mutex_unlock(&g_mc_lock);
}

void mouse_camera_get_diag(MouseCameraDiag *out) {
    if (!out) return;
    pthread_mutex_lock(&g_mc_lock);
    out->epoch = g_epoch;
    out->last_tick_id = g_last_tick_id;
    out->unapplied_count = g_unapplied_count;
    out->applied_count = g_applied_count;
    out->unclassified_count = g_unclassified_count;
    out->owner = g_owner;
    out->captured = g_captured;
    out->stick_armed = g_stick_armed;
    out->pending_dx = g_pending_dx;
    out->pending_dy = g_pending_dy;
    out->last_cmd = g_current_cmd;
    pthread_mutex_unlock(&g_mc_lock);
}
