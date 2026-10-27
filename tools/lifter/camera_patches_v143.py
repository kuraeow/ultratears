"""Direct mouse camera patches for TOTK 1.4.3 (src/host/camera_v143.c).

Every patch verifies the original 32-bit ARM64 instruction word via expect() before emitting
the replacement C statement, so lifting any other game version fails loudly.
"""


def expect(gen, addr, word, what):
    got = gen.word(addr)
    if got != word:
        raise SystemExit(f'camera patch {what} at {addr:#x}: expected {word:#010x}, found {got:#010x} (not TOTK 1.4.3?)')


def cam_read_stick(gen, addr):
    """f_800874c1c (0x800874d80): zero processed right stick and set manual-look flags when mouse owns look."""
    expect(gen, addr, 0xb9041a88, 'cam_read_stick')  # str w8, [x20, #0x418]
    return '{ extern void totk_camera_v143_on_read_stick(Cpu *); totk_camera_v143_on_read_stick(c); }'


def cam_suppress_gyro(gen, addr):
    """f_800874e6c (0x800874ec0): take native inactive-gyro path (L_800874f10) when mouse owns look."""
    expect(gen, addr, 0x54000280, 'cam_suppress_gyro')  # b.eq 0x800874f10
    return ('{ extern int totk_camera_v143_suppress_gyro(void); '
            'if (totk_camera_v143_suppress_gyro() || COND_EQ(c)) { goto L_800874f10; } }')


def cam_switcher_mode(gen, addr):
    """ExecutePlayerCameraSwitcher::execute (0x800d85e70): record active camera switcher mode enum."""
    expect(gen, addr, 0xb9002669, 'cam_switcher_mode')  # str w9, [x19, #0x24]
    return '{ extern void totk_camera_v143_on_switcher_mode(Cpu *); totk_camera_v143_on_switcher_mode(c); }'


def cam_mode_pre_calc(gen, addr):
    """ExecutePlayerCameraBase::execute (0x8002e2340, 0x8002e2430): classify camera context and begin logical camera tick."""
    expect(gen, addr, 0xf9409108, 'cam_mode_pre_calc')  # ldr x8, [x8, #0x120]
    return '{ extern void totk_camera_v143_on_mode_pre_calc(Cpu *); totk_camera_v143_on_mode_pre_calc(c); }'


def cam_chase_yaw(gen, addr):
    """ExecutePlayerCameraChase::calc (0x80018ec94): apply direct mouse yaw delta to Chase free-roam camera."""
    expect(gen, addr, 0xbd004bec, 'cam_chase_yaw')  # str s12, [sp, #0x48]
    return '{ extern void totk_camera_v143_on_chase_yaw(Cpu *); totk_camera_v143_on_chase_yaw(c); }'


def cam_chase_pitch(gen, addr):
    """ExecutePlayerCameraChase::calc (0x80018edec): apply direct mouse pitch delta to Chase free-roam camera."""
    expect(gen, addr, 0xbd0047e0, 'cam_chase_pitch')  # str s0, [sp, #0x44]
    return '{ extern void totk_camera_v143_on_chase_pitch(Cpu *); totk_camera_v143_on_chase_pitch(c); }'


def cam_tail_yaw(gen, addr):
    """ExecutePlayerCameraTail::calc (0x8023d4f34): apply direct mouse yaw delta to tail_cam->yaw_deg."""
    expect(gen, addr, 0xbd40c260, 'cam_tail_yaw')  # ldr s0, [x19, #0xc0]
    return '{ extern void totk_camera_v143_on_tail_yaw(Cpu *); totk_camera_v143_on_tail_yaw(c); }'


def cam_tail_pitch(gen, addr):
    """ExecutePlayerCameraTail::calc (0x8023d5028): apply direct mouse pitch delta to tail_cam->pitch_deg."""
    expect(gen, addr, 0xbd40b661, 'cam_tail_pitch')  # ldr s1, [x19, #0xb4]
    return '{ extern void totk_camera_v143_on_tail_pitch(Cpu *); totk_camera_v143_on_tail_pitch(c); }'


