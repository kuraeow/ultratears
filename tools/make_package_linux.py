"""Make the Linux builder package and the AppImage for a release (run on Linux: WSL or a Linux PC):
dev-local/dist/UltraTears-builder-<version>-linux-x86_64.tar.gz and UltraTears-<version>-x86_64.AppImage.

usage: python3 tools/make_package_linux.py --xlate <linux ultrs-shaders> --launcher <linux ultratears-launcher>
                                           [--linux-kit dev-local/dist/ultrs-linux-kit.zip] [--test] [--no-appimage]
The same as the Windows builder package (tools/make_package.py) with the Linux tools: the source (the public tree of
HEAD, without NOT_IN_PACKAGE), deps/python (python-build-standalone with the build's wheels) and deps/llvm (clang, lld,
llvm-ar, llvm-objcopy, the clang resource headers, ninja) from the pins of tools/deps.json "linux_host"
(tools/deps_install.py installs them here as on a source build; their debug symbols stripped), deps/linux (the Linux
kit: the sysroot every Linux build compiles against), deps/shaders/ultrs-shaders (tools/build_xlate.py --rid
linux-x64), ultratears-launcher with libSDL3.so.0 beside it. Every ELF file is checked against glibc 2.34, the floor the
official LLVM binaries set (as totk-pc-recomp's make_release_linux.py check_glibc). The tar keeps the executable bits
and the links; the Windows builder downloads it for a Steam Deck's tar, the Linux launcher for its updates.

The AppImage: the same folder with AppRun (the launcher) and a desktop entry, packed by appimagetool with the static
type-2 runtime (no libfuse2 on the host), both pinned by hash as totk-pc-recomp pins them. Started, the launcher copies
the builder into the folder the player picks (tools/install_to.py) and builds the game there.
"""
import argparse
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import deps_install  # noqa: E402
import public_tree  # noqa: E402
import version  # noqa: E402
from make_package import NOT_IN_PACKAGE, LICENSE_COPIES, git  # noqa: E402

GLIBC_FLOOR = (2, 34)
APPIMAGETOOL = {   # from release tags, pinned by hash: "continuous" uploads change under the same name (totk-pc-recomp's pins)
    'tool': {'url': 'https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage',
             'sha256': 'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0'},
    'runtime': {'url': 'https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64',
                'sha256': '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'},
}
NOTICES = '''# Third-party notices (Linux builder package)

UltraTears itself is GPL-3.0-or-later (LICENSE). Its source's third-party code is listed in third_party/README.md.
This package also carries tools to build it; each keeps its license, copied into licenses/:

| Part | What | License | Text |
|---|---|---|---|
| deps/python | Python 3.14 (python-build-standalone; PSF License and the licenses of what it is built with) with lz4 (BSD), capstone (BSD), unicorn (GPL-2.0) | as listed | deps/python/share, each package's *.dist-info in its site-packages |
| deps/llvm | LLVM: clang, lld, llvm-ar, llvm-objcopy, the clang resource headers (Apache-2.0 WITH LLVM-exception); ninja (Apache-2.0) | as listed | deps/llvm/LICENSE.TXT |
| deps/linux | The Linux kit: files of the Steam Runtime 3 "sniper" SDK sysroot (Valve), unchanged: glibc (LGPL-2.1+), libstdc++ and libgcc (GPL-3.0+ with the GCC Runtime Library Exception), SDL3 (zlib), Opus and libvpx (BSD); zstd (BSD) built static | as listed | licenses/linux-kit/ (with where their source is) |
| ultratears-launcher, libSDL3.so.0 | The launcher (GPL-3.0-or-later) with Dear ImGui (MIT), nanosvg (zlib), stb_image (MIT), the fonts Inter and Montserrat Alternates (SIL OFL 1.1), libstdc++ (GPL-3.0+ with the GCC Runtime Library Exception); SDL3 (zlib) | as listed | licenses/imgui.txt, licenses/nanosvg.txt, licenses/stb.txt, licenses/OFL-*.txt, licenses/linux-kit/ |
| deps/shaders | ultrs-shaders: totk_xlate (MacNativeGames, MIT) around the Ryujinx 1.3.2 shader translator (MIT) with this project's patches, published self-contained with the .NET runtime (MIT) | MIT | licenses/Ryujinx.txt, licenses/totk_xlate.txt |

Source code. The launcher and the shader translator come built; their source, like all of this project's, is at
https://github.com/kuraeow/ultratears/tree/{commit}. Others: LLVM https://github.com/llvm/llvm-project, ninja
https://github.com/ninja-build/ninja, python-build-standalone https://github.com/astral-sh/python-build-standalone,
unicorn https://github.com/unicorn-engine/unicorn, capstone https://github.com/capstone-engine/capstone, Ryujinx
https://github.com/Ryubing, MacNativeGames (third_party/README.md), the Steam Runtime (licenses/linux-kit/README.txt).
'''
APPRUN = '''#!/bin/sh
# UltraTears: the launcher of this AppImage (it copies the builder into the folder the player picks and builds there)
HERE="$(dirname "$(readlink -f "$0")")"
exec "$HERE/ultratears-launcher" "$@"
'''
DESKTOP = '''[Desktop Entry]
Type=Application
Name=UltraTears
Comment=Builds a native Linux / Steam Deck program from your own copy of the game
Exec=ultratears-launcher
Icon=ultratears
Categories=Game;
Terminal=false
'''


