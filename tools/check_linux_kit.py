"""Compile every runtime source of the Linux build against a player's Linux kit (syntax only): a header the port starts
using that the kit does not carry (it holds only what a Linux build read when it was made, tools/linux_used.py) stops a
player's build on Linux and on the Steam Deck, while a developer's full kit builds fine (2026-11-04: <elf.h> in
src/host/profiler.c). Development tool; run before making an AppImage (dev-local/docs/release.md).

usage: python tools/check_linux_kit.py <build folder with build-linux/build.ninja> <the kit: ultrs-linux-kit.zip or a folder>
"""
import concurrent.futures
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    home, kit = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    tmp = None
    if kit.suffix == '.zip':
        tmp = tempfile.mkdtemp()
        zipfile.ZipFile(kit).extractall(tmp)
        kit = pathlib.Path(tmp)
    text = (home / 'build-linux/build.ninja').read_text(encoding='utf-8')
    flags = dict(re.findall(r'^(\w+flags) = (.*)$', text, re.M))
    jobs = re.findall(r'^build build-linux/obj/src_\S+: (cc|gfxcc) (\S+)$', text, re.M)
    clang = shutil.which('clang') or 'C:/msys64/clang64/bin/clang'

    def fix(f):   # the build's flags with the kit's paths (quoted in build.ninja where they have spaces) for this kit's
        args = shlex.split(f.replace('$$', '$'), posix=True)
        args = ['--sysroot=' + kit.as_posix() + '/sysroot' if a.startswith('--sysroot=') else
                '-I' + kit.as_posix() + '/include' if a.startswith('-I') and a.endswith('deps/linux/include') else a for a in args]
        return args + ['-I' + (home / 'build-linux').as_posix()]

    def run(job):
        rule, src = job
        r = subprocess.run([clang] + fix(flags['cflags' if rule == 'cc' else 'gfxflags']) + ['-fsyntax-only', src],
                           capture_output=True, text=True, cwd=ROOT)
        return src, r.returncode, r.stderr

    bad = 0
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for src, rc, err in ex.map(run, jobs):
            if rc:
                bad += 1
                print('FAIL', src)
                print('\n'.join(l for l in err.splitlines() if 'error' in l))
    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f'{len(jobs)} sources, {bad} failed against the kit')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
