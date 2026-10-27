#include "camera_v143.h"
#include "../runtime/cpu.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RAD_TO_DEG (180.0 / M_PI)
#define DEG_TO_RAD (M_PI / 180.0)

/* Adapter state for the active logical camera tick */
typedef struct {
    uint64_t p_cam;
    uint64_t p_ctrl;
    uint64_t cam_mode;
    uint64_t calc_fn_addr;
    uint32_t switcher_mode;
    bool     stick_blocked_by_ui_or_manip;
    bool     direct_active;
    bool     applied_this_tick;
    uint64_t next_tick_id;
    uint64_t tick_id;
    uint64_t last_tick_ns;
    bool     has_test_time;
    uint64_t test_now_ns;
    bool     is_alt_5c1_branch;
    uint64_t normal_branch_ticks;
    uint64_t alt_branch_ticks;
    bool     logged_first_tick;
    bool     logged_first_alt_tick;
    bool     logged_first_motion;
    float    yaw_delta_deg;
    float    pitch_delta_deg;
    LookCommand cmd;
} TotkCameraV143State;

static TotkCameraV143State g_cam_state;
static uint64_t g_logged_unclassified[16];
static unsigned g_logged_unclassified_count;

static inline float read_f32(uint64_t ea) {
    uint32_t raw = LD32(ea);
    float val;
    memcpy(&val, &raw, sizeof(val));
    return val;
}

static inline void write_f32(uint64_t ea, float val) {
    uint32_t raw;
    memcpy(&raw, &val, sizeof(raw));
    ST32(ea, raw);
}

static inline float get_vf32(const Cpu *c, int reg) {
    uint32_t raw = (uint32_t)c->v[reg];
    float val;
    memcpy(&val, &raw, sizeof(val));
    return val;
}

static inline void set_vf32(Cpu *c, int reg, float val) {
    uint32_t raw;
    memcpy(&raw, &val, sizeof(raw));
    c->v[reg] = (uint64_t)raw;
}

