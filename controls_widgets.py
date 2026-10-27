"""Native Qt editing of the keyboard and direct mouse camera profile consumed by the SDL host."""
import sys
from PyQt6.QtCore import Qt, QEvent, pyqtSignal
from PyQt6.QtWidgets import (QWidget, QPushButton, QLabel, QFrame, QVBoxLayout,
                             QHBoxLayout, QTableWidget, QTableWidgetItem, QHeaderView,
                             QAbstractItemView, QComboBox, QDoubleSpinBox, QCheckBox, QMessageBox)
from input_config import ACTIONS, InputSettings, RESERVED_KEYS, key_name


def event_scancode(event):
    """Store physical SDL scancodes, including when Windows is using Arabic input."""
    key, native, scan = event.key(), event.nativeVirtualKey(), event.nativeScanCode()
    if sys.platform == 'win32' and native:
        if native == 0x13: return 72  # Pause has an E1 sequence, unlike Num Lock.
        if scan & 0xff00 == 0xe000: scan = (scan & 0xff) | 0x100
        physical = {
            0x1e: 4, 0x30: 5, 0x2e: 6, 0x20: 7, 0x12: 8, 0x21: 9, 0x22: 10,
            0x23: 11, 0x17: 12, 0x24: 13, 0x25: 14, 0x26: 15, 0x32: 16,
            0x31: 17, 0x18: 18, 0x19: 19, 0x10: 20, 0x13: 21, 0x1f: 22,
            0x14: 23, 0x16: 24, 0x2f: 25, 0x11: 26, 0x2d: 27, 0x15: 28, 0x2c: 29,
            **{2 + n: 30 + n for n in range(9)}, 0x0b: 39,
            0x1c: 40, 1: 41, 0x0e: 42, 0x0f: 43, 0x39: 44,
            0x0c: 45, 0x0d: 46, 0x1a: 47, 0x1b: 48, 0x2b: 49,
            0x27: 51, 0x28: 52, 0x29: 53, 0x33: 54, 0x34: 55, 0x35: 56, 0x3a: 57,
            **{0x3b + n: 58 + n for n in range(10)}, 0x57: 68, 0x58: 69,
            0x137: 70, 0x46: 71, 0x152: 73, 0x147: 74, 0x149: 75, 0x153: 76,
            0x14f: 77, 0x151: 78, 0x14d: 79, 0x14b: 80, 0x150: 81, 0x148: 82,
            0x45: 83, 0x135: 84, 0x37: 85, 0x4a: 86, 0x4e: 87, 0x11c: 88,
            0x4f: 90 - 1, 0x50: 90, 0x51: 91, 0x4b: 92, 0x4c: 93, 0x4d: 94,
            0x47: 95, 0x48: 96, 0x49: 97, 0x52: 98, 0x53: 99,
            0x1d: 224, 0x2a: 225, 0x38: 226, 0x11d: 228, 0x36: 229, 0x138: 230,
        }
        if scan in physical: return physical[scan]
        if 0x41 <= native <= 0x5a: return native - 0x41 + 4
        if 0x31 <= native <= 0x39: return native - 0x31 + 30
        if 0x70 <= native <= 0x7b: return native - 0x70 + 58
        if 0x61 <= native <= 0x69: return native - 0x61 + 89
        if native == 0x10: return 229 if scan == 0x36 else 225
        if native == 0x11: return 228 if scan & 0x100 else 224
        if native == 0x12: return 230 if scan & 0x100 else 226
        if native == 0x0d: return 88 if scan & 0x100 else 40
        virtual = {0x30: 39, 0x08: 42, 0x09: 43, 0x1b: 41, 0x20: 44,
                   0x14: 57, 0x2c: 70, 0x91: 71, 0x13: 72, 0x2d: 73,
                   0x24: 74, 0x21: 75, 0x2e: 76, 0x23: 77, 0x22: 78,
                   0x27: 79, 0x25: 80, 0x28: 81, 0x26: 82, 0x90: 83,
                   0x6f: 84, 0x6a: 85, 0x6d: 86, 0x6b: 87, 0x60: 98, 0x6e: 99,
                   0xa0: 225, 0xa1: 229, 0xa2: 224, 0xa3: 228, 0xa4: 226, 0xa5: 230,
                   0xbd: 45, 0xbb: 46, 0xdb: 47, 0xdd: 48, 0xdc: 49,
                   0xba: 51, 0xde: 52, 0xc0: 53, 0xbc: 54, 0xbe: 55, 0xbf: 56}
        if native in virtual: return virtual[native]
    if Qt.Key.Key_A <= key <= Qt.Key.Key_Z: return key - Qt.Key.Key_A + 4
    if Qt.Key.Key_1 <= key <= Qt.Key.Key_9: return key - Qt.Key.Key_1 + 30
    if Qt.Key.Key_F1 <= key <= Qt.Key.Key_F12: return key - Qt.Key.Key_F1 + 58
    special = {Qt.Key.Key_0: 39, Qt.Key.Key_Return: 40, Qt.Key.Key_Enter: 88,
               Qt.Key.Key_Escape: 41, Qt.Key.Key_Backspace: 42, Qt.Key.Key_Tab: 43,
               Qt.Key.Key_Backtab: 43, Qt.Key.Key_Space: 44, Qt.Key.Key_Minus: 45,
               Qt.Key.Key_Equal: 46, Qt.Key.Key_BracketLeft: 47, Qt.Key.Key_BracketRight: 48,
               Qt.Key.Key_Backslash: 49, Qt.Key.Key_Semicolon: 51, Qt.Key.Key_Apostrophe: 52,
               Qt.Key.Key_QuoteLeft: 53, Qt.Key.Key_Comma: 54, Qt.Key.Key_Period: 55,
               Qt.Key.Key_Slash: 56, Qt.Key.Key_CapsLock: 57, Qt.Key.Key_Insert: 73,
               Qt.Key.Key_Home: 74, Qt.Key.Key_PageUp: 75, Qt.Key.Key_Delete: 76,
               Qt.Key.Key_End: 77, Qt.Key.Key_PageDown: 78, Qt.Key.Key_Right: 79,
               Qt.Key.Key_Left: 80, Qt.Key.Key_Down: 81, Qt.Key.Key_Up: 82,
               Qt.Key.Key_Control: 224, Qt.Key.Key_Shift: 225, Qt.Key.Key_Alt: 226,
               Qt.Key.Key_AltGr: 230}
    return special.get(key, 0)


