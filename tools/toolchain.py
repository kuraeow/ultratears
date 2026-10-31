"""Where the build's tools are (tools/deps_install.py chooses and installs them).

CLANG64: a folder with clang, clang++, lld, ninja and the static libraries the port links (SDL3, opus, vpx, zstd,
iconv, libc++): deps/clang64/bin when installed (the builder package's trimmed MSYS2 CLANG64, or the MSYS2 packages a
source build downloads), else ULTRS_CLANG64, else C:/msys64/clang64/bin. XLATE: the shader translator ultrs-shaders.exe
(deps/shaders, built from this source when missing or stale; tools/build_xlate.py). Linux uses
the system clang, clang++, lld and ninja.
"""
import os
import pathlib
import shutil
import sys

import deps_install

ROOT = pathlib.Path(__file__).resolve().parent.parent


def _linux_tool(name):
    found = shutil.which(name)
    if found:
        return pathlib.Path(found)
    fallback = pathlib.Path('/usr/bin') / name
    return fallback if fallback.is_file() and os.access(fallback, os.X_OK) else None


def _clang64():
    prefix = deps_install.clang64_for(ROOT)
    return prefix / 'bin' if prefix else None


LINUX_TOOLS = {name: _linux_tool(name) for name in ('clang', 'clang++', 'ninja')} if sys.platform == 'linux' else {}
CLANG64 = None if sys.platform == 'linux' else _clang64()
XLATE_CANDIDATES = [
    ROOT / "deps/shaders/ultrs-shaders",
    ROOT / "deps/shaders/ultrs-shaders.exe",
    ROOT / "dev-local/build/shaders/ultrs-shaders",
    ROOT / "dev-local/build/shaders/ultrs-shaders.exe",
]
XLATE = next((p for p in XLATE_CANDIDATES if p.is_file()), None)


def env():
    """The environment for the build's commands: the platform's tool directory first on PATH."""
    if sys.platform == 'linux':
        missing = [name for name, path in LINUX_TOOLS.items() if path is None]
        if missing:
            raise SystemExit('Linux build tools not found: ' + ', '.join(missing) +
                             ' (install clang, lld and ninja or add them to PATH)')
        tool_dirs = dict.fromkeys(str(path.parent) for path in LINUX_TOOLS.values())
        return dict(os.environ, PATH=os.pathsep.join((*tool_dirs, os.environ.get('PATH', ''))))
    if CLANG64 is None:
        raise SystemExit('clang not found: unpack the builder package (deps/clang64) or install MSYS2 CLANG64 '
                         '(see BUILDING.md), or set ULTRS_CLANG64 to its bin folder')
    return dict(os.environ, PATH=str(CLANG64) + os.pathsep + os.environ.get('PATH', ''))