static inline float wrap_deg_180(float deg) {
    if (!isfinite(deg)) return 0.0f;
    deg = fmodf(deg + 180.0f, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg - 180.0f;
}

static inline float clamp_f32(float val, float lo, float hi) {
    if (!isfinite(val)) return 0.0f;
    if (lo > hi) {
        float tmp = lo;
        lo = hi;
        hi = tmp;
    }
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static inline void mark_applied_current_tick(void) {
    if (!g_cam_state.applied_this_tick) {
        g_cam_state.applied_this_tick = true;
        mouse_camera_mark_applied(g_cam_state.tick_id, g_cam_state.cam_mode);
        if (!g_cam_state.has_test_time && !g_cam_state.logged_first_motion &&
            (fabsf(g_cam_state.yaw_delta_deg) > 1e-5f || fabsf(g_cam_state.pitch_delta_deg) > 1e-5f)) {
            g_cam_state.logged_first_motion = true;
            fprintf(stderr,
                    "[totk-camera-v143] first mouse rotation applied: calc=%#llx branch=%s dyaw=%.3f deg dpitch=%.3f deg (normal_ticks=%llu alt_ticks=%llu)\n",
                    (unsigned long long)g_cam_state.calc_fn_addr,
                    g_cam_state.is_alt_5c1_branch ? "alt_5c1" : "normal",
                    (double)g_cam_state.yaw_delta_deg,
                    (double)g_cam_state.pitch_delta_deg,
                    (unsigned long long)g_cam_state.normal_branch_ticks,
                    (unsigned long long)g_cam_state.alt_branch_ticks);
        }
    }
}

static bool is_known_cutscene_or_disabled_calc(uint64_t calc_fn_addr) {
    switch (calc_fn_addr) {
        case 0x8023c4704ull: /* ExecutePlayerCameraEvent */
        case 0x8023c4968ull: /* ExecutePlayerCameraEventRail */
        case 0x8023c531cull: /* ExecutePlayerCameraEventSimple */
        case 0x8023c64f4ull: /* ExecutePlayerCameraEventTalk */
        case 0x8023c7de0ull: /* ExecutePlayerCameraEventTurn */
        case 0x8023c89f0ull: /* ExecutePlayerCameraEventWatch */
        case 0x8023c8ffcull: /* ExecutePlayerCameraGimmick */
        case 0x8023c9dd4ull: /* ExecutePlayerCameraEventTalkManualCtrlRet */
        case 0x8023cdbc8ull: /* ExecutePlayerCameraInitialize */
        case 0x8023cdb18ull: /* ExecutePlayerCameraKeep */
        case 0x8023c222cull: /* ExecutePlayerCameraBase (null/base stub) */
            return true;
        default:
            return false;
    }
}

static void log_unclassified_once(uint64_t calc_fn_addr) {
    for (unsigned i = 0; i < g_logged_unclassified_count; i++) {
        if (g_logged_unclassified[i] == calc_fn_addr) return;
    }
    if (g_logged_unclassified_count < 16) {
        g_logged_unclassified[g_logged_unclassified_count++] = calc_fn_addr;
        fprintf(stderr, "[totk-camera-v143] Unclassified camera mode calc=%#llx (switcher=%u); preserving native behavior\n",
                (unsigned long long)calc_fn_addr, g_cam_state.switcher_mode);
    }
}

void totk_camera_v143_reset_state(void) {
    memset(&g_cam_state, 0, sizeof(g_cam_state));
    memset(g_logged_unclassified, 0, sizeof(g_logged_unclassified));
    g_logged_unclassified_count = 0;
}

void totk_camera_v143_set_test_time_ns(uint64_t now_ns) {
    if (g_cam_state.has_test_time && g_cam_state.last_tick_ns == 0) {
        g_cam_state.last_tick_ns = g_cam_state.test_now_ns;
    }
    g_cam_state.has_test_time = true;
    g_cam_state.test_now_ns = now_ns;
}

void totk_camera_v143_get_branch_counts(uint64_t *out_normal, uint64_t *out_alt) {
    if (out_normal) *out_normal = g_cam_state.normal_branch_ticks;
    if (out_alt) *out_alt = g_cam_state.alt_branch_ticks;
}

CameraContext totk_camera_v143_classify_context(uint64_t calc_fn_addr,
                                                bool stick_blocked_by_ui_or_manip,
                                                bool *out_menu_wheel,
                                                bool *out_lock_on,
                                                bool *out_ultrahand_manip,
                                                bool *out_vehicle_manual_ok) {
    if (out_menu_wheel) *out_menu_wheel = false;
    if (out_lock_on) *out_lock_on = false;
    if (out_ultrahand_manip) *out_ultrahand_manip = false;
    if (out_vehicle_manual_ok) *out_vehicle_manual_ok = false;

    if (stick_blocked_by_ui_or_manip) {
        if (calc_fn_addr == TOTK_V143_CALC_SPECIAL_POWER ||
            calc_fn_addr == TOTK_V143_CALC_ULTRAHAND) {
            if (out_ultrahand_manip) *out_ultrahand_manip = true;
            return CAMERA_CTX_ULTRAHAND_OBJECT;
        }
        if (out_menu_wheel) *out_menu_wheel = true;
        return CAMERA_CTX_MENU_WHEEL;
    }

    switch (calc_fn_addr) {
        case TOTK_V143_CALC_CHASE:
        case TOTK_V143_CALC_TAIL:
        case TOTK_V143_CALC_CLIMB_OBJ:
        case TOTK_V143_CALC_WATERFALL_CLIMB:
        case TOTK_V143_CALC_SHIELD_SURFING:
        case TOTK_V143_CALC_WAKEBOARD:
        case TOTK_V143_CALC_ABYSS:
            return CAMERA_CTX_FREE_ROAM;

        case TOTK_V143_CALC_SPECIAL_POWER:
            return CAMERA_CTX_ULTRAHAND_LOOK;

        case TOTK_V143_CALC_AIMING:
            return CAMERA_CTX_AIM;

        case TOTK_V143_CALC_AIMING_TELESCOPE:
        case TOTK_V143_CALC_CAMERA_FINDER:
            return CAMERA_CTX_SCOPE;

        case TOTK_V143_CALC_LOCKON_TARGET:
        case TOTK_V143_CALC_LOCKON_AIMING_AT:
            if (out_lock_on) *out_lock_on = true;
            return CAMERA_CTX_LOCK_ON;

        case TOTK_V143_CALC_ULTRAHAND:
            if (out_ultrahand_manip) *out_ultrahand_manip = true;
            return CAMERA_CTX_ULTRAHAND_OBJECT;

        case TOTK_V143_CALC_HORSE:
            if (out_vehicle_manual_ok) *out_vehicle_manual_ok = true;
            return CAMERA_CTX_VEHICLE;

        case TOTK_V143_CALC_HORSE_REVOLVE:
        case TOTK_V143_CALC_HORSE_LOCKON_EMPTY:
            if (out_vehicle_manual_ok) *out_vehicle_manual_ok = false;
            return CAMERA_CTX_VEHICLE;

        default:
            if (is_known_cutscene_or_disabled_calc(calc_fn_addr)) {
                return CAMERA_CTX_DISABLED;
            }
            log_unclassified_once(calc_fn_addr);
            return CAMERA_CTX_UNCLASSIFIED;
    }
}

int totk_camera_v143_suppress_gyro(void) {
    MouseCameraConfig cfg;
    mouse_camera_get_config(&cfg);
    if (cfg.backend != MOUSE_BACKEND_NATIVE) return 0;
    return mouse_camera_should_suppress_gyro() ? 1 : 0;
}

/* Hook at 0x800874d80 (str w8, [x20, #0x418] inside f_800874c1c):
 * Live registers:
 *   x20 = p_ctrl (PlayerCameraController* = p_cam + 0x490)
 *   x19 = pad_input_ptr
 *   x21 = (p_cam->flags_34 >> 4) & 7 (bit 0 = right-stick camera look blocked)
 *   x8  = stick_flags
 */
void totk_camera_v143_on_read_stick(struct Cpu *c) {
    uint64_t p_ctrl = c->x[20];
    uint32_t cam_input_flags = (uint32_t)c->x[21];
    bool stick_blocked = ((cam_input_flags & 1u) != 0);

    g_cam_state.p_ctrl = p_ctrl;
    g_cam_state.stick_blocked_by_ui_or_manip = stick_blocked;

    uint64_t now_ns = g_cam_state.has_test_time ? g_cam_state.test_now_ns : mouse_camera_now_ns();
    mouse_camera_advance_time(now_ns);

    MouseCameraConfig cfg;
    MouseCameraDiag diag;
    mouse_camera_get_config(&cfg);
    mouse_camera_get_diag(&diag);

    if (cfg.mouse_mode != 0 &&
        cfg.backend == MOUSE_BACKEND_NATIVE &&
        diag.captured &&
        diag.owner == LOOK_OWNER_MOUSE &&
        !stick_blocked) {
        /* Zero processed right stick on p_ctrl so stick curve/acceleration never adds to camera */
        ST64(p_ctrl + 0x420ull, 0ull);
        /* Clear bit 3 (0x08) and bit 5 (0x20) ("right stick neutral") so Tail/Horse stay in
         * manual-look sub_state (sub_state=4 / sub_state=2), preventing auto-centering and
         * slope pitch pull while mouse owns look. */
        c->x[8] = (uint32_t)((uint32_t)c->x[8] & ~0x28u);
    }

    ST32(p_ctrl + 0x418ull, (uint32_t)c->x[8]);
}

/* Hook at 0x800d85e70 (str w9, [x19, #0x24] inside ExecutePlayerCameraSwitcher::execute):
 * Live registers:
 *   x19 = switcher
 *   x20 = p_cam
 *   x9  = mode_enum
 */
void totk_camera_v143_on_switcher_mode(struct Cpu *c) {
    uint64_t switcher = c->x[19];
    uint32_t mode_enum = (uint32_t)c->x[9];
    ST32(switcher + 0x24ull, mode_enum);
    g_cam_state.p_cam = c->x[20];
    g_cam_state.switcher_mode = mode_enum;
}

/* Hook at 0x8002e2340 and 0x8002e2430 (ldr x8, [x8, #0x120] inside ExecutePlayerCameraBase::execute):
 * Live registers:
 *   x19 = cam_mode (ExecutePlayerCamera* instance)
 *   x22 = p_ctrl   (p_cam + 0x490)
 *   x8  = vtable   (LD64(x8 + 0x120) is mode's calc function address)
 */
void totk_camera_v143_on_mode_pre_calc(struct Cpu *c) {
    uint64_t vtable = c->x[8];
    uint64_t calc_fn_addr = LD64(vtable + 0x120ull);
    c->x[8] = calc_fn_addr;

    uint64_t cam_mode = c->x[19];
    uint64_t p_ctrl = c->x[22];
    bool is_alt_branch = ((LD8(p_ctrl + 0x5c1ull) >> 2) & 1u) != 0;
    g_cam_state.cam_mode = cam_mode;
    g_cam_state.p_ctrl = p_ctrl;
    g_cam_state.calc_fn_addr = calc_fn_addr;
    g_cam_state.applied_this_tick = false;
    g_cam_state.is_alt_5c1_branch = is_alt_branch;
    if (is_alt_branch) {
        g_cam_state.alt_branch_ticks++;
    } else {
        g_cam_state.normal_branch_ticks++;
    }

    bool menu_wheel = false;
    bool lock_on = false;
    bool ultrahand_manip = false;
    bool vehicle_manual_ok = false;
    CameraContext ctx = totk_camera_v143_classify_context(
        calc_fn_addr,
        g_cam_state.stick_blocked_by_ui_or_manip,
        &menu_wheel,
        &lock_on,
        &ultrahand_manip,
        &vehicle_manual_ok);

    float base_fov = read_f32(p_ctrl + 0x35cull);
    float offset_fov = read_f32(p_ctrl + 0x61cull);
    float current_vfov_deg = base_fov + offset_fov;
    if (!isfinite(current_vfov_deg) || current_vfov_deg < 5.0f || current_vfov_deg > 150.0f) {
        current_vfov_deg = 50.0f;
    }

    bool allow_manual_look = true;
    if (ctx == CAMERA_CTX_VEHICLE) {
        allow_manual_look = vehicle_manual_ok;
    } else if (ctx == CAMERA_CTX_DISABLED) {
        allow_manual_look = false;
    }

    uint64_t now_ns = g_cam_state.has_test_time ? g_cam_state.test_now_ns : mouse_camera_now_ns();
    double dt_sec = 1.0 / 60.0;
    if (g_cam_state.last_tick_ns != 0 && now_ns > g_cam_state.last_tick_ns) {
        double raw_dt = (double)(now_ns - g_cam_state.last_tick_ns) * 1e-9;
        if (raw_dt < 0.0005) raw_dt = 0.0005;
        if (raw_dt > 0.25) raw_dt = 0.25;
        dt_sec = raw_dt;
    }
    g_cam_state.last_tick_ns = now_ns;

    CameraFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.tick_id = ++g_cam_state.next_tick_id;
    frame.camera_generation = cam_mode;
    frame.context = ctx;
    frame.now_ns = now_ns;
    frame.dt_sec = dt_sec;
    frame.allow_manual_look = allow_manual_look;
    frame.current_yaw_rad = 0.0f;
    frame.current_pitch_rad = 0.0f;
    frame.pitch_min_rad = -(float)(85.0 * DEG_TO_RAD);
    frame.pitch_max_rad = (float)(85.0 * DEG_TO_RAD);
    frame.current_vfov_rad = (float)(current_vfov_deg * DEG_TO_RAD);
    frame.neutral_vfov_rad = (float)(50.0 * DEG_TO_RAD);

    mouse_camera_begin_tick(&frame, &g_cam_state.cmd);
    g_cam_state.tick_id = g_cam_state.cmd.tick_id;

    if (!g_cam_state.has_test_time) {
        if (!g_cam_state.logged_first_tick) {
            g_cam_state.logged_first_tick = true;
            MouseCameraDiag diag;
            mouse_camera_get_diag(&diag);
            fprintf(stderr,
                    "[totk-camera-v143] first camera tick: calc=%#llx ctx=%d branch=%s captured=%d owner=%d route=%d\n",
                    (unsigned long long)calc_fn_addr,
                    (int)ctx,
                    is_alt_branch ? "alt_5c1" : "normal",
                    diag.captured ? 1 : 0,
                    (int)g_cam_state.cmd.owner,
                    (int)g_cam_state.cmd.route);
        }
        if (is_alt_branch && !g_cam_state.logged_first_alt_tick) {
            g_cam_state.logged_first_alt_tick = true;
            fprintf(stderr,
                    "[totk-camera-v143] alternate 0x5c1 branch active: calc=%#llx ctx=%d route=%d (normal_ticks=%llu alt_ticks=%llu)\n",
                    (unsigned long long)calc_fn_addr,
                    (int)ctx,
                    (int)g_cam_state.cmd.route,
                    (unsigned long long)g_cam_state.normal_branch_ticks,
                    (unsigned long long)g_cam_state.alt_branch_ticks);
        }
    }

    if (g_cam_state.cmd.route == LOOK_ROUTE_DIRECT_ANGLES &&
        g_cam_state.cmd.owner == LOOK_OWNER_MOUSE) {
        g_cam_state.direct_active = true;
        /* TOTK spherical convention: looking right decreases yaw_deg, looking up decreases pitch_deg */
        g_cam_state.yaw_delta_deg = -(float)(g_cam_state.cmd.yaw_delta_rad * RAD_TO_DEG);
        g_cam_state.pitch_delta_deg = -(float)(g_cam_state.cmd.pitch_delta_rad * RAD_TO_DEG);
        /* Ensure right stick on p_ctrl is zeroed and manual-look flags are active */
        ST64(p_ctrl + 0x420ull, 0ull);
        uint32_t f418 = LD32(p_ctrl + 0x418ull);
        ST32(p_ctrl + 0x418ull, f418 & ~0x28u);
    } else {
        g_cam_state.direct_active = false;
        g_cam_state.yaw_delta_deg = 0.0f;
        g_cam_state.pitch_delta_deg = 0.0f;
    }
}

/* ExecutePlayerCameraChase::calc (0x80018e414):
 * Primary third-person free-roam gameplay camera (mode 0 in ExecutePlayerCameraSwitcher).
 * 0x80018ec94: str s12, [sp, #0x48] (chase yaw_deg before spherical-to-Cartesian pos update)
 * 0x80018edec: str s0, [sp, #0x44]  (chase pitch_deg before distance/FOV/pos update)
 */
void totk_camera_v143_on_chase_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[20];
    uint64_t p_ctrl = c->x[26];
    float yaw = get_vf32(c, 12);
    if (g_cam_state.direct_active) {
        uint32_t sub_state = LD32(cam_mode + 0x180ull);
        bool has_mouse_delta = (fabsf(g_cam_state.yaw_delta_deg) > 1e-6f) ||
                               (fabsf(g_cam_state.pitch_delta_deg) > 1e-6f);
        if ((sub_state == 1u || sub_state == 2u) && !has_mouse_delta) {
            /* Preserve ZL camera recenter behind Link when mouse is stationary */
            write_f32(c->sp + 0x48ull, yaw);
            return;
        }
        if (sub_state == 1u || sub_state == 2u) {
            ST32(cam_mode + 0x180ull, 3u);
        }
        /* Read unperturbed spherical yaw from start of frame (sp + 0x48) so walking
         * sideways/backwards in sub_state 3 never auto-orbits or fights the mouse. */
        yaw = wrap_deg_180(read_f32(c->sp + 0x48ull) + g_cam_state.yaw_delta_deg);
        set_vf32(c, 12, yaw);
        write_f32(c->sp + 0x48ull, yaw);
        write_f32(c->sp + 0x4cull, yaw);
        write_f32(cam_mode + 0x120ull, yaw);
        write_f32(cam_mode + 0x124ull, 0.0f);
        write_f32(cam_mode + 0x128ull, 0.0f);
        c->x[23] = 0ull;
        if (p_ctrl != 0) {
            write_f32(p_ctrl + 0x414ull, yaw);
            if (has_mouse_delta) {
                ST32(p_ctrl + 0x5c0ull, LD32(p_ctrl + 0x5c0ull) | 0x1u);
            }
        }
        return;
    }
    write_f32(c->sp + 0x48ull, yaw);
}

void totk_camera_v143_on_chase_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[20];
    uint64_t p_ctrl = c->x[26];
    float pitch = get_vf32(c, 0);
    if (g_cam_state.direct_active) {
        uint32_t sub_state = LD32(cam_mode + 0x180ull);
        bool has_mouse_delta = (fabsf(g_cam_state.yaw_delta_deg) > 1e-6f) ||
                               (fabsf(g_cam_state.pitch_delta_deg) > 1e-6f);
        if ((sub_state == 1u || sub_state == 2u) && !has_mouse_delta) {
            write_f32(c->sp + 0x44ull, pitch);
            mark_applied_current_tick();
            return;
        }
        /* Read unperturbed spherical pitch from start of frame (sp + 0x44) so slope/default
         * pitch pull (f_800191970) never fights mouse vertical look. */
        pitch = read_f32(c->sp + 0x44ull) + g_cam_state.pitch_delta_deg;
        float lo = read_f32(cam_mode + 0xf8ull);
        float hi = read_f32(cam_mode + 0xfcull);
        if (!isfinite(lo) || !isfinite(hi) || fabsf(lo - hi) < 1e-3f) {
            lo = -85.0f;
            hi = 85.0f;
        } else {
            if (lo > hi) {
                float tmp = lo;
                lo = hi;
                hi = tmp;
            }
            if (lo < -85.0f) lo = -85.0f;
            if (hi > 85.0f) hi = 85.0f;
        }
        pitch = clamp_f32(pitch, lo, hi);
        set_vf32(c, 0, pitch);
        write_f32(c->sp + 0x44ull, pitch);
        write_f32(cam_mode + 0x108ull, pitch);
        write_f32(cam_mode + 0x10cull, 0.0f);
        if (p_ctrl != 0) {
            write_f32(p_ctrl + 0x410ull, pitch);
        }
        mark_applied_current_tick();
        return;
    }
    write_f32(c->sp + 0x44ull, pitch);
}

