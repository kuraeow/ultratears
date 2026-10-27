"""Unit and behavioral tests for Phase 2 (src/host/mouse_camera.c)."""
import math
import pathlib
import subprocess
import sys
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64, env  # noqa: E402


class MouseCameraLayerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = ROOT / 'build' / ('mouse-cam-test-' + uuid.uuid4().hex)
        cls.work.mkdir(parents=True, exist_ok=True)
        harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mouse_camera.h"

int main(void) {
    char line[512], k[128], v[128], kv[260];
    mouse_camera_init_from_env();
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'e') {
            if (sscanf(line + 1, "%127s %127s", k, v) == 2) {
                snprintf(kv, sizeof kv, "%s=%s", k, v);
                _putenv(kv);
            }
            puts("ok");
        } else if (line[0] == 'i') {
            mouse_camera_init_from_env();
            puts("ok");
        } else if (line[0] == 'c') {
            int cap = 0;
            sscanf(line + 1, "%d", &cap);
            mouse_camera_set_capture(cap != 0);
            puts("ok");
        } else if (line[0] == 'r') {
            int reason = 0;
            sscanf(line + 1, "%d", &reason);
            mouse_camera_reset((MouseResetReason)reason);
            puts("ok");
        } else if (line[0] == 'm') {
            double dx = 0, dy = 0;
            unsigned long long ts = 0;
            sscanf(line + 1, "%lf %lf %llu", &dx, &dy, &ts);
            mouse_camera_push_motion(dx, dy, ts);
            puts("ok");
        } else if (line[0] == 's') {
            float rx = 0, ry = 0;
            unsigned long long ts = 0;
            sscanf(line + 1, "%f %f %llu", &rx, &ry, &ts);
            mouse_camera_offer_stick(rx, ry, ts);
            puts("ok");
        } else if (line[0] == 't') {
            unsigned long long tick = 0, gen = 1, now_ns = 0;
            int ctx = 1, allow = 1;
            double dt = 1.0 / 60.0;
            float yaw = 0, pitch = 0, pmin = -1.4f, pmax = 1.4f, cfov = 0.8726646f, nfov = 0.8726646f;
            sscanf(line + 1, "%llu %llu %d %llu %lf %d %f %f %f %f %f %f",
                   &tick, &gen, &ctx, &now_ns, &dt, &allow, &yaw, &pitch, &pmin, &pmax, &cfov, &nfov);
            CameraFrame frame = {
                .tick_id = tick,
                .camera_generation = gen,
                .context = (CameraContext)ctx,
                .now_ns = now_ns,
                .dt_sec = dt,
                .allow_manual_look = (allow != 0),
                .current_yaw_rad = yaw,
                .current_pitch_rad = pitch,
                .pitch_min_rad = pmin,
                .pitch_max_rad = pmax,
                .current_vfov_rad = cfov,
                .neutral_vfov_rad = nfov,
            };
            LookCommand cmd;
            mouse_camera_begin_tick(&frame, &cmd);
            printf("%d %d %llu %.9f %.9f %.9f %.9f %d %d %d %d\n",
                   (int)cmd.owner, (int)cmd.route, (unsigned long long)cmd.epoch,
                   cmd.yaw_delta_rad, cmd.pitch_delta_rad,
                   cmd.target_yaw_rad, cmd.target_pitch_rad,
                   cmd.compat_rx, cmd.compat_ry,
                   cmd.suppress_gyro ? 1 : 0, cmd.suppress_gamepad_rstick ? 1 : 0);
        } else if (line[0] == 'a') {
            unsigned long long tick = 0, gen = 1;
            sscanf(line + 1, "%llu %llu", &tick, &gen);
            mouse_camera_mark_applied(tick, gen);
            puts("ok");
        } else if (line[0] == 'd') {
            MouseCameraDiag diag;
            mouse_camera_get_diag(&diag);
            printf("%llu %llu %llu %llu %d %d %d %.6f %.6f\n",
                   (unsigned long long)diag.epoch,
                   (unsigned long long)diag.unapplied_count,
                   (unsigned long long)diag.applied_count,
                   (unsigned long long)diag.unclassified_count,
                   (int)diag.owner,
                   diag.captured ? 1 : 0,
                   diag.stick_armed ? 1 : 0,
                   diag.pending_dx, diag.pending_dy);
        } else if (line[0] == 'n') {
            int klx, kly, krx, kry, plx, ply, prx, pry;
            int olx = 0, oly = 0, orx = 0, ory = 0;
            sscanf(line + 1, "%d %d %d %d %d %d %d %d",
                   &klx, &kly, &krx, &kry, &plx, &ply, &prx, &pry);
            mouse_camera_resolve_npad_sticks(klx, kly, krx, kry, plx, ply, prx, pry,
                                             &olx, &oly, &orx, &ory);
            printf("%d %d %d %d\n", olx, oly, orx, ory);
        }
        fflush(stdout);
    }
    return 0;
}
'''
        (cls.work / 'harness.c').write_text(harness, encoding='utf-8')
        subprocess.run([
            str(CLANG64 / 'clang.exe'), '-O2', '-pthread',
            '-I' + str(ROOT / 'src/host'),
            str(ROOT / 'src/host/mouse_camera.c'),
            str(cls.work / 'harness.c'),
            '-o', str(cls.work / 'harness.exe')
        ], env=env(), check=True)
        cls.proc = subprocess.Popen(
            [str(cls.work / 'harness.exe')],
            env=env(), text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )

    @classmethod
    def tearDownClass(cls):
        cls.proc.stdin.close()
        cls.proc.wait(timeout=5)
        for f in cls.work.iterdir():
            f.unlink()
        cls.work.rmdir()

    def cmd(self, *args):
        self.proc.stdin.write(' '.join(map(str, args)) + '\n')
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError('Mouse camera harness exited unexpectedly')
        return line.strip().split()

    def setUp(self):
        defaults = {
            'TOTK_CAMERA_MODE': 'mouse',
            'TOTK_MOUSE_CAMERA_VERSION': '2',
            'TOTK_MOUSE_BACKEND': 'native',
            'TOTK_MOUSE_LOOK_DEG_PER_COUNT': '0.04',
            'TOTK_MOUSE_AIM_MULTIPLIER': '1.0',
            'TOTK_MOUSE_SCOPE_MULTIPLIER': '1.0',
            'TOTK_MOUSE_SMOOTH_MS': '0',
            'TOTK_MOUSE_INVERT_Y': '0',
            'TOTK_MOUSE_SENSITIVITY': '1.0',
            'TOTK_MOUSE_TRACE': '0',
        }
        for k, v in defaults.items():
            self.cmd('e', k, v)
        self.cmd('i')
        self.cmd('c', 1)

    def tick(self, tick_id, ctx=1, now_ns=1_000_000_000, dt=1.0/60.0, allow=1,
             yaw=0.0, pitch=0.0, pmin=-1.4, pmax=1.4, cfov=0.8726646, nfov=0.8726646, gen=1):
        parts = self.cmd('t', tick_id, gen, ctx, now_ns, f'{dt:.8f}', allow,
                         f'{yaw:.6f}', f'{pitch:.6f}', f'{pmin:.6f}', f'{pmax:.6f}',
                         f'{cfov:.6f}', f'{nfov:.6f}')
        return {
            'owner': int(parts[0]),
            'route': int(parts[1]),
            'epoch': int(parts[2]),
            'dyaw': float(parts[3]),
            'dpitch': float(parts[4]),
            'tyaw': float(parts[5]),
            'tpitch': float(parts[6]),
            'rx': int(parts[7]),
            'ry': int(parts[8]),
            'suppress_gyro': int(parts[9]) == 1,
            'suppress_rstick': int(parts[10]) == 1,
        }

    def diag(self):
        parts = self.cmd('d')
        return {
            'epoch': int(parts[0]),
            'unapplied': int(parts[1]),
            'applied': int(parts[2]),
            'unclassified': int(parts[3]),
            'owner': int(parts[4]),
            'captured': int(parts[5]) == 1,
            'stick_armed': int(parts[6]) == 1,
            'pending_dx': float(parts[7]),
            'pending_dy': float(parts[8]),
        }

    def test_same_total_delta_across_different_event_splits_produces_identical_rotation(self):
        splits = [
            [(100.0, -50.0)],
            [(10.0, -5.0)] * 10,
            [(2.5, -1.25)] * 40,
            [(0.5, -0.25)] * 200,
        ]
        results = []
        for idx, events in enumerate(splits, start=1):
            self.cmd('r', 0)
            self.cmd('c', 1)
            for i, (dx, dy) in enumerate(events):
                self.cmd('m', dx, dy, 1_000_000 + i * 1000)
            res = self.tick(idx)
            self.cmd('a', idx, 1)
            results.append((res['dyaw'], res['dpitch']))
        expected_yaw = math.radians(100.0 * 0.04)
        expected_pitch = math.radians(50.0 * 0.04)
        for dyaw, dpitch in results:
            self.assertAlmostEqual(dyaw, expected_yaw, places=6)
            self.assertAlmostEqual(dpitch, expected_pitch, places=6)

    def test_fps_independence_30_60_90_120_and_no_acceleration(self):
        angles = []
        for tick_id, fps in enumerate((30, 60, 90, 120), start=1):
            self.cmd('r', 0)
            self.cmd('c', 1)
            self.cmd('m', 75.0, -30.0, 10_000_000)
            res = self.tick(tick_id, dt=1.0 / fps)
            self.cmd('a', tick_id, 1)
            angles.append((res['dyaw'], res['dpitch']))
        base_yaw, base_pitch = angles[0]
        for dyaw, dpitch in angles[1:]:
            self.assertAlmostEqual(dyaw, base_yaw, places=7)
            self.assertAlmostEqual(dpitch, base_pitch, places=7)

    def test_single_unit_and_fractional_deltas_and_invert_y(self):
        self.cmd('m', 1.0, 1.0, 1000)
        res = self.tick(1)
        self.cmd('a', 1, 1)
        self.assertGreater(res['dyaw'], 0.0)
        self.assertLess(res['dpitch'], 0.0)  # +dy (down) -> negative pitch when invert_y=0

        # Fractional delta while mouse owns look
        self.cmd('m', 0.25, -0.5, 2000)
        res2 = self.tick(2)
        self.cmd('a', 2, 1)
        self.assertAlmostEqual(res2['dyaw'], math.radians(0.25 * 0.04), places=7)
        self.assertAlmostEqual(res2['dpitch'], math.radians(0.5 * 0.04), places=7)

        # Invert Y flips only vertical sign
        self.cmd('e', 'TOTK_MOUSE_INVERT_Y', '1')
        self.cmd('i')
        self.cmd('c', 1)
        self.cmd('m', 10.0, 10.0, 3000)
        res3 = self.tick(3)
        self.cmd('a', 3, 1)
        self.assertAlmostEqual(res3['dyaw'], math.radians(10.0 * 0.04), places=6)
        self.assertAlmostEqual(res3['dpitch'], math.radians(10.0 * 0.04), places=6)

    def test_diagonal_sweep_preserves_ratio_in_direct_and_compat_stick_modes(self):
        # Direct mode large diagonal sweep
        self.cmd('m', 600.0, -300.0, 1000)
        res = self.tick(1)
        self.cmd('a', 1, 1)
        self.assertAlmostEqual(res['dyaw'] / res['dpitch'], 2.0, places=5)

        # Menu wheel / compat stick mode large diagonal sweep: radial clamp preserves 2:1 ratio
        self.cmd('m', 2000.0, -1000.0, 2000)
        res_wheel = self.tick(2, ctx=5, dt=1.0 / 60.0)  # CAMERA_CTX_MENU_WHEEL = 5
        self.assertEqual(res_wheel['route'], 2)  # LOOK_ROUTE_COMPAT_STICK
        mag = math.hypot(res_wheel['rx'], res_wheel['ry'])
        self.assertAlmostEqual(mag, 32767.0, delta=2.0)
        self.assertAlmostEqual(res_wheel['rx'] / res_wheel['ry'], 2.0, delta=0.01)

    def test_single_consumption_and_unapplied_motion_detection(self):
        self.cmd('m', 50.0, -25.0, 1000)
        first = self.tick(10)
        self.assertEqual(first['route'], 1)
        self.assertGreater(abs(first['dyaw']), 0.0)
        # Mark applied on tick 10
        self.cmd('a', 10, 1)
        # Second query on same tick 10 returns route=NONE and 0 delta
        dup = self.tick(10)
        self.assertEqual(dup['route'], 0)
        self.assertEqual(dup['dyaw'], 0.0)
        self.assertEqual(dup['dpitch'], 0.0)
        self.assertEqual(self.diag()['unapplied'], 0)
        self.assertEqual(self.diag()['applied'], 1)

        # Now test unapplied motion: tick 11 consumes motion, never calls mark_applied, then tick 12 runs
        self.cmd('m', 40.0, -20.0, 2000)
        t11 = self.tick(11)
        self.assertGreater(abs(t11['dyaw']), 0.0)
        # Advance to tick 12 without marking tick 11 applied:
        t12 = self.tick(12)
        self.assertEqual(t12['dyaw'], 0.0)  # Never replays stale motion
        self.assertEqual(self.diag()['unapplied'], 1)

    def test_long_frame_preserves_motion_without_replay(self):
        self.cmd('m', 120.0, -60.0, 1000)
        res = self.tick(1, dt=0.200)  # 200ms long frame
        self.cmd('a', 1, 1)
        self.assertAlmostEqual(res['dyaw'], math.radians(120.0 * 0.04), places=6)
        res_next = self.tick(2, dt=0.016)
        self.assertEqual(res_next['dyaw'], 0.0)

    def test_stop_has_zero_tail_when_smooth_zero_and_exponential_smoothing_conserves_angle(self):
        self.cmd('m', 100.0, -50.0, 1000)
        r1 = self.tick(1)
        self.cmd('a', 1, 1)
        self.assertGreater(abs(r1['dyaw']), 0.0)
        r2 = self.tick(2)
        self.assertEqual(r2['dyaw'], 0.0)
        self.assertEqual(r2['dpitch'], 0.0)

        # Enable 10ms smoothing and verify total integrated angle equals unsmoothed target
        self.cmd('e', 'TOTK_MOUSE_SMOOTH_MS', '10')
        self.cmd('i')
        self.cmd('c', 1)
        self.cmd('m', 100.0, -50.0, 2000)
        total_yaw = 0.0
        total_pitch = 0.0
        for t in range(1, 40):
            rt = self.tick(t, dt=1.0 / 60.0)
            if rt['route'] == 1 and (abs(rt['dyaw']) > 0 or abs(rt['dpitch']) > 0):
                self.cmd('a', t, 1)
            total_yaw += rt['dyaw']
            total_pitch += rt['dpitch']
        self.assertAlmostEqual(total_yaw, math.radians(100.0 * 0.04), places=5)
        self.assertAlmostEqual(total_pitch, math.radians(50.0 * 0.04), places=5)

        # Verify context switch kills remaining smoothing tail immediately
        self.cmd('m', 100.0, -50.0, 3000)
        first_step = self.tick(50, ctx=1, dt=0.005)
        self.cmd('a', 50, 1)
        self.assertLess(first_step['dyaw'], math.radians(100.0 * 0.04))
        # Switch context to AIM (ctx=2) with no new mouse input: tail from FREE_ROAM must not bleed
        switched = self.tick(51, ctx=2, dt=0.005)
        self.assertEqual(switched['dyaw'], 0.0)
        self.assertEqual(switched['dpitch'], 0.0)

    def test_focus_loss_f8_and_loading_clear_pending_motion(self):
        self.cmd('m', 80.0, -40.0, 1000)
        self.cmd('c', 0)  # F8 release or focus lost
        self.cmd('c', 1)
        res = self.tick(1)
        self.assertEqual(res['dyaw'], 0.0)

        # Loading / cutscene (ctx=9 CAMERA_CTX_DISABLED) drops pending motion
        self.cmd('m', 80.0, -40.0, 2000)
        disabled = self.tick(2, ctx=9, allow=0)
        self.assertEqual(disabled['route'], 0)
        self.assertEqual(disabled['dyaw'], 0.0)
        resumed = self.tick(3, ctx=1, allow=1)
        self.assertEqual(resumed['dyaw'], 0.0)

    def test_stick_and_mouse_ownership_handoff_rules(self):
        # 1. Start in mouse ownership; stick drift (<25%) does not steal ownership
        self.cmd('s', 0.18, 0.10, 10_000_000)
        self.cmd('s', 0.18, 0.10, 100_000_000)
        self.assertEqual(self.diag()['owner'], 0)  # LOOK_OWNER_MOUSE

        # 2. Stick > 25% for < 50ms (e.g. 30ms) does not steal ownership
        self.cmd('s', 0.50, 0.0, 200_000_000)
        self.cmd('s', 0.50, 0.0, 230_000_000)
        self.assertEqual(self.diag()['owner'], 0)

        # 3. Stick > 25% for >= 50ms steals ownership to GAMEPAD
        self.cmd('s', 0.50, 0.0, 255_000_000)
        self.assertEqual(self.diag()['owner'], 1)  # LOOK_OWNER_GAMEPAD
        r_pad = self.tick(1)
        self.assertEqual(r_pad['owner'], 1)
        self.assertFalse(r_pad['suppress_gyro'])

        # 4. Fractional mouse motion (< 1.0 count) does not reclaim yet, but accumulates
        self.cmd('m', 0.4, 0.3, 260_000_000)  # hypot = 0.5 < 1.0
        self.assertEqual(self.diag()['owner'], 1)
        # Next motion brings accumulated hypot to >= 1.0: reclaims MOUSE and keeps full accumulated delta!
        self.cmd('m', 0.4, 0.3, 265_000_000)  # total dx=0.8, dy=0.6 -> hypot = 1.0
        self.assertEqual(self.diag()['owner'], 0)
        r_mouse = self.tick(2)
        self.cmd('a', 2, 1)
        self.assertEqual(r_mouse['owner'], 0)
        self.assertTrue(r_mouse['suppress_gyro'])
        self.assertAlmostEqual(r_mouse['dyaw'], math.radians(0.8 * 0.04), places=6)
        self.assertAlmostEqual(r_mouse['dpitch'], math.radians(-0.6 * 0.04), places=6)

        # 5. Stick is STILL tilted (0.50): must NOT reclaim ownership until it returns to <15% for 100ms
        self.cmd('s', 0.60, 0.0, 300_000_000)
        self.cmd('s', 0.60, 0.0, 400_000_000)
        self.assertEqual(self.diag()['owner'], 0)  # Still MOUSE!

        # 6. Stick enters <15% for 60ms (not yet 100ms), then pushes >25%: still disarmed!
        self.cmd('s', 0.05, 0.0, 500_000_000)
        self.cmd('s', 0.05, 0.0, 560_000_000)
        self.cmd('s', 0.60, 0.0, 570_000_000)
        self.cmd('s', 0.60, 0.0, 650_000_000)
        self.assertEqual(self.diag()['owner'], 0)

        # 7. Stick stays <15% for >= 100ms to re-arm, then >25% for >= 50ms to reclaim GAMEPAD
        self.cmd('s', 0.02, 0.0, 700_000_000)
        self.cmd('s', 0.02, 0.0, 805_000_000)  # 105ms in neutral -> armed!
        self.assertTrue(self.diag()['stick_armed'])
        self.cmd('s', 0.40, 0.0, 810_000_000)
        self.cmd('s', 0.40, 0.0, 865_000_000)  # 55ms > 25% -> GAMEPAD!
        self.assertEqual(self.diag()['owner'], 1)

    def test_steady_stick_takeover_and_rearm_advance_on_camera_ticks_without_extra_events(self):
        # Single stick deflection event at t=100ms, held completely steady (no further SDL axis events)
        self.cmd('s', 0.60, 0.0, 100_000_000)
        self.assertEqual(self.diag()['owner'], 0)

        # At t=130ms (30ms elapsed), camera tick runs -> still MOUSE
        t1 = self.tick(1, now_ns=130_000_000)
        self.assertEqual(t1['owner'], 0)

        # At t=150ms (50ms elapsed), camera tick runs -> transitions to GAMEPAD without needing another stick event!
        t2 = self.tick(2, now_ns=150_000_000)
        self.assertEqual(t2['owner'], 1)
        self.assertEqual(self.diag()['owner'], 1)

        # Mouse reclaims ownership at t=160ms, disarming the stick
        self.cmd('m', 2.0, 0.0, 160_000_000)
        self.assertEqual(self.diag()['owner'], 0)
        self.assertFalse(self.diag()['stick_armed'])

        # Single stick release to neutral at t=200ms, then untouched (no further SDL axis events)
        self.cmd('s', 0.0, 0.0, 200_000_000)
        self.assertFalse(self.diag()['stick_armed'])

        # At t=260ms (60ms in neutral), camera tick runs -> still disarmed
        self.tick(3, now_ns=260_000_000)
        self.assertFalse(self.diag()['stick_armed'])

        # At t=300ms (100ms in neutral), camera tick runs -> re-armed without needing another stick event!
        self.tick(4, now_ns=300_000_000)
        self.assertTrue(self.diag()['stick_armed'])

    def test_no_axis_mixing_between_devices(self):
        # Left stick: keyboard has (32767, 0), pad has (0, 20000). Stronger vector wins as a whole!
        parts = list(map(int, self.cmd('n', 32767, 0, 0, 0, 0, 20000, 15000, -15000)))
        # Mouse owns look -> right stick is (0, 0), left stick is keyboard's (32767, 0) without mixing pad's Y=20000!
        self.assertEqual(parts, [32767, 0, 0, 0])

    def test_fov_scaling_in_aim_and_scope_and_lock_on_preservation(self):
        self.cmd('e', 'TOTK_MOUSE_AIM_MULTIPLIER', '0.8')
        self.cmd('e', 'TOTK_MOUSE_SCOPE_MULTIPLIER', '0.5')
        self.cmd('i')
        self.cmd('c', 1)

        nfov = math.radians(50.0)
        cfov = math.radians(25.0)
        fov_scale = math.tan(cfov / 2.0) / math.tan(nfov / 2.0)

        self.cmd('m', 100.0, 0.0, 1000)
        aim_res = self.tick(1, ctx=2, cfov=cfov, nfov=nfov)  # CAMERA_CTX_AIM = 2
        self.cmd('a', 1, 1)
        expected_aim = math.radians(100.0 * 0.04 * fov_scale * 0.8)
        self.assertAlmostEqual(aim_res['dyaw'], expected_aim, places=6)

        self.cmd('m', 100.0, 0.0, 2000)
        scope_res = self.tick(2, ctx=3, cfov=cfov, nfov=nfov)  # CAMERA_CTX_SCOPE = 3
        self.cmd('a', 2, 1)
        expected_scope = math.radians(100.0 * 0.04 * fov_scale * 0.5)
        self.assertAlmostEqual(scope_res['dyaw'], expected_scope, places=6)

        # Lock-on (ctx=4): mouse motion does not break lock-on or emit Npad stick
        self.cmd('m', 100.0, -50.0, 3000)
        lock_res = self.tick(3, ctx=4)
        self.assertEqual(lock_res['route'], 0)
        self.assertEqual(lock_res['rx'], 0)
        self.assertEqual(lock_res['ry'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
