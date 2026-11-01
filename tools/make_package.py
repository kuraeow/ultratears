"""Make the Windows builder package for a release: release/UltraTears-<version>-windows-x86_64.zip.

usage: python tools/make_package.py [--home build] [--python <Python 3.14>] [--no-zip]   (--no-zip: the folder, no zip)
The version is the tag v<x.y.z> on HEAD (tools/version.py): tag the release commit first.
Run on the developer's machine after a complete build in the game folder --home (tools/setup.py --output: its build/build.ninja
and objects tell which parts of clang64 the build reads) and tools/build_xlate.py (dev-local/build/shaders):
  - the source: the public tree of HEAD (tools/public_tree.py: what git tracks, without the internal notes), so nothing
    made from the game can slip in; without what the player's build does not use (NOT_IN_PACKAGE: the sources of the
    launcher and the translator, which come built, with their licenses in licenses/ and a link to the source in
    THIRD-PARTY-NOTICES.md; developer tools; GitHub's files);
  - deps/clang64: the part of MSYS2 CLANG64 the build uses, found by measuring, not guessing: the headers ninja
    recorded for every compiled file (and those of tools/bc7build), the libraries and objects lld reads when it links
    UltraTears.exe and bc7build.exe (its --verbose "Reading" lines), the programs (clang, clang++, lld, llvm-windres, ninja) and every DLL
    they load (llvm-objdump -p, followed recursively), and the licenses of the MSYS2 packages these come from;
  - deps/python: Python 3.14 (the given install, default the one running this) without its tests, docs and Tk, with
    the packages the build imports (lz4, capstone, unicorn) from this project's .venv;
  - deps/shaders/ultrs-shaders.exe, the shader translator (tools/build_xlate.py);
  - deps/linux: the Linux kit for builds for the Steam Deck (tools/make_linux_kit.py, --linux-kit), only the files such
    a build reads (tools/linux_used.py, --used);
  - deps/linux-package.json: the AppImage of the same release, which the Steam Deck's tar carries (downloaded when a
    build for the Deck needs it): the release is this zip and that AppImage;
  - UltraTears Launcher.exe (tools/build_launcher.py), the window that builds and starts the game; no .bat;
  - VERSION.txt, THIRD-PARTY-NOTICES.md and package-files.txt (every file of the package: what
    tools/apply_update.py replaces and removes when the launcher updates it).
Refuses a dirty working tree: the package must be the commit it names.
"""
import argparse
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
from toolchain import CLANG64, env as tool_env  # noqa: E402
import deps_install  # noqa: E402
import linux_used  # noqa: E402
import public_tree  # noqa: E402
import version  # noqa: E402

PREFIX = CLANG64.parent if CLANG64 else None   # .../clang64
# llvm-objcopy: the game for the Steam Deck without its debug information (tools/make_deck_tar.py); llvm-ar: the Linux
# kit made again from Valve's sysroot when deps/linux was deleted (tools/make_linux_kit.py)
PROGRAMS = ['clang.exe', 'clang++.exe', 'ld.lld.exe', 'lld.exe', 'llvm-windres.exe', 'llvm-rc.exe', 'llvm-objcopy.exe',
            'llvm-ar.exe', 'ninja.exe']
# MSYS2 package (share/licenses/<name>) of each part of clang64 the package carries
LICENSES = ['clang', 'clang-libs', 'compiler-rt', 'crt', 'headers', 'libc++', 'libunwind', 'libwinpthread', 'winpthreads',
            'lld', 'llvm', 'llvm-libs', 'llvm-tools', 'ninja', 'SDL3', 'opus', 'libvpx', 'zstd', 'libiconv', 'zlib', 'xz',
            'libffi', 'libxml2', 'vulkan-headers']
PY_PACKAGES = ['lz4', 'capstone', 'unicorn']
PY_SKIP = {'test', 'idlelib', 'tkinter', 'turtledemo', 'ensurepip', 'lib2to3', '__pycache__'}