/* ExecutePlayerCameraTail::calc (0x8023d4950):
 * 0x8023d4f34: ldr s0, [x19, #0xc0] (tail_cam->yaw_deg)
 * 0x8023d5028: ldr s1, [x19, #0xb4] (tail_cam->pitch_deg)
 */
void totk_camera_v143_on_tail_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float yaw = read_f32(cam_mode + 0xc0ull);
    if (g_cam_state.direct_active) {
        yaw = wrap_deg_180(yaw + g_cam_state.yaw_delta_deg);
        write_f32(cam_mode + 0xc0ull, yaw);
        /* Zero auto-orbit offset and angular velocity so manual yaw is exact */
        write_f32(cam_mode + 0xc4ull, 0.0f);
        write_f32(cam_mode + 0xc8ull, 0.0f);
    }
    set_vf32(c, 0, yaw);
}

void totk_camera_v143_on_tail_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0xb4ull);
    if (g_cam_state.direct_active) {
        pitch += g_cam_state.pitch_delta_deg;
        write_f32(cam_mode + 0xb4ull, pitch);
        write_f32(cam_mode + 0xb8ull, 0.0f);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, pitch);
}

/* ExecutePlayerCameraAiming::calc (0x8023be978):
 * 0x8023bebcc: fadd s0, s1, s0 (target pitch before native mode clamps)
 * 0x8023bec3c: bl 0x800191970  (spring damper on x19+0x15c / x19+0x160)
 * 0x8023beeec: ldur s1, [x29, #-0x48] (yaw before native wrap & store to x19+0x158)
 */