class KeyBindingButton(QPushButton):
    chosen = pyqtSignal(int)
    cleared = pyqtSignal()
    rejected = pyqtSignal(str)

    def __init__(self, code, label, parent=None):
        super().__init__(parent)
        self.code = code
        self.waiting = False
        self.setAccessibleName(label)
        self.setToolTip('Click, then press a key or mouse button. Right-click when not capturing to clear. Click elsewhere to cancel.')
        self.clicked.connect(self.begin_capture)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumHeight(30)
        self.set_code(code)

    def _update_visual_style(self):
        if self.waiting:
            self.setStyleSheet("""
                QPushButton {
                    background: #fffbeb;
                    color: #b45309;
                    border: 2px solid #d97706;
                    border-radius: 6px;
                    font-weight: 700;
                    font-size: 12px;
                    padding: 4px 8px;
                }
            """)
        elif self.code == 0:
            self.setStyleSheet("""
                QPushButton {
                    background: #f8fafc;
                    color: #64748b;
                    border: 1px dashed #cbd5e1;
                    border-radius: 6px;
                    font-weight: 600;
                    font-size: 12px;
                    padding: 4px 8px;
                }
                QPushButton:hover {
                    background: #f1f5f9;
                    border: 1px solid #0f766e;
                    color: #0f766e;
                }
            """)
        else:
            self.setStyleSheet("""
                QPushButton {
                    background: #ffffff;
                    color: #0f172a;
                    border: 1px solid #cbd5e1;
                    border-radius: 6px;
                    font-weight: 700;
                    font-size: 12px;
                    padding: 4px 8px;
                }
                QPushButton:hover {
                    background: #f0fdfa;
                    border: 1px solid #0f766e;
                    color: #0f766e;
                }
                QPushButton:disabled {
                    background: #f1f5f9;
                    color: #94a3b8;
                    border: 1px solid #e2e8f0;
                }
            """)

    def set_code(self, code):
        self.code = code
        self.waiting = False
        self.setText(key_name(code))
        self._update_visual_style()

    def begin_capture(self):
        self.waiting = True
        self.setText('Key / mouse...')
        self._update_visual_style()
        self.setFocus()

    def event(self, event):
        # Intercept Tab before Qt turns it into focus navigation.
        if self.waiting and event.type() == QEvent.Type.KeyPress:
            if not event.isAutoRepeat():
                code = event_scancode(event)
                self.set_code(self.code)
                if code in RESERVED_KEYS:
                    self.rejected.emit('F8 is reserved for mouse capture; F11 is reserved for fullscreen.')
                elif not code:
                    self.rejected.emit('Choose a supported key or mouse button.')
                else:
                    self.chosen.emit(code)
            return True
        return super().event(event)

    def focusOutEvent(self, event):
        self.set_code(self.code)
        super().focusOutEvent(event)

    def mousePressEvent(self, event):
        if self.waiting:
            code = {Qt.MouseButton.LeftButton: 512, Qt.MouseButton.RightButton: 513,
                    Qt.MouseButton.MiddleButton: 514}.get(event.button())
            self.set_code(self.code)
            if code: self.chosen.emit(code)
            else: self.rejected.emit('Use the left, right or middle mouse button.')
            event.accept()
        elif event.button() == Qt.MouseButton.RightButton:
            self.cleared.emit()
            event.accept()
        else:
            super().mousePressEvent(event)


