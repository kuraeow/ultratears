"""Test real Qt controls, config round trips and launch arguments without launching TOTK.

Run with the launcher's Python: python tools/test_prelauncher.py.
"""
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
from PyQt6.QtWidgets import QApplication


class PrelauncherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.work = ROOT / 'build' / ('launcher-test-' + uuid.uuid4().hex)
        self.work.mkdir(mode=0o777)
        self.config = self.work / 'ultratears.cfg'
        self.config.write_text('# Keep this comment\nromfs=data/romfs\ntextures=custom/textures\n'
                               'TOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=120\n'
                               'TOTK_OPERATION_MODE=docked\nTOTK_WIDTH=1920\nTOTK_HEIGHT=1080\n')
        self.exe = self.work / 'fixture.exe'
        self.exe.touch()
        self.patchers = [patch.object(launcher, 'CFG_PATH', self.config),
                         patch.object(launcher, 'LOG_PATH', self.work / 'test.log'),
                         patch.object(launcher, 'EXE_PATH', self.exe),
                         patch.object(launcher, 'is_controller_connected', return_value=False)]
        for item in self.patchers: item.start()
        self.windows = []
        self.window = self.open_window()

    def open_window(self):
        window = launcher.PrelauncherWindow()
        self.windows.append(window)
        return window

    def tearDown(self):
        for window in self.windows: window.close()
        for item in reversed(self.patchers): item.stop()
        for file in self.work.iterdir(): file.unlink()
        self.work.rmdir()

    def test_initial_labels_match_saved_limit_without_rewriting_config(self):
        before = self.config.read_bytes()
        self.assertEqual(self.window.fps_limit, 120)
        self.assertIn('120 FPS', self.window.status_lbl.text())
        self.assertIn('120 FPS', self.window.goal_exp_lbl.text())
        self.assertEqual(self.open_window().fps_limit_seg.active_key, '120')
        self.assertEqual(before, self.config.read_bytes())

    def test_each_cap_is_saved_and_restored_from_real_buttons(self):
        for fps in (60, 90, 120):
            self.window.fps_limit_seg.buttons[str(fps)].click()
            saved = launcher.read_config()
            self.assertEqual(saved['TOTK_UNCAPPED_HZ'], str(fps))
            self.assertEqual(saved['TOTK_DYNAMIC_FPS'], '1')
            self.assertEqual(saved['TOTK_FPS60'], '1')
            restored = self.open_window()
            self.assertEqual(restored.fps_limit, fps)
            self.assertEqual(restored.fps_goal, 'smooth')
            self.assertIn(f'{fps} FPS', restored.goal_exp_lbl.text())

    def test_quiet_and_balanced_round_trip_and_keep_smooth_preference(self):
        self.window.fps_limit_seg.buttons['90'].click()
        for goal, operation in [('quiet', 'handheld'), ('balanced', 'docked')]:
            self.window.fps_seg.buttons[goal].click()
            saved = launcher.read_config()
            self.assertEqual(saved['TOTK_UNCAPPED'], '0')
            self.assertEqual(saved['TOTK_FPS60'], '0')
            self.assertEqual(saved['TOTK_UNCAPPED_HZ'], '30')
            self.assertEqual(saved['TOTK_OPERATION_MODE'], operation)
            restored = self.open_window()
            self.assertEqual(restored.fps_goal, goal)
            self.assertTrue(restored.fps_limit_seg.isHidden())
            restored.fps_seg.buttons['smooth'].click()
            self.assertEqual(launcher.read_config()['TOTK_UNCAPPED_HZ'], '90')
            self.assertFalse(restored.fps_limit_seg.isHidden())

    def test_launch_uses_exact_saved_values_and_overrides_stale_environment(self):
        self.window.fps_limit_seg.buttons['90'].click()
        self.window.set_resolution('2560x1440')
        self.window.set_window_mode('windowed')
        stale = {'TOTK_DYNAMIC_FPS': '0', 'TOTK_REALTIME': '0', 'TOTK_FPS60': '0',
                 'TOTK_UNCAPPED_HZ': '240', 'DM_PRESENT_HZ': '0', 'DM_PRESENT_INTERVAL': 'display'}
        with patch.dict(os.environ, stale), patch.object(launcher.subprocess, 'Popen') as start:
            self.window.launch_game()
        start.assert_called_once()
        args, kwargs = start.call_args
        self.assertEqual(args[0], [str(self.exe)])
        saved = launcher.read_config()
        for key, value in self.window.runtime_settings().items():
            self.assertEqual(kwargs['env'][key], saved[key])
        self.assertEqual(kwargs['env']['TOTK_UNCAPPED_HZ'], '90')
        self.assertNotIn('DM_PRESENT_HZ', kwargs['env'])
        self.assertNotIn('DM_PRESENT_INTERVAL', kwargs['env'])
        self.assertEqual(kwargs['env']['TOTK_WIDTH'], '2560')
        self.assertEqual(kwargs['env']['TOTK_WINDOW_MODE'], 'windowed')

    def test_saving_preserves_paths_comments_and_removes_duplicate_managed_keys(self):
        with self.config.open('a') as file:
            file.write(' TOTK_UNCAPPED_HZ = 60\nTOTK_UNCAPPED_HZ=90\n')
        self.window.fps_limit_seg.buttons['60'].click()
        text = self.config.read_text()
        self.assertIn('# Keep this comment', text)
        self.assertIn('textures=custom/textures', text)
        self.assertEqual(text.count('TOTK_UNCAPPED_HZ='), 1)
        self.assertEqual(launcher.read_config()['TOTK_UNCAPPED_HZ'], '60')

    def test_save_error_preserves_old_file_and_blocks_launch(self):
        before = self.config.read_bytes()
        with patch.object(pathlib.Path, 'replace', side_effect=PermissionError('read only')), \
             patch.object(launcher.QMessageBox, 'warning') as warning, \
             patch.object(launcher.subprocess, 'Popen') as start:
            self.window.launch_game()
        warning.assert_called_once()
        start.assert_not_called()
        self.assertEqual(before, self.config.read_bytes())
        self.assertFalse(self.config.with_name('ultratears.cfg.tmp').exists())

    def test_all_quality_buttons_are_visible_in_grid_and_round_trip(self):
        from display_config import RESOLUTIONS
        for rid, _ in RESOLUTIONS:
            button = self.window.detailed_res_btns[rid]
            self.assertIsNotNone(button.parentWidget())
            button.click()
            saved = launcher.read_config()
            w,h = rid.split('x')
            self.assertEqual((saved['TOTK_INTERNAL_WIDTH'],saved['TOTK_INTERNAL_HEIGHT']),(w,h))
            self.assertEqual(self.open_window().selected_res_id,rid)

    def test_fit_switch_can_be_disabled_and_window_size_is_independent(self):
        self.window.set_resolution('6144x3456')
        self.window.set_window_mode('windowed')
        self.window.window_width_spin.setValue(1000)
        self.window.window_height_spin.setValue(777)
        self.window.downscale_switch.setChecked(False)
        saved=launcher.read_config()
        self.assertEqual(saved['TOTK_DOWNSCALE'],'0')
        self.assertEqual(saved['TOTK_FIT_SCREEN'],'0')
        self.assertEqual((saved['TOTK_WINDOW_WIDTH'],saved['TOTK_WINDOW_HEIGHT']),('1000','777'))
        restored=self.open_window()
        self.assertFalse(restored.downscale_switch.isChecked())
        self.assertEqual(restored.selected_res_id,'6144x3456')
        self.assertEqual(restored.window_width_spin.value(),1000)
        self.assertTrue(restored.window_width_spin.isEnabled())
        restored.set_window_mode('borderless')
        self.assertFalse(restored.window_width_spin.isEnabled())

    def test_launch_overrides_stale_scene_window_drs_and_fit_aliases(self):
        self.window.set_resolution('1280x720')
        self.window.downscale_switch.setChecked(False)
        stale={'TOTK_INTERNAL_WIDTH':'7680','TOTK_INTERNAL_HEIGHT':'4320','TOTK_DRS':'1',
               'TOTK_WINDOW_WIDTH':'7680','TOTK_FIT_SCREEN':'1','TOTK_NO_OVERRIDE_VP':'1'}
        with patch.dict(os.environ,stale),patch.object(launcher.subprocess,'Popen') as start:
            self.window.launch_game()
        launched=start.call_args.kwargs['env']
        self.assertEqual((launched['TOTK_INTERNAL_WIDTH'],launched['TOTK_INTERNAL_HEIGHT']),('1280','720'))
        self.assertEqual(launched['TOTK_DRS'],'0')
        self.assertEqual(launched['TOTK_FIT_SCREEN'],'0')
        self.assertEqual(launched['TOTK_WINDOW_WIDTH'],'1280')
        self.assertNotIn('TOTK_NO_OVERRIDE_VP',launched)

    def test_invalid_limit_falls_back_to_a_supported_choice(self):
        self.config.write_text('TOTK_UNCAPPED=1\nTOTK_UNCAPPED_HZ=display\n')
        restored = self.open_window()
        self.assertEqual(restored.fps_limit, 60)
        self.assertEqual(restored.fps_limit_seg.active_key, '60')

    def test_shadow_resolution_selection_and_config_round_trip(self):
        self.assertEqual(self.window.selected_shadow_res, '1024')
        self.assertIn('Shadow 1024x', self.window.status_lbl.text())
        self.window.set_shadow_resolution('2048')
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_SHADOW_RES'], '2048')
        self.assertIn('Shadow 2048x', self.window.status_lbl.text())
        restored = self.open_window()
        self.assertEqual(restored.selected_shadow_res, '2048')
        self.assertIn('Shadow 2048x', restored.status_lbl.text())

    def test_xess_sr_toggle_and_mode_round_trip(self):
        self.assertFalse(self.window.xess_sr_on)
        self.window.set_xess_sr_enabled(True)
        self.window.set_xess_sr_mode('performance')
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_XESS_SR'], '1')
        self.assertEqual(saved['TOTK_XESS_SR_MODE'], 'performance')
        self.assertNotIn('TOTK_DLSS', saved)
        self.assertIn('XeSS (Performance)', self.window.status_lbl.text())
        restored = self.open_window()
        self.assertTrue(restored.xess_sr_on)
        self.assertEqual(restored.selected_xess_sr_mode, 'performance')
        self.assertIn('XeSS (Performance)', restored.status_lbl.text())

    def test_xess_sr_render_resolution_downscaling(self):
        self.window.set_resolution('2560x1440')
        self.window.set_xess_sr_enabled(True)

        for mode, expected_w, expected_h in [
            ('ultra_quality_plus', '1920', '1080'),
            ('ultra_quality', '1920', '1080'),
            ('quality', '1920', '1080'),
            ('balanced', '1600', '900'),
            ('performance', '1280', '720'),
            ('ultra_performance', '1280', '720'),
            ('aa', '2560', '1440'),
        ]:
            self.window.set_xess_sr_mode(mode)
            saved = launcher.read_config()
            self.assertEqual(saved['TOTK_INTERNAL_WIDTH'], expected_w)
            self.assertEqual(saved['TOTK_INTERNAL_HEIGHT'], expected_h)
            self.assertEqual(saved['TOTK_TARGET_WIDTH'], '2560')
            self.assertEqual(saved['TOTK_TARGET_HEIGHT'], '1440')
            self.assertEqual(saved['TOTK_WIDTH'], '2560')
            self.assertEqual(saved['TOTK_HEIGHT'], '1440')
            self.assertEqual(launcher.scene_resolution(saved), '2560x1440')

        self.window.set_xess_sr_enabled(False)
        saved = launcher.read_config()
        self.assertEqual(saved['TOTK_INTERNAL_WIDTH'], '2560')
        self.assertEqual(saved['TOTK_INTERNAL_HEIGHT'], '1440')


if __name__ == '__main__':
    unittest.main(verbosity=2)