void totk_camera_v143_on_aim_pitch_add(struct Cpu *c) {
    float s1 = get_vf32(c, 1);
    float s0 = get_vf32(c, 0);
    float res = s1 + s0;
    if (g_cam_state.direct_active) {
        res += g_cam_state.pitch_delta_deg;
    }
    set_vf32(c, 0, res);
}

int totk_camera_v143_on_aim_pitch_commit(struct Cpu *c) {
    if (!g_cam_state.direct_active) return 0;
    uint64_t cam_mode = c->x[19];
    /* Copy freshly clamped target pitch (x19+0x154) directly to current pitch (x19+0x15c)
     * and zero spring velocity (x19+0x160) so bow/throw aiming has zero stick damping lag. */
    float clamped_pitch = read_f32(cam_mode + 0x154ull);
    write_f32(cam_mode + 0x15cull, clamped_pitch);
    write_f32(cam_mode + 0x160ull, 0.0f);
    return 1;
}

void totk_camera_v143_on_aim_yaw(struct Cpu *c) {
    uint64_t ea = c->x[29] - 72ull;
    float yaw = read_f32(ea);
    if (g_cam_state.direct_active) {
        yaw += g_cam_state.yaw_delta_deg;
        write_f32(ea, yaw);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, yaw);
}

/* ExecutePlayerCameraAimingTelescope::calc (0x8023c08ac):
 * 0x8023c0b00: ldr s0, [x19, #0x160] (target pitch before native [min,max] clamp)
 * 0x8023c0b78: ldr s0, [x19, #0x50]  (current pitch before interpolation)
 * 0x8023c0e80: ldr s1, [sp, #0x58]   (yaw before native wrap & store to x19+0x164)
 */
