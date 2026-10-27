"""Unit tests for TOTK 1.4.3 camera adapter (src/host/camera_v143.c) and lifter patches (camera_patches_v143.py)."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "lifter"))
from toolchain import CLANG64, env as tool_env  # noqa: E402
from camera_patches_v143 import CAMERA_PATCHES_V143  # noqa: E402


HARNESS_C = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_v143.h"
#include "cpu.h"

volatile int g_fnprof = 0;
void fnprof_hit(unsigned long long a, unsigned long long b, unsigned long long c) {
    (void)a;
    (void)b;
    (void)c;
}

static uint32_t f2u32(float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static float u32f(uint32_t u) {
    float v;
    memcpy(&v, &u, 4);
    return v;
}

static void check_near(float actual, float expected, float eps, const char *msg) {
    if (fabsf(actual - expected) > eps) {
        fprintf(stderr, "FAIL %s: actual=%.6f expected=%.6f\n", msg, actual, expected);
        exit(1);
    }
}

static void reset_mouse_cam(void) {
    MouseCameraConfig cfg = {
        .version = 2,
        .mouse_mode = 1,
        .backend = MOUSE_BACKEND_NATIVE,
        .look_deg_per_count = 0.04,
        .aim_multiplier = 1.0,
        .scope_multiplier = 1.0,
        .smooth_ms = 0,
        .invert_y = 0,
        .legacy_sensitivity = 1.0f,
        .trace_enabled = 0,
    };
    mouse_camera_configure(&cfg);
    mouse_camera_set_capture(true);
    totk_camera_v143_reset_state();
}

static void test_context_classification(void) {
    bool mw = false, lo = false, uh = false, vh = false;

    const uint64_t free_roam_addrs[] = {
        TOTK_V143_CALC_CHASE,
        TOTK_V143_CALC_TAIL,
        TOTK_V143_CALC_CLIMB_OBJ,
        TOTK_V143_CALC_WATERFALL_CLIMB,
        TOTK_V143_CALC_SHIELD_SURFING,
        TOTK_V143_CALC_WAKEBOARD,
        TOTK_V143_CALC_ABYSS,
    };
    for (size_t i = 0; i < sizeof(free_roam_addrs) / sizeof(free_roam_addrs[0]); i++) {
        CameraContext ctx = totk_camera_v143_classify_context(free_roam_addrs[i], false, &mw, &lo, &uh, &vh);
        assert(ctx == CAMERA_CTX_FREE_ROAM);
    }

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_SPECIAL_POWER, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_ULTRAHAND_LOOK);
    assert(!uh);

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_AIMING, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_AIM);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_AIMING_TELESCOPE, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_SCOPE);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_CAMERA_FINDER, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_SCOPE);

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_LOCKON_TARGET, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_LOCK_ON);
    assert(lo);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_LOCKON_AIMING_AT, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_LOCK_ON);
    assert(lo);

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_TAIL, true, &mw, &lo, &uh, &vh) == CAMERA_CTX_MENU_WHEEL);
    assert(mw);

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_ULTRAHAND, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_ULTRAHAND_OBJECT);
    assert(uh);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_SPECIAL_POWER, true, &mw, &lo, &uh, &vh) == CAMERA_CTX_ULTRAHAND_OBJECT);
    assert(uh);

    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_HORSE, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_VEHICLE);
    assert(vh);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_HORSE_REVOLVE, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_VEHICLE);
    assert(!vh);
    assert(totk_camera_v143_classify_context(TOTK_V143_CALC_HORSE_LOCKON_EMPTY, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_VEHICLE);
    assert(!vh);

    assert(totk_camera_v143_classify_context(0x8023c4704ull, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_DISABLED);
    assert(totk_camera_v143_classify_context(0x8023c222cull, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_DISABLED);
    assert(totk_camera_v143_classify_context(0x809999990ull, false, &mw, &lo, &uh, &vh) == CAMERA_CTX_UNCLASSIFIED);
}

static void test_chase_camera_direct_rotation(void) {
    reset_mouse_cam();
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    uint8_t p_ctrl_buf[0x800];
    uint8_t mode_buf[0x400];
    uint8_t vt_buf[0x200];
    uint8_t stack_buf[0x200];
    memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
    memset(mode_buf, 0, sizeof(mode_buf));
    memset(vt_buf, 0, sizeof(vt_buf));
    memset(stack_buf, 0, sizeof(stack_buf));

    uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
    uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
    uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;
    uint64_t sp_base = (uint64_t)(uintptr_t)(stack_buf + 0x80);

    ST64(vtable + 0x120ull, TOTK_V143_CALC_CHASE);
    ST32(p_ctrl + 0x35cull, f2u32(50.0f));
    /* p_ctrl+0x148 bit 1 (0x02) is set during normal Chase camera mode and MUST NOT block mouse look */
    ST8(p_ctrl + 0x148ull, 0x02u);
    ST32(p_ctrl + 0x410ull, f2u32(-12.0f));
    ST32(p_ctrl + 0x414ull, f2u32(25.0f));
    ST32(cam_mode + 0xf8ull, f2u32(-70.0f));
    ST32(cam_mode + 0xfcull, f2u32(80.0f));
    ST32(cam_mode + 0x180ull, 3u); /* sub_state 3 (normal chase follow) */

    cpu.x[20] = p_ctrl;
    cpu.x[21] = 0; /* cam_input_flags bit 0 == 0 (not blocked) */
    cpu.x[8] = 0x28;
    totk_camera_v143_on_read_stick(&cpu);

    mouse_camera_push_motion(100.0, -50.0, 1500000ull);

    cpu.x[19] = cam_mode;
    cpu.x[22] = p_ctrl;
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    assert(cpu.x[8] == TOTK_V143_CALC_CHASE);

    /* Simulate ExecutePlayerCameraChase::calc prologue storing p_ctrl+0x410/0x414 to sp+0x44/0x48,
     * followed by sub_state 3 attempting to auto-orbit v12 to 99.0f and slope-pull v0 to 0.0f. */
    cpu.sp = sp_base;
    cpu.x[20] = cam_mode;
    cpu.x[26] = p_ctrl;
    cpu.x[23] = 1ull;
    ST32(sp_base + 0x44ull, f2u32(-12.0f));
    ST32(sp_base + 0x48ull, f2u32(25.0f));
    ST32(sp_base + 0x4cull, f2u32(25.0f));
    cpu.v[12] = f2u32(99.0f); /* unwanted auto-orbit yaw */
    totk_camera_v143_on_chase_yaw(&cpu);

    cpu.v[0] = f2u32(0.0f); /* unwanted auto-pitch reset */
    totk_camera_v143_on_chase_pitch(&cpu);

    float expected_yaw = 25.0f - (100.0f * 0.04f);
    float expected_pitch = -12.0f - (50.0f * 0.04f);
    check_near(u32f(LD32(sp_base + 0x48ull)), expected_yaw, 1e-4f, "chase sp+0x48 yaw");
    check_near(u32f(LD32(cam_mode + 0x120ull)), expected_yaw, 1e-4f, "chase cam_mode+0x120 yaw");
    check_near(u32f(LD32(p_ctrl + 0x414ull)), expected_yaw, 1e-4f, "chase p_ctrl+0x414 yaw");
    assert(cpu.x[23] == 0ull);
    assert((LD32(p_ctrl + 0x5c0ull) & 1u) == 1u);

    check_near(u32f(LD32(sp_base + 0x44ull)), expected_pitch, 1e-4f, "chase sp+0x44 pitch");
    check_near(u32f(LD32(cam_mode + 0x108ull)), expected_pitch, 1e-4f, "chase cam_mode+0x108 pitch");
    check_near(u32f(LD32(p_ctrl + 0x410ull)), expected_pitch, 1e-4f, "chase p_ctrl+0x410 pitch");

    MouseCameraDiag diag;
    mouse_camera_get_diag(&diag);
    assert(diag.applied_count == 1);
    assert(diag.unapplied_count == 0);
}

static void test_tail_camera_direct_rotation(void) {
    reset_mouse_cam();
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    uint8_t p_ctrl_buf[0x800];
    uint8_t mode_buf[0x400];
    uint8_t vt_buf[0x200];
    memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
    memset(mode_buf, 0, sizeof(mode_buf));
    memset(vt_buf, 0, sizeof(vt_buf));

    uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
    uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
    uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;

    ST64(vtable + 0x120ull, TOTK_V143_CALC_TAIL);
    ST32(p_ctrl + 0x35cull, f2u32(50.0f));
    ST32(p_ctrl + 0x420ull, f2u32(0.75f));
    ST32(p_ctrl + 0x424ull, f2u32(-0.50f));
    ST32(cam_mode + 0xc0ull, f2u32(10.0f));
    ST32(cam_mode + 0xb4ull, f2u32(-15.0f));

    cpu.x[20] = p_ctrl;
    cpu.x[21] = 0;
    cpu.x[8] = 0x28;
    totk_camera_v143_on_read_stick(&cpu);
    assert(u32f(LD32(p_ctrl + 0x420ull)) == 0.0f);
    assert(u32f(LD32(p_ctrl + 0x424ull)) == 0.0f);
    assert((LD32(p_ctrl + 0x418ull) & 0x28u) == 0);
    assert(totk_camera_v143_suppress_gyro() == 1);

    mouse_camera_push_motion(100.0, -50.0, 1000000ull);

    cpu.x[19] = cam_mode;
    cpu.x[22] = p_ctrl;
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    assert(cpu.x[8] == TOTK_V143_CALC_TAIL);

    totk_camera_v143_on_tail_yaw(&cpu);
    totk_camera_v143_on_tail_pitch(&cpu);

    check_near(u32f(LD32(cam_mode + 0xc0ull)), 10.0f - (100.0f * 0.04f), 1e-4f, "tail yaw");
    check_near(u32f(LD32(cam_mode + 0xb4ull)), -15.0f - (50.0f * 0.04f), 1e-4f, "tail pitch");

    MouseCameraDiag diag;
    mouse_camera_get_diag(&diag);
    assert(diag.applied_count == 1);
    assert(diag.unapplied_count == 0);

    /* Fallback in PlayerCamera::calc must NOT double-apply */
    cpu.x[21] = p_ctrl;
    totk_camera_v143_on_player_cam_calc(&cpu);
    mouse_camera_get_diag(&diag);
    assert(diag.applied_count == 1);
}

static void test_aim_scope_and_fallback_spherical(void) {
    reset_mouse_cam();
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    uint8_t p_ctrl_buf[0x800];
    uint8_t mode_buf[0x400];
    uint8_t vt_buf[0x200];
    uint8_t stack_buf[0x200];
    memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
    memset(mode_buf, 0, sizeof(mode_buf));
    memset(vt_buf, 0, sizeof(vt_buf));
    memset(stack_buf, 0, sizeof(stack_buf));

    uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
    uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
    uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;
    uint64_t sp_base = (uint64_t)(uintptr_t)(stack_buf + 0x100);

    /* Aiming at zoomed vFOV = 25 deg */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_AIMING);
    ST32(p_ctrl + 0x35cull, f2u32(25.0f));
    ST32(sp_base - 72ull, f2u32(0.0f));

    mouse_camera_push_motion(100.0, -50.0, 2000000ull);
    cpu.x[19] = cam_mode;
    cpu.x[22] = p_ctrl;
    cpu.x[29] = sp_base;
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);

    cpu.v[1] = f2u32(0.0f);
    cpu.v[0] = f2u32(0.0f);
    totk_camera_v143_on_aim_pitch_add(&cpu);
    float pitch_out = u32f((uint32_t)cpu.v[0]);
    float fov_scale = (float)(tan(12.5 * 3.14159265358979323846 / 180.0) / tan(25.0 * 3.14159265358979323846 / 180.0));
    check_near(pitch_out, -(50.0f * 0.04f * fov_scale), 1e-4f, "aim pitch");

    ST32(cam_mode + 0x154ull, f2u32(pitch_out));
    int committed = totk_camera_v143_on_aim_pitch_commit(&cpu);
    assert(committed == 1);
    check_near(u32f(LD32(cam_mode + 0x15cull)), pitch_out, 1e-4f, "aim pitch commit");

    totk_camera_v143_on_aim_yaw(&cpu);
    check_near(u32f(LD32(sp_base - 72ull)), -(100.0f * 0.04f * fov_scale), 1e-4f, "aim yaw");

    /* Scope (AimingTelescope 0x8023c08ac) at zoomed vFOV = 20 deg */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_AIMING_TELESCOPE);
    ST32(p_ctrl + 0x35cull, f2u32(20.0f));
    ST32(cam_mode + 0x160ull, f2u32(5.0f));
    ST32(cam_mode + 0x50ull, f2u32(5.0f));
    ST32(cam_mode + 0x58ull, f2u32(1.5f));
    ST32(sp_base + 0x58ull, f2u32(12.0f));

    mouse_camera_push_motion(100.0, -50.0, 2500000ull);
    cpu.x[19] = cam_mode;
    cpu.x[22] = p_ctrl;
    cpu.sp = sp_base;
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);

    float scope_fov_scale = (float)(tan(10.0 * 3.14159265358979323846 / 180.0) / tan(25.0 * 3.14159265358979323846 / 180.0));
    totk_camera_v143_on_scope_pitch_load(&cpu);
    float expected_scope_pitch = 5.0f - (50.0f * 0.04f * scope_fov_scale);
    check_near(u32f(LD32(cam_mode + 0x160ull)), expected_scope_pitch, 1e-4f, "scope pitch load");
    totk_camera_v143_on_scope_pitch_commit(&cpu);
    check_near(u32f(LD32(cam_mode + 0x50ull)), expected_scope_pitch, 1e-4f, "scope pitch commit");
    check_near(u32f(LD32(cam_mode + 0x58ull)), 0.0f, 1e-6f, "scope pitch vel zero");
    totk_camera_v143_on_scope_yaw(&cpu);
    check_near(u32f(LD32(sp_base + 0x58ull)), 12.0f - (100.0f * 0.04f * scope_fov_scale), 1e-4f, "scope yaw");

    /* CameraFinder (0x8023c26c8) at vFOV = 25 deg */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_CAMERA_FINDER);
    ST32(p_ctrl + 0x35cull, f2u32(25.0f));
    ST32(cam_mode + 0x54ull, f2u32(2.0f));
    ST32(sp_base + 0x48ull, f2u32(-8.0f));

    mouse_camera_push_motion(100.0, -50.0, 2600000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    cpu.v[0] = f2u32(3.0f);
    totk_camera_v143_on_finder_pitch(&cpu);
    check_near(u32f(LD32(cam_mode + 0x50ull)), 3.0f - (50.0f * 0.04f * fov_scale), 1e-4f, "finder pitch");
    check_near(u32f(LD32(cam_mode + 0x54ull)), 0.0f, 1e-6f, "finder pitch vel zero");
    totk_camera_v143_on_finder_yaw(&cpu);
    check_near(u32f(LD32(sp_base + 0x48ull)), -8.0f - (100.0f * 0.04f * fov_scale), 1e-4f, "finder yaw");

    /* Fallback spherical solver on Abyss mode (0x8023bd8d8) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_ABYSS);
    ST32(p_ctrl + 0x35cull, f2u32(50.0f));
    ST32(p_ctrl + 0x338ull, f2u32(0.0f));
    ST32(p_ctrl + 0x33cull, f2u32(0.0f));
    ST32(p_ctrl + 0x340ull, f2u32(5.0f));
    ST32(p_ctrl + 0x344ull, f2u32(0.0f));
    ST32(p_ctrl + 0x348ull, f2u32(0.0f));
    ST32(p_ctrl + 0x34cull, f2u32(0.0f));
    ST32(p_ctrl + 0x410ull, f2u32(0.0f));
    ST32(p_ctrl + 0x414ull, f2u32(0.0f));

    mouse_camera_push_motion(100.0, 0.0, 3000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    cpu.x[21] = p_ctrl;
    totk_camera_v143_on_player_cam_calc(&cpu);

    MouseCameraDiag diag;
    mouse_camera_get_diag(&diag);
    assert(diag.applied_count == 4);
    assert(diag.unapplied_count == 0);
    check_near(u32f(LD32(p_ctrl + 0x414ull)), -(100.0f * 0.04f), 1e-4f, "abyss fallback yaw");
}

static void test_special_climb_surf_wake_and_horse_hooks(void) {
    reset_mouse_cam();
    Cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    uint8_t p_ctrl_buf[0x800];
    uint8_t mode_buf[0x400];
    uint8_t vt_buf[0x200];
    uint8_t stack_buf[0x200];
    memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
    memset(mode_buf, 0, sizeof(mode_buf));
    memset(vt_buf, 0, sizeof(vt_buf));
    memset(stack_buf, 0, sizeof(stack_buf));

    uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
    uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
    uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;
    uint64_t sp_base = (uint64_t)(uintptr_t)(stack_buf + 0x100);
    ST32(p_ctrl + 0x35cull, f2u32(50.0f));

    cpu.x[19] = cam_mode;
    cpu.x[22] = p_ctrl;
    cpu.x[29] = sp_base;

    /* 1. SpecialPower (Ultrahand free look 0x8023d2d40) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_SPECIAL_POWER);
    ST32(cam_mode + 0x160ull, f2u32(10.0f));
    ST32(sp_base - 80ull, f2u32(20.0f));
    mouse_camera_push_motion(50.0, -25.0, 4000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    totk_camera_v143_on_special_pitch_load(&cpu);
    check_near(u32f(LD32(cam_mode + 0x160ull)), 10.0f - (25.0f * 0.04f), 1e-4f, "special pitch load");
    totk_camera_v143_on_special_pitch_commit(&cpu);
    check_near(u32f(LD32(cam_mode + 0x50ull)), 10.0f - (25.0f * 0.04f), 1e-4f, "special pitch commit");
    totk_camera_v143_on_special_yaw(&cpu);
    check_near(u32f(LD32(sp_base - 80ull)), 20.0f - (50.0f * 0.04f), 1e-4f, "special yaw");

    /* 2. ClimbObj (0x8023c3a98) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_CLIMB_OBJ);
    ST32(cam_mode + 0x90ull, f2u32(30.0f));
    mouse_camera_push_motion(50.0, -25.0, 5000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    totk_camera_v143_on_climb_yaw(&cpu);
    check_near(u32f(LD32(cam_mode + 0x90ull)), 30.0f - (50.0f * 0.04f), 1e-4f, "climb yaw");
    cpu.v[0] = f2u32(5.0f);
    totk_camera_v143_on_climb_pitch(&cpu);
    check_near(u32f(LD32(cam_mode + 0x98ull)), 5.0f - (25.0f * 0.04f), 1e-4f, "climb pitch");

    /* 3. ShieldSurfing (0x8023d0088) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_SHIELD_SURFING);
    ST32(cam_mode + 0xf4ull, f2u32(-10.0f));
    ST32(cam_mode + 0x100ull, f2u32(8.0f));
    mouse_camera_push_motion(50.0, -25.0, 6000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    totk_camera_v143_on_surf_yaw(&cpu);
    check_near(u32f(LD32(cam_mode + 0xf4ull)), -10.0f - (50.0f * 0.04f), 1e-4f, "surf yaw");
    totk_camera_v143_on_surf_pitch(&cpu);
    check_near(u32f(LD32(cam_mode + 0x100ull)), 8.0f - (25.0f * 0.04f), 1e-4f, "surf pitch");

    /* 4. Wakeboard (0x8023da61c) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_WAKEBOARD);
    ST32(cam_mode + 0xf0ull, f2u32(15.0f));
    ST32(cam_mode + 0xfcull, f2u32(-4.0f));
    mouse_camera_push_motion(50.0, -25.0, 7000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    totk_camera_v143_on_wake_yaw(&cpu);
    check_near(u32f(LD32(cam_mode + 0xf0ull)), 15.0f - (50.0f * 0.04f), 1e-4f, "wake yaw");
    totk_camera_v143_on_wake_pitch(&cpu);
    check_near(u32f(LD32(cam_mode + 0xfcull)), -4.0f - (25.0f * 0.04f), 1e-4f, "wake pitch");

    /* 5. Horse (0x8023ca518) */
    ST64(vtable + 0x120ull, TOTK_V143_CALC_HORSE);
    ST32(cam_mode + 0xb4ull, f2u32(45.0f));
    ST32(cam_mode + 0xacull, f2u32(-12.0f));
    mouse_camera_push_motion(50.0, -25.0, 8000000ull);
    cpu.x[8] = vtable;
    totk_camera_v143_on_mode_pre_calc(&cpu);
    totk_camera_v143_on_horse_yaw(&cpu);
    check_near(u32f(LD32(cam_mode + 0xb4ull)), 45.0f - (50.0f * 0.04f), 1e-4f, "horse yaw");
    totk_camera_v143_on_horse_pitch(&cpu);
    check_near(u32f(LD32(cam_mode + 0xacull)), -12.0f - (25.0f * 0.04f), 1e-4f, "horse pitch");

    MouseCameraDiag diag;
    mouse_camera_get_diag(&diag);
    assert(diag.applied_count == 5);
    assert(diag.unapplied_count == 0);
}

static void test_adapter_real_dt_smoothing_and_compat_stick_across_fps(void) {
    const int fps_list[4] = {30, 60, 90, 120};
    Cpu cpu;
    uint8_t p_ctrl_buf[0x800];
    uint8_t mode_buf[0x400];
    uint8_t vt_buf[0x200];

    /* Part A: Exponential smoothing (smooth_ms = 5) through totk_camera_v143_on_mode_pre_calc uses real dt_sec */
    for (int i = 0; i < 4; i++) {
        int fps = fps_list[i];
        double dt_sec = 1.0 / (double)fps;
        uint64_t dt_ns = (uint64_t)llround(dt_sec * 1e9);

        MouseCameraConfig cfg = {
            .version = 2,
            .mouse_mode = 1,
            .backend = MOUSE_BACKEND_NATIVE,
            .look_deg_per_count = 0.04,
            .aim_multiplier = 1.0,
            .scope_multiplier = 1.0,
            .smooth_ms = 5,
            .invert_y = 0,
            .legacy_sensitivity = 1.0f,
            .trace_enabled = 0,
        };
        mouse_camera_configure(&cfg);
        mouse_camera_set_capture(true);
        totk_camera_v143_reset_state();

        memset(&cpu, 0, sizeof(cpu));
        memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
        memset(mode_buf, 0, sizeof(mode_buf));
        memset(vt_buf, 0, sizeof(vt_buf));
        uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
        uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
        uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;
        ST64(vtable + 0x120ull, TOTK_V143_CALC_TAIL);
        ST32(p_ctrl + 0x35cull, f2u32(50.0f));
        ST32(cam_mode + 0xc0ull, f2u32(0.0f));
        ST32(cam_mode + 0xb4ull, f2u32(0.0f));

        uint64_t t0 = 1000000000ull;
        totk_camera_v143_set_test_time_ns(t0);
        totk_camera_v143_set_test_time_ns(t0 + dt_ns);
        mouse_camera_push_motion(100.0, 0.0, t0 + dt_ns);

        cpu.x[19] = cam_mode;
        cpu.x[22] = p_ctrl;
        cpu.x[8] = vtable;
        totk_camera_v143_on_mode_pre_calc(&cpu);
        totk_camera_v143_on_tail_yaw(&cpu);
        totk_camera_v143_on_tail_pitch(&cpu);

        double actual_dt = (double)dt_ns * 1e-9;
        double expected_alpha = 1.0 - exp(-actual_dt / 0.005);
        float expected_first_yaw = -(float)(100.0 * 0.04 * expected_alpha);
        check_near(u32f(LD32(cam_mode + 0xc0ull)), expected_first_yaw, 1e-4f, "smooth first step scales with real dt");

        /* Run remaining ticks for 200ms wall-clock and verify full convergence to -4.0 deg */
        int steps = fps / 5;
        for (int s = 2; s <= steps + 10; s++) {
            totk_camera_v143_set_test_time_ns(t0 + (uint64_t)s * dt_ns);
            cpu.x[8] = vtable;
            totk_camera_v143_on_mode_pre_calc(&cpu);
            totk_camera_v143_on_tail_yaw(&cpu);
            totk_camera_v143_on_tail_pitch(&cpu);
        }
        check_near(u32f(LD32(cam_mode + 0xc0ull)), -4.0f, 1e-3f, "smooth converged total angle");
    }

    /* Part B: Compat stick route in Menu Wheel through totk_camera_v143_on_mode_pre_calc is FPS-invariant */
    int32_t ref_rx = 0;
    for (int i = 0; i < 4; i++) {
        int fps = fps_list[i];
        double dt_sec = 1.0 / (double)fps;
        uint64_t dt_ns = (uint64_t)llround(dt_sec * 1e9);

        reset_mouse_cam();
        memset(&cpu, 0, sizeof(cpu));
        memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
        memset(mode_buf, 0, sizeof(mode_buf));
        memset(vt_buf, 0, sizeof(vt_buf));
        uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
        uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
        uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;
        ST64(vtable + 0x120ull, TOTK_V143_CALC_TAIL);
        ST32(p_ctrl + 0x35cull, f2u32(50.0f));

        /* Block stick camera look (menu wheel active: cam_input_flags bit 0 = 1) */
        uint64_t t0 = 2000000000ull;
        totk_camera_v143_set_test_time_ns(t0);
        totk_camera_v143_set_test_time_ns(t0 + dt_ns);

        cpu.x[20] = p_ctrl;
        cpu.x[21] = 1;
        cpu.x[8] = 0;
        totk_camera_v143_on_read_stick(&cpu);

        /* Constant 300 counts/sec mouse velocity */
        mouse_camera_push_motion(300.0 * dt_sec, 0.0, t0 + dt_ns);
        cpu.x[19] = cam_mode;
        cpu.x[22] = p_ctrl;
        cpu.x[8] = vtable;
        totk_camera_v143_on_mode_pre_calc(&cpu);

        MouseCameraDiag diag;
        mouse_camera_get_diag(&diag);
        assert(diag.last_cmd.route == LOOK_ROUTE_COMPAT_STICK);
        if (i == 0) {
            ref_rx = diag.last_cmd.compat_rx;
            assert(ref_rx > 9000 && ref_rx < 10500);
        } else {
            assert(abs(diag.last_cmd.compat_rx - ref_rx) <= 2);
        }
    }
}

static void test_execute_player_camera_base_both_branches(void) {
    /* Verify both normal (0x8002e2340) and alternate 0x5c1 (0x8002e2430) branches
     * of ExecutePlayerCameraBase::execute consume mouse motion once and rotate Tail camera. */
    for (int alt = 0; alt < 2; alt++) {
        reset_mouse_cam();
        totk_camera_v143_set_test_time_ns(5000000000ull);

        Cpu cpu;
        memset(&cpu, 0, sizeof(cpu));
        uint8_t p_ctrl_buf[0x800];
        uint8_t mode_buf[0x400];
        uint8_t vt_buf[0x200];
        memset(p_ctrl_buf, 0, sizeof(p_ctrl_buf));
        memset(mode_buf, 0, sizeof(mode_buf));
        memset(vt_buf, 0, sizeof(vt_buf));

        uint64_t p_ctrl = (uint64_t)(uintptr_t)p_ctrl_buf;
        uint64_t cam_mode = (uint64_t)(uintptr_t)mode_buf;
        uint64_t vtable = (uint64_t)(uintptr_t)vt_buf;

        ST64(cam_mode + 0x0ull, vtable);
        ST64(cam_mode + 0x20ull, p_ctrl);
        ST64(vtable + 0x120ull, TOTK_V143_CALC_TAIL);
        ST32(p_ctrl + 0x35cull, f2u32(50.0f));
        ST32(cam_mode + 0xc0ull, f2u32(0.0f));
        ST32(cam_mode + 0xb4ull, f2u32(0.0f));
        ST8(p_ctrl + 0x5c1ull, alt ? 0x04u : 0x00u);

        mouse_camera_push_motion(100.0, -50.0, 5000000000ull);

        cpu.x[19] = cam_mode;
        cpu.x[22] = p_ctrl;
        {
            uint64_t ea = cpu.x[19] + 0x20ull;
            uint64_t v = LD64(ea);
            cpu.x[8] = v;
        }
        {
            uint64_t ea = cpu.x[8] + 0x5c1ull;
            uint64_t v = LD8(ea);
            cpu.x[8] = v;
        }
        if (((cpu.x[8] >> 2) & 1) != 0) {
            /* Alternate L_8002e23f0 branch at 0x8002e2430 */
            cpu.x[8] = LD64(cpu.x[19] + 0x0ull);
            cpu.x[0] = cpu.x[19];
            totk_camera_v143_on_mode_pre_calc(&cpu);
        } else {
            /* Normal branch at 0x8002e2340 */
            cpu.x[8] = LD64(cpu.x[19] + 0x0ull);
            cpu.x[0] = cpu.x[19];
            totk_camera_v143_on_mode_pre_calc(&cpu);
        }

        assert(cpu.x[8] == TOTK_V143_CALC_TAIL);
        totk_camera_v143_on_tail_yaw(&cpu);
        totk_camera_v143_on_tail_pitch(&cpu);

        check_near(u32f(LD32(cam_mode + 0xc0ull)), -4.0f, 1e-4f, "branch tail yaw");
        check_near(u32f(LD32(cam_mode + 0xb4ull)), -2.0f, 1e-4f, "branch tail pitch");

        MouseCameraDiag diag;
        mouse_camera_get_diag(&diag);
        assert(diag.applied_count == 1);
        assert(diag.unapplied_count == 0);
        check_near((float)diag.last_cmd.consumed_dx, 100.0f, 1e-4f, "branch consumed_dx");
        check_near((float)diag.last_cmd.consumed_dy, -50.0f, 1e-4f, "branch consumed_dy");

        uint64_t normal_ticks = 0, alt_ticks = 0;
        totk_camera_v143_get_branch_counts(&normal_ticks, &alt_ticks);
        if (alt == 0) {
            assert(normal_ticks == 1 && alt_ticks == 0);
        } else {
            assert(normal_ticks == 0 && alt_ticks == 1);
        }
    }
}

int main(void) {
    test_context_classification();
    test_chase_camera_direct_rotation();
    test_tail_camera_direct_rotation();
    test_aim_scope_and_fallback_spherical();
    test_special_climb_surf_wake_and_horse_hooks();
    test_adapter_real_dt_smoothing_and_compat_stick_across_fps();
    test_execute_player_camera_base_both_branches();
    puts("ALL_CAMERA_V143_TESTS_PASSED");
    return 0;
}
'''


class TestCameraV143(unittest.TestCase):
    def test_01_all_lifter_patch_words_match_totk_143_main(self):
        """Every patch in CAMERA_PATCHES_V143 must match the exact instruction word in data/exefs/main."""
        from nso import Nso

        class _NsoWordReader:
            def __init__(self, path, base):
                self.nso = Nso(path)
                self.base = base

            def word(self, addr):
                return self.nso.u32(addr - self.base)

        m = _NsoWordReader(str(ROOT / "data/exefs/main"), 0x800000000)
        self.assertEqual(len(CAMERA_PATCHES_V143), 33)
        self.assertIn(0x8002e2340, CAMERA_PATCHES_V143)
        self.assertIn(0x8002e2430, CAMERA_PATCHES_V143)
        self.assertIn(0x80018ec94, CAMERA_PATCHES_V143)
        self.assertIn(0x80018edec, CAMERA_PATCHES_V143)
        for addr, patch_fn in CAMERA_PATCHES_V143.items():
            code = patch_fn(m, addr)
            self.assertTrue(code.startswith("{"))
            self.assertTrue(code.endswith("}"))

    def test_03_generated_main_164_hooks_both_execute_branches(self):
        """Both normal (0x8002e2340) and alternate (0x8002e2430) branches in f_8002e228c must call totk_camera_v143_on_mode_pre_calc."""
        main_164 = (ROOT / "data/generated/main/main_164.c").read_text(encoding="utf-8")
        fn_start = main_164.index("void f_8002e228c(Cpu *c)")
        fn_end = main_164.index("void f_8002e2448(Cpu *c)", fn_start)
        fn_body = main_164[fn_start:fn_end]
        hook_stmt = "{ extern void totk_camera_v143_on_mode_pre_calc(Cpu *); totk_camera_v143_on_mode_pre_calc(c); }"
        self.assertEqual(fn_body.count(hook_stmt), 2)
        self.assertNotIn("c->x[8] + 0x120ull", fn_body)

    def test_04_generated_main_104_hooks_chase_yaw_and_pitch(self):
        """ExecutePlayerCameraChase::calc (f_80018e414 in main_104.c) must call both chase yaw and pitch hooks."""
        main_104 = (ROOT / "data/generated/main/main_104.c").read_text(encoding="utf-8")
        self.assertIn("{ extern void totk_camera_v143_on_chase_yaw(Cpu *); totk_camera_v143_on_chase_yaw(c); }", main_104)
        self.assertIn("{ extern void totk_camera_v143_on_chase_pitch(Cpu *); totk_camera_v143_on_chase_pitch(c); }", main_104)

    def test_02_c_adapter_hooks_and_classification(self):
        """Compile and run C test suite for src/host/camera_v143.c with -Wall -Wextra -Werror."""
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = pathlib.Path(tmpdir)
            harness_c = tmp / "harness.c"
            harness_exe = tmp / "harness.exe"
            harness_c.write_text(HARNESS_C, encoding="utf-8")

            clang = CLANG64 / "clang.exe"
            cmd = [
                str(clang),
                "-O2",
                "-pthread",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I" + str(ROOT / "src/host"),
                "-I" + str(ROOT / "src/runtime"),
                str(ROOT / "src/host/mouse_camera.c"),
                str(ROOT / "src/host/camera_v143.c"),
                str(harness_c),
                "-o",
                str(harness_exe),
            ]
            subprocess.run(cmd, cwd=ROOT, env=tool_env(), check=True)
            res = subprocess.run([str(harness_exe)], cwd=ROOT, env=tool_env(), capture_output=True, text=True, check=True)
            self.assertIn("ALL_CAMERA_V143_TESTS_PASSED", res.stdout)


if __name__ == "__main__":
    unittest.main()
