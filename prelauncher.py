import os
import sys
import re
import ctypes
import subprocess
from pathlib import Path
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QFrame, QStackedWidget, QListWidget, QListWidgetItem,
    QGridLayout, QSizePolicy, QDialog, QTextEdit, QMessageBox, QProgressBar, QScrollArea, QSpinBox
)
from PyQt6.QtCore import Qt, QSize, QPoint, pyqtSignal, QThread
from PyQt6.QtGui import QFont, QIcon, QColor, QPainter, QBrush, QPen, QPixmap, QPalette
import qtawesome as qta
from input_config import InputSettings
from controls_widgets import ControlsPage
from display_config import (
    RESOLUTIONS, scene_resolution, window_dimension, display_values,
    SHADOW_RESOLUTIONS, shadow_resolution,
    XESS_SR_MODES, xess_sr_enabled, xess_sr_mode, xess_sr_render_resolution,
    XESS_FG_MULTIPLIERS, xess_fg_enabled, xess_fg_multiplier
)


BASE_DIR = Path(__file__).resolve().parent
BUILDER_DIR = BASE_DIR / "UltraTears-builder-0.1.0-windows-x86_64"
if not (BASE_DIR / "UltraTears.exe").is_file() and BUILDER_DIR.is_dir():
    BASE_DIR = BUILDER_DIR

EXE_PATH = BASE_DIR / "UltraTears.exe"
CFG_PATH = BASE_DIR / "ultratears.cfg"
LOG_PATH = BASE_DIR / "ultratears.log"
ROMFS_DIR = BASE_DIR / "data" / "romfs"
EXEFS_DIR = BASE_DIR / "data" / "exefs"
SAVES_DIR = BASE_DIR / "userdata"

EMBLEM_PATH = BASE_DIR / "assets" / "zonai_emblem.jpg"
if not EMBLEM_PATH.is_file():
    EMBLEM_PATH = BASE_DIR.parent / "zonai_emblem.jpg"


class XINPUT_STATE(ctypes.Structure):
    _fields_ = [("dwPacketNumber", ctypes.c_ulong), ("Gamepad", ctypes.c_ubyte * 16)]


def is_controller_connected():
    try:
        xinput = getattr(ctypes.windll, "xinput1_4", None) or getattr(ctypes.windll, "xinput9_1_0", None)
        if not xinput:
            return False
        state = XINPUT_STATE()
        return xinput.XInputGetState(0, ctypes.byref(state)) == 0
    except Exception:
        return False


def is_vulkan_available():
    return Path("C:/Windows/System32/vulkan-1.dll").is_file()


def read_config():
    cfg = {}
    if CFG_PATH.is_file():
        for line in CFG_PATH.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    return cfg


def write_config_values(values):
    """Save a complete change, preserving unrelated paths and comments.
    When migrating a pre-v2 config file to v2, keep a local .v1.bak backup."""
    lines = []
    found = set()
    old_bytes = None
    existing_version = None
    obsolete_keys = {"TOTK_DLSS", "TOTK_DLSS_MODE", "TOTK_DLSS_PRESET"}
    if CFG_PATH.is_file():
        old_bytes = CFG_PATH.read_bytes()
        for line in old_bytes.decode("utf-8", errors="replace").splitlines():
            stripped = line.strip()
            key = stripped.split("=", 1)[0].strip() if "=" in stripped and not stripped.startswith("#") else None
            if key in obsolete_keys:
                continue
            if key == "TOTK_MOUSE_CAMERA_VERSION":
                existing_version = stripped.split("=", 1)[1].strip()
            if key in values:
                if key not in found:
                    lines.append(f"{key}={values[key]}")
                    found.add(key)
            else:
                lines.append(line)
    lines.extend(f"{key}={value}" for key, value in values.items() if key not in found)
    temporary = CFG_PATH.with_name(CFG_PATH.name + ".tmp")
    try:
        temporary.write_text("\n".join(lines) + "\n", encoding="utf-8")
        temporary.replace(CFG_PATH)
        if old_bytes is not None and existing_version != "2" and values.get("TOTK_MOUSE_CAMERA_VERSION") == "2":
            backup = CFG_PATH.with_name(CFG_PATH.name + ".v1.bak")
            if not backup.exists():
                backup.write_bytes(old_bytes)
    finally:
        temporary.unlink(missing_ok=True)


class ShaderPrepareWorker(QThread):
    progress_updated = pyqtSignal(int, int, float, int, int, int)
    log_line = pyqtSignal(str)
    finished = pyqtSignal(int)

    def __init__(self, env):
        super().__init__()
        self.env = env
        self.process = None
        self._is_cancelled = False

    def run(self):
        try:
            self.process = subprocess.Popen(
                [str(EXE_PATH), "--prepare-only"],
                cwd=str(BASE_DIR),
                env=self.env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                errors="replace",
                bufsize=1
            )
            prog_re = re.compile(r"\[shader-prepare\]\s+(\d+)/(\d+)\s+\(([\d.]+)%\)\s+-\s+cached:\s+(\d+),\s+compiled:\s+(\d+),\s+failed:\s+(\d+)")

            for line in self.process.stdout:
                if self._is_cancelled:
                    break
                line_str = line.strip()
                if not line_str:
                    continue
                self.log_line.emit(line_str)
                m = prog_re.search(line_str)
                if m:
                    done, total, pct, cached, compiled, failed = m.groups()
                    self.progress_updated.emit(int(done), int(total), float(pct), int(cached), int(compiled), int(failed))

            self.process.wait()
            self.finished.emit(self.process.returncode)
        except Exception as e:
            self.log_line.emit(f"Error: {e}")
            self.finished.emit(1)

    def cancel(self):
        self._is_cancelled = True
        if self.process:
            try:
                self.process.terminate()
                self.process.kill()
            except Exception:
                pass


class ShaderPrepareDialog(QDialog):
    def __init__(self, parent=None, env=None):
        super().__init__(parent)
        self.env = env or os.environ.copy()
        self.setWindowTitle("Preparing Shaders - UltraTears")
        self.setFixedSize(540, 260)
        self.setWindowFlags(self.windowFlags() & ~Qt.WindowType.WindowContextHelpButtonHint)
        self.setStyleSheet("""
            QDialog {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 12px;
            }
        """)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(28, 24, 28, 24)
        layout.setSpacing(14)

        header_layout = QHBoxLayout()
        icon_lbl = QLabel()
        icon_lbl.setPixmap(qta.icon("fa5s.bolt", color="#0f766e").pixmap(28, 28))
        header_layout.addWidget(icon_lbl)

        title_layout = QVBoxLayout()
        title_layout.setSpacing(2)
        title_lbl = QLabel("Preparing Shaders")
        title_lbl.setStyleSheet("font-size: 18px; font-weight: 800; color: #0f172a;")
        sub_lbl = QLabel("Verifying Vulkan shaders and preparing known shader variants.")
        sub_lbl.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        title_layout.addWidget(title_lbl)
        title_layout.addWidget(sub_lbl)
        header_layout.addLayout(title_layout)
        header_layout.addStretch()
        layout.addLayout(header_layout)

        self.prog_bar = QProgressBar()
        self.prog_bar.setRange(0, 1000)
        self.prog_bar.setValue(0)
        self.prog_bar.setTextVisible(False)
        self.prog_bar.setFixedHeight(14)
        self.prog_bar.setStyleSheet("""
            QProgressBar {
                background-color: #e2e8f0;
                border: 1px solid #cbd5e1;
                border-radius: 7px;
            }
            QProgressBar::chunk {
                background-color: #0f766e;
                border-radius: 6px;
            }
        """)
        layout.addWidget(self.prog_bar)

        self.status_lbl = QLabel("Initializing shader compiler...")
        self.status_lbl.setStyleSheet("color: #0f172a; font-size: 13px; font-weight: 700;")
        layout.addWidget(self.status_lbl)

        self.details_lbl = QLabel("Scanning shaders directory...")
        self.details_lbl.setStyleSheet("color: #475569; font-size: 12px; font-family: 'Consolas', monospace; font-weight: 600;")
        layout.addWidget(self.details_lbl)

        layout.addStretch()

        btn_layout = QHBoxLayout()
        btn_layout.addStretch()
        self.cancel_btn = QPushButton("Cancel")
        self.cancel_btn.setFixedSize(110, 36)
        self.cancel_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.cancel_btn.setStyleSheet("""
            QPushButton {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 6px;
                color: #0f172a;
                font-weight: 700;
                font-size: 12px;
            }
            QPushButton:hover {
                background-color: #fef2f2;
                border-color: #dc2626;
                color: #dc2626;
            }
        """)
        self.cancel_btn.clicked.connect(self.on_cancel)
        btn_layout.addWidget(self.cancel_btn)
        layout.addLayout(btn_layout)

        self.worker = ShaderPrepareWorker(self.env)
        self.worker.progress_updated.connect(self.on_progress)
        self.worker.finished.connect(self.on_finished)
        self.worker.start()

    def on_progress(self, done, total, pct, cached, compiled, failed):
        self.prog_bar.setValue(int(pct * 10))
        self.status_lbl.setText(f"{done:,} / {total:,} Shaders Ready ({pct:.1f}%)")
        self.details_lbl.setText(f"Cached: {cached:,}  |  Compiled: {compiled:,}  |  Failed: {failed:,}")

    def on_finished(self, code):
        if code == 0:
            self.accept()
        elif code == 2:
            self.status_lbl.setText("Finished with shader failures")
            self.details_lbl.setText("Some shaders failed verification. Check the preparation log.")
            self.cancel_btn.setText("Close")
        else:
            self.status_lbl.setText("Shader preparation failed")
            self.details_lbl.setText("Preparation did not complete. Check the log and retry.")
            self.cancel_btn.setText("Close")

    def on_cancel(self):
        self.worker.cancel()
        self.reject()

    def closeEvent(self, event):
        self.worker.cancel()
        super().closeEvent(event)