void totk_camera_v143_on_scope_pitch_load(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0x160ull);
    if (g_cam_state.direct_active) {
        pitch += g_cam_state.pitch_delta_deg;
        write_f32(cam_mode + 0x160ull, pitch);
    }
    set_vf32(c, 0, pitch);
}

void totk_camera_v143_on_scope_pitch_commit(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    if (g_cam_state.direct_active) {
        float clamped = read_f32(cam_mode + 0x160ull);
        write_f32(cam_mode + 0x50ull, clamped);
        write_f32(cam_mode + 0x58ull, 0.0f);
        set_vf32(c, 0, clamped);
        return;
    }
    set_vf32(c, 0, read_f32(cam_mode + 0x50ull));
}

void totk_camera_v143_on_scope_yaw(struct Cpu *c) {
    uint64_t ea = c->sp + 0x58ull;
    float yaw = read_f32(ea);
    if (g_cam_state.direct_active) {
        yaw += g_cam_state.yaw_delta_deg;
        write_f32(ea, yaw);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, yaw);
}

/* ExecutePlayerCameraCameraFinder::calc (0x8023c26c8):
 * 0x8023c28c4: str s0, [x19, #0x50] (pitch before native MinPitch/MaxPitch clamp)
 * 0x8023c2df0: ldr s1, [sp, #0x48]  (yaw before native wrap & limit check)
 */
