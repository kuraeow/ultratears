"""What a build for Linux reads of the Linux kit (deps/linux) and of clang's own headers, measured on a game folder that
built the game for Linux (tools/setup.py --platform linux or both: build-linux/): the packages carry only these
(tools/make_package.py trims the kit of the Windows package, tools/make_package_linux.py the kit and LLVM of the Linux
one and of the AppImage), as totk-pc-recomp's make_release_linux.py keeps only the sysroot files its build read.

Measured, not guessed: the headers ninja recorded for every compiled file of the game, the headers of the texture
encoder's sources (tools/bc7_build.py builds it on Linux too; clang -M with the same flags), and what lld reads when it
links the game and the encoder (--verbose). The result is a list of paths: linux/<path in deps/linux> and
clang/include/<header> (clang's resource headers, whatever its version).

usage: python tools/linux_used.py --home ../release-test [--out dev-local/release-inputs/linux-used.txt]
"""
import argparse
import pathlib
import posixpath
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

KIT = re.compile(r'[/\\]deps[/\\]linux[/\\](.+)$', re.I)
RESOURCE = re.compile(r'[/\\]lib[/\\]clang[/\\]\d+[/\\](include[/\\].+)$', re.I)
# kept whatever the measurement says: the licenses, the kit's own record of what it is, and the SDL3 the game's build
# copies beside the game (tools/build_game.py: not read by the compiler or lld)
ALWAYS = ('linux/licenses/', 'linux/README', 'linux/kit.json', 'linux/sysroot/usr/lib/x86_64-linux-gnu/libSDL3.so.0')


def classify(path, used):
    p = path.strip().strip('"')
    m = KIT.search(p)
    if m:   # normalized: libstdc++'s headers come as lib/gcc/x86_64-linux-gnu/10/../../../../include/c++/10/...
        used.add('linux/' + posixpath.normpath(m.group(1).replace('\\', '/')))
        return
    m = RESOURCE.search(p)
    if m:
        used.add('clang/' + posixpath.normpath(m.group(1).replace('\\', '/')))


def tool(name):
    from toolchain import CLANG64
    return str(CLANG64 / (name + ('.exe' if sys.platform == 'win32' else '')))


