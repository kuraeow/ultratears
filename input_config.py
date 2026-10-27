"""Keyboard/mouse settings shared with the compiled SDL input layer through the .def file."""
import csv
import math
from pathlib import Path

_definition = Path(__file__).resolve().parent / 'src/host/keyboard_bindings.def'
ACTIONS = []
for _line in _definition.read_text(encoding='utf-8').splitlines():
    if _line.startswith('INPUT_BINDING('):
        _fields = next(csv.reader([_line[len('INPUT_BINDING('):-1]], skipinitialspace=True))
        _id, _group, _switch, _description, _primary, _secondary, _legacy = _fields
        ACTIONS.append(dict(id=_id, group=_group, switch=_switch, description=_description,
                            defaults=(int(_primary), int(_secondary)), legacy=int(_legacy)))

KEY_NAMES = {0: 'Unbound', **{4 + n: chr(65 + n) for n in range(26)},
             **{30 + n: str(n + 1) for n in range(9)}, 39: '0',
             40: 'Enter', 41: 'Escape', 42: 'Backspace', 43: 'Tab', 44: 'Space',
             45: '-', 46: '=', 47: '[', 48: ']', 49: '\\', 51: ';', 52: "'",
             53: '`', 54: ',', 55: '.', 56: '/', 57: 'Caps Lock',
             **{58 + n: f'F{n + 1}' for n in range(12)},
             70: 'Print Screen', 71: 'Scroll Lock', 72: 'Pause', 73: 'Insert',
             74: 'Home', 75: 'Page Up', 76: 'Delete', 77: 'End', 78: 'Page Down',
             79: 'Right', 80: 'Left', 81: 'Down', 82: 'Up', 83: 'Num Lock',
             84: 'Num /', 85: 'Num *', 86: 'Num -', 87: 'Num +', 88: 'Num Enter',
             **{89 + n: f'Num {n + 1}' for n in range(9)}, 98: 'Num 0', 99: 'Num .',
             224: 'Left Ctrl', 225: 'Left Shift', 226: 'Left Alt',
             228: 'Right Ctrl', 229: 'Right Shift', 230: 'Right Alt',
             512: 'LMB', 513: 'RMB', 514: 'MMB'}
INPUT_CODE_COUNT = 515  # 0..511 SDL scancodes, followed by three mouse buttons
RESERVED_KEYS = {65, 68}  # F8: cursor capture; F11: fullscreen

MOUSE_CAMERA_VERSION = 2
VALID_MOUSE_BACKENDS = ('native', 'legacy_stick')
VALID_PROMPT_MODES = ('auto', 'kb_mouse', 'controller')
DEFAULT_PROMPT_MODE = 'auto'
VALID_SMOOTH_MS = (0, 5, 10)
DEFAULT_LOOK_DEG_PER_COUNT = 0.04
DEFAULT_AIM_MULTIPLIER = 1.0
DEFAULT_SCOPE_MULTIPLIER = 1.0
DEFAULT_SMOOTH_MS = 0
DEFAULT_LEGACY_SENSITIVITY = 1.0


def key_name(code):
    return KEY_NAMES.get(code, f'Key {code}')


def _parse_bounded_float(raw, default, lo, hi):
    try:
        if raw is None:
            return default
        val = float(raw)
        if not math.isfinite(val) or not (lo <= val <= hi):
            return default
        return val
    except (ValueError, TypeError):
        return default


def _format_deg_per_count(val):
    text = f'{val:.4f}'.rstrip('0').rstrip('.')
    if '.' not in text:
        text += '.0'
    return text


def _format_multiplier(val):
    text = f'{val:.2f}'.rstrip('0').rstrip('.')
    if '.' not in text:
        text += '.0'
    return text


def is_legacy_camera_config(config):
    """Return True if a non-empty config dictionary precedes camera version 2."""
    if not config:
        return False
    return str(config.get('TOTK_MOUSE_CAMERA_VERSION', '')).strip() != str(MOUSE_CAMERA_VERSION)