void totk_camera_v143_on_finder_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = get_vf32(c, 0);
    if (g_cam_state.direct_active) {
        pitch += g_cam_state.pitch_delta_deg;
        set_vf32(c, 0, pitch);
        write_f32(cam_mode + 0x54ull, 0.0f);
    }
    write_f32(cam_mode + 0x50ull, pitch);
}

void totk_camera_v143_on_finder_yaw(struct Cpu *c) {
    uint64_t ea = c->sp + 0x48ull;
    float yaw = read_f32(ea);
    if (g_cam_state.direct_active) {
        yaw += g_cam_state.yaw_delta_deg;
        write_f32(ea, yaw);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, yaw);
}

/* ExecutePlayerCameraSpecialPower::calc (0x8023d2d40):
 * 0x8023d3158: ldr s0, [x19, #0x160] (target pitch before native clamp)
 * 0x8023d31d0: ldr s0, [x19, #0x58]  (after spring damper 0x800191970)
 * 0x8023d34cc: ldur s1, [x29, #-0x50] (yaw before native wrap & store to x19+0x164)
 */
void totk_camera_v143_on_special_pitch_load(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0x160ull);
    if (g_cam_state.direct_active) {
        pitch += g_cam_state.pitch_delta_deg;
        write_f32(cam_mode + 0x160ull, pitch);
    }
    set_vf32(c, 0, pitch);
}

