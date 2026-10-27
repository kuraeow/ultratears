"""Real Qt key capture, input-profile persistence and process-environment tests."""
import os
import pathlib
import sys
import unittest
import uuid
from unittest.mock import patch

os.environ['QT_QPA_PLATFORM'] = 'offscreen'
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import prelauncher as launcher
from controls_widgets import event_scancode
from input_config import ACTIONS, InputSettings
from PyQt6.QtCore import Qt, QEvent
from PyQt6.QtGui import QKeyEvent
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication, QMessageBox


class ControlsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.work = ROOT / 'build' / ('controls-test-' + uuid.uuid4().hex)
        self.work.mkdir(mode=0o777)
        self.config = self.work / 'ultratears.cfg'
        self.config.write_text('# Preserve\nromfs=data/romfs\nTOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=120\n')
        self.exe = self.work / 'fixture.exe'
        self.exe.touch()
        self.patchers = [patch.object(launcher, 'CFG_PATH', self.config),
                         patch.object(launcher, 'LOG_PATH', self.work / 'test.log'),
                         patch.object(launcher, 'EXE_PATH', self.exe),
                         patch.object(launcher, 'is_controller_connected', return_value=False)]
        for item in self.patchers: item.start()
        self.windows = []
        self.window = self.open_window()
        self.page = self.window.controls_page
        self.window.pages_stack.setCurrentIndex(2)
        self.window.show()
        self.app.processEvents()

    def open_window(self):
        window = launcher.PrelauncherWindow()
        self.windows.append(window)
        return window

    def tearDown(self):
        for window in self.windows: window.close()
        for item in reversed(self.patchers): item.stop()
        for file in self.work.iterdir(): file.unlink()
        self.work.rmdir()

    def capture(self, action, slot, key):
        button = self.page.buttons[action][slot]
        QTest.mouseClick(button, Qt.MouseButton.LeftButton)
        self.assertTrue(button.waiting)
        QTest.keyClick(button, key)
        return button

    def test_key_capture_saves_and_restores_actual_scancodes(self):
        self.capture('A', 0, Qt.Key.Key_O)
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_KEY_A'], '18,0')
        restored = self.open_window()
        self.assertEqual(restored.controls_page.buttons['A'][0].text(), 'O')
        self.assertEqual(restored.runtime_settings()['TOTK_KEY_A'], '18,0')
        self.assertEqual(restored.runtime_settings()['TOTK_UNCAPPED_HZ'], '120')

    def test_conflict_can_be_cancelled_or_moved_without_duplicate_bindings(self):
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.No):
            self.capture('A', 0, Qt.Key.Key_Space)
        self.assertEqual(self.window.input_settings.bindings['A'][0], 8)
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            self.capture('A', 0, Qt.Key.Key_Space)
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_KEY_A'], '44,0')
        self.assertEqual(saved['TOTK_KEY_X'], '0,0')
        self.assertEqual(self.page.buttons['X'][0].text(), 'Unbound')

    def test_alternative_key_and_right_click_clear(self):
        self.capture('A', 1, Qt.Key.Key_I)
        QTest.mouseClick(self.page.buttons['A'][0], Qt.MouseButton.RightButton)
        self.assertEqual(launcher.read_config()['TOTK_KEY_A'], '0,12')
        self.assertEqual(self.page.buttons['A'][0].text(), 'Unbound')

    def test_tab_is_captured_and_fullscreen_capture_shortcuts_are_reserved(self):
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            self.capture('A', 0, Qt.Key.Key_Tab)
        self.assertEqual(launcher.read_config()['TOTK_KEY_A'], '43,0')
        for key in (Qt.Key.Key_F8, Qt.Key.Key_F11):
            self.capture('A', 0, key)
            self.assertIn('reserved', self.page.status.text())
            self.assertEqual(launcher.read_config()['TOTK_KEY_A'], '43,0')

    def test_windows_physical_keys_work_with_non_latin_and_extended_keys(self):
        def event(key, scan, vk):
            return QKeyEvent(QEvent.Type.KeyPress, key, Qt.KeyboardModifier.NoModifier, scan, vk, 0)
        self.assertEqual(event_scancode(event(0x634, 0x1e, 0x41)), 4)  # Arabic layout, physical A
        self.assertEqual(event_scancode(event(Qt.Key.Key_Z, 0x15, 0x5a)), 28)  # physical Y
        self.assertEqual(event_scancode(event(Qt.Key.Key_AltGr, 0x138, 0x12)), 230)
        self.assertEqual(event_scancode(event(Qt.Key.Key_End, 0x4f, 0x23)), 89)  # numpad 1

    def test_modifiers_and_escape_can_be_assigned_as_single_keys(self):
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            self.capture('A', 0, Qt.Key.Key_Control)
        self.assertEqual(launcher.read_config()['TOTK_KEY_A'], '224,0')
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            self.capture('A', 0, Qt.Key.Key_Escape)
        self.assertEqual(launcher.read_config()['TOTK_KEY_A'], '41,0')
        self.assertEqual(launcher.read_config()['TOTK_KEY_B'], '225,0')

    def test_mouse_buttons_can_be_captured_moved_and_restored(self):
        for mouse_button, code in ((Qt.MouseButton.LeftButton, 512),
                                   (Qt.MouseButton.RightButton, 513),
                                   (Qt.MouseButton.MiddleButton, 514)):
            button = self.page.buttons['A'][0]
            QTest.mouseClick(button, Qt.MouseButton.LeftButton)
            self.assertTrue(button.waiting)
            with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
                QTest.mouseClick(button, mouse_button)
            self.assertFalse(button.waiting)
            self.assertEqual(launcher.read_config()['TOTK_KEY_A'], f'{code},0')
            self.assertEqual(self.open_window().input_settings.bindings['A'][0], code)

    def test_camera_mode_sensitivity_and_inversion_round_trip(self):
        self.page.camera_mode.setCurrentIndex(self.page.camera_mode.findData('mouse'))
        self.page.sensitivity.setValue(1.7)
        self.page.invert_y.setChecked(True)
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_CAMERA_MODE'], 'mouse')
        self.assertEqual(saved['TOTK_MOUSE_SENSITIVITY'], '1.70')
        self.assertEqual(saved['TOTK_MOUSE_INVERT_Y'], '1')
        self.assertFalse(self.page.buttons['LOOK_UP'][0].isEnabled())
        self.assertTrue(self.page.buttons['A'][0].isEnabled())
        restored = self.open_window()
        self.assertEqual(restored.input_settings.camera_mode, 'mouse')
        self.assertEqual(restored.controls_page.sensitivity.value(), 1.7)
        restored.controls_page.camera_mode.setCurrentIndex(0)
        self.assertTrue(restored.controls_page.buttons['LOOK_UP'][0].isEnabled())
        self.assertEqual(restored.input_settings.bindings['LOOK_UP'][0], 82)

    def test_restore_defaults_only_resets_input_preferences(self):
        self.capture('A', 0, Qt.Key.Key_O)
        self.page.camera_mode.setCurrentIndex(1)
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.StandardButton.Yes):
            self.page.restore_defaults()
        self.assertEqual(self.window.input_settings.runtime_values(), InputSettings().runtime_values())
        self.assertEqual(launcher.read_config()['TOTK_UNCAPPED_HZ'], '120')
        self.assertEqual(launcher.read_config()['romfs'], 'data/romfs')

    def test_failed_save_restores_input_and_visible_binding(self):
        before = self.config.read_bytes()
        with patch.object(launcher, 'write_config_values', side_effect=PermissionError('read only')), \
             patch.object(QMessageBox, 'warning'):
            self.capture('A', 0, Qt.Key.Key_O)
        self.assertEqual(self.window.input_settings.bindings['A'][0], 8)
        self.assertEqual(self.page.buttons['A'][0].text(), 'E')
        self.assertIn('Not saved', self.page.status.text())
        self.assertEqual(self.config.read_bytes(), before)

    def test_launch_environment_contains_saved_mapping_and_camera_values(self):
        self.capture('A', 0, Qt.Key.Key_O)
        self.page.camera_mode.setCurrentIndex(1)
        with patch.dict(os.environ, {'TOTK_KEY_A': '8,0', 'TOTK_CAMERA_MODE': 'keys'}), \
             patch.object(launcher.subprocess, 'Popen') as start:
            self.window.launch_game()
        child = start.call_args.kwargs['env']
        saved = launcher.read_config()
        for key, value in self.window.input_settings.runtime_values().items():
            self.assertEqual(child[key], value)
            self.assertEqual(saved[key], value)
        self.assertEqual(child['TOTK_KEY_A'], '18,0')
        self.assertEqual(child['TOTK_CAMERA_MODE'], 'mouse')
        self.assertEqual(len(self.page.buttons), len(ACTIONS))

    def test_direct_mouse_camera_v2_settings_and_legacy_migration_backup(self):
        # Legacy v1 config with TOTK_MOUSE_SENSITIVITY=2.50 must not alter look_deg_per_count default (0.04)
        self.config.write_text(
            '# Legacy v1\nromfs=data/romfs\nTOTK_CAMERA_MODE=mouse\n'
            'TOTK_MOUSE_SENSITIVITY=2.50\nTOTK_MOUSE_INVERT_Y=1\nTOTK_KEY_A=18,0\n'
        )
        v1_bytes = self.config.read_bytes()
        w = self.open_window()
        self.assertEqual(w.input_settings.camera_mode, 'mouse')
        self.assertEqual(w.input_settings.mouse_backend, 'native')
        self.assertAlmostEqual(w.input_settings.look_deg_per_count, 0.04, places=5)
        self.assertAlmostEqual(w.input_settings.aim_multiplier, 1.0, places=5)
        self.assertAlmostEqual(w.input_settings.scope_multiplier, 1.0, places=5)
        self.assertEqual(w.input_settings.smooth_ms, 0)
        self.assertAlmostEqual(w.input_settings.sensitivity, 2.50, places=5)
        self.assertTrue(w.input_settings.invert_y)

        # Modify v2 settings via Qt controls and verify migration backup .v1.bak is created
        p = w.controls_page
        p.look_deg_per_count.setValue(0.065)
        p.aim_multiplier.setValue(0.85)
        p.scope_multiplier.setValue(0.75)
        p.smooth_ms.setCurrentIndex(p.smooth_ms.findData(5))

        backup = self.config.with_name('ultratears.cfg.v1.bak')
        self.assertTrue(backup.is_file())
        self.assertEqual(backup.read_bytes(), v1_bytes)

        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_MOUSE_CAMERA_VERSION'], '2')
        self.assertEqual(saved['TOTK_MOUSE_BACKEND'], 'native')
        self.assertEqual(saved['TOTK_MOUSE_LOOK_DEG_PER_COUNT'], '0.065')
        self.assertEqual(saved['TOTK_MOUSE_AIM_MULTIPLIER'], '0.85')
        self.assertEqual(saved['TOTK_MOUSE_SCOPE_MULTIPLIER'], '0.75')
        self.assertEqual(saved['TOTK_MOUSE_SMOOTH_MS'], '5')
        self.assertEqual(saved['TOTK_MOUSE_SENSITIVITY'], '2.50')
        self.assertEqual(saved['TOTK_KEY_A'], '18,0')

        # Switching to legacy_stick backend disables direct spinboxes and saves backend
        p.mouse_backend.setCurrentIndex(p.mouse_backend.findData('legacy_stick'))
        self.assertEqual(launcher.read_config()['TOTK_MOUSE_BACKEND'], 'legacy_stick')
        self.assertFalse(p.look_deg_per_count.isEnabled())


if __name__ == '__main__': unittest.main(verbosity=2)
