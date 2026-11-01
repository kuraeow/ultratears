"""Where the build's tools are (tools/deps_install.py chooses and installs them).

CLANG64: the folder with clang, clang++, lld and ninja (and on Windows the static libraries the port links: SDL3, opus,
vpx, zstd, iconv, libc++). Windows: deps/clang64/bin when installed (the builder package's trimmed MSYS2 CLANG64, or
the MSYS2 packages a source build downloads), else ULTRS_CLANG64, else C:/msys64/clang64/bin. Linux: deps/llvm/bin (the
builder package's, or LLVM's release a source build downloads), else ULTRS_LLVM (its bin folder). The libraries of a
Linux build are the Linux kit's (deps/linux). XLATE: the shader translator ultrs-shaders (.exe on Windows; deps/shaders,
built from this source when missing or stale; tools/build_xlate.py).
"""
import os
import pathlib

import deps_install

ROOT = pathlib.Path(__file__).resolve().parent.parent
WINDOWS = os.name == 'nt'


def exe(name):
    """A program's file name on this system: name.exe on Windows."""
    return name + '.exe' if WINDOWS else name


def _clang64():
    prefix = deps_install.clang64_for(ROOT)
    return prefix / 'bin' if prefix else None


CLANG64 = _clang64()
XLATE = next((p for p in (ROOT / 'deps/shaders' / exe('ultrs-shaders'), ROOT / 'dev-local/build/shaders' / exe('ultrs-shaders'))
              if p.is_file()), None)


def env():
    """The environment for the build's commands: CLANG64 first on PATH."""
    if CLANG64 is None:
        raise SystemExit('clang not found: unpack the builder package (deps/clang64 or deps/llvm) or run tools/setup.py, '
                         'which installs it (see BUILDING.md); or set ULTRS_CLANG64 / ULTRS_LLVM to its bin folder')
    return dict(os.environ, PATH=str(CLANG64) + os.pathsep + os.environ.get('PATH', ''))