NOTICES = '''# Third-party notices (builder package)

UltraTears itself is GPL-3.0-or-later (LICENSE). Its source's third-party code is listed in third_party/README.md.
This package also carries tools to build it; each keeps its license, copied into licenses/:

| Part | What | License | Text |
|---|---|---|---|
| deps/clang64 | Parts of MSYS2 CLANG64: LLVM clang, lld, libc++, libunwind, compiler-rt (Apache-2.0 WITH LLVM-exception); mingw-w64 headers, CRT and winpthreads (ZPL / MIT / public domain); ninja (Apache-2.0); SDL3 (zlib); opus, libvpx, zstd (BSD); libiconv (LGPL-2.1+, linked statically: the objects to relink are made on your PC by the build itself); zlib, xz, libffi, libxml2 (their DLLs, loaded by clang); Vulkan headers (Apache-2.0 / MIT) | as listed | licenses/msys2/<package>/ |
| deps/python | Python 3.14 (PSF License) with lz4 (BSD), capstone (BSD), unicorn (GPL-2.0) | as listed | licenses/Python.txt; each package's *.dist-info in deps/python/Lib/site-packages |
| UltraTears Launcher.exe | The launcher (GPL-3.0-or-later) with SDL3 (zlib), Dear ImGui (MIT), nanosvg (zlib), stb_image (MIT), the fonts Inter and Montserrat Alternates (SIL OFL 1.1), libc++ (Apache-2.0 WITH LLVM-exception) | as listed | licenses/msys2/SDL3, licenses/imgui.txt, licenses/nanosvg.txt, licenses/stb.txt, licenses/OFL-Inter.txt, licenses/OFL-MontserratAlternates.txt |
| bin-linux/ultratears-launcher | The launcher for Linux / Steam Deck, put into the Deck's tar (GPL-3.0-or-later) with Dear ImGui (MIT), nanosvg (zlib), stb_image (MIT), the fonts (SIL OFL 1.1), libstdc++ (GPL-3.0+ with the GCC Runtime Library Exception); SDL3 (zlib) beside it in the tar | as listed | licenses/imgui.txt, licenses/nanosvg.txt, licenses/stb.txt, licenses/OFL-*.txt |
| deps/linux | The Linux kit for builds for the Steam Deck (tools/make_linux_kit.py): files of the Steam Runtime 3 "sniper" SDK sysroot (Valve), unchanged: glibc (LGPL-2.1+), libstdc++ and libgcc (GPL-3.0+ with the GCC Runtime Library Exception), SDL3 (zlib), Opus and libvpx (BSD); zstd (BSD) built static | as listed | licenses/linux-kit/ (with where their source is) |
| deps/shaders | ultrs-shaders: totk_xlate (MacNativeGames, MIT) around the Ryujinx 1.3.2 shader translator (MIT) with this project's patches, published self-contained with the .NET runtime (MIT) | MIT | licenses/Ryujinx.txt, licenses/totk_xlate.txt |

Source code. The launcher and the shader translator come built; their source, like all of this project's, is at
https://github.com/kuraeow/ultratears/tree/{commit} (src/launcher, third_party/shader_translator,
third_party/maxwell_shader; tools/build_launcher.py and tools/build_xlate.py build them). Others: MSYS2 packages
https://packages.msys2.org (CLANG64), Python https://www.python.org, unicorn https://github.com/unicorn-engine/unicorn,
capstone https://github.com/capstone-engine/capstone, Ryujinx https://github.com/Ryubing, MacNativeGames
(third_party/README.md).
'''

# The source tree in the package is what the build on the player's PC uses: what only builds the parts that come built
# (the launcher, the shader translator), the developers' tools and GitHub's files stay in the repository.
NOT_IN_PACKAGE = ['src/launcher', 'assets/launcher',
                  'assets/cover.png', 'third_party/maxwell_shader', 'third_party/shader_translator', 'tools/build_xlate.py',
                  'tools/build_launcher.py', 'tools/make_package.py', 'tools/eden_xlate', 'tools/run_eden_xlate.py',
                  'tools/compare_spirv_opt.py', 'tools/validate_spirv.py', 'tools/gen_nvn_thunks.py', 'tools/recon_code.py',
                  'tools/recon_romfs.py', 'tools/dev_glsl_tree.py', 'tools/dev_lum.py', 'tools/dev_player_dump.py',
                  'tools/dev_timeline.py', '.github', '.gitignore', '.gitattributes', 'CONTRIBUTING.md', 'BUILDING.md']
# licenses of what is compiled into the launcher and the translator, kept when their sources are left out
LICENSE_COPIES = {'deps/launcher/imgui/LICENSE.txt': 'imgui.txt', 'deps/launcher/nanosvg/LICENSE.txt': 'nanosvg.txt',
                  'deps/launcher/stb/LICENSE': 'stb.txt', 'assets/launcher/fonts/OFL-Inter.txt': 'OFL-Inter.txt',
                  'assets/launcher/fonts/OFL-MontserratAlternates.txt': 'OFL-MontserratAlternates.txt',
                  'third_party/shader_translator/LICENSE': 'totk_xlate.txt'}


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args), check=True, capture_output=True, text=True).stdout