class ToggleSwitch(QWidget):
    toggled = pyqtSignal(bool)

    def __init__(self, checked=False, parent=None, active_color="#0f766e", inactive_color="#cbd5e1"):
        super().__init__(parent)
        self._checked = checked
        self._active_color = active_color
        self._inactive_color = inactive_color
        self.setFixedSize(46, 24)
        self.setCursor(Qt.CursorShape.PointingHandCursor)

    def isChecked(self):
        return self._checked

    def setChecked(self, val):
        if self._checked != val:
            self._checked = val
            self.update()
            self.toggled.emit(self._checked)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setChecked(not self._checked)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        track_color = QColor(self._active_color) if self._checked else QColor(self._inactive_color)
        painter.setBrush(QBrush(track_color))
        painter.setPen(QPen(QColor("#0d9488" if self._checked else "#94a3b8"), 1.0))
        painter.drawRoundedRect(1, 2, 44, 20, 10, 10)

        thumb_x = 25 if self._checked else 3
        painter.setBrush(QBrush(QColor("#ffffff")))
        painter.setPen(QPen(QColor("#94a3b8" if not self._checked else "#0f766e"), 1.0))
        painter.drawEllipse(thumb_x, 4, 16, 16)


class SegmentedButtons(QFrame):
    selectionChanged = pyqtSignal(str)

    def __init__(self, options, active_key=None, parent=None):
        super().__init__(parent)
        self.options = options
        self.active_key = active_key or (options[0][0] if options else "")
        self.buttons = {}

        self.setObjectName("SegmentedFrame")
        self.setStyleSheet("""
            #SegmentedFrame {
                background-color: #f1f5f9;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                padding: 3px;
            }
        """)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(3, 3, 3, 3)
        layout.setSpacing(4)

        for key, label in self.options:
            btn = QPushButton(label)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
            btn.setFixedHeight(32)
            btn.clicked.connect(lambda checked, k=key: self.set_active(k))
            self.buttons[key] = btn
            layout.addWidget(btn)

        self.update_styles()

    def set_active(self, key):
        if key in self.buttons and self.active_key != key:
            self.active_key = key
            self.update_styles()
            self.selectionChanged.emit(key)

    def update_styles(self):
        for k, btn in self.buttons.items():
            if k == self.active_key:
                btn.setStyleSheet("""
                    QPushButton {
                        background-color: #0f766e;
                        color: #ffffff;
                        font-weight: 800;
                        font-size: 12px;
                        border: 1px solid #0d9488;
                        border-radius: 6px;
                        padding: 0 12px;
                    }
                """)
            else:
                btn.setStyleSheet("""
                    QPushButton {
                        background-color: transparent;
                        color: #1e293b;
                        font-weight: 700;
                        font-size: 12px;
                        border: none;
                        border-radius: 6px;
                        padding: 0 12px;
                    }
                    QPushButton:hover {
                        background-color: #e2e8f0;
                        color: #0f172a;
                    }
                """)


class ReadyCheckItem(QFrame):
    clicked = pyqtSignal()

    def __init__(self, icon_name, text, icon_color="#0f766e", show_arrow=False, has_action=False):
        super().__init__()
        self.has_action = has_action
        if has_action:
            self.setCursor(Qt.CursorShape.PointingHandCursor)

        self.setStyleSheet("""
            ReadyCheckItem {
                background-color: #f8fafc;
                border: 1px solid #e2e8f0;
                border-radius: 6px;
                padding: 4px 8px;
            }
            ReadyCheckItem:hover {
                background-color: #f1f5f9;
                border-color: #cbd5e1;
            }
        """)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(10)

        icon_lbl = QLabel()
        icon_lbl.setPixmap(qta.icon(icon_name, color=icon_color).pixmap(15, 15))
        icon_lbl.setStyleSheet("border: none; background: transparent;")
        layout.addWidget(icon_lbl)

        self.text_lbl = QLabel(text)
        self.text_lbl.setStyleSheet("color: #0f172a; font-size: 12px; font-weight: 600; border: none; background: transparent;")
        layout.addWidget(self.text_lbl, 1)

        if show_arrow:
            arrow_lbl = QLabel()
            arrow_lbl.setPixmap(qta.icon("fa5s.chevron-right", color="#475569").pixmap(11, 11))
            arrow_lbl.setStyleSheet("border: none; background: transparent;")
            layout.addWidget(arrow_lbl)

    def set_text(self, text):
        self.text_lbl.setText(text)

    def mousePressEvent(self, event):
        if self.has_action and event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()


class ModeCard(QFrame):
    def __init__(self, mode_id, title, desc, icon_name, is_selected=False, on_select=None):
        super().__init__()
        self.mode_id = mode_id
        self.on_select = on_select
        self.is_selected = is_selected
        self.icon_name = icon_name

        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setObjectName("ModeCard")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(14)

        self.icon_lbl = QLabel()
        self.icon_lbl.setStyleSheet("border: none; background: transparent;")
        layout.addWidget(self.icon_lbl)

        text_vbox = QVBoxLayout()
        text_vbox.setSpacing(2)
        self.title_lbl = QLabel(title)
        self.title_lbl.setObjectName("ModeTitle")
        text_vbox.addWidget(self.title_lbl)

        self.desc_lbl = QLabel(desc)
        self.desc_lbl.setObjectName("ModeDesc")
        text_vbox.addWidget(self.desc_lbl)
        layout.addLayout(text_vbox, 1)

        self.radio_lbl = QLabel()
        self.radio_lbl.setStyleSheet("border: none; background: transparent;")
        layout.addWidget(self.radio_lbl)

        self.update_style()

    def set_selected(self, selected):
        self.is_selected = selected
        self.update_style()

    def update_style(self):
        color = "#0f766e" if self.is_selected else "#64748b"
        self.radio_lbl.setPixmap(qta.icon("fa5s.dot-circle" if self.is_selected else "fa5s.circle", color=color).pixmap(16, 16))
        self.icon_lbl.setPixmap(qta.icon(self.icon_name, color="#0f766e" if self.is_selected else "#334155").pixmap(22, 22))

        if self.is_selected:
            self.setStyleSheet("""
                #ModeCard {
                    background-color: #f0fdfa;
                    border: 2px solid #0f766e;
                    border-radius: 8px;
                }
                #ModeTitle { font-size: 13px; font-weight: 800; color: #0f766e; border: none; background: transparent; }
                #ModeDesc { font-size: 12px; font-weight: 500; color: #134e4a; border: none; background: transparent; }
            """)
        else:
            self.setStyleSheet("""
                #ModeCard {
                    background-color: #ffffff;
                    border: 1px solid #cbd5e1;
                    border-radius: 8px;
                }
                #ModeCard:hover {
                    background-color: #f8fafc;
                    border-color: #94a3b8;
                }
                #ModeTitle { font-size: 13px; font-weight: 700; color: #0f172a; border: none; background: transparent; }
                #ModeDesc { font-size: 12px; font-weight: 500; color: #334155; border: none; background: transparent; }
            """)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self.on_select:
            self.on_select(self.mode_id)


class ResChip(QPushButton):
    def __init__(self, res_id, label, is_selected=False, on_select=None):
        super().__init__(label)
        self.res_id = res_id
        self.on_select = on_select
        self.is_selected = is_selected
        self.setFixedHeight(30)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.clicked.connect(lambda: self.on_select(self.res_id) if self.on_select else None)
        self.update_style()

    def set_selected(self, val):
        self.is_selected = val
        self.update_style()

    def update_style(self):
        if self.is_selected:
            self.setStyleSheet("""
                QPushButton {
                    background-color: #0f766e;
                    color: #ffffff;
                    font-weight: 800;
                    font-size: 11px;
                    border: 1px solid #0d9488;
                    border-radius: 6px;
                    padding: 0 10px;
                }
            """)
        else:
            self.setStyleSheet("""
                QPushButton {
                    background-color: #ffffff;
                    color: #0f172a;
                    font-weight: 700;
                    font-size: 11px;
                    border: 1px solid #cbd5e1;
                    border-radius: 6px;
                    padding: 0 10px;
                }
                QPushButton:hover {
                    background-color: #f1f5f9;
                    color: #0f766e;
                    border-color: #0f766e;
                }
            """)


class LogDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Engine Log - UltraTears")
        self.resize(760, 490)
        self.setStyleSheet("background-color: #f8fafc; color: #0f172a;")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(20, 20, 20, 20)
        layout.setSpacing(12)

        header = QLabel("Runtime & Console Output")
        header.setStyleSheet("font-size: 16px; font-weight: 800; color: #0f172a;")
        layout.addWidget(header)

        self.text_edit = QTextEdit()
        self.text_edit.setReadOnly(True)
        self.text_edit.setStyleSheet("""
            QTextEdit {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                color: #0f172a;
                font-family: 'Consolas', 'Courier New', monospace;
                font-size: 12px;
                padding: 12px;
            }
        """)
        layout.addWidget(self.text_edit)

        close_btn = QPushButton("Close")
        close_btn.setFixedHeight(34)
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.setStyleSheet("""
            QPushButton {
                background-color: #0f766e;
                border: 1px solid #0d9488;
                border-radius: 6px;
                color: #ffffff;
                font-weight: 700;
                padding: 0 22px;
            }
            QPushButton:hover { background-color: #0d9488; }
        """)
        close_btn.clicked.connect(self.close)
        layout.addWidget(close_btn, 0, Qt.AlignmentFlag.AlignRight)

        self.load_log()

    def load_log(self):
        if LOG_PATH.is_file():
            text = LOG_PATH.read_text(encoding="utf-8", errors="ignore")
            self.text_edit.setPlainText(text if text.strip() else "[log] Engine log file is empty. Launch the game to capture runtime logs.")
        else:
            self.text_edit.setPlainText("[log] No previous game session log found. Launch the game to generate runtime output.")


class PrelauncherWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Tears of the Kingdom - PC Port")
        self.resize(1100, 720)
        self.setMinimumSize(1000, 650)

        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)

        self._drag_pos = QPoint()

        # Load config
        self.cfg = read_config()
        self.input_settings = InputSettings(self.cfg)
        self.selected_window_mode = self.cfg.get("TOTK_WINDOW_MODE", "borderless")
        self.selected_res_id = scene_resolution(self.cfg)
        self.window_width = window_dimension(self.cfg.get("TOTK_WINDOW_WIDTH"), 1280, 640, 7680)
        self.window_height = window_dimension(self.cfg.get("TOTK_WINDOW_HEIGHT"), 720, 360, 4320)
        self.fit_window = self.cfg.get("TOTK_FIT_SCREEN", self.cfg.get("TOTK_DOWNSCALE", "1")) != "0"

        self.fps_goal = ("smooth" if self.cfg.get("TOTK_UNCAPPED") == "1" else
                         "quiet" if self.cfg.get("TOTK_OPERATION_MODE") == "handheld" else "balanced")
        saved_limit = self.cfg.get("launcher_fps_limit", self.cfg.get("TOTK_UNCAPPED_HZ", "60"))
        self.fps_limit = int(saved_limit) if saved_limit in ("60", "90", "120") else 60

        self.selected_shadow_res = shadow_resolution(self.cfg)
        self.xess_sr_on = xess_sr_enabled(self.cfg)
        self.selected_xess_sr_mode = xess_sr_mode(self.cfg)
        self.xess_fg_on = xess_fg_enabled(self.cfg)
        self.selected_xess_fg_mult = xess_fg_multiplier(self.cfg)
        self.res_chips = []
        self.shadow_chips = []
        self.detailed_shadow_btns = {}
        self.detailed_xess_sr_btns = {}
        self.detailed_xess_fg_btns = {}
        self.mode_cards = []

        self.apply_theme()
        self.init_ui()
        self.update_fps_labels()

    def apply_theme(self):
        self.setStyleSheet("""
            QMainWindow {
                background-color: #f1f5f9;
            }
            QWidget {
                color: #0f172a;
                font-family: 'Segoe UI', -apple-system, BlinkMacSystemFont, sans-serif;
            }
            #RootFrame {
                background-color: #f1f5f9;
                border: 1px solid #cbd5e1;
            }
            #TitleBar {
                background-color: #ffffff;
                border-bottom: 1px solid #cbd5e1;
            }
            #SidebarFrame {
                background-color: #ffffff;
                border-right: 1px solid #cbd5e1;
            }
            #NavList {
                background-color: transparent;
                border: none;
                outline: none;
            }
            #NavList::item {
                height: 42px;
                padding-left: 14px;
                margin: 3px 6px;
                border-radius: 8px;
                color: #334155;
                font-size: 13px;
                font-weight: 700;
            }
            #NavList::item:hover {
                background-color: #f1f5f9;
                color: #0f172a;
            }
            #NavList::item:selected {
                background-color: #f0fdfa;
                color: #0f766e;
                font-weight: 800;
                border: 1px solid #99f6e4;
                border-left: 4px solid #0f766e;
            }
            #CardFrame {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 10px;
                padding: 16px;
            }
            #FooterBar {
                background-color: #ffffff;
                border-top: 1px solid #cbd5e1;
            }
            QScrollBar:vertical {
                background: #f1f5f9;
                width: 10px;
                margin: 0;
            }
            QScrollBar::handle:vertical {
                background: #cbd5e1;
                min-height: 30px;
                border-radius: 5px;
            }
            QScrollBar::handle:vertical:hover {
                background: #94a3b8;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0;
            }
        """)

    def init_ui(self):
        root_widget = QWidget()
        root_widget.setObjectName("RootFrame")
        self.setCentralWidget(root_widget)

        root_vbox = QVBoxLayout(root_widget)
        root_vbox.setContentsMargins(0, 0, 0, 0)
        root_vbox.setSpacing(0)

        # 1. Custom Titlebar
        root_vbox.addWidget(self.create_titlebar())

        # 2. Body: Sidebar + Main Area
        body_hbox = QHBoxLayout()
        body_hbox.setContentsMargins(0, 0, 0, 0)
        body_hbox.setSpacing(0)

        body_hbox.addWidget(self.create_sidebar())

        self.pages_stack = QStackedWidget()
        self.pages_stack.addWidget(self.create_play_page())
        self.pages_stack.addWidget(self.create_graphics_page())
        self.pages_stack.addWidget(self.create_controls_page())
        self.pages_stack.addWidget(self.create_game_files_page())
        self.pages_stack.addWidget(self.create_mods_page())
        self.pages_stack.addWidget(self.create_saves_page())

        body_hbox.addWidget(self.pages_stack, 1)
        root_vbox.addLayout(body_hbox, 1)

        # 3. Bottom Action Bar
        root_vbox.addWidget(self.create_footer())

    def create_titlebar(self):
        bar = QFrame()
        bar.setObjectName("TitleBar")
        bar.setFixedHeight(38)

        layout = QHBoxLayout(bar)
        layout.setContentsMargins(16, 0, 10, 0)
        layout.setSpacing(10)

        icon_lbl = QLabel()
        icon_lbl.setPixmap(qta.icon("fa5s.dragon", color="#0f766e").pixmap(15, 15))
        layout.addWidget(icon_lbl)

        title_lbl = QLabel("UltraTears  |  Tears of the Kingdom PC Port")
        title_lbl.setStyleSheet("color: #0f172a; font-size: 12px; font-weight: 800;")
        layout.addWidget(title_lbl)

        layout.addStretch(1)

        min_btn = QPushButton()
        min_btn.setFixedSize(30, 26)
        min_btn.setIcon(qta.icon("fa5s.minus", color="#334155"))
        min_btn.setIconSize(QSize(11, 11))
        min_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        min_btn.setStyleSheet("""
            QPushButton { background: transparent; border: none; border-radius: 5px; }
            QPushButton:hover { background-color: #e2e8f0; }
        """)
        min_btn.clicked.connect(self.showMinimized)
        layout.addWidget(min_btn)

        close_btn = QPushButton()
        close_btn.setFixedSize(30, 26)
        close_btn.setIcon(qta.icon("fa5s.times", color="#334155"))
        close_btn.setIconSize(QSize(13, 13))
        close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        close_btn.setStyleSheet("""
            QPushButton { background: transparent; border: none; border-radius: 5px; }
            QPushButton:hover { background-color: #fee2e2; }
        """)
        close_btn.clicked.connect(self.close)
        layout.addWidget(close_btn)

        return bar

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and event.position().y() <= 40:
            self._drag_pos = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()

    def mouseMoveEvent(self, event):
        if event.buttons() == Qt.MouseButton.LeftButton and not self._drag_pos.isNull():
            self.move(event.globalPosition().toPoint() - self._drag_pos)
            event.accept()

    def mouseReleaseEvent(self, event):
        self._drag_pos = QPoint()

    def create_sidebar(self):
        sidebar = QFrame()
        sidebar.setObjectName("SidebarFrame")
        sidebar.setFixedWidth(235)

        vbox = QVBoxLayout(sidebar)
        vbox.setContentsMargins(14, 20, 14, 16)
        vbox.setSpacing(16)

        # Brand header
        brand_hbox = QHBoxLayout()
        brand_hbox.setSpacing(10)

        brand_icon = QLabel()
        brand_icon.setPixmap(qta.icon("fa5s.dragon", color="#0f766e").pixmap(26, 26))
        brand_hbox.addWidget(brand_icon)

        brand_text_vbox = QVBoxLayout()
        brand_text_vbox.setSpacing(1)

        brand_name = QLabel("TEARS OF THE KINGDOM")
        brand_name.setStyleSheet("font-size: 12px; font-weight: 800; color: #0f172a; letter-spacing: 0.4px;")
        brand_text_vbox.addWidget(brand_name)

        brand_sub = QLabel("ULTRATEARS LAUNCHER")
        brand_sub.setStyleSheet("font-size: 10px; font-weight: 800; color: #0f766e; letter-spacing: 0.8px;")
        brand_text_vbox.addWidget(brand_sub)

        brand_hbox.addLayout(brand_text_vbox, 1)
        vbox.addLayout(brand_hbox)

        divider = QFrame()
        divider.setFixedHeight(1)
        divider.setStyleSheet("background-color: #e2e8f0; border: none;")
        vbox.addWidget(divider)

        # Navigation Menu
        self.nav_list = QListWidget()
        self.nav_list.setObjectName("NavList")
        self.nav_list.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.nav_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        menu_items = [
            ("Overview & Play", "fa5s.play", 0),
            ("Picture & Performance", "fa5s.tv", 1),
            ("Controls & Input", "fa5s.gamepad", 2),
            ("Game Files & Shaders", "fa5s.folder-open", 3),
            ("Mods", "fa5s.puzzle-piece", 4),
            ("Save Data", "fa5s.save", 5),
        ]

        for text, icon, _ in menu_items:
            item = QListWidgetItem(qta.icon(icon, color="#0f766e"), text)
            item.setSizeHint(QSize(0, 42))
            self.nav_list.addItem(item)

        self.nav_list.setCurrentRow(0)
        self.nav_list.currentRowChanged.connect(self.on_nav_changed)
        vbox.addWidget(self.nav_list, 1)

        # Quick Shader Precompile button in sidebar bottom
        shader_btn = QPushButton("Prepare Shaders")
        shader_btn.setIcon(qta.icon("fa5s.bolt", color="#0f766e"))
        shader_btn.setFixedHeight(38)
        shader_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        shader_btn.setStyleSheet("""
            QPushButton {
                background-color: #f8fafc;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                color: #0f172a;
                font-weight: 700;
                font-size: 12px;
            }
            QPushButton:hover {
                background-color: #f0fdfa;
                border-color: #0f766e;
                color: #0f766e;
            }
        """)
        shader_btn.clicked.connect(self.open_shader_precompile)
        vbox.addWidget(shader_btn)

        return sidebar

    def on_nav_changed(self, row):
        page_map = {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5}
        self.pages_stack.setCurrentIndex(page_map.get(row, 0))

    def switch_page(self, idx):
        self.nav_list.setCurrentRow(idx)

    def create_play_page(self):
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(28, 22, 28, 20)
        vbox.setSpacing(18)

        # Top Hero Banner Card
        hero_card = QFrame()
        hero_card.setStyleSheet("""
            QFrame {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 10px;
            }
        """)
        hero_hbox = QHBoxLayout(hero_card)
        hero_hbox.setContentsMargins(22, 18, 22, 18)
        hero_hbox.setSpacing(20)

        hero_left = QVBoxLayout()
        hero_left.setSpacing(6)

        game_title = QLabel("THE LEGEND OF ZELDA: TEARS OF THE KINGDOM")
        game_title.setStyleSheet("""
            font-size: 22px;
            font-weight: 800;
            color: #0f172a;
            letter-spacing: 0.4px;
            border: none;
        """)
        hero_left.addWidget(game_title)

        sub_desc = QLabel("Native Windows x86_64 Recompilation  |  Vulkan 1.3 Renderer  |  Intel XeSS Upscaler & Frame Generation")
        sub_desc.setStyleSheet("color: #334155; font-size: 12px; font-weight: 600; border: none;")
        hero_left.addWidget(sub_desc)

        chips_hbox = QHBoxLayout()
        chips_hbox.setSpacing(8)

        chip1 = QLabel("Title ID: 0100F2C0115B6000 (v1.4.3)")
        chip1.setStyleSheet("""
            background-color: #f1f5f9;
            border: 1px solid #cbd5e1;
            border-radius: 6px;
            padding: 4px 10px;
            font-size: 11px;
            font-weight: 700;
            color: #0f172a;
        """)
        chips_hbox.addWidget(chip1)

        chip2 = QLabel("Vulkan 1.3 Native")
        chip2.setStyleSheet("""
            background-color: #f0fdfa;
            border: 1px solid #0f766e;
            border-radius: 6px;
            padding: 4px 10px;
            font-size: 11px;
            font-weight: 800;
            color: #0f766e;
        """)
        chips_hbox.addWidget(chip2)

        chip3 = QLabel("Intel XeSS 2.1 Ready")
        chip3.setStyleSheet("""
            background-color: #eff6ff;
            border: 1px solid #1d4ed8;
            border-radius: 6px;
            padding: 4px 10px;
            font-size: 11px;
            font-weight: 800;
            color: #1d4ed8;
        """)
        chips_hbox.addWidget(chip3)
        chips_hbox.addStretch(1)

        hero_left.addLayout(chips_hbox)
        hero_hbox.addLayout(hero_left, 1)

        emblem_lbl = QLabel()
        emblem_lbl.setStyleSheet("border: none; background: transparent;")
        if EMBLEM_PATH.is_file():
            pm = QPixmap(str(EMBLEM_PATH)).scaled(88, 88, Qt.AspectRatioMode.KeepAspectRatio, Qt.TransformationMode.SmoothTransformation)
            emblem_lbl.setPixmap(pm)
        else:
            emblem_lbl.setPixmap(qta.icon("fa5s.dragon", color="#0f766e").pixmap(68, 68))

        hero_hbox.addWidget(emblem_lbl, 0, Qt.AlignmentFlag.AlignCenter)
        vbox.addWidget(hero_card)

        # Middle Cards Area
        cards_hbox = QHBoxLayout()
        cards_hbox.setSpacing(18)

        # Left Card: SYSTEM & GAME STATUS
        ready_card = QFrame()
        ready_card.setObjectName("CardFrame")
        rc_vbox = QVBoxLayout(ready_card)
        rc_vbox.setContentsMargins(18, 16, 18, 16)
        rc_vbox.setSpacing(8)

        rc_title = QLabel("System & Game Verification")
        rc_title.setStyleSheet("color: #0f172a; font-size: 15px; font-weight: 800;")
        rc_vbox.addWidget(rc_title)

        exe_ok = EXE_PATH.is_file()
        rc1 = ReadyCheckItem(
            "fa5s.check-circle" if exe_ok else "fa5s.exclamation-circle",
            "Game Executable: UltraTears.exe (Ready)" if exe_ok else "Game Executable: UltraTears.exe not found",
            icon_color="#059669" if exe_ok else "#dc2626"
        )
        rc_vbox.addWidget(rc1)

        romfs_ok = ROMFS_DIR.is_dir()
        rc2 = ReadyCheckItem(
            "fa5s.check" if romfs_ok else "fa5s.exclamation-triangle",
            "Game Data: data/romfs directory verified" if romfs_ok else "Game Data: data/romfs directory missing",
            icon_color="#059669" if romfs_ok else "#d97706"
        )
        rc_vbox.addWidget(rc2)

        exefs_ok = EXEFS_DIR.is_dir()
        rc3 = ReadyCheckItem(
            "fa5s.check" if exefs_ok else "fa5s.exclamation-triangle",
            "Game Binaries: data/exefs (v1.4.3) verified" if exefs_ok else "Game Binaries: data/exefs missing",
            icon_color="#059669" if exefs_ok else "#d97706"
        )
        rc_vbox.addWidget(rc3)

        mode_text = "Borderless Fullscreen" if self.selected_window_mode == "borderless" else ("Exclusive Fullscreen" if self.selected_window_mode == "fullscreen" else "Windowed")
        self.rc_display_item = ReadyCheckItem("fa5s.tv", f"Display: {mode_text} at {self.selected_res_id}", icon_color="#0f766e", show_arrow=True, has_action=True)
        self.rc_display_item.clicked.connect(lambda: self.switch_page(1))
        rc_vbox.addWidget(self.rc_display_item)

        vk_ok = is_vulkan_available()
        rc5 = ReadyCheckItem(
            "fa5s.microchip" if vk_ok else "fa5s.exclamation-triangle",
            "Graphics API: Vulkan 1.3 driver verified" if vk_ok else "Graphics API: vulkan-1.dll not found",
            icon_color="#059669" if vk_ok else "#dc2626"
        )
        rc_vbox.addWidget(rc5)

        pad_ok = is_controller_connected()
        rc6 = ReadyCheckItem(
            "fa5s.gamepad",
            "Input: Gamepad detected (Ready)" if pad_ok else "Input: Keyboard & Mouse active",
            icon_color="#059669" if pad_ok else "#0f766e",
            show_arrow=True,
            has_action=True
        )
        rc6.clicked.connect(lambda: self.switch_page(2))
        rc_vbox.addWidget(rc6)

        rc_vbox.addStretch(1)
        cards_hbox.addWidget(ready_card, 1)

        # Right Card: QUICK PERFORMANCE & PICTURE SETUP
        goal_card = QFrame()
        goal_card.setObjectName("CardFrame")
        gc_vbox = QVBoxLayout(goal_card)
        gc_vbox.setContentsMargins(18, 16, 18, 16)
        gc_vbox.setSpacing(10)

        gc_title = QLabel("Performance & Picture Quick Setup")
        gc_title.setStyleSheet("color: #0f172a; font-size: 15px; font-weight: 800;")
        gc_vbox.addWidget(gc_title)

        fps_opts = [("smooth", "Smooth"), ("balanced", "Balanced"), ("quiet", "Quiet")]
        self.fps_seg = SegmentedButtons(fps_opts, active_key=self.fps_goal)
        self.fps_seg.selectionChanged.connect(self.on_fps_goal_changed)
        gc_vbox.addWidget(self.fps_seg)

        self.fps_limit_seg = SegmentedButtons(
            [("60", "60 FPS"), ("90", "90 FPS"), ("120", "120 FPS")], active_key=str(self.fps_limit))
        self.fps_limit_seg.setAccessibleName("Smooth frame rate limit")
        self.fps_limit_seg.selectionChanged.connect(self.on_fps_limit_changed)
        gc_vbox.addWidget(self.fps_limit_seg)

        self.goal_desc_lbl = QLabel("Higher frame rates with real-time game and animation clocks.")
        self.goal_desc_lbl.setWordWrap(True)
        self.goal_desc_lbl.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        gc_vbox.addWidget(self.goal_desc_lbl)

        self.goal_exp_lbl = QLabel(f"Target: up to {self.fps_limit} FPS at {self.selected_res_id}")
        self.goal_exp_lbl.setStyleSheet("color: #0f766e; font-size: 12px; font-weight: 800;")
        gc_vbox.addWidget(self.goal_exp_lbl)

        # Fullscreen Toggle switch
        fs_hbox = QHBoxLayout()
        self.fs_switch = ToggleSwitch(checked=(self.selected_window_mode != "windowed"), active_color="#0f766e")
        self.fs_switch.toggled.connect(self.on_fullscreen_switch_toggled)
        fs_hbox.addWidget(self.fs_switch)

        fs_text = QLabel("Fullscreen Display Mode")
        fs_text.setStyleSheet("color: #0f172a; font-size: 12px; font-weight: 700;")
        fs_hbox.addWidget(fs_text)
        fs_hbox.addStretch(1)
        gc_vbox.addLayout(fs_hbox)

        # Resolution quick chips
        res_vbox = QVBoxLayout()
        res_vbox.setSpacing(4)
        res_quick_lbl = QLabel("Target Scene Resolution")
        res_quick_lbl.setStyleSheet("color: #0f172a; font-size: 12px; font-weight: 700;")
        res_vbox.addWidget(res_quick_lbl)

        res_chips_hbox = QHBoxLayout()
        res_chips_hbox.setSpacing(6)
        self.res_chips = []
        res_list = RESOLUTIONS[:5]
        for res_id, label in res_list:
            chip = ResChip(res_id, label, is_selected=(res_id == self.selected_res_id), on_select=self.set_resolution)
            self.res_chips.append(chip)
            res_chips_hbox.addWidget(chip)

        res_vbox.addLayout(res_chips_hbox)
        gc_vbox.addLayout(res_vbox)

        # Shadow resolution quick chips
        shadow_vbox = QVBoxLayout()
        shadow_vbox.setSpacing(4)
        shadow_quick_lbl = QLabel("Shadow Map Resolution")
        shadow_quick_lbl.setStyleSheet("color: #0f172a; font-size: 12px; font-weight: 700;")
        shadow_vbox.addWidget(shadow_quick_lbl)

        shadow_chips_hbox = QHBoxLayout()
        shadow_chips_hbox.setSpacing(6)
        self.shadow_chips = []
        for sid, label, _ in SHADOW_RESOLUTIONS:
            chip = ResChip(sid, label, is_selected=(sid == self.selected_shadow_res), on_select=self.set_shadow_resolution)
            self.shadow_chips.append(chip)
            shadow_chips_hbox.addWidget(chip)

        shadow_vbox.addLayout(shadow_chips_hbox)
        gc_vbox.addLayout(shadow_vbox)
        gc_vbox.addStretch(1)

        cards_hbox.addWidget(goal_card, 1)
        vbox.addLayout(cards_hbox, 1)

        return page

    def create_graphics_page(self):
        page = QWidget()
        palette = page.palette()
        palette.setColor(QPalette.ColorRole.Window, QColor("#f1f5f9"))
        page.setPalette(palette)
        page.setAutoFillBackground(True)
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(28, 22, 28, 24)
        vbox.setSpacing(14)

        title = QLabel("Picture & Performance Settings")
        title.setStyleSheet("font-size: 20px; font-weight: 800; color: #0f172a;")
        vbox.addWidget(title)

        desc = QLabel("Configure display mode, internal rendering resolution, shadow cascades, Intel XeSS Super Resolution, and Frame Generation.")
        desc.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        vbox.addWidget(desc)

        # Display Mode Cards
        mode_sec = QLabel("Display Mode")
        mode_sec.setStyleSheet("color: #0f172a; font-size: 14px; font-weight: 800; margin-top: 6px;")
        vbox.addWidget(mode_sec)

        self.mode_cards = []
        modes = [
            ("borderless", "Borderless Fullscreen", "Runs at desktop resolution for seamless Alt+Tab and crisp scaling.", "fa5s.window-maximize"),
            ("fullscreen", "Exclusive Fullscreen", "Uses the current monitor mode; scene quality does not change its size.", "fa5s.expand"),
            ("windowed", "Windowed", "Opens in a resizable window with native OS borders.", "fa5s.window-restore"),
        ]

        for mid, mtitle, mdesc, micon in modes:
            card = ModeCard(mid, mtitle, mdesc, micon, is_selected=(mid == self.selected_window_mode), on_select=self.set_window_mode)
            self.mode_cards.append(card)
            vbox.addWidget(card)

        # Resolution Grid
        res_sec = QLabel("Target Scene Resolution")
        res_sec.setStyleSheet("color: #0f172a; font-size: 14px; font-weight: 800; margin-top: 10px;")
        vbox.addWidget(res_sec)

        grid = QGridLayout()
        grid.setSpacing(10)
        res_options = [(rid, label, rid.replace("x", " x ")) for rid, label in RESOLUTIONS]

        self.detailed_res_btns = {}
        for i, (rid, rname, rdesc) in enumerate(res_options):
            btn = QPushButton(f"{rname}\n{rdesc}")
            btn.setFixedHeight(52)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            is_sel = (rid == self.selected_res_id)
            self.style_detailed_res_btn(btn, is_sel)
            btn.clicked.connect(lambda ch, r=rid: self.set_resolution(r))
            self.detailed_res_btns[rid] = btn
            grid.addWidget(btn, i // 3, i % 3)
        vbox.addLayout(grid)

        # Shadow Quality & Resolution Grid
        shadow_sec = QLabel("Shadow Quality & Cascade Resolution")
        shadow_sec.setStyleSheet("color: #0f172a; font-size: 14px; font-weight: 800; margin-top: 10px;")
        vbox.addWidget(shadow_sec)

        shadow_desc = QLabel("Controls shadow map cascade resolution. 1024x matches Switch Docked; 2048x+ renders crisp PC shadows.")
        shadow_desc.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        vbox.addWidget(shadow_desc)

        shadow_grid = QGridLayout()
        shadow_grid.setSpacing(10)
        self.detailed_shadow_btns = {}
        for i, (sid, sname, sdesc) in enumerate(SHADOW_RESOLUTIONS):
            btn = QPushButton(f"{sname}\n{sdesc}")
            btn.setFixedHeight(50)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            is_sel = (sid == self.selected_shadow_res)
            self.style_detailed_res_btn(btn, is_sel)
            btn.clicked.connect(lambda ch, s=sid: self.set_shadow_resolution(s))
            self.detailed_shadow_btns[sid] = btn
            shadow_grid.addWidget(btn, i // 3, i % 3)
        vbox.addLayout(shadow_grid)

        # Intel XeSS Super Resolution (Upscaler)
        xess_sr_sec = QLabel("Intel XeSS Super Resolution (Upscaler)")
        xess_sr_sec.setStyleSheet("color: #0f172a; font-size: 14px; font-weight: 800; margin-top: 10px;")
        vbox.addWidget(xess_sr_sec)

        xess_sr_desc = QLabel("High-efficiency AI & spatial-temporal upscaling compatible with all Vulkan 1.3 GPUs (Intel, NVIDIA, AMD). Renders internally at a lower resolution and reconstructs sharp target output.")
        xess_sr_desc.setWordWrap(True)
        xess_sr_desc.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        vbox.addWidget(xess_sr_desc)

        xess_sr_box = QFrame()
        xess_sr_box.setStyleSheet("background-color: #ffffff; border: 1px solid #cbd5e1; border-radius: 8px;")
        xess_sr_layout = QHBoxLayout(xess_sr_box)
        xess_sr_layout.setContentsMargins(16, 12, 16, 12)
        xess_sr_layout.setSpacing(14)

        self.xess_sr_switch = ToggleSwitch(checked=self.xess_sr_on, active_color="#0f766e")
        self.xess_sr_switch.toggled.connect(self.set_xess_sr_enabled)
        xess_sr_layout.addWidget(self.xess_sr_switch)

        xess_sr_txt_layout = QVBoxLayout()
        xess_sr_txt_layout.setSpacing(2)
        xess_sr_title = QLabel("Enable Intel XeSS Super Resolution (XeSS-SR)")
        xess_sr_title.setStyleSheet("color: #0f172a; font-size: 13px; font-weight: 700; border: none; background: transparent;")
        xess_sr_subtitle = QLabel("Uses libxess.dll in xess/ when present or built-in Vulkan 1.3 upscaling on any GPU.")
        xess_sr_subtitle.setStyleSheet("color: #475569; font-size: 11px; font-weight: 500; border: none; background: transparent;")
        xess_sr_txt_layout.addWidget(xess_sr_title)
        xess_sr_txt_layout.addWidget(xess_sr_subtitle)
        xess_sr_layout.addLayout(xess_sr_txt_layout)
        xess_sr_layout.addStretch(1)
        vbox.addWidget(xess_sr_box)

        xess_sr_grid = QGridLayout()
        xess_sr_grid.setSpacing(10)
        self.detailed_xess_sr_btns = {}
        for i, (mid, mname) in enumerate(XESS_SR_MODES):
            btn = QPushButton(mname)
            btn.setFixedHeight(44)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            is_sel = (mid == self.selected_xess_sr_mode)
            self.style_detailed_res_btn(btn, is_sel)
            btn.clicked.connect(lambda ch, m=mid: self.set_xess_sr_mode(m))
            self.detailed_xess_sr_btns[mid] = btn
            xess_sr_grid.addWidget(btn, i // 3, i % 3)
        vbox.addLayout(xess_sr_grid)

        # Intel XeSS Frame Generation (XeFG / XeLL)
        xess_sec = QLabel("Intel XeSS Frame Generation (XeFG & XeLL Low Latency)")
        xess_sec.setStyleSheet("color: #0f172a; font-size: 14px; font-weight: 800; margin-top: 10px;")
        vbox.addWidget(xess_sec)

        xess_desc = QLabel("Optical-flow motion-compensated frame interpolation with 2x, 3x, and 4x multipliers and sub-frame display pacing.")
        xess_desc.setWordWrap(True)
        xess_desc.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        vbox.addWidget(xess_desc)

        xess_box = QFrame()
        xess_box.setStyleSheet("background-color: #ffffff; border: 1px solid #cbd5e1; border-radius: 8px;")
        xess_layout = QHBoxLayout(xess_box)
        xess_layout.setContentsMargins(16, 12, 16, 12)
        xess_layout.setSpacing(14)

        self.xess_fg_switch = ToggleSwitch(checked=self.xess_fg_on, active_color="#0f766e")
        self.xess_fg_switch.toggled.connect(self.set_xess_fg_enabled)
        xess_layout.addWidget(self.xess_fg_switch)

        xess_txt_layout = QVBoxLayout()
        xess_txt_layout.setSpacing(2)
        xess_title = QLabel("Enable Intel XeSS Frame Generation (XeFG)")
        xess_title.setStyleSheet("color: #0f172a; font-size: 13px; font-weight: 700; border: none; background: transparent;")
        xess_subtitle = QLabel("Synthesizes motion-compensated intermediate frames (2x / 3x / 4x) with scene-cut protection.")
        xess_subtitle.setStyleSheet("color: #475569; font-size: 11px; font-weight: 500; border: none; background: transparent;")
        xess_txt_layout.addWidget(xess_title)
        xess_txt_layout.addWidget(xess_subtitle)
        xess_layout.addLayout(xess_txt_layout)
        xess_layout.addStretch(1)
        vbox.addWidget(xess_box)

        xess_grid = QGridLayout()
        xess_grid.setSpacing(10)
        self.detailed_xess_fg_btns = {}
        active_mult_key = self.selected_xess_fg_mult if self.xess_fg_on else "off"
        for i, item in enumerate(XESS_FG_MULTIPLIERS):
            mid = item[0]
            mname = item[1]
            mdesc = item[2] if len(item) > 2 else ""
            btn = QPushButton(f"{mname}\n{mdesc}".strip() if mdesc else mname)
            btn.setFixedHeight(48)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            is_sel = (mid == active_mult_key)
            self.style_detailed_res_btn(btn, is_sel)
            btn.clicked.connect(lambda ch, m=mid: self.set_xess_fg_multiplier(m))
            self.detailed_xess_fg_btns[mid] = btn
            xess_grid.addWidget(btn, i // 2, i % 2)
        vbox.addLayout(xess_grid)

        # Downscale / Fit to screen option
        ds_box = QFrame()
        ds_box.setStyleSheet("background-color: #ffffff; border: 1px solid #cbd5e1; border-radius: 8px; margin-top: 8px;")
        ds_layout = QHBoxLayout(ds_box)
        ds_layout.setContentsMargins(16, 12, 16, 12)
        ds_layout.setSpacing(14)

        self.downscale_switch = ToggleSwitch(checked=self.fit_window, active_color="#0f766e")
        self.downscale_switch.toggled.connect(lambda ch: self.save_settings())
        ds_layout.addWidget(self.downscale_switch)

        ds_txt_layout = QVBoxLayout()
        ds_txt_layout.setSpacing(2)
        ds_title = QLabel("Keep window within the desktop work area")
        ds_title.setStyleSheet("color: #0f172a; font-size: 13px; font-weight: 700; border: none; background: transparent;")
        ds_desc = QLabel("Windowed mode only. The game image always fits the display with its 16:9 aspect ratio preserved.")
        ds_desc.setWordWrap(True)
        ds_desc.setStyleSheet("color: #475569; font-size: 11px; font-weight: 500; border: none; background: transparent;")
        ds_txt_layout.addWidget(ds_title)
        ds_txt_layout.addWidget(ds_desc)
        ds_layout.addLayout(ds_txt_layout)
        ds_layout.addStretch(1)
        vbox.addWidget(ds_box)

        size_row = QHBoxLayout()
        win_size_lbl = QLabel("Window size:")
        win_size_lbl.setStyleSheet("color: #0f172a; font-weight: 700; font-size: 12px;")
        size_row.addWidget(win_size_lbl)
        self.window_width_spin = QSpinBox()
        self.window_width_spin.setRange(640, 7680)
        self.window_width_spin.setValue(self.window_width)
        self.window_height_spin = QSpinBox()
        self.window_height_spin.setRange(360, 4320)
        self.window_height_spin.setValue(self.window_height)
        size_row.addWidget(self.window_width_spin)
        sep_lbl = QLabel("x")
        sep_lbl.setStyleSheet("color: #334155; font-weight: 700;")
        size_row.addWidget(sep_lbl)
        size_row.addWidget(self.window_height_spin)
        size_row.addStretch()
        for spin in (self.window_width_spin, self.window_height_spin):
            spin.setStyleSheet(
                "QSpinBox { background: #ffffff; color: #0f172a; border: 1px solid #cbd5e1; border-radius: 6px; padding: 5px 10px; font-weight: 700; }"
                "QSpinBox:disabled { background: #f1f5f9; color: #94a3b8; border-color: #e2e8f0; }"
            )
            spin.setEnabled(self.selected_window_mode == "windowed")
            spin.valueChanged.connect(lambda _: self.save_settings())
        vbox.addLayout(size_row)

        self.resolution_note = QLabel("Higher scene quality uses more GPU VRAM. Other display aspect ratios use letterboxing to preserve the native 16:9 composition.")
        self.resolution_note.setWordWrap(True)
        self.resolution_note.setStyleSheet("color: #475569; font-size: 11px; font-weight: 500;")
        vbox.addWidget(self.resolution_note)
        vbox.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setStyleSheet("background: #f1f5f9;")
        scroll.setWidget(page)
        return scroll

    def style_detailed_res_btn(self, btn, is_selected):
        if is_selected:
            btn.setStyleSheet("""
                QPushButton {
                    background-color: #f0fdfa;
                    border: 2px solid #0f766e;
                    border-radius: 8px;
                    color: #0f766e;
                    font-weight: 800;
                    font-size: 12px;
                    text-align: center;
                    padding: 6px;
                }
            """)
        else:
            btn.setStyleSheet("""
                QPushButton {
                    background-color: #ffffff;
                    border: 1px solid #cbd5e1;
                    border-radius: 8px;
                    color: #0f172a;
                    font-weight: 700;
                    font-size: 12px;
                    text-align: center;
                    padding: 6px;
                }
                QPushButton:hover {
                    background-color: #f8fafc;
                    border-color: #0f766e;
                    color: #0f766e;
                }
            """)

    def create_controls_page(self):
        self.controls_page = ControlsPage(self.input_settings, self.save_settings, is_controller_connected())
        return self.controls_page

    def _light_action_button_style(self):
        return """
            QPushButton {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                color: #0f172a;
                font-weight: 700;
                font-size: 13px;
                padding-left: 16px;
                text-align: left;
            }
            QPushButton:hover {
                background-color: #f0fdfa;
                border-color: #0f766e;
                color: #0f766e;
            }
        """

    def create_game_files_page(self):
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(28, 22, 28, 22)
        vbox.setSpacing(14)

        title = QLabel("Game Files & Shader Cache")
        title.setStyleSheet("font-size: 20px; font-weight: 800; color: #0f172a;")
        vbox.addWidget(title)

        desc = QLabel(f"Installation Root: {BASE_DIR}")
        desc.setStyleSheet("color: #334155; font-size: 12px; font-family: 'Consolas', monospace; font-weight: 600;")
        vbox.addWidget(desc)

        card = QFrame()
        card.setObjectName("CardFrame")
        cv = QVBoxLayout(card)
        cv.setSpacing(10)

        buttons = [
            ("Pre-compile / Verify Shaders", "fa5s.bolt", "#0f766e", self.open_shader_precompile),
            ("Open Vulkan Shader Cache Folder", "fa5s.archive", "#1d4ed8", lambda: os.startfile(str(BASE_DIR / "userdata" / "cache" / "vulkan-shaders")) if (BASE_DIR / "userdata" / "cache" / "vulkan-shaders").is_dir() else None),
            ("Open Game Root Directory", "fa5s.folder-open", "#0f766e", lambda: os.startfile(str(BASE_DIR))),
            ("Open RomFS Data Folder", "fa5s.database", "#0f766e", lambda: os.startfile(str(ROMFS_DIR)) if ROMFS_DIR.is_dir() else None),
            ("Open ExeFS Binaries Folder", "fa5s.cogs", "#0f766e", lambda: os.startfile(str(EXEFS_DIR)) if EXEFS_DIR.is_dir() else None),
            ("Open Configuration File (ultratears.cfg)", "fa5s.file-alt", "#1d4ed8", lambda: os.startfile(str(CFG_PATH)) if CFG_PATH.is_file() else None),
        ]

        for btitle, bicon, bcolor, baction in buttons:
            btn = QPushButton(btitle)
            btn.setIcon(qta.icon(bicon, color=bcolor))
            btn.setFixedHeight(42)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setStyleSheet(self._light_action_button_style())
            btn.clicked.connect(baction)
            cv.addWidget(btn)

        vbox.addWidget(card)
        vbox.addStretch(1)
        return page

    def create_mods_page(self):
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(28, 22, 28, 22)
        vbox.setSpacing(14)

        title = QLabel("Mods & Custom RomFS Patches")
        title.setStyleSheet("font-size: 20px; font-weight: 800; color: #0f172a;")
        vbox.addWidget(title)

        desc = QLabel(f"RomFS mods path: {ROMFS_DIR}")
        desc.setStyleSheet("color: #334155; font-size: 12px; font-family: 'Consolas', monospace; font-weight: 600;")
        vbox.addWidget(desc)

        card = QFrame()
        card.setObjectName("CardFrame")
        cv = QVBoxLayout(card)
        cv.setSpacing(12)

        open_romfs_btn = QPushButton("Open RomFS Folder")
        open_romfs_btn.setIcon(qta.icon("fa5s.folder-open", color="#0f766e"))
        open_romfs_btn.setFixedHeight(42)
        open_romfs_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        open_romfs_btn.setStyleSheet(self._light_action_button_style())
        open_romfs_btn.clicked.connect(lambda: os.startfile(str(ROMFS_DIR)) if ROMFS_DIR.is_dir() else None)
        cv.addWidget(open_romfs_btn)

        info_lbl = QLabel("To install mods: copy mod files directly into their matching folders inside data/romfs.")
        info_lbl.setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;")
        cv.addWidget(info_lbl)

        vbox.addWidget(card)
        vbox.addStretch(1)
        return page

    def create_saves_page(self):
        page = QWidget()
        vbox = QVBoxLayout(page)
        vbox.setContentsMargins(28, 22, 28, 22)
        vbox.setSpacing(14)

        title = QLabel("Save Data & User Profiles")
        title.setStyleSheet("font-size: 20px; font-weight: 800; color: #0f172a;")
        vbox.addWidget(title)

        desc = QLabel(f"Save game directory: {SAVES_DIR}")
        desc.setStyleSheet("color: #334155; font-size: 12px; font-family: 'Consolas', monospace; font-weight: 600;")
        vbox.addWidget(desc)

        card = QFrame()
        card.setObjectName("CardFrame")
        cv = QVBoxLayout(card)
        cv.setSpacing(12)

        open_save_btn = QPushButton("Open Save Directory (userdata)")
        open_save_btn.setIcon(qta.icon("fa5s.folder-open", color="#0f766e"))
        open_save_btn.setFixedHeight(42)
        open_save_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        open_save_btn.setStyleSheet(self._light_action_button_style())
        open_save_btn.clicked.connect(self.open_saves_folder)
        cv.addWidget(open_save_btn)

        vbox.addWidget(card)
        vbox.addStretch(1)
        return page

    def open_saves_folder(self):
        SAVES_DIR.mkdir(parents=True, exist_ok=True)
        os.startfile(str(SAVES_DIR))

    def create_footer(self):
        footer = QFrame()
        footer.setObjectName("FooterBar")
        footer.setFixedHeight(66)

        layout = QHBoxLayout(footer)
        layout.setContentsMargins(24, 0, 24, 0)
        layout.setSpacing(14)
        layout.setAlignment(Qt.AlignmentFlag.AlignVCenter)

        self.status_lbl = QLabel()
        self.update_status_bar()
        layout.addWidget(self.status_lbl)

        layout.addStretch(1)

        log_btn = QPushButton("View Log")
        log_btn.setIcon(qta.icon("fa5s.terminal", color="#0f172a"))
        log_btn.setFixedHeight(40)
        log_btn.setFixedWidth(105)
        log_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        log_btn.setStyleSheet("""
            QPushButton {
                background-color: #ffffff;
                border: 1px solid #cbd5e1;
                border-radius: 8px;
                color: #0f172a;
                font-weight: 700;
                font-size: 12px;
            }
            QPushButton:hover {
                background-color: #f1f5f9;
                border-color: #0f766e;
                color: #0f766e;
            }
        """)
        log_btn.clicked.connect(self.show_log_dialog)
        layout.addWidget(log_btn)

        self.play_btn = QPushButton("LAUNCH GAME")
        self.play_btn.setIcon(qta.icon("fa5s.play", color="#ffffff"))
        self.play_btn.setIconSize(QSize(13, 13))
        self.play_btn.setFixedHeight(40)
        self.play_btn.setFixedWidth(175)
        self.play_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.play_btn.setStyleSheet("""
            QPushButton {
                background-color: #0f766e;
                border: 1px solid #0d9488;
                border-radius: 8px;
                color: #ffffff;
                font-weight: 800;
                font-size: 13px;
                letter-spacing: 0.8px;
            }
            QPushButton:hover {
                background-color: #0d9488;
            }
            QPushButton:pressed {
                background-color: #115e59;
            }
        """)
        self.play_btn.clicked.connect(self.launch_game)
        layout.addWidget(self.play_btn)

        return footer

    def update_status_bar(self):
        mode_text = "Borderless" if self.selected_window_mode == "borderless" else ("Exclusive Fullscreen" if self.selected_window_mode == "fullscreen" else "Windowed")
        fps_text = f"Smooth (up to {self.fps_limit} FPS)" if self.fps_goal == "smooth" else ("Balanced (30 FPS)" if self.fps_goal == "balanced" else "Quiet (30 FPS)")
        xess_sr_text = ""
        if getattr(self, "xess_sr_on", False):
            render_res = xess_sr_render_resolution(self.selected_res_id, True, self.selected_xess_sr_mode)
            render_name = dict(RESOLUTIONS).get(render_res, render_res)
            mode_label = self.selected_xess_sr_mode.replace("_", " ").title()
            if self.selected_xess_sr_mode == "aa":
                mode_label = "Native AA"
            xess_sr_text = f"  |  XeSS ({mode_label}) [Render {render_name}]"
        xess_fg_text = f"  |  XeSS-FG ({getattr(self, 'selected_xess_fg_mult', '2')}x)" if getattr(self, "xess_fg_on", False) else ""
        self.status_lbl.setText(f"{fps_text}  |  {mode_text}  |  {self.selected_res_id}{xess_sr_text}{xess_fg_text}  |  Shadow {self.selected_shadow_res}x  |  Vulkan 1.3 Ready")
        self.status_lbl.setStyleSheet("color: #1e293b; font-size: 12px; font-weight: 700;")

    def runtime_settings(self):
        smooth = self.fps_goal == "smooth"
        fit = self.downscale_switch.isChecked() if hasattr(self, "downscale_switch") else self.fit_window
        ww = self.window_width_spin.value() if hasattr(self, "window_width_spin") else self.window_width
        wh = self.window_height_spin.value() if hasattr(self, "window_height_spin") else self.window_height
        xess_sr_on = getattr(self, "xess_sr_on", False)
        xess_sr_mode_val = getattr(self, "selected_xess_sr_mode", "quality")
        xess_fg_on = getattr(self, "xess_fg_on", False)
        xess_fg_mult = str(getattr(self, "selected_xess_fg_mult", "2"))
        return {
            **display_values(self.selected_res_id, self.selected_window_mode, fit, ww, wh,
                             xess_sr_on=xess_sr_on, xess_sr_mode_name=xess_sr_mode_val),
            "TOTK_SHADOW_RES": str(self.selected_shadow_res),
            "TOTK_XESS_SR": "1" if xess_sr_on else "0",
            "TOTK_XESS_SR_MODE": xess_sr_mode_val,
            "TOTK_XESS_FG": "1" if xess_fg_on else "0",
            "TOTK_XESS_FG_MULTIPLIER": xess_fg_mult,
            "TOTK_UNCAPPED": "1" if smooth else "0",
            "TOTK_UNCAPPED_HZ": str(self.fps_limit) if smooth else "30",
            "TOTK_FPS60": "1" if smooth else "0",
            "TOTK_MAX_INTERVAL": "1" if smooth else "2",
            "TOTK_UNCAPPED_INTERVAL": "1" if smooth else "0",
            "TOTK_DYNAMIC_FPS": "1", "TOTK_REALTIME": "1",
            "TOTK_OPERATION_MODE": "handheld" if self.fps_goal == "quiet" else "docked",
            **self.input_settings.runtime_values(),
        }

    def save_settings(self):
        values = {**self.runtime_settings(), "launcher_fps_limit": str(self.fps_limit)}
        try:
            write_config_values(values)
        except OSError as error:
            QMessageBox.warning(self, "Settings not saved", str(error))
            return False
        self.cfg.update(values)
        return True

    def update_fps_labels(self):
        self.fps_limit_seg.setVisible(self.fps_goal == "smooth")
        target = self.fps_limit if self.fps_goal == "smooth" else 30
        if self.fps_goal == "smooth":
            self.goal_desc_lbl.setText("Higher frame rates with real-time game and animation clocks.")
        elif self.fps_goal == "balanced":
            self.goal_desc_lbl.setText("30 FPS with the docked profile and real-time game speed.")
        else:
            self.goal_desc_lbl.setText("30 FPS with the handheld profile for a lighter workload.")
        self.goal_exp_lbl.setText(f"Target: up to {target} FPS at {self.selected_res_id}")
        self.update_status_bar()

    def on_fps_goal_changed(self, goal):
        self.fps_goal = goal
        self.save_settings()
        self.update_fps_labels()

    def on_fps_limit_changed(self, limit):
        self.fps_limit = int(limit)
        self.save_settings()
        self.update_fps_labels()

    def on_fullscreen_switch_toggled(self, is_checked):
        new_mode = "borderless" if is_checked else "windowed"
        self.set_window_mode(new_mode)

    def set_window_mode(self, mode_id):
        self.selected_window_mode = mode_id
        for name in ("window_width_spin", "window_height_spin"):
            if hasattr(self, name): getattr(self, name).setEnabled(mode_id == "windowed")
        self.save_settings()

        for card in self.mode_cards:
            card.set_selected(card.mode_id == mode_id)

        if hasattr(self, "fs_switch"):
            self.fs_switch.setChecked(mode_id != "windowed")

        if hasattr(self, "rc_display_item"):
            mode_text = "Borderless Fullscreen" if mode_id == "borderless" else ("Exclusive Fullscreen" if mode_id == "fullscreen" else "Windowed")
            self.rc_display_item.set_text(f"Display: {mode_text} at {self.selected_res_id}")

        self.update_status_bar()

    def set_resolution(self, res_id):
        if res_id not in dict(RESOLUTIONS):
            return
        self.selected_res_id = res_id
        self.save_settings()

        for chip in self.res_chips:
            chip.set_selected(chip.res_id == res_id)

        if hasattr(self, "detailed_res_btns"):
            for rid, btn in self.detailed_res_btns.items():
                self.style_detailed_res_btn(btn, rid == res_id)

        if hasattr(self, "rc_display_item"):
            mode_text = "Borderless Fullscreen" if self.selected_window_mode == "borderless" else ("Exclusive Fullscreen" if self.selected_window_mode == "fullscreen" else "Windowed")
            self.rc_display_item.set_text(f"Display: {mode_text} at {self.selected_res_id}")

        if hasattr(self, "goal_exp_lbl"):
            self.update_fps_labels()

        self.update_status_bar()

    def set_shadow_resolution(self, res_id):
        valid = dict((r[0], r[1]) for r in SHADOW_RESOLUTIONS)
        if res_id not in valid:
            return
        self.selected_shadow_res = res_id
        self.save_settings()

        for chip in self.shadow_chips:
            chip.set_selected(chip.res_id == res_id)

        if hasattr(self, "detailed_shadow_btns"):
            for sid, btn in self.detailed_shadow_btns.items():
                self.style_detailed_res_btn(btn, sid == res_id)

        self.update_status_bar()

    def set_xess_sr_enabled(self, enabled):
        self.xess_sr_on = bool(enabled)
        if hasattr(self, "xess_sr_switch") and self.xess_sr_switch.isChecked() != self.xess_sr_on:
            self.xess_sr_switch.setChecked(self.xess_sr_on)
        self.save_settings()
        self.update_status_bar()

    def set_xess_sr_mode(self, mode_id):
        valid = dict(XESS_SR_MODES)
        if mode_id not in valid:
            return
        self.selected_xess_sr_mode = mode_id
        self.save_settings()
        if hasattr(self, "detailed_xess_sr_btns"):
            for mid, btn in self.detailed_xess_sr_btns.items():
                self.style_detailed_res_btn(btn, mid == mode_id)
        self.update_status_bar()

    def _sync_xess_fg_buttons(self):
        active_key = self.selected_xess_fg_mult if self.xess_fg_on else "off"
        if hasattr(self, "detailed_xess_fg_btns"):
            for mid, btn in self.detailed_xess_fg_btns.items():
                self.style_detailed_res_btn(btn, mid == active_key)

    def set_xess_fg_enabled(self, enabled):
        self.xess_fg_on = bool(enabled)
        if self.xess_fg_on and self.selected_xess_fg_mult not in ("2", "3", "4"):
            self.selected_xess_fg_mult = "2"
        if hasattr(self, "xess_fg_switch") and self.xess_fg_switch.isChecked() != self.xess_fg_on:
            self.xess_fg_switch.setChecked(self.xess_fg_on)
        self._sync_xess_fg_buttons()
        self.save_settings()
        self.update_status_bar()

    def set_xess_fg_multiplier(self, mult_id):
        mult_str = str(mult_id).strip().lower()
        if mult_str in ("off", "0", "1"):
            self.xess_fg_on = False
            if hasattr(self, "xess_fg_switch") and self.xess_fg_switch.isChecked():
                self.xess_fg_switch.setChecked(False)
        elif mult_str in ("2", "3", "4"):
            self.selected_xess_fg_mult = mult_str
            self.xess_fg_on = True
            if hasattr(self, "xess_fg_switch") and not self.xess_fg_switch.isChecked():
                self.xess_fg_switch.setChecked(True)
        else:
            return
        self._sync_xess_fg_buttons()
        self.save_settings()
        self.update_status_bar()

    def show_log_dialog(self):
        dlg = LogDialog(self)
        dlg.exec()

    def open_shader_precompile(self):
        env = os.environ.copy()
        env.pop("DM_PRESENT_HZ", None)
        env.pop("DM_PRESENT_INTERVAL", None)
        env.pop("TOTK_NO_OVERRIDE_NVN", None)
        env.pop("TOTK_NO_OVERRIDE_VP", None)
        env.update(self.runtime_settings())
        dlg = ShaderPrepareDialog(self, env)
        dlg.exec()

    def launch_game(self):
        if not EXE_PATH.is_file():
            print(f"Executable not found at {EXE_PATH}")
            return

        if not self.save_settings():
            return
        env = os.environ.copy()
        env.pop("DM_PRESENT_HZ", None)
        env.pop("DM_PRESENT_INTERVAL", None)
        env.pop("TOTK_NO_OVERRIDE_NVN", None)
        env.pop("TOTK_NO_OVERRIDE_VP", None)
        env.update(self.runtime_settings())

        try:
            with open(LOG_PATH, "w", encoding="utf-8") as log_f:
                subprocess.Popen([str(EXE_PATH)], cwd=str(BASE_DIR), env=env, stdout=log_f, stderr=subprocess.STDOUT)
            self.close()
        except Exception as e:
            print(f"Failed to launch game: {e}")


def main():
    app = QApplication(sys.argv)
    window = PrelauncherWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