class InputSettings:
    def __init__(self, config=None):
        config = config or {}
        self.bindings = {}
        used = set()
        for action in ACTIONS:
            raw = config.get('TOTK_KEY_' + action['id'])
            try:
                pair = tuple(map(int, raw.split(','))) if raw is not None else action['defaults']
                if len(pair) != 2 or any(k < 0 or k >= INPUT_CODE_COUNT or k in RESERVED_KEYS for k in pair):
                    raise ValueError
            except (ValueError, TypeError):
                pair = action['defaults']
            clean = []
            for code in pair:
                clean.append(0 if code in used else code)
                if code:
                    used.add(code)
            self.bindings[action['id']] = clean

        self.camera_mode = 'mouse' if config.get('TOTK_CAMERA_MODE') == 'mouse' else 'keys'
        self.camera_version = MOUSE_CAMERA_VERSION

        prompt_mode = config.get('TOTK_PROMPT_MODE', DEFAULT_PROMPT_MODE)
        self.prompt_mode = prompt_mode if prompt_mode in VALID_PROMPT_MODES else DEFAULT_PROMPT_MODE

        backend = config.get('TOTK_MOUSE_BACKEND', 'native')
        self.mouse_backend = backend if backend in VALID_MOUSE_BACKENDS else 'native'

        # Legacy TOTK_MOUSE_SENSITIVITY is never interpreted as degrees per count;
        # it remains exclusively for the legacy_stick / compatibility path.
        self.sensitivity = _parse_bounded_float(
            config.get('TOTK_MOUSE_SENSITIVITY'), DEFAULT_LEGACY_SENSITIVITY, 0.1, 5.0
        )
        self.look_deg_per_count = _parse_bounded_float(
            config.get('TOTK_MOUSE_LOOK_DEG_PER_COUNT'), DEFAULT_LOOK_DEG_PER_COUNT, 0.001, 1.0
        )
        self.aim_multiplier = _parse_bounded_float(
            config.get('TOTK_MOUSE_AIM_MULTIPLIER'), DEFAULT_AIM_MULTIPLIER, 0.1, 2.0
        )
        self.scope_multiplier = _parse_bounded_float(
            config.get('TOTK_MOUSE_SCOPE_MULTIPLIER'), DEFAULT_SCOPE_MULTIPLIER, 0.1, 2.0
        )

        try:
            smooth_raw = int(config.get('TOTK_MOUSE_SMOOTH_MS', str(DEFAULT_SMOOTH_MS)))
            self.smooth_ms = smooth_raw if smooth_raw in VALID_SMOOTH_MS else DEFAULT_SMOOTH_MS
        except (ValueError, TypeError):
            self.smooth_ms = DEFAULT_SMOOTH_MS

        self.invert_y = config.get('TOTK_MOUSE_INVERT_Y') == '1'

    def runtime_values(self):
        result = {'TOTK_KEY_' + action['id']: ','.join(map(str, self.bindings[action['id']]))
                  for action in ACTIONS}
        result.update(
            TOTK_CAMERA_MODE=self.camera_mode,
            TOTK_PROMPT_MODE=self.prompt_mode,
            TOTK_MOUSE_CAMERA_VERSION=str(MOUSE_CAMERA_VERSION),
            TOTK_MOUSE_BACKEND=self.mouse_backend,
            TOTK_MOUSE_LOOK_DEG_PER_COUNT=_format_deg_per_count(self.look_deg_per_count),
            TOTK_MOUSE_AIM_MULTIPLIER=_format_multiplier(self.aim_multiplier),
            TOTK_MOUSE_SCOPE_MULTIPLIER=_format_multiplier(self.scope_multiplier),
            TOTK_MOUSE_SMOOTH_MS=str(self.smooth_ms),
            TOTK_MOUSE_SENSITIVITY=f'{self.sensitivity:.2f}',
            TOTK_MOUSE_INVERT_Y='1' if self.invert_y else '0',
        )
        return result
