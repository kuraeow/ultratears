"""Scene quality presets from the bundled NX Optimizer Options.json.

Window size is a separate preference; presentation always preserves aspect.
"""
RESOLUTIONS = (
    ("1280x720", "720p"), ("1600x900", "900p"), ("1920x1080", "1080p"),
    ("2560x1440", "1440p"), ("3840x2160", "4K"),
    ("6144x3456", "6K"), ("7680x4320", "8K"),
)

SHADOW_RESOLUTIONS = (
    ("512", "512x", "Low / Handheld"),
    ("1024", "1024x", "Medium / Docked (Default)"),
    ("1536", "1536x", "High Quality"),
    ("2048", "2048x", "Ultra Sharp"),
    ("3072", "3072x", "Very High"),
    ("4096", "4096x", "Cinematic 4K"),
)

XESS_SR_MODES = (
    ("ultra_quality_plus", "Ultra Quality Plus (1.2x)"),
    ("ultra_quality", "Ultra Quality (1.3x)"),
    ("quality", "Quality (1.5x)"),
    ("balanced", "Balanced (1.7x)"),
    ("performance", "Performance (2.0x)"),
    ("ultra_performance", "Ultra Performance (3.0x)"),
    ("aa", "Native AA (1.0x)"),
)

XESS_FG_MULTIPLIERS = (
    ("off", "Off (1x Native)"),
    ("2", "2x Frame Generation"),
    ("3", "3x Multi-Frame Gen"),
    ("4", "4x Multi-Frame Gen"),
)


def xess_sr_enabled(cfg):
    val = cfg.get("TOTK_XESS_SR", "0").strip().lower()
    return val in ("1", "true", "yes", "on")


def xess_sr_mode(cfg):
    val = cfg.get("TOTK_XESS_SR_MODE", "quality").strip().lower()
    if val in ("dlaa", "native_aa", "xess_aa"):
        return "aa"
    valid = dict(XESS_SR_MODES)
    return val if val in valid else "quality"


def xess_fg_enabled(cfg):
    val = cfg.get("TOTK_XESS_FG", "0").strip().lower()
    if val in ("2", "2x", "3", "3x", "4", "4x"):
        return True
    if val in ("1", "true", "yes", "on"):
        mult = cfg.get("TOTK_XESS_FG_MULTIPLIER", "2").strip().lower()
        return mult not in ("0", "1", "1x", "off", "false")
    return False


def xess_fg_multiplier(cfg):
    fg = cfg.get("TOTK_XESS_FG", "0").strip().lower()
    if fg in ("2", "2x"):
        return "2"
    if fg in ("3", "3x"):
        return "3"
    if fg in ("4", "4x"):
        return "4"
    val = cfg.get("TOTK_XESS_FG_MULTIPLIER", "2").strip().lower().rstrip("x")
    return val if val in ("2", "3", "4") else "2"


def shadow_resolution(cfg):
    val = cfg.get("TOTK_SHADOW_RES", "1024")
    return val if val in dict((r[0], r[1]) for r in SHADOW_RESOLUTIONS) else "1024"


def xess_sr_render_resolution(target_scene, xess_sr_on, mode):
    if not xess_sr_on or mode == "aa":
        return target_scene
    table = {
        "ultra_quality_plus": {
            "7680x4320": "6144x3456",
            "6144x3456": "3840x2160",
            "3840x2160": "2560x1440",
            "2560x1440": "1920x1080",
            "1920x1080": "1600x900",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
        "ultra_quality": {
            "7680x4320": "6144x3456",
            "6144x3456": "3840x2160",
            "3840x2160": "2560x1440",
            "2560x1440": "1920x1080",
            "1920x1080": "1600x900",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
        "quality": {
            "7680x4320": "3840x2160",
            "6144x3456": "3840x2160",
            "3840x2160": "2560x1440",
            "2560x1440": "1920x1080",
            "1920x1080": "1600x900",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
        "balanced": {
            "7680x4320": "3840x2160",
            "6144x3456": "2560x1440",
            "3840x2160": "1920x1080",
            "2560x1440": "1600x900",
            "1920x1080": "1280x720",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
        "performance": {
            "7680x4320": "2560x1440",
            "6144x3456": "1920x1080",
            "3840x2160": "1920x1080",
            "2560x1440": "1280x720",
            "1920x1080": "1280x720",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
        "ultra_performance": {
            "7680x4320": "1920x1080",
            "6144x3456": "1280x720",
            "3840x2160": "1280x720",
            "2560x1440": "1280x720",
            "1920x1080": "1280x720",
            "1600x900": "1280x720",
            "1280x720": "1280x720",
        },
    }
    mode_table = table.get(mode, table["quality"])
    return mode_table.get(target_scene, target_scene)


def scene_resolution(cfg):
    if "TOTK_TARGET_WIDTH" in cfg and "TOTK_TARGET_HEIGHT" in cfg:
        target_pair = f"{cfg.get('TOTK_TARGET_WIDTH', '')}x{cfg.get('TOTK_TARGET_HEIGHT', '')}"
        if target_pair in dict(RESOLUTIONS):
            return target_pair
    # A partially specified new pair must not mix with the legacy pair.
    prefix = "TOTK_INTERNAL_" if any(k in cfg for k in
        ("TOTK_INTERNAL_WIDTH", "TOTK_INTERNAL_HEIGHT")) else "TOTK_"
    pair = f"{cfg.get(prefix + 'WIDTH', '')}x{cfg.get(prefix + 'HEIGHT', '')}"
    return pair if pair in dict(RESOLUTIONS) else "1920x1080"


def window_dimension(value, default, low, high):
    try:
        result = int(value)
        return result if low <= result <= high else default
    except (TypeError, ValueError):
        return default


def display_values(scene, mode, fit, window_w, window_h, xess_sr_on=False, xess_sr_mode_name="quality"):
    if scene not in dict(RESOLUTIONS):
        raise ValueError("Unsupported scene resolution")
    target_w, target_h = scene.split("x")
    render_scene = xess_sr_render_resolution(scene, xess_sr_on, xess_sr_mode_name)
    rw, rh = render_scene.split("x")
    return {
        "TOTK_WINDOW_MODE": mode,
        "TOTK_TARGET_WIDTH": target_w,
        "TOTK_TARGET_HEIGHT": target_h,
        "TOTK_INTERNAL_WIDTH": rw,
        "TOTK_INTERNAL_HEIGHT": rh,
        # Mirror old keys so existing launch scripts retain their quality choice.
        "TOTK_WIDTH": target_w,
        "TOTK_HEIGHT": target_h,
        "TOTK_WINDOW_WIDTH": str(window_w),
        "TOTK_WINDOW_HEIGHT": str(window_h),
        "TOTK_DOWNSCALE": "1" if fit else "0",
        "TOTK_FIT_SCREEN": "1" if fit else "0",
        "TOTK_DRS": "0",
    }