def clang64_files_used(home):
    """Headers, libraries and objects of clang64 that the build reads, measured."""
    used = set()
    deps = subprocess.run([str(CLANG64 / 'ninja.exe'), '-C', str(home), '-f', 'build/build.ninja', '-t', 'deps'],
                          capture_output=True, text=True, env=tool_env()).stdout
    # the game folder's build may have used its own deps/clang64 (a previous package): the same files of PREFIX
    prefixes = [str(PREFIX).replace('\\', '/').lower(), str(home / 'deps/clang64').replace('\\', '/').lower()]
    for line in deps.splitlines():
        p = line.strip().replace('\\', '/')
        for pre in prefixes:
            if p.lower().startswith(pre + '/'):
                used.add(PREFIX / p[len(pre) + 1:])
    tmp = ROOT / 'dev-local/build/package-probe'
    tmp.mkdir(parents=True, exist_ok=True)
    for src, cxx in (('tools/bc7build/bc7build.c', False), ('src/gfx/tex/astc.c', False), ('src/gfx/tex/bc7.cpp', True),
                     ('src/gfx/tex/bc7enc/bc7enc.cpp', True), ('src/gfx/tex/bc7enc/rgbcx.cpp', True)):
        cmd = [str(CLANG64 / ('clang++.exe' if cxx else 'clang.exe')), '-M', str(ROOT / src)]
        if cxx:
            cmd.append('-std=c++17')
        out = subprocess.run(cmd, capture_output=True, text=True, env=tool_env()).stdout
        for p in re.split(r'\s+', out.replace('\\\n', ' ')):
            p = p.replace('\\', '/')
            if p.lower().startswith(str(PREFIX).replace('\\', '/').lower()):
                used.add(pathlib.Path(p))
    # the launcher's headers: tools/setup.py builds it in a game folder made from the source (build_launcher_into), and
    # such a folder may be one unpacked from this package, with only these tools
    import build_launcher
    deps_install.launcher_libs(ROOT)
    build_launcher.resources(tmp)   # its version.h
    inc = [f'-I{ROOT / d}' for d in ('deps/launcher/imgui', 'deps/launcher/imgui/backends', 'deps/launcher/nanosvg',
                                      'deps/launcher/stb')] + [f'-I{tmp}', '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS', '-DUNICODE',
                                                               '-D_UNICODE', '-march=x86-64-v2']
    for src in build_launcher.SOURCES:
        cxx = src.endswith('.cpp')
        cmd = [str(CLANG64 / ('clang++.exe' if cxx else 'clang.exe')), '-M', *inc, str(ROOT / src)] + (['-std=c++17'] if cxx else [])
        r = subprocess.run(cmd, capture_output=True, text=True, env=tool_env())
        if r.returncode:
            sys.exit(f'the launcher header probe of {src} failed:\n{r.stderr[-2000:]}')
        for p in re.split(r'\s+', r.stdout.replace('\\\n', ' ')):
            p = p.replace('\\', '/')
            if p.lower().startswith(str(PREFIX).replace('\\', '/').lower()):
                used.add(pathlib.Path(p))
    # libraries: what lld reads in the links of the game, the texture encoder and the launcher (--verbose)
    ninja = (home / 'build/build.ninja').read_text()
    rsp = re.search(r'^build UltraTears\.exe: link (.*)$', ninja, re.M).group(1).split()   # ninja deletes the .rsp
    link = re.search(r'command = clang (.*) -o \$out @\$out.rsp (.*)', ninja)
    flags = link.group(1).split() + link.group(2).split()
    for objs, libs in ((rsp, flags), ([], ['-static', '-pthread', '-lzstd', '-lc++']), ([], build_launcher.LIBS.split())):
        probe = tmp / 'probe.c'
        probe.write_text('int main(void) { return 0; }\n')
        cmd = [str(CLANG64 / 'clang.exe'), '-fuse-ld=lld', '-Wl,--verbose', '-o', str(tmp / 'probe.exe')]
        rsp_file = tmp / 'probe.rsp'   # 1500 objects: past Windows' command-line limit
        rsp_file.write_text('\n'.join((home / o).as_posix() for o in objs) if objs else probe.as_posix())
        cmd += ['@' + str(rsp_file)] + [f for f in libs if f not in ('-g',)]
        cmd = [c for c in cmd if not c.startswith('-Wl,--pdb')]
        r = subprocess.run(cmd, capture_output=True, text=True, env=tool_env(), cwd=home)
        if r.returncode:
            sys.exit('the probe link failed:\n' + (r.stdout + r.stderr)[-2000:])
        for line in (r.stdout + r.stderr).splitlines():
            m = re.match(r'ld\.lld: Reading (.+)$', line.strip())
            if not m:
                continue
            p = m.group(1).replace('\\', '/')
            if p.lower().startswith(str(PREFIX).replace('\\', '/').lower()) and os.path.isfile(p):
                used.add(pathlib.Path(p))
    shutil.rmtree(tmp, ignore_errors=True)
    return used


