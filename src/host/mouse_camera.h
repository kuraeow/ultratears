#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOOK_OWNER_MOUSE = 0,
    LOOK_OWNER_GAMEPAD = 1,
} LookOwner;

typedef enum {
    LOOK_ROUTE_NONE = 0,
    LOOK_ROUTE_DIRECT_ANGLES = 1,
    LOOK_ROUTE_COMPAT_STICK = 2,
} LookRoute;

typedef enum {
    MOUSE_BACKEND_NATIVE = 0,
    MOUSE_BACKEND_LEGACY_STICK = 1,
} MouseBackend;

typedef enum {
    CAMERA_CTX_UNCLASSIFIED = 0,
    CAMERA_CTX_FREE_ROAM = 1,
    CAMERA_CTX_AIM = 2,
    CAMERA_CTX_SCOPE = 3,
    CAMERA_CTX_LOCK_ON = 4,
    CAMERA_CTX_MENU_WHEEL = 5,
    CAMERA_CTX_ULTRAHAND_LOOK = 6,
    CAMERA_CTX_ULTRAHAND_OBJECT = 7,
    CAMERA_CTX_VEHICLE = 8,
    CAMERA_CTX_DISABLED = 9,
} CameraContext;

typedef enum {
    MOUSE_RESET_INIT = 0,
    MOUSE_RESET_FOCUS_LOST = 1,
    MOUSE_RESET_CAPTURE_CHANGED = 2,
    MOUSE_RESET_CONTEXT_CHANGED = 3,
    MOUSE_RESET_OWNER_CHANGED = 4,
    MOUSE_RESET_CAMERA_GENERATION = 5,
    MOUSE_RESET_DISABLED_STATE = 6,
    MOUSE_RESET_CONFIG_CHANGED = 7,
} MouseResetReason;

typedef struct {
    int version;
    int mouse_mode;
    MouseBackend backend;
    double look_deg_per_count;
    double aim_multiplier;
    double scope_multiplier;
    int smooth_ms;
    int invert_y;
    float legacy_sensitivity;
    int trace_enabled;
} MouseCameraConfig;

typedef struct {
    uint64_t tick_id;
    uint64_t camera_generation;
    CameraContext context;
    uint64_t now_ns;
    double dt_sec;
    bool allow_manual_look;
    float current_yaw_rad;
    float current_pitch_rad;
    float pitch_min_rad;
    float pitch_max_rad;
    float current_vfov_rad;
    float neutral_vfov_rad;
} CameraFrame;

typedef struct {
    LookOwner owner;
    LookRoute route;
    uint64_t tick_id;
    uint64_t epoch;
    uint64_t camera_generation;
    CameraContext context;
    double consumed_dx;
    double consumed_dy;
    float yaw_delta_rad;
    float pitch_delta_rad;
    float target_yaw_rad;
    float target_pitch_rad;
    int32_t compat_rx;
    int32_t compat_ry;
    bool suppress_gyro;
    bool suppress_gamepad_rstick;
} LookCommand;

typedef struct {
    uint64_t epoch;
    uint64_t last_tick_id;
    uint64_t unapplied_count;
    uint64_t applied_count;
    uint64_t unclassified_count;
    LookOwner owner;
    bool captured;
    bool stick_armed;
    double pending_dx;
    double pending_dy;
    LookCommand last_cmd;
} MouseCameraDiag;

typedef uint64_t (*MouseCameraClockFn)(void);

void mouse_camera_set_clock_fn(MouseCameraClockFn fn);
uint64_t mouse_camera_now_ns(void);
void mouse_camera_advance_time(uint64_t now_ns);
void mouse_camera_sample_compat_stick(int32_t *out_rx, int32_t *out_ry);
void mouse_camera_notify_frame_boundary(void);

void mouse_camera_init_from_env(void);
void mouse_camera_configure(const MouseCameraConfig *cfg);
void mouse_camera_get_config(MouseCameraConfig *out);

void mouse_camera_push_motion(double dx, double dy, uint64_t event_ns);
void mouse_camera_offer_stick(float rx, float ry, uint64_t event_ns);
void mouse_camera_set_capture(bool captured);
void mouse_camera_reset(MouseResetReason reason);
void mouse_camera_begin_tick(const CameraFrame *frame, LookCommand *out);
void mouse_camera_mark_applied(uint64_t tick_id, uint64_t camera_generation);

LookOwner mouse_camera_current_owner(void);
bool mouse_camera_is_captured(void);
bool mouse_camera_should_suppress_gyro(void);
void mouse_camera_resolve_npad_sticks(int32_t key_lx, int32_t key_ly,
                                      int32_t key_rx, int32_t key_ry,
                                      int32_t pad_lx, int32_t pad_ly,
                                      int32_t pad_rx, int32_t pad_ry,
                                      int32_t *out_lx, int32_t *out_ly,
                                      int32_t *out_rx, int32_t *out_ry);
void mouse_camera_get_diag(MouseCameraDiag *out);

float mouse_camera_wrap_pi(double angle_rad);
float mouse_camera_clamp_pitch(double pitch_rad, float min_rad, float max_rad);
void mouse_camera_clamp_stick_vector(double vx, double vy, int32_t *out_rx, int32_t *out_ry);

#ifdef __cplusplus
}
#endif