def cam_aim_gyro_branch(gen, addr):
    """ExecutePlayerCameraAiming::calc (0x8023be9ec): skip gyro aiming branch when mouse owns look."""
    expect(gen, addr, 0x360002e0, 'cam_aim_gyro_branch')  # tbz w0, #0, 0x8023bea48
    return ('{ extern int totk_camera_v143_suppress_gyro(void); '
            'if (totk_camera_v143_suppress_gyro() || ((c->x[0] >> 0) & 1) == 0) { goto L_8023bea48; } }')


def cam_aim_pitch_add(gen, addr):
    """ExecutePlayerCameraAiming::calc (0x8023bebcc): add direct mouse pitch delta before native aiming clamps."""
    expect(gen, addr, 0x1e202820, 'cam_aim_pitch_add')  # fadd s0, s1, s0
    return '{ extern void totk_camera_v143_on_aim_pitch_add(Cpu *); totk_camera_v143_on_aim_pitch_add(c); }'


def cam_aim_pitch_commit(gen, addr):
    """ExecutePlayerCameraAiming::calc (0x8023bec3c): bypass stick spring damper on pitch when mouse owns look."""
    expect(gen, addr, 0x97774b4d, 'cam_aim_pitch_commit')  # bl 0x800191970
    return ('{ extern int totk_camera_v143_on_aim_pitch_commit(Cpu *); '
            'extern void f_800191970(Cpu *); '
            'if (!totk_camera_v143_on_aim_pitch_commit(c)) { c->x[30] = 0x8023bec40ull; f_800191970(c); } }')


def cam_aim_yaw(gen, addr):
    """ExecutePlayerCameraAiming::calc (0x8023beeec): add direct mouse yaw delta before native wrap & store."""
    expect(gen, addr, 0xbc5b83a1, 'cam_aim_yaw')  # ldur s1, [x29, #-0x48]
    return '{ extern void totk_camera_v143_on_aim_yaw(Cpu *); totk_camera_v143_on_aim_yaw(c); }'


def cam_scope_gyro_branch(gen, addr):
    """ExecutePlayerCameraAimingTelescope::calc (0x8023c0944): skip gyro scope branch when mouse owns look."""
    expect(gen, addr, 0x54000501, 'cam_scope_gyro_branch')  # b.ne 0x8023c09e4
    return ('{ extern int totk_camera_v143_suppress_gyro(void); '
            'if (totk_camera_v143_suppress_gyro() || COND_NE(c)) { goto L_8023c09e4; } }')


def cam_scope_pitch_load(gen, addr):
    """ExecutePlayerCameraAimingTelescope::calc (0x8023c0b00): add direct mouse pitch delta before native clamp."""
    expect(gen, addr, 0xbd416260, 'cam_scope_pitch_load')  # ldr s0, [x19, #0x160]
    return '{ extern void totk_camera_v143_on_scope_pitch_load(Cpu *); totk_camera_v143_on_scope_pitch_load(c); }'


def cam_scope_pitch_commit(gen, addr):
    """ExecutePlayerCameraAimingTelescope::calc (0x8023c0b78): commit clamped pitch without stick damping."""
    expect(gen, addr, 0xbd405260, 'cam_scope_pitch_commit')  # ldr s0, [x19, #0x50]
    return '{ extern void totk_camera_v143_on_scope_pitch_commit(Cpu *); totk_camera_v143_on_scope_pitch_commit(c); }'


def cam_scope_yaw(gen, addr):
    """ExecutePlayerCameraAimingTelescope::calc (0x8023c0e80): add direct mouse yaw delta before native wrap."""
    expect(gen, addr, 0xbd405be1, 'cam_scope_yaw')  # ldr s1, [sp, #0x58]
    return '{ extern void totk_camera_v143_on_scope_yaw(Cpu *); totk_camera_v143_on_scope_yaw(c); }'


def cam_finder_gyro_flag(gen, addr):
    """ExecutePlayerCameraCameraFinder::calc (0x8023c276c): force non-gyro flag w23=1 when mouse owns look."""
    expect(gen, addr, 0x52000017, 'cam_finder_gyro_flag')  # eor w23, w0, #1
    return ('{ extern int totk_camera_v143_suppress_gyro(void); '
            'c->x[23] = totk_camera_v143_suppress_gyro() ? 1u : (uint32_t)((uint32_t)c->x[0] ^ 0x1ull); }')


