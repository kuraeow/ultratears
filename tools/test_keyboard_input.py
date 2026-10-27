"""Compile the real mapping and HID-state code and inspect the controller seen by the game."""
import pathlib
import subprocess
import sys
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from input_config import InputSettings, ACTIONS
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64, env


class KeyboardInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = ROOT / 'build' / ('input-test-' + uuid.uuid4().hex)
        cls.work.mkdir(mode=0o777)
        source = r'''
#include <stdio.h>
#include <stdlib.h>
#include "host.h"
void hle_native_thunk(Cpu *c) { (void)c; }
int runtime_handheld_mode(void) { return 0; }
uint64_t runtime_now_ns(void) { return 1000000000; }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); exit(2); }
#include "mouse_camera.c"
#include "overrides_input.c"
#include "keyboard_input.c"
int main(void) {
    char line[256], name[80], value[100], setting[200]; unsigned scan, down;
    unsigned long long now, bits; float dx, dy; int lx, ly, rx, ry;
    initialize();
    while (fgets(line, sizeof line, stdin)) {
        switch (line[0]) {
        case 'e':
            sscanf(line + 1, "%79s %99s", name, value);
            snprintf(setting, sizeof setting, "%s=%s", name, value); _putenv(setting); puts("ok"); break;
        case 'i': keyboard_input_clear(); keyboard_input_init(); puts("ok"); break;
        case 'k': sscanf(line + 1, "%u %u", &scan, &down); keyboard_input_key(scan, down); puts("ok"); break;
        case 'b': sscanf(line + 1, "%u %u", &scan, &down); keyboard_input_mouse_button(scan, down); puts("ok"); break;
        case 'r': keyboard_input_clear(); puts("ok"); break;
        case 'm': sscanf(line + 1, "%f %f", &dx, &dy); runtime_input_mouse_motion(dx, dy); puts("ok"); break;
        case 't': sscanf(line + 1, "%llu", &now); runtime_input_mouse_frame(now); puts("ok"); break;
        case 'c': runtime_input_mouse_clear(); keyboard_input_release_mouse_buttons(); puts("ok"); break;
        case 'p': {
            sscanf(line + 1, "%llu %d %d %d %d", &bits, &lx, &ly, &rx, &ry);
            RuntimeGamepadState pad = {.buttons=bits,.lx=lx,.ly=ly,.rx=rx,.ry=ry,.connected=1};
            runtime_gamepad_set(&pad); puts("ok"); break;
        }
        case 's': {
            NpadState state; uint32_t id = 0; full_states(&state, 1, &id);
            printf("%llu %d %d %d %d\n", (unsigned long long)state.buttons, state.lx, state.ly, state.rx, state.ry);
            break;
        }
        }
        fflush(stdout);
    }
}
'''
        (cls.work / 'test.c').write_text(source)
        subprocess.run([str(CLANG64 / 'clang.exe'), '-O2', '-pthread',
                        '-I' + str(ROOT / 'src/host'), '-I' + str(ROOT / 'src/runtime'),
                        str(cls.work / 'test.c'), '-o', str(cls.work / 'test.exe')], env=env(), check=True)
        cls.process = subprocess.Popen([str(cls.work / 'test.exe')], env=env(), text=True,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    @classmethod
    def tearDownClass(cls):
        cls.process.stdin.close()
        cls.process.wait(timeout=5)
        for file in cls.work.iterdir(): file.unlink()
        cls.work.rmdir()

    def request(self, *args):
        self.process.stdin.write(' '.join(map(str, args)) + '\n')
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line: raise RuntimeError('Native input harness exited')
        return line.split()

    def state(self): return tuple(map(int, self.request('s')))

    def configure(self, config=None):
        for key, value in InputSettings(config).runtime_values().items(): self.request('e', key, value)
        self.request('i')

    def setUp(self): self.configure()

    def test_default_buttons_reach_correct_switch_bits(self):
        bits = dict(A=1, B=2, X=4, Y=8, STICK_L=16, STICK_R=32, L=64, R=128,
                    ZL=256, ZR=512, PLUS=1024, MINUS=2048,
                    DPAD_LEFT=4096, DPAD_UP=8192, DPAD_RIGHT=16384, DPAD_DOWN=32768)
        for action in ACTIONS:
            if action['id'] not in bits: continue
            for scan in action['defaults']:
                if not scan: continue
                self.request('r'); self.request('k', scan, 1)
                self.assertEqual(self.state()[0], bits[action['id']], action['id'])

    def test_remap_replaces_default_and_secondary_survives_primary_release(self):
        self.configure({'TOTK_KEY_A': '27,25'})
        self.request('k', 8, 1)  # old E binding no longer drives A
        self.assertEqual(self.state()[0], 0)
        self.request('k', 27, 1); self.request('k', 25, 1)
        self.assertEqual(self.state()[0], 1)
        self.request('k', 27, 0)
        self.assertEqual(self.state()[0], 1)
        self.request('k', 25, 0)
        self.assertEqual(self.state()[0], 0)

    def test_quick_tap_is_reported_once_and_focus_clear_unsticks_keys(self):
        self.request('k', 8, 1); self.request('k', 8, 0)
        self.assertEqual(self.state()[0], 1)
        self.assertEqual(self.state()[0], 0)
        self.request('k', 26, 1)
        self.assertEqual(self.state()[2], 32767)
        self.request('r')
        self.assertEqual(self.state(), (0, 0, 0, 0, 0))
        self.request('k', 26, 1)
        self.assertEqual(self.state()[2], 32767)

    def test_remapped_sticks_and_opposite_directions(self):
        self.configure({'TOTK_KEY_MOVE_UP': '28,0', 'TOTK_KEY_LOOK_UP': '75,0'})
        self.request('k', 26, 1)
        self.assertEqual(self.state()[2], 0)
        self.request('k', 28, 1); self.request('k', 75, 1)
        self.assertEqual(self.state()[2:], (32767, 0, 32767))
        self.request('k', 22, 1)
        self.assertEqual(self.state()[2], 0)

    def test_mouse_velocity_is_stable_across_fps_and_repeated_hid_polls(self):
        # In native direct camera mode (default), mouse motion does NOT inject Npad right-stick deflection
        self.configure({'TOTK_CAMERA_MODE': 'mouse', 'TOTK_MOUSE_BACKEND': 'native'})
        self.request('c'); self.request('t', 1_000_000_000)
        self.request('m', 10, -5); self.request('t', 1_020_000_000)
        self.assertEqual(self.state()[3:], (0, 0))

        # In legacy_stick compatibility mode, mouse motion produces an FPS-stable Npad right-stick sample
        self.configure({'TOTK_CAMERA_MODE': 'mouse', 'TOTK_MOUSE_BACKEND': 'legacy_stick'})
        for fps in (30, 60, 90, 120):
            self.request('c'); self.request('t', 1_000_000_000)
            self.request('m', 250 / fps, -125 / fps)
            self.request('t', 1_000_000_000 + round(1e9 / fps))
            first = self.state()
            self.assertAlmostEqual(first[3], 8192, delta=1)
            self.assertAlmostEqual(first[4], 4096, delta=1)
            self.assertEqual(self.state(), first)
            self.request('t', 1_000_000_000 + round(2e9 / fps))
            self.assertEqual(self.state()[3:], (0, 0))

    def test_camera_modes_sensitivity_inversion_and_release(self):
        self.request('k', 82, 1); self.request('m', 5, -5)
        self.assertEqual(self.state()[4], 32767)  # arrows mode ignores mouse
        self.configure({'TOTK_CAMERA_MODE': 'mouse', 'TOTK_MOUSE_BACKEND': 'legacy_stick',
                        'TOTK_MOUSE_SENSITIVITY': '2.0', 'TOTK_MOUSE_INVERT_Y': '1'})
        self.request('k', 82, 1)
        self.assertEqual(self.state()[4], 0)  # mouse mode ignores camera keys
        self.request('t', 1_000_000_000); self.request('m', 10, -5); self.request('t', 1_020_000_000)
        rx, ry = self.state()[3:]
        # Radial clamping preserves the 2:-1 direction without saturating X independently
        self.assertAlmostEqual(rx, 29308, delta=2)
        self.assertAlmostEqual(ry, -14654, delta=2)
        self.request('c')
        self.assertEqual(self.state()[3:], (0, 0))

    def test_gamepad_input_is_preserved_and_combined_without_axis_mixing(self):
        self.request('k', 8, 1); self.request('p', 8, 12000, 0, 22000, -18000)
        self.assertEqual(self.state(), (9, 12000, 0, 22000, -18000))
        # When keyboard holds W (ly=32767) and gamepad has lx=12000, ly=0, keyboard's stronger 2D vector wins without mixing lx=12000
        self.request('k', 26, 1)
        self.assertEqual(self.state(), (9, 0, 32767, 22000, -18000))

    def test_unbound_reserved_and_duplicate_config_match_launcher(self):
        profile = InputSettings({'TOTK_KEY_A': '0,0', 'TOTK_KEY_B': '65,68', 'TOTK_KEY_Y': '4,0'})
        self.configure(profile.runtime_values())
        self.request('k', 8, 1)
        self.assertEqual(self.state()[0], 0)
        self.request('k', 225, 1)
        self.assertEqual(self.state()[0], 2)  # invalid F8/F11 settings use defaults
        self.request('k', 4, 1)
        self.assertEqual(self.state()[0], 2)  # duplicate A key drives movement, not Y

    def test_native_loader_handles_invalid_and_duplicate_hand_edited_keys(self):
        for invalid in ('not-a-key', '65,0', '68,0', '515,0', '-1,0'):
            self.request('e', 'TOTK_KEY_A', invalid)
            self.request('i'); self.request('k', 8, 1)
            self.assertEqual(self.state()[0], 1)
        self.request('e', 'TOTK_KEY_A', '4,0')
        self.request('i'); self.request('k', 4, 1)
        self.assertEqual(self.state()[0], 0)
        self.assertEqual(self.state()[1], -32767)

    def test_shift_sprint_and_space_jump_work_together_while_moving(self):
        self.request('k', 26, 1)   # W
        self.request('k', 225, 1)  # Left Shift -> Switch B
        self.request('k', 44, 1)   # Space -> Switch X
        self.assertEqual(self.state(), (2 | 4, 0, 32767, 0, 0))

    def test_mouse_buttons_drive_combat_and_release_with_cursor(self):
        self.request('b', 1, 1)  # SDL left -> Switch Y
        self.request('b', 3, 1)  # SDL right -> Switch ZR
        self.request('b', 2, 1)  # SDL middle -> Switch ZL
        self.assertEqual(self.state()[0], 8 | 512 | 256)
        self.request('c')
        self.assertEqual(self.state()[0], 0)
        self.request('b', 3, 1); self.request('k', 21, 1)  # RMB + its R alternative
        self.assertEqual(self.state()[0], 512)
        self.request('b', 3, 0)
        self.assertEqual(self.state()[0], 512)
        self.request('k', 21, 0)
        self.assertEqual(self.state()[0], 0)


if __name__ == '__main__': unittest.main(verbosity=2)