void totk_camera_v143_on_special_pitch_commit(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    if (g_cam_state.direct_active) {
        float clamped = read_f32(cam_mode + 0x160ull);
        write_f32(cam_mode + 0x50ull, clamped);
        write_f32(cam_mode + 0x58ull, 0.0f);
        set_vf32(c, 0, 0.0f);
        return;
    }
    set_vf32(c, 0, read_f32(cam_mode + 0x58ull));
}

void totk_camera_v143_on_special_yaw(struct Cpu *c) {
    uint64_t ea = c->x[29] - 80ull;
    float yaw = read_f32(ea);
    if (g_cam_state.direct_active) {
        yaw += g_cam_state.yaw_delta_deg;
        write_f32(ea, yaw);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, yaw);
}

/* ExecutePlayerCameraClimbObj::calc (0x8023c3a98):
 * 0x8023c3bd8: ldr s1, [x19, #0x90] (climb yaw_deg)
 * 0x8023c3c10: ldr s1, [x19, #0x9c] (climb pitch_deg in s0 / x19+0x98)
 */
void totk_camera_v143_on_climb_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float yaw = read_f32(cam_mode + 0x90ull);
    if (g_cam_state.direct_active) {
        yaw = wrap_deg_180(yaw + g_cam_state.yaw_delta_deg);
        write_f32(cam_mode + 0x90ull, yaw);
        write_f32(cam_mode + 0x94ull, 0.0f);
        set_vf32(c, 0, 0.0f);
    }
    set_vf32(c, 1, yaw);
}

void totk_camera_v143_on_climb_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    if (g_cam_state.direct_active) {
        float pitch = clamp_f32(get_vf32(c, 0) + g_cam_state.pitch_delta_deg, -85.0f, 85.0f);
        write_f32(cam_mode + 0x98ull, pitch);
        write_f32(cam_mode + 0xd8ull, pitch);
        write_f32(cam_mode + 0x9cull, 0.0f);
        set_vf32(c, 0, pitch);
        set_vf32(c, 1, 0.0f);
        mark_applied_current_tick();
        return;
    }
    set_vf32(c, 1, read_f32(cam_mode + 0x9cull));
}

/* ExecutePlayerCameraShieldSurfing::calc (0x8023d0088):
 * 0x8023d0564: ldr s1, [x19, #0xf4]  (surf yaw_deg)
 * 0x8023d0584: ldr s0, [x19, #0x100] (surf pitch_deg)
 */
void totk_camera_v143_on_surf_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float yaw = read_f32(cam_mode + 0xf4ull);
    if (g_cam_state.direct_active) {
        yaw = wrap_deg_180(yaw + g_cam_state.yaw_delta_deg);
        write_f32(cam_mode + 0xf4ull, yaw);
        write_f32(cam_mode + 0xf8ull, 0.0f);
        set_vf32(c, 0, 0.0f);
    }
    set_vf32(c, 1, yaw);
}

void totk_camera_v143_on_surf_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0x100ull);
    if (g_cam_state.direct_active) {
        pitch = clamp_f32(pitch + g_cam_state.pitch_delta_deg, -85.0f, 85.0f);
        write_f32(cam_mode + 0xfcull, pitch);
        write_f32(cam_mode + 0x100ull, pitch);
        write_f32(cam_mode + 0x104ull, 0.0f);
        mark_applied_current_tick();
    }
    set_vf32(c, 0, pitch);
}

/* ExecutePlayerCameraWakeboard::calc (0x8023da61c):
 * 0x8023daad0: ldr s1, [x19, #0xf0]       (wakeboard yaw_deg)
 * 0x8023dab30: ldp s0, s1, [x19, #0xfc]   (wakeboard pitch_deg, pitch_vel)
 */
void totk_camera_v143_on_wake_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float yaw = read_f32(cam_mode + 0xf0ull);
    if (g_cam_state.direct_active) {
        yaw = wrap_deg_180(yaw + g_cam_state.yaw_delta_deg);
        write_f32(cam_mode + 0xf0ull, yaw);
        write_f32(cam_mode + 0xf4ull, 0.0f);
        set_vf32(c, 0, 0.0f);
    }
    set_vf32(c, 1, yaw);
}