def cam_finder_pitch(gen, addr):
    """ExecutePlayerCameraCameraFinder::calc (0x8023c28c4): add direct mouse pitch delta before Min/MaxPitch clamp."""
    expect(gen, addr, 0xbd005260, 'cam_finder_pitch')  # str s0, [x19, #0x50]
    return '{ extern void totk_camera_v143_on_finder_pitch(Cpu *); totk_camera_v143_on_finder_pitch(c); }'


def cam_finder_yaw(gen, addr):
    """ExecutePlayerCameraCameraFinder::calc (0x8023c2df0): add direct mouse yaw delta before native wrap."""
    expect(gen, addr, 0xbd404be1, 'cam_finder_yaw')  # ldr s1, [sp, #0x48]
    return '{ extern void totk_camera_v143_on_finder_yaw(Cpu *); totk_camera_v143_on_finder_yaw(c); }'


def cam_special_gyro_branch(gen, addr):
    """ExecutePlayerCameraSpecialPower::calc (0x8023d2e24): skip gyro branch when mouse owns look."""
    expect(gen, addr, 0x360004e0, 'cam_special_gyro_branch')  # tbz w0, #0, 0x8023d2ec0
    return ('{ extern int totk_camera_v143_suppress_gyro(void); '
            'if (totk_camera_v143_suppress_gyro() || ((c->x[0] >> 0) & 1) == 0) { goto L_8023d2ec0; } }')


def cam_special_pitch_load(gen, addr):
    """ExecutePlayerCameraSpecialPower::calc (0x8023d3158): add direct mouse pitch delta before native clamp."""
    expect(gen, addr, 0xbd416260, 'cam_special_pitch_load')  # ldr s0, [x19, #0x160]
    return '{ extern void totk_camera_v143_on_special_pitch_load(Cpu *); totk_camera_v143_on_special_pitch_load(c); }'


def cam_special_pitch_commit(gen, addr):
    """ExecutePlayerCameraSpecialPower::calc (0x8023d31d0): commit clamped pitch without stick damping."""
    expect(gen, addr, 0xbd405a60, 'cam_special_pitch_commit')  # ldr s0, [x19, #0x58]
    return '{ extern void totk_camera_v143_on_special_pitch_commit(Cpu *); totk_camera_v143_on_special_pitch_commit(c); }'


def cam_special_yaw(gen, addr):
    """ExecutePlayerCameraSpecialPower::calc (0x8023d34cc): add direct mouse yaw delta before native wrap."""
    expect(gen, addr, 0xbc5b03a1, 'cam_special_yaw')  # ldur s1, [x29, #-0x50]
    return '{ extern void totk_camera_v143_on_special_yaw(Cpu *); totk_camera_v143_on_special_yaw(c); }'


def cam_climb_yaw(gen, addr):
    """ExecutePlayerCameraClimbObj::calc (0x8023c3bd8): apply direct mouse yaw delta."""
    expect(gen, addr, 0xbd409261, 'cam_climb_yaw')  # ldr s1, [x19, #0x90]
    return '{ extern void totk_camera_v143_on_climb_yaw(Cpu *); totk_camera_v143_on_climb_yaw(c); }'


def cam_climb_pitch(gen, addr):
    """ExecutePlayerCameraClimbObj::calc (0x8023c3c10): apply direct mouse pitch delta."""
    expect(gen, addr, 0xbd409e61, 'cam_climb_pitch')  # ldr s1, [x19, #0x9c]
    return '{ extern void totk_camera_v143_on_climb_pitch(Cpu *); totk_camera_v143_on_climb_pitch(c); }'


def cam_surf_yaw(gen, addr):
    """ExecutePlayerCameraShieldSurfing::calc (0x8023d0564): apply direct mouse yaw delta."""
    expect(gen, addr, 0xbd40f661, 'cam_surf_yaw')  # ldr s1, [x19, #0xf4]
    return '{ extern void totk_camera_v143_on_surf_yaw(Cpu *); totk_camera_v143_on_surf_yaw(c); }'


def cam_surf_pitch(gen, addr):
    """ExecutePlayerCameraShieldSurfing::calc (0x8023d0584): apply direct mouse pitch delta."""
    expect(gen, addr, 0xbd410260, 'cam_surf_pitch')  # ldr s0, [x19, #0x100]
    return '{ extern void totk_camera_v143_on_surf_pitch(Cpu *); totk_camera_v143_on_surf_pitch(c); }'