def measure(home):
    from toolchain import env as tool_env
    import bc7_build
    used = set()
    ninja = home / 'build-linux/build.ninja'
    if not ninja.is_file():
        sys.exit(f'{ninja} is missing: build the game for Linux there first (setup.py --platform linux or both)')
    deps = subprocess.run([tool('ninja'), '-C', str(home), '-f', 'build-linux/build.ninja', '-t', 'deps'],
                          capture_output=True, text=True, env=tool_env()).stdout
    for line in deps.splitlines():
        classify(line, used)
    text = ninja.read_text(encoding='utf-8')
    kit = re.search(r'--sysroot=(\S+)/sysroot', text).group(1)   # the kit this build used
    flags = ['--target=x86_64-linux-gnu', f'--sysroot={kit}/sysroot', f'-I{kit}/include']
    tmp = ROOT / 'dev-local/build/linux-used-probe'
    tmp.mkdir(parents=True, exist_ok=True)
    try:
        # the texture encoder's headers (bc7_build.build_tool's flags for Linux)
        for s in bc7_build.SOURCES_C + bc7_build.SOURCES_CXX:
            cxx = s.endswith('.cpp')
            cmd = [tool('clang++' if cxx else 'clang'), '-M', '-O2', '-march=x86-64-v3', *flags, '-D_FILE_OFFSET_BITS=64',
                   '-D_fseeki64=fseeko', '-D_ftelli64=ftello', str(ROOT / s)]
            if cxx:
                cmd += ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-stdlib=libstdc++']
            out = subprocess.run(cmd, capture_output=True, text=True, env=tool_env())
            if out.returncode:
                sys.exit(f'the header probe of {s} failed:\n{out.stderr[-2000:]}')
            for p in re.split(r'\s+', out.stdout.replace('\\\n', ' ')):
                classify(p, used)
        # the launcher's headers: tools/setup.py builds it in a game folder made from the source (build_launcher_into),
        # which may take its tools from a package (the AppImage's deps/)
        import build_launcher
        import deps_install
        deps_install.launcher_libs(ROOT)
        build_launcher.resources(tmp)   # its version.h
        inc = [f'-I{ROOT / d}' for d in ('deps/launcher/imgui', 'deps/launcher/imgui/backends', 'deps/launcher/nanosvg',
                                          'deps/launcher/stb')] + [f'-I{tmp}', '-DIMGUI_DISABLE_OBSOLETE_FUNCTIONS',
                                                                   '-march=x86-64-v2']
        for s in build_launcher.SOURCES:
            cxx = s.endswith('.cpp')
            cmd = [tool('clang++' if cxx else 'clang'), '-M', *flags, *inc, str(ROOT / s)]
            if cxx:
                cmd += ['-std=c++17', '-stdlib=libstdc++']
            out = subprocess.run(cmd, capture_output=True, text=True, env=tool_env())
            if out.returncode:
                sys.exit(f'the header probe of {s} failed:\n{out.stderr[-2000:]}')
            for p in re.split(r'\s+', out.stdout.replace('\\\n', ' ')):
                classify(p, used)
        # what lld reads linking the game (its link line, the objects of build-linux), the encoder and the launcher
        link = re.search(r'^rule link\n  command = clang (.*) -o \$out @\$out\.rsp (.*)$', text, re.M)
        objs = re.search(r'^build linux/ultratears: link (.*)$', text, re.M).group(1).split()
        probe = tmp / 'probe.cpp'
        probe.write_text('int main() { return 0; }\n')
        rsp = tmp / 'objs.rsp'   # ~1500 objects: past Windows' command-line limit
        rsp.write_text('\n'.join((home / o).as_posix() for o in objs if not o.startswith('|')))
        runs = [[tool('clang'), *link.group(1).replace('$$ORIGIN', '$ORIGIN').split(), '-o', str(tmp / 'game'),
                 '@' + str(rsp), *link.group(2).split()],
                [tool('clang++'), *flags, '-fuse-ld=lld', '--rtlib=libgcc', '--unwindlib=libgcc', '-stdlib=libstdc++',
                 '-static-libstdc++', '-o', str(tmp / 'bc7build'), str(probe), f'-L{kit}/lib', '-l:libzstd.a', '-pthread',
                 '-lm'],
                [tool('clang++'), *flags, '--rtlib=libgcc', '--unwindlib=libgcc', '-no-pie', f'-L{kit}/lib', '-fuse-ld=lld',
                 '-o', str(tmp / 'launcher'), str(probe),
                 *build_launcher.LINUX_LIBS.replace("'$$ORIGIN'", '$ORIGIN').replace('$$ORIGIN', '$ORIGIN').split()]]
        for cmd in runs:
            cmd = [c for c in cmd if c != '-g'] + ['-Wl,--verbose']
            r = subprocess.run(cmd, capture_output=True, text=True, env=tool_env(), cwd=home)
            if r.returncode:
                sys.exit('a probe link failed:\n' + (r.stdout + r.stderr)[-2000:])
            for line in (r.stdout + r.stderr).splitlines():
                classify(line.split(': ')[-1] if line.startswith('ld.lld:') else line, used)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return used


def keep(rel, used):
    """Whether the package keeps a file of the kit (linux/<path>) or of clang's headers (clang/include/<name>)."""
    return rel in used or rel.startswith(ALWAYS)


def load(path):
    """The list linux_used.py wrote."""
    if not path.is_file():
        sys.exit(f'{path} is missing: python tools/linux_used.py --home <a game folder built for Linux> first')
    return set(path.read_text(encoding='utf-8').split())


def trim(top, prefix, used):
    """Removes the files under top (the kit: prefix 'linux/'; clang's resource dir lib/clang/<n>: prefix 'clang/') that
    the build does not read; returns the bytes removed."""
    gone = 0
    for p in sorted(top.rglob('*'), reverse=True):
        rel = prefix + p.relative_to(top).as_posix()
        if (p.is_file() or p.is_symlink()) and not keep(rel, used):
            gone += 0 if p.is_symlink() else p.stat().st_size
            p.unlink()
        elif p.is_dir() and not p.is_symlink() and not any(p.iterdir()):
            p.rmdir()
    return gone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--home', type=pathlib.Path, required=True, help='a game folder that built the game for Linux')
    ap.add_argument('--out', type=pathlib.Path, default=ROOT / 'dev-local/release-inputs/linux-used.txt')
    a = ap.parse_args()
    used = measure(a.home.resolve())
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text('\n'.join(sorted(used)) + '\n', encoding='utf-8')
    print(f'{a.out}: {sum(u.startswith("linux/") for u in used)} files of the Linux kit, '
          f'{sum(u.startswith("clang/") for u in used)} of clang\'s headers')


if __name__ == '__main__':
    main()
