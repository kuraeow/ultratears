#ifndef ULTRATEARS_CAMERA_V143_H
#define ULTRATEARS_CAMERA_V143_H

#include <stdint.h>
#include <stdbool.h>
#include "mouse_camera.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Cpu;

/* Verified TOTK 1.4.3 ExecutePlayerCamera* mode calc entry points (vtable + 0x120) */
#define TOTK_V143_CALC_CHASE              0x80018e414ull
#define TOTK_V143_CALC_ABYSS              0x8023bd8d8ull
#define TOTK_V143_CALC_AIMING             0x8023be978ull
#define TOTK_V143_CALC_AIMING_TELESCOPE   0x8023c08acull
#define TOTK_V143_CALC_CAMERA_FINDER      0x8023c26c8ull
#define TOTK_V143_CALC_CLIMB_OBJ          0x8023c3a98ull
#define TOTK_V143_CALC_HORSE              0x8023ca518ull
#define TOTK_V143_CALC_HORSE_LOCKON_EMPTY 0x8023cc210ull
#define TOTK_V143_CALC_HORSE_REVOLVE      0x8023ccff8ull
#define TOTK_V143_CALC_LOCKON_AIMING_AT   0x8023ce260ull
#define TOTK_V143_CALC_SHIELD_SURFING     0x8023d0088ull
#define TOTK_V143_CALC_SPECIAL_POWER      0x8023d2d40ull
#define TOTK_V143_CALC_TAIL               0x8023d4950ull
#define TOTK_V143_CALC_LOCKON_TARGET      0x8023d7140ull
#define TOTK_V143_CALC_ULTRAHAND          0x8023d8e74ull
#define TOTK_V143_CALC_WAKEBOARD          0x8023da61cull
#define TOTK_V143_CALC_WATERFALL_CLIMB    0x8023dce00ull

/* Verified TOTK 1.4.3 right-stick deadzone and response curve constants (f_800874c1c) */
#define TOTK_V143_STICK_INNER_DEADZONE    0.005f
#define TOTK_V143_STICK_OUTER_THRESHOLD   0.995f
#define TOTK_V143_STICK_RESCALE_INV       1.010101f
#define TOTK_V143_STICK_ACTIVE_RADIAL_SQ  0.01f

/* Pure context classifier exposed for unit testing */
CameraContext totk_camera_v143_classify_context(uint64_t calc_fn_addr,
                                                bool stick_blocked_by_ui_or_manip,
                                                bool *out_menu_wheel,
                                                bool *out_lock_on,
                                                bool *out_ultrahand_manip,
                                                bool *out_vehicle_manual_ok);

/* Query whether gyro should be suppressed from camera/aiming while mouse owns look */
int totk_camera_v143_suppress_gyro(void);

/* Reset adapter state (for unit tests) */
void totk_camera_v143_reset_state(void);
void totk_camera_v143_set_test_time_ns(uint64_t now_ns);
void totk_camera_v143_get_branch_counts(uint64_t *out_normal, uint64_t *out_alt);

/* Guest hook callbacks invoked from lifted TOTK 1.4.3 main module */
void totk_camera_v143_on_read_stick(struct Cpu *c);
void totk_camera_v143_on_switcher_mode(struct Cpu *c);
void totk_camera_v143_on_mode_pre_calc(struct Cpu *c);

void totk_camera_v143_on_chase_yaw(struct Cpu *c);
void totk_camera_v143_on_chase_pitch(struct Cpu *c);

void totk_camera_v143_on_tail_yaw(struct Cpu *c);
void totk_camera_v143_on_tail_pitch(struct Cpu *c);

void totk_camera_v143_on_aim_pitch_add(struct Cpu *c);
int  totk_camera_v143_on_aim_pitch_commit(struct Cpu *c);
void totk_camera_v143_on_aim_yaw(struct Cpu *c);

void totk_camera_v143_on_scope_pitch_load(struct Cpu *c);
void totk_camera_v143_on_scope_pitch_commit(struct Cpu *c);
void totk_camera_v143_on_scope_yaw(struct Cpu *c);

void totk_camera_v143_on_finder_pitch(struct Cpu *c);
void totk_camera_v143_on_finder_yaw(struct Cpu *c);

void totk_camera_v143_on_special_pitch_load(struct Cpu *c);
void totk_camera_v143_on_special_pitch_commit(struct Cpu *c);
void totk_camera_v143_on_special_yaw(struct Cpu *c);

void totk_camera_v143_on_climb_yaw(struct Cpu *c);
void totk_camera_v143_on_climb_pitch(struct Cpu *c);

void totk_camera_v143_on_surf_yaw(struct Cpu *c);
void totk_camera_v143_on_surf_pitch(struct Cpu *c);

void totk_camera_v143_on_wake_yaw(struct Cpu *c);
void totk_camera_v143_on_wake_pitch(struct Cpu *c);

void totk_camera_v143_on_horse_yaw(struct Cpu *c);
void totk_camera_v143_on_horse_pitch(struct Cpu *c);

void totk_camera_v143_on_player_cam_calc(struct Cpu *c);

#ifdef __cplusplus
}
#endif

#endif /* ULTRATEARS_CAMERA_V143_H */