def cam_wake_yaw(gen, addr):
    """ExecutePlayerCameraWakeboard::calc (0x8023daad0): apply direct mouse yaw delta."""
    expect(gen, addr, 0xbd40f261, 'cam_wake_yaw')  # ldr s1, [x19, #0xf0]
    return '{ extern void totk_camera_v143_on_wake_yaw(Cpu *); totk_camera_v143_on_wake_yaw(c); }'


def cam_wake_pitch(gen, addr):
    """ExecutePlayerCameraWakeboard::calc (0x8023dab30): apply direct mouse pitch delta."""
    expect(gen, addr, 0x2d5f8660, 'cam_wake_pitch')  # ldp s0, s1, [x19, #0xfc]
    return '{ extern void totk_camera_v143_on_wake_pitch(Cpu *); totk_camera_v143_on_wake_pitch(c); }'


def cam_horse_yaw(gen, addr):
    """ExecutePlayerCameraHorse::calc (0x8023caa38): apply direct mouse yaw delta."""
    expect(gen, addr, 0xbd40b661, 'cam_horse_yaw')  # ldr s1, [x19, #0xb4]
    return '{ extern void totk_camera_v143_on_horse_yaw(Cpu *); totk_camera_v143_on_horse_yaw(c); }'


def cam_horse_pitch(gen, addr):
    """ExecutePlayerCameraHorse::calc (0x8023caaac): apply direct mouse pitch delta."""
    expect(gen, addr, 0xbd40ae61, 'cam_horse_pitch')  # ldr s1, [x19, #0xac]
    return '{ extern void totk_camera_v143_on_horse_pitch(Cpu *); totk_camera_v143_on_horse_pitch(c); }'


def cam_player_calc_fallback(gen, addr):
    """PlayerCamera::calc (0x8007156c0): fallback direct spherical rotation before f_800717668 and collision."""
    expect(gen, addr, 0x940007ea, 'cam_player_calc_fallback')  # bl 0x800717668
    return ('{ extern void totk_camera_v143_on_player_cam_calc(Cpu *); '
            'extern void f_800717668(Cpu *); '
            'totk_camera_v143_on_player_cam_calc(c); '
            'c->x[30] = 0x8007156c4ull; f_800717668(c); }')


CAMERA_PATCHES_V143 = {
    0x800874d80: cam_read_stick,
    0x800874ec0: cam_suppress_gyro,
    0x800d85e70: cam_switcher_mode,
    0x8002e2340: cam_mode_pre_calc,
    0x8002e2430: cam_mode_pre_calc,
    0x80018ec94: cam_chase_yaw,
    0x80018edec: cam_chase_pitch,
    0x8023d4f34: cam_tail_yaw,
    0x8023d5028: cam_tail_pitch,
    0x8023be9ec: cam_aim_gyro_branch,
    0x8023bebcc: cam_aim_pitch_add,
    0x8023bec3c: cam_aim_pitch_commit,
    0x8023beeec: cam_aim_yaw,
    0x8023c0944: cam_scope_gyro_branch,
    0x8023c0b00: cam_scope_pitch_load,
    0x8023c0b78: cam_scope_pitch_commit,
    0x8023c0e80: cam_scope_yaw,
    0x8023c276c: cam_finder_gyro_flag,
    0x8023c28c4: cam_finder_pitch,
    0x8023c2df0: cam_finder_yaw,
    0x8023d2e24: cam_special_gyro_branch,
    0x8023d3158: cam_special_pitch_load,
    0x8023d31d0: cam_special_pitch_commit,
    0x8023d34cc: cam_special_yaw,
    0x8023c3bd8: cam_climb_yaw,
    0x8023c3c10: cam_climb_pitch,
    0x8023d0564: cam_surf_yaw,
    0x8023d0584: cam_surf_pitch,
    0x8023daad0: cam_wake_yaw,
    0x8023dab30: cam_wake_pitch,
    0x8023caa38: cam_horse_yaw,
    0x8023caaac: cam_horse_pitch,
    0x8007156c0: cam_player_calc_fallback,
}