def dll_closure(programs):
    seen, todo = set(), [CLANG64 / p for p in programs]
    while todo:
        f = todo.pop()
        if f in seen or not f.is_file():
            continue
        seen.add(f)
        out = subprocess.run([str(CLANG64 / 'llvm-objdump.exe'), '-p', str(f)], capture_output=True, text=True).stdout
        for dll in re.findall(r'DLL Name: (\S+)', out):
            cand = CLANG64 / dll
            if cand.is_file():
                todo.append(cand)
    return seen


def copy_python(src, dst):
    for item in src.iterdir():
        if item.name in ('python.exe', 'pythonw.exe', 'python3.dll', 'python314.dll', 'vcruntime140.dll',
                         'vcruntime140_1.dll', 'LICENSE.txt'):
            shutil.copy2(item, dst / item.name)
    shutil.copytree(src / 'DLLs', dst / 'DLLs', ignore=shutil.ignore_patterns('_tkinter*', 'tcl*', 'tk*'))
    shutil.copytree(src / 'Lib', dst / 'Lib', ignore=lambda d, names: [n for n in names if n in PY_SKIP or n == 'site-packages'])
    site = dst / 'Lib/site-packages'
    site.mkdir(parents=True, exist_ok=True)
    venv_site = ROOT / '.venv/Lib/site-packages'
    for name in os.listdir(venv_site):
        base = name.split('-')[0].split('.')[0]
        if base.lower() in {p.lower() for p in PY_PACKAGES} or (name.endswith('.dist-info') and
                                                               name.split('-')[0].lower() in ('lz4', 'capstone', 'unicorn')):
            s = venv_site / name
            # without the import libraries and C headers (unicorn.lib alone is 50 MB): Python loads the DLLs
            (shutil.copytree(s, site / name, ignore=shutil.ignore_patterns('__pycache__', '*.lib', 'include')) if s.is_dir()
             else shutil.copy2(s, site / name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--python', type=pathlib.Path, default=pathlib.Path(sys.base_prefix))
    ap.add_argument('--no-zip', action='store_true')
    ap.add_argument('--home', type=pathlib.Path, default=ROOT / 'build')   # tools/setup.py's default output
    ap.add_argument('--linux-kit', type=pathlib.Path, default=ROOT / 'dev-local/release-inputs/ultrs-linux-kit.zip',
                    help='the Linux kit (tools/make_linux_kit.py) the package carries in deps/linux')
    ap.add_argument('--used', type=pathlib.Path, default=ROOT / 'dev-local/release-inputs/linux-used.txt',
                    help='what a build for Linux reads of the Linux kit (tools/linux_used.py)')
    ap.add_argument('--linux-package', type=pathlib.Path, default=ROOT / 'release',
                    help="the folder with this release's AppImage (tools/make_package_linux.py): named with its hash in "
                         "deps/linux-package.json, for the Steam Deck's tar")
    ap.add_argument('--test', action='store_true',
                    help='a package of an untagged commit, named after git describe: for testing, never for Releases')
    a = ap.parse_args()
    if git('status', '--porcelain', '--untracked-files=no').strip():
        sys.exit('the working tree has uncommitted changes: commit first, the package must be the commit it names')
    a.version = version.current()[0] + '-test' if a.test else version.release()
    if not a.version:
        sys.exit('HEAD has no release tag: git tag v<x.y.z> (the package is named after it)')
    home, xlate = a.home.resolve(), ROOT / 'dev-local/build/shaders/ultrs-shaders.exe'
    # the release's AppImage is UltraTears.AppImage, the name it has in a game folder; a test one carries its version
    linux_files = {'appimage': a.linux_package / (f'UltraTears-{a.version}-x86_64.AppImage' if a.test else 'UltraTears.AppImage')}
    for need in (home / 'UltraTears.exe', home / 'build/build.ninja', xlate, a.linux_kit, a.used, *linux_files.values()):
        if not need.is_file():
            sys.exit(f'{need} is missing: build first (tools/setup.py --output, tools/build_xlate.py, tools/make_linux_kit.py)')
    name = f'UltraTears-{a.version}-windows-x86_64'
    out = ROOT / 'release' / name
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    # the source: the public tree of HEAD
    tar = subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar', public_tree.tree()], check=True,
                         capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(out, filter='data')
    commit = git('rev-parse', '--short', 'HEAD').strip()
    lic = out / 'licenses'
    lic.mkdir()
    deps_install.launcher_libs(ROOT)   # their licenses come from deps/launcher
    for src, dst in LICENSE_COPIES.items():
        shutil.copy2(ROOT / src, lic / dst)
    for p in NOT_IN_PACKAGE:
        p = out / p
        if p.is_dir():
            shutil.rmtree(p)
        elif p.is_file():
            p.unlink()
    # deps/clang64
    used = clang64_files_used(home) | dll_closure(PROGRAMS)
    for f in sorted(used):
        rel = f.relative_to(PREFIX) if f.is_absolute() else f
        dst = out / 'deps/clang64' / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dst)
    for pkg in LICENSES:
        d = PREFIX / 'share/licenses' / pkg
        if d.is_dir():
            shutil.copytree(d, lic / 'msys2' / pkg)
    # deps/python, deps/shaders
    py = out / 'deps/python'
    py.mkdir(parents=True)
    copy_python(a.python, py)
    (out / 'deps/shaders').mkdir(parents=True)
    shutil.copy2(xlate, out / 'deps/shaders/ultrs-shaders.exe')
    stamp = xlate.parent / 'source.txt'   # the translator must be made from this commit's sources
    if not stamp.is_file() or stamp.read_text().strip() != deps_install.xlate_stamp(ROOT):
        sys.exit(f'{xlate} was not built from this source: run tools/build_xlate.py again')
    shutil.copy2(stamp, out / 'deps/shaders/source.txt')
    # deps/linux: the Linux kit, for a build for the Steam Deck (setup.py --platform linux / both), only the files such
    # a build reads (tools/linux_used.py), so no download of its own
    with zipfile.ZipFile(a.linux_kit) as z:
        z.extractall(out / 'deps/linux')
    shutil.copytree(out / 'deps/linux/licenses', lic / 'linux-kit')
    linux_used.trim(out / 'deps/linux', 'linux/', linux_used.load(a.used))
    # the package's own tools: a build from it installs nothing (tools/deps_install.py)
    (out / 'deps/installed.json').write_text(json.dumps({'python': 'package', 'clang64': 'package', 'shaders': 'package',
                                                         'linux_kit': 'package'}, indent=1))
    shutil.copy2(ROOT / 'third_party/maxwell_shader/LICENSE.txt', lic / 'Ryujinx.txt')
    shutil.copy2(a.python / 'LICENSE.txt', lic / 'Python.txt')
    (out / 'THIRD-PARTY-NOTICES.md').write_text(NOTICES.replace('{commit}', git('rev-parse', 'HEAD').strip()))
    (out / 'VERSION.txt').write_text(version.version_txt(a.version, commit))
    # the AppImage of this release, which the Steam Deck's tar carries (deps_install.linux_package): made first from the
    # same commit (tools/make_package_linux.py), published beside this zip; the release has these two files only
    pins = {}
    for key, f in linux_files.items():
        pins[key] = {'name': f.name, 'url': f'https://github.com/kuraeow/ultratears/releases/download/v{a.version}/{f.name}',
                     'size': f.stat().st_size, 'sha256': deps_install.sha256_file(f)}
    (out / deps_install.LINUX_PACKAGE).write_text(json.dumps(pins, indent=1), encoding='utf-8')
    # the launcher, built from this commit (it holds no code of the game: shipped built)
    launcher = ROOT / 'dev-local/build/launcher-release'
    subprocess.run([sys.executable, str(ROOT / 'tools/build_launcher.py'), '--out', str(launcher)], check=True)
    shutil.copy2(launcher / 'UltraTears Launcher.exe', out / 'UltraTears Launcher.exe')
    files = sorted(f.relative_to(out).as_posix() for f in out.rglob('*') if f.is_file()) + ['package-files.txt']
    (out / 'package-files.txt').write_text('\n'.join(sorted(files)) + '\n', encoding='utf-8')
    size = sum(f.stat().st_size for f in out.rglob('*') if f.is_file())
    print(f'{out}: {size / 2**20:.0f} MiB, {len(used)} files of clang64')
    if not a.no_zip:
        z = out.parent / (name + '.zip')
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for f in sorted(out.rglob('*')):
                if f.is_file():
                    zf.write(f, pathlib.Path(name) / f.relative_to(out))
        print(f'{z}: {z.stat().st_size / 2**20:.0f} MiB')
        shutil.rmtree(out)   # the zip is the package; --no-zip keeps the folder instead (checks without unpacking)


if __name__ == '__main__':
    main()