def elf_glibc(path):
    """The highest GLIBC_x.y an ELF file asks for, or None (not an ELF; none asked)."""
    data = path.read_bytes()
    if data[:4] != b'\x7fELF':
        return None
    found = [tuple(int(x) for x in m.group(1).split(b'.')) for m in re.finditer(rb'GLIBC_(\d+\.\d+(?:\.\d+)?)\x00', data)]
    return max(found) if found else None


def strip_elves(where, tool):
    """The debug symbols of the ELF files under where (LLVM's release ships them: clang ~270 MB)."""
    for p in sorted(where.rglob('*')):
        if p.is_file() and not p.is_symlink() and p.read_bytes()[:4] == b'\x7fELF':
            subprocess.run([str(tool), '--strip-unneeded', str(p)], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlate', type=pathlib.Path, required=True)
    ap.add_argument('--launcher', type=pathlib.Path, required=True)
    ap.add_argument('--linux-kit', type=pathlib.Path, default=ROOT / 'dev-local/dist/ultrs-linux-kit.zip')
    ap.add_argument('--test', action='store_true', help='a package of an untagged commit (for testing, never released)')
    ap.add_argument('--no-appimage', action='store_true')
    a = ap.parse_args()
    if not sys.platform.startswith('linux'):
        sys.exit('make the Linux package on Linux (WSL): the links and modes of the tools must be made there')
    if git('status', '--porcelain', '--untracked-files=no').strip():
        sys.exit('the working tree has uncommitted changes: commit first, the package must be the commit it names')
    ver = version.current()[0] + '-test' if a.test else version.release()
    if not ver:
        sys.exit('HEAD has no release tag: git tag v<x.y.z>')
    for need in (a.xlate, a.launcher, a.linux_kit):
        if not need.is_file():
            sys.exit(f'{need} is missing')
    name = f'UltraTears-builder-{ver}-linux-x86_64'
    dist = ROOT / 'dev-local/dist'
    out = dist / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # the source: the public tree of HEAD, without what the player's build does not use
    tar = subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar', public_tree.tree()], check=True,
                         capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(out, filter='data')
    commit = git('rev-parse', '--short', 'HEAD').strip()
    lic = out / 'licenses'
    lic.mkdir()
    deps_install.launcher_libs(ROOT)
    for src, dst in LICENSE_COPIES.items():
        shutil.copy2(ROOT / src, lic / dst)
    shutil.copy2(ROOT / 'third_party/maxwell_shader/LICENSE.txt', lic / 'Ryujinx.txt')
    for p in NOT_IN_PACKAGE + ['tools/make_package_linux.py']:
        p = out / p
        if p.is_dir():
            shutil.rmtree(p)
        elif p.is_file():
            p.unlink()
    # the tools, from the same pins a source build installs
    deps_install.install_python_linux(out)
    deps_install.install_llvm(out)
    strip_elves(out / 'deps/llvm/bin', out / 'deps/llvm/bin/llvm-strip')
    with zipfile.ZipFile(a.linux_kit) as z:
        z.extractall(out / 'deps/linux')
    shutil.copytree(out / 'deps/linux/licenses', lic / 'linux-kit')
    (out / 'deps/shaders').mkdir(parents=True)
    shutil.copy2(a.xlate, out / 'deps/shaders/ultrs-shaders')
    (out / 'deps/shaders/ultrs-shaders').chmod(0o755)
    stamp = a.xlate.parent / 'source.txt'
    if not stamp.is_file() or stamp.read_text().strip() != deps_install.xlate_stamp(ROOT):
        sys.exit(f'{a.xlate} was not built from this source: run tools/build_xlate.py --rid linux-x64 again')
    shutil.copy2(stamp, out / 'deps/shaders/source.txt')
    shutil.rmtree(out / 'deps/downloads', ignore_errors=True)
    (out / 'deps/installed.json').write_text(json.dumps({'python': 'package', 'clang64': 'package', 'shaders': 'package',
                                                         'linux_kit': 'package'}, indent=1))
    # the launcher and the SDL3 it loads (rpath $ORIGIN)
    shutil.copy2(a.launcher, out / 'ultratears-launcher')
    subprocess.run([str(out / 'deps/llvm/bin/llvm-strip'), '--strip-debug', str(out / 'ultratears-launcher')], check=True)
    (out / 'ultratears-launcher').chmod(0o755)
    shutil.copy2(out / 'deps/linux/sysroot/usr/lib/x86_64-linux-gnu/libSDL3.so.0', out / 'libSDL3.so.0')
    (out / 'THIRD-PARTY-NOTICES.md').write_text(NOTICES.replace('{commit}', git('rev-parse', 'HEAD').strip()))
    (out / 'VERSION.txt').write_text(version.version_txt(ver, commit))
    # no bytecode: it is Python's cache of where it ran (the paths in it differ), made again where the builder runs
    for d in sorted(out.rglob('__pycache__'), reverse=True):
        shutil.rmtree(d)
    # every program and library runs on glibc 2.34 (SteamOS 3, Ubuntu 22.04, Debian 12, Fedora 35)
    over = []
    for p in sorted(out.rglob('*')):
        if p.is_file() and not p.is_symlink() and 'sysroot' not in p.parts:
            g = elf_glibc(p)
            if g and g > GLIBC_FLOOR:
                over.append(f'{p.relative_to(out)}: GLIBC_{".".join(map(str, g))}')
    if over:
        sys.exit('needs a newer glibc than 2.34:\n  ' + '\n  '.join(over))
    files = sorted(f.relative_to(out).as_posix() for f in out.rglob('*') if f.is_file() or f.is_symlink()) + ['package-files.txt']
    (out / 'package-files.txt').write_text('\n'.join(sorted(files)) + '\n', encoding='utf-8')
    size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file() and not f.is_symlink())
    print(f'{out}: {size / 2**20:.0f} MiB', flush=True)
    tgz = dist / (name + '.tar.gz')
    with tarfile.open(tgz, 'w:gz', compresslevel=6) as t:
        t.add(out, arcname=name)
    print(f'{tgz}: {tgz.stat().st_size / 2**20:.0f} MiB', flush=True)
    if a.no_appimage:
        return
    # the AppImage: the same folder with AppRun, a desktop entry and its icon
    (out / 'AppRun').write_text(APPRUN)
    (out / 'AppRun').chmod(0o755)
    (out / 'ultratears.desktop').write_text(DESKTOP)
    shutil.copy2(ROOT / 'assets/launcher/l_emblem.png', out / 'ultratears.png')
    cache = dist / 'appimage-tools'
    items = [dict(url=v['url'], sha256=v['sha256']) for v in APPIMAGETOOL.values()]
    tool, runtime = deps_install.fetch_all(items, cache, 'appimagetool')
    tool.chmod(0o755)
    image = dist / f'UltraTears-{ver}-x86_64.AppImage'
    env = dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1')   # appimagetool is an AppImage: no FUSE needed
    subprocess.run([str(tool), '--no-appstream', '--runtime-file', str(runtime), str(out), str(image)], check=True, env=env)
    for f in ('AppRun', 'ultratears.desktop', 'ultratears.png'):
        (out / f).unlink()
    print(f'{image}: {image.stat().st_size / 2**20:.0f} MiB', flush=True)


if __name__ == '__main__':
    main()