class ControlsPage(QWidget):
    def __init__(self, settings, save, gamepad_connected=False):
        super().__init__()
        self.settings, self.save = settings, save
        layout = QVBoxLayout(self)
        layout.setContentsMargins(28, 22, 28, 22)
        layout.setSpacing(12)
        title = QLabel('Controls & Input Configuration')
        title.setStyleSheet('font-size: 20px; font-weight: 800; color: #0f172a;')
        layout.addWidget(title)
        note = QLabel('Click a binding, then press a key or mouse button. Right-click an idle binding to clear. '
                      'Descriptions use the default in-game layout; button swaps and context still apply.')
        note.setWordWrap(True)
        note.setStyleSheet('color: #334155; font-size: 12px; font-weight: 500;')
        layout.addWidget(note)

        camera = QFrame()
        camera.setObjectName('CardFrame')
        camera_layout = QVBoxLayout(camera)
        camera_layout.setContentsMargins(16, 14, 16, 14)
        camera_layout.setSpacing(10)

        row = QHBoxLayout()
        row.setSpacing(10)
        lbl_cam = QLabel('Camera control')
        lbl_cam.setStyleSheet('color: #0f172a; font-weight: 700; font-size: 12px;')
        row.addWidget(lbl_cam)
        self.camera_mode = QComboBox()
        self.camera_mode.addItem('Keyboard directions', 'keys')
        self.camera_mode.addItem('Mouse movement (PC Direct)', 'mouse')
        self.camera_mode.setAccessibleName('Camera control mode')
        row.addWidget(self.camera_mode, 1)

        lbl_path = QLabel('Mouse path')
        lbl_path.setStyleSheet('color: #0f172a; font-weight: 700; font-size: 12px;')
        row.addWidget(lbl_path)
        self.mouse_backend = QComboBox()
        self.mouse_backend.addItem('Native direct camera', 'native')
        self.mouse_backend.addItem('Compatibility stick emulation', 'legacy_stick')
        self.mouse_backend.setAccessibleName('Mouse camera backend')
        row.addWidget(self.mouse_backend, 1)

        lbl_prompts = QLabel('Button Prompts')
        lbl_prompts.setStyleSheet('color: #0f172a; font-weight: 700; font-size: 12px;')
        row.addWidget(lbl_prompts)
        self.prompt_mode = QComboBox()
        self.prompt_mode.addItem('Auto (Switch on Input)', 'auto')
        self.prompt_mode.addItem('Keyboard & Mouse Only', 'kb_mouse')
        self.prompt_mode.addItem('Switch Controller Only', 'controller')
        self.prompt_mode.setAccessibleName('Button prompts display mode')
        row.addWidget(self.prompt_mode, 1)
        camera_layout.addLayout(row)

        self.mouse_options = QWidget()
        mouse_vbox = QVBoxLayout(self.mouse_options)
        mouse_vbox.setContentsMargins(0, 0, 0, 0)
        mouse_vbox.setSpacing(8)

        row_direct = QHBoxLayout()
        row_direct.setContentsMargins(0, 0, 0, 0)
        row_direct.setSpacing(10)
        lbl_look = QLabel('Look deg/count')
        lbl_look.setStyleSheet('color: #1e293b; font-weight: 600; font-size: 12px;')
        row_direct.addWidget(lbl_look)
        self.look_deg_per_count = QDoubleSpinBox()
        self.look_deg_per_count.setRange(0.001, 1.0)
        self.look_deg_per_count.setDecimals(4)
        self.look_deg_per_count.setSingleStep(0.005)
        self.look_deg_per_count.setAccessibleName('Mouse look degrees per count')
        row_direct.addWidget(self.look_deg_per_count)

        lbl_aim = QLabel('Aim mult')
        lbl_aim.setStyleSheet('color: #1e293b; font-weight: 600; font-size: 12px;')
        row_direct.addWidget(lbl_aim)
        self.aim_multiplier = QDoubleSpinBox()
        self.aim_multiplier.setRange(0.1, 2.0)
        self.aim_multiplier.setDecimals(2)
        self.aim_multiplier.setSingleStep(0.05)
        self.aim_multiplier.setAccessibleName('Bow and throw aim multiplier')
        row_direct.addWidget(self.aim_multiplier)

        lbl_scope = QLabel('Scope/Zoom mult')
        lbl_scope.setStyleSheet('color: #1e293b; font-weight: 600; font-size: 12px;')
        row_direct.addWidget(lbl_scope)
        self.scope_multiplier = QDoubleSpinBox()
        self.scope_multiplier.setRange(0.1, 2.0)
        self.scope_multiplier.setDecimals(2)
        self.scope_multiplier.setSingleStep(0.05)
        self.scope_multiplier.setAccessibleName('Scope and zoom multiplier')
        row_direct.addWidget(self.scope_multiplier)

        lbl_smooth = QLabel('Smoothing')
        lbl_smooth.setStyleSheet('color: #1e293b; font-weight: 600; font-size: 12px;')
        row_direct.addWidget(lbl_smooth)
        self.smooth_ms = QComboBox()
        self.smooth_ms.addItem('0 ms (Raw)', 0)
        self.smooth_ms.addItem('5 ms', 5)
        self.smooth_ms.addItem('10 ms', 10)
        self.smooth_ms.setAccessibleName('Mouse smoothing milliseconds')
        row_direct.addWidget(self.smooth_ms)
        row_direct.addStretch()
        mouse_vbox.addLayout(row_direct)

        row_compat = QHBoxLayout()
        row_compat.setContentsMargins(0, 0, 0, 0)
        row_compat.setSpacing(10)
        self.invert_y = QCheckBox('Invert mouse Y')
        self.invert_y.setStyleSheet('color: #0f172a; font-weight: 600; font-size: 12px;')
        row_compat.addWidget(self.invert_y)
        row_compat.addSpacing(12)
        self.compat_sens_label = QLabel('Wheel / compat stick speed')
        self.compat_sens_label.setStyleSheet('color: #1e293b; font-weight: 600; font-size: 12px;')
        row_compat.addWidget(self.compat_sens_label)
        self.sensitivity = QDoubleSpinBox()
        self.sensitivity.setRange(0.1, 5.0)
        self.sensitivity.setSingleStep(0.1)
        self.sensitivity.setDecimals(2)
        self.sensitivity.setAccessibleName('Mouse camera sensitivity')
        row_compat.addWidget(self.sensitivity)
        row_compat.addStretch()
        mouse_vbox.addLayout(row_compat)

        camera_layout.addWidget(self.mouse_options)
        self.camera_hint = QLabel()
        self.camera_hint.setWordWrap(True)
        self.camera_hint.setStyleSheet('color: #475569; font-size: 11px; font-weight: 500;')
        camera_layout.addWidget(self.camera_hint)
        layout.addWidget(camera)

        self.table = QTableWidget(len(ACTIONS), 4)
        self.table.setHorizontalHeaderLabels(['Switch input', 'In-game action', 'Primary', 'Alternative'])
        self.table.verticalHeader().hide()
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.table.setWordWrap(True)
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.ResizeToContents)
        self.table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        for column in (2, 3):
            self.table.horizontalHeader().setSectionResizeMode(column, QHeaderView.ResizeMode.Fixed)
            self.table.setColumnWidth(column, 124)
        self.buttons = {}
        for index, action in enumerate(ACTIONS):
            self.table.setRowHeight(index, 42)
            for col, value in enumerate((action['switch'], action['description'])):
                item = QTableWidgetItem(value)
                item.setToolTip(action['group'] + ': ' + action['description'])
                self.table.setItem(index, col, item)
            pair = []
            for slot in range(2):
                button = KeyBindingButton(settings.bindings[action['id']][slot],
                                          f"{action['switch']} {'primary' if not slot else 'alternative'} binding")
                button.chosen.connect(lambda code, a=action['id'], s=slot: self.assign(a, s, code))
                button.cleared.connect(lambda a=action['id'], s=slot: self.assign(a, s, 0))
                button.rejected.connect(lambda message: self.status.setText(message))
                self.table.setCellWidget(index, slot + 2, button)
                pair.append(button)
            self.buttons[action['id']] = pair
        layout.addWidget(self.table, 1)
        footer = QHBoxLayout()
        reset = QPushButton('Restore defaults')
        reset.setCursor(Qt.CursorShape.PointingHandCursor)
        reset.clicked.connect(self.restore_defaults)
        footer.addWidget(reset)
        self.status = QLabel('Changes are saved automatically. Restart the game to apply.')
        self.status.setWordWrap(True)
        self.status.setStyleSheet('color: #0f766e; font-weight: 600; font-size: 12px;')
        footer.addWidget(self.status, 1)
        layout.addLayout(footer)
        pad = 'Gamepad connected. ' if gamepad_connected else ''
        shortcuts = QLabel(pad + 'Keyboard mapping does not change gamepad buttons. F11 / Alt+Enter: fullscreen.')
        shortcuts.setWordWrap(True)
        shortcuts.setStyleSheet('color: #475569; font-size: 11px; font-weight: 500;')
        layout.addWidget(shortcuts)
        self.setStyleSheet('''
            QTableWidget {
                background: #ffffff;
                color: #0f172a;
                gridline-color: #e2e8f0;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                font-size: 12px;
                font-weight: 600;
            }
            QHeaderView::section {
                background: #f1f5f9;
                color: #0f172a;
                font-weight: 700;
                font-size: 12px;
                padding: 8px;
                border: none;
                border-bottom: 1px solid #cbd5e1;
            }
            QPushButton, QComboBox, QDoubleSpinBox {
                background: #ffffff;
                color: #0f172a;
                border: 1px solid #cbd5e1;
                border-radius: 6px;
                padding: 5px 10px;
                font-weight: 600;
                font-size: 12px;
            }
            QPushButton:hover, QComboBox:hover, QDoubleSpinBox:hover {
                border-color: #0f766e;
                background: #f8fafc;
            }
            QPushButton:focus, QComboBox:focus, QDoubleSpinBox:focus {
                border-color: #0f766e;
            }
            QPushButton:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
                background: #f1f5f9;
                color: #94a3b8;
                border-color: #e2e8f0;
            }
            QScrollBar:vertical {
                background: #f1f5f9;
                width: 10px;
                margin: 0;
            }
            QScrollBar::handle:vertical {
                background: #cbd5e1;
                min-height: 28px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical:hover {
                background: #94a3b8;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0;
            }
        ''')
        self.refresh()
        self.camera_mode.currentIndexChanged.connect(self.camera_changed)
        self.mouse_backend.currentIndexChanged.connect(self.camera_changed)
        self.prompt_mode.currentIndexChanged.connect(self.camera_changed)
        self.look_deg_per_count.valueChanged.connect(self.camera_changed)
        self.aim_multiplier.valueChanged.connect(self.camera_changed)
        self.scope_multiplier.valueChanged.connect(self.camera_changed)
        self.smooth_ms.currentIndexChanged.connect(self.camera_changed)
        self.sensitivity.valueChanged.connect(self.camera_changed)
        self.invert_y.toggled.connect(self.camera_changed)

    def refresh(self):
        for action in ACTIONS:
            for slot, button in enumerate(self.buttons[action['id']]):
                button.set_code(self.settings.bindings[action['id']][slot])
                button.setEnabled(not (action['group'] == 'Camera' and self.settings.camera_mode == 'mouse'))
        widgets = (self.camera_mode, self.mouse_backend, self.prompt_mode, self.look_deg_per_count,
                   self.aim_multiplier, self.scope_multiplier, self.smooth_ms,
                   self.sensitivity, self.invert_y)
        for widget in widgets:
            widget.blockSignals(True)
        self.camera_mode.setCurrentIndex(self.camera_mode.findData(self.settings.camera_mode))
        self.mouse_backend.setCurrentIndex(self.mouse_backend.findData(self.settings.mouse_backend))
        self.prompt_mode.setCurrentIndex(max(0, self.prompt_mode.findData(self.settings.prompt_mode)))
        self.look_deg_per_count.setValue(self.settings.look_deg_per_count)
        self.aim_multiplier.setValue(self.settings.aim_multiplier)
        self.scope_multiplier.setValue(self.settings.scope_multiplier)
        self.smooth_ms.setCurrentIndex(max(0, self.smooth_ms.findData(self.settings.smooth_ms)))
        self.sensitivity.setValue(self.settings.sensitivity)
        self.invert_y.setChecked(self.settings.invert_y)
        for widget in widgets:
            widget.blockSignals(False)

        is_mouse = self.settings.camera_mode == 'mouse'
        is_native = self.settings.mouse_backend == 'native'
        self.mouse_backend.setEnabled(is_mouse)
        self.mouse_options.setVisible(is_mouse)
        for w in (self.look_deg_per_count, self.aim_multiplier, self.scope_multiplier, self.smooth_ms):
            w.setEnabled(is_mouse and is_native)
        self.camera_hint.setMinimumHeight(self.camera_hint.fontMetrics().lineSpacing() * 2 + 8 if is_mouse else 0)
        if not is_mouse:
            self.camera_hint.setText('Camera uses the four right-stick bindings below. Mouse movement is ignored.')
        elif is_native:
            self.camera_hint.setText(
                'Native PC direct mouse camera: 1:1 angle rotation per count (FPS-independent), '
                'FOV-scaled bow/scope aiming, and automatic controller handoff. F8 releases/captures cursor.'
            )
        else:
            self.camera_hint.setText(
                'Compatibility mode: mouse emulates the Switch right stick with radial vector clamping. '
                'F8 releases/captures the cursor.'
            )

    def commit(self, previous):
        if not self.save():
            self.settings.__dict__.update(InputSettings(previous).__dict__)
            self.status.setText('Not saved. Previous settings restored.')
        else:
            self.status.setText('Saved. Restart the game to apply.')
        self.refresh()

    def assign(self, action_id, slot, code):
        previous = self.settings.runtime_values()
        conflicts = [(a['id'], s, a['switch']) for a in ACTIONS for s in range(2)
                     if code and self.settings.bindings[a['id']][s] == code and (a['id'], s) != (action_id, slot)]
        if conflicts:
            answer = QMessageBox.question(self, 'Input already assigned',
                f"{key_name(code)} is assigned to {conflicts[0][2]}. Move it to this binding?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No, QMessageBox.StandardButton.No)
            if answer != QMessageBox.StandardButton.Yes: return
            for other, other_slot, _ in conflicts: self.settings.bindings[other][other_slot] = 0
        self.settings.bindings[action_id][slot] = code
        self.commit(previous)

    def camera_changed(self, *_):
        previous = self.settings.runtime_values()
        self.settings.camera_mode = self.camera_mode.currentData()
        self.settings.mouse_backend = self.mouse_backend.currentData()
        self.settings.prompt_mode = self.prompt_mode.currentData()
        self.settings.look_deg_per_count = self.look_deg_per_count.value()
        self.settings.aim_multiplier = self.aim_multiplier.value()
        self.settings.scope_multiplier = self.scope_multiplier.value()
        self.settings.smooth_ms = int(self.smooth_ms.currentData())
        self.settings.sensitivity = self.sensitivity.value()
        self.settings.invert_y = self.invert_y.isChecked()
        self.commit(previous)

    def restore_defaults(self):
        if QMessageBox.question(self, 'Restore controls', 'Restore all keyboard and camera defaults?',
                               QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                               QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes: return
        previous = self.settings.runtime_values()
        self.settings.__dict__.update(InputSettings().__dict__)
        self.commit(previous)