void totk_camera_v143_on_wake_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0xfcull);
    float vel = read_f32(cam_mode + 0x100ull);
    if (g_cam_state.direct_active) {
        pitch = clamp_f32(pitch + g_cam_state.pitch_delta_deg, -85.0f, 85.0f);
        vel = 0.0f;
        write_f32(cam_mode + 0xf8ull, pitch);
        write_f32(cam_mode + 0xfcull, pitch);
        write_f32(cam_mode + 0x100ull, 0.0f);
        mark_applied_current_tick();
    }
    set_vf32(c, 0, pitch);
    set_vf32(c, 1, vel);
}

/* ExecutePlayerCameraHorse::calc (0x8023ca518):
 * 0x8023caa38: ldr s1, [x19, #0xb4] (horse yaw_deg)
 * 0x8023caaac: ldr s1, [x19, #0xac] (horse pitch_deg)
 */
void totk_camera_v143_on_horse_yaw(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float yaw = read_f32(cam_mode + 0xb4ull);
    if (g_cam_state.direct_active) {
        yaw = wrap_deg_180(yaw + g_cam_state.yaw_delta_deg);
        write_f32(cam_mode + 0x68ull, yaw);
        write_f32(cam_mode + 0xb4ull, yaw);
        write_f32(cam_mode + 0xb8ull, 0.0f);
        set_vf32(c, 0, 0.0f);
    }
    set_vf32(c, 1, yaw);
}

void totk_camera_v143_on_horse_pitch(struct Cpu *c) {
    uint64_t cam_mode = c->x[19];
    float pitch = read_f32(cam_mode + 0xacull);
    if (g_cam_state.direct_active) {
        pitch = clamp_f32(pitch + g_cam_state.pitch_delta_deg, -85.0f, 85.0f);
        write_f32(cam_mode + 0x64ull, pitch);
        write_f32(cam_mode + 0xacull, pitch);
        write_f32(cam_mode + 0xb0ull, 0.0f);
        set_vf32(c, 0, 0.0f);
        mark_applied_current_tick();
    }
    set_vf32(c, 1, pitch);
}

/* Fallback hook at 0x8007156c0 (bl 0x800717668 inside PlayerCamera::calc f_800715408):
 * Live registers:
 *   x19 = p_cam
 *   x21 = p_ctrl (p_cam + 0x490)
 * Applies direct spherical rotation to p_ctrl->pose_338 for any direct-camera mode
 * (such as WaterfallClimb or Abyss) that did not hit a mode-specific hook above,
 * right before f_800717668 and camera collision (f_80071aec4) execute.
 */
void totk_camera_v143_on_player_cam_calc(struct Cpu *c) {
    if (!g_cam_state.direct_active || g_cam_state.applied_this_tick) return;

    uint64_t p_ctrl = c->x[21];
    if (p_ctrl == 0) return;

    float pos_x = read_f32(p_ctrl + 0x338ull);
    float pos_y = read_f32(p_ctrl + 0x33cull);
    float pos_z = read_f32(p_ctrl + 0x340ull);
    float at_x  = read_f32(p_ctrl + 0x344ull);
    float at_y  = read_f32(p_ctrl + 0x348ull);
    float at_z  = read_f32(p_ctrl + 0x34cull);

    float dx = pos_x - at_x;
    float dy = pos_y - at_y;
    float dz = pos_z - at_z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist > 0.001f && isfinite(dist)) {
        float pitch_deg = read_f32(p_ctrl + 0x410ull) + g_cam_state.pitch_delta_deg;
        float yaw_deg   = wrap_deg_180(read_f32(p_ctrl + 0x414ull) + g_cam_state.yaw_delta_deg);
        pitch_deg = clamp_f32(pitch_deg, -85.0f, 85.0f);

        float pitch_rad = (float)(pitch_deg * DEG_TO_RAD);
        float yaw_rad   = (float)(yaw_deg * DEG_TO_RAD);
        float cos_p = cosf(pitch_rad);
        float sin_p = sinf(pitch_rad);
        float cos_y = cosf(yaw_rad);
        float sin_y = sinf(yaw_rad);

        float new_dx = dist * cos_p * sin_y;
        float new_dy = dist * sin_p;
        float new_dz = dist * cos_p * cos_y;

        write_f32(p_ctrl + 0x338ull, at_x + new_dx);
        write_f32(p_ctrl + 0x33cull, at_y + new_dy);
        write_f32(p_ctrl + 0x340ull, at_z + new_dz);
        write_f32(p_ctrl + 0x410ull, pitch_deg);
        write_f32(p_ctrl + 0x414ull, yaw_deg);
    }

    mark_applied_current_tick();
}
