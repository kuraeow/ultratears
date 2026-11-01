"""Make the Linux kit: what a Windows PC needs to build UltraTears for Linux / Steam Deck (tools/build_game.py
--platform linux), as one zip: a builder package carries it in deps/linux (tools/make_package.py --linux-kit); a build
from the source makes it from the same pinned downloads (tools/deps_install.py).

From the Steam Runtime 3 "sniper" SDK sysroot (Valve's, glibc 2.31: the game runs on SteamOS and any newer distribution)
only the files a compile and link of the game read: the C and C++ headers, glibc's link files, the GCC runtime and
libstdc++ (linked static), SDL3 (shared, it goes beside the game), Opus and libvpx (static), with their copyright files;
and zstd built static from its source. Nothing of the game is in it. Needs clang for the zstd build and llvm-ar
(CLANG64: deps/clang64 of a source build has both).
usage: python tools/make_linux_kit.py --sniper <sniper sysroot .tar.gz> --zstd <zstd-1.5.7.tar.gz> --out <kit.zip>
"""
import argparse
import fnmatch
import hashlib
import json
import os
import pathlib
import posixpath
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import deps_install  # noqa: E402


def exe(name):
    return name + '.exe' if os.name == 'nt' else name

SNIPER = '3.0.20261021.266025'   # the SDK snapshot the kit is made from (tools/deps.json pins the result)
ZSTD = '1.5.7'
U, L, GCC = 'usr/lib/x86_64-linux-gnu/', 'lib/x86_64-linux-gnu/', 'usr/lib/gcc/x86_64-linux-gnu/10/'
# headers: glibc's top-level ones and whole folders a change of our runtime may reach into
HEADER_DIRS = ('x86_64-linux-gnu', 'c++', 'SDL3', 'linux', 'asm-generic', 'vk_video', 'vulkan', 'vpx', 'opus', 'tbb')
GLIBC = ('c', 'm', 'mvec', 'dl', 'pthread', 'rt', 'util')
LIBS = ([U + n for n in ('crt1.o', 'crti.o', 'crtn.o', 'Scrt1.o', 'libc_nonshared.a', 'libSDL3.so', 'libSDL3.so.0',
                         'libopus.a', 'libvpx.a')] + [U + f'lib{n}.so' for n in GLIBC] +
        [L + f'lib{n}.so.*' for n in GLIBC] + [L + 'libgcc_s.so.1', 'lib64/ld-linux-x86-64.so.2'] +
        [GCC + n for n in ('crtbegin.o', 'crtbeginS.o', 'crtbeginT.o', 'crtend.o', 'crtendS.o', 'libgcc.a',
                           'libgcc_eh.a', 'libgcc_s.so', 'libstdc++.a', 'libsupc++.a')])
LICENSES = ('libsdl3-0', 'libopus0', 'libvpx6', 'libc6', 'gcc-10-base', 'libstdc++6', 'libgcc-s1')


def wanted(name):
    if name.startswith('usr/include/'):
        rest = name[len('usr/include/'):]
        return ('/' not in rest and rest.endswith('.h')) or rest.split('/', 1)[0] in HEADER_DIRS
    if any(fnmatch.fnmatchcase(name, p) for p in LIBS):
        return True
    return name.startswith('usr/share/doc/') and name.endswith('/copyright') and name.split('/')[3] in LICENSES


def norm(name):
    return name[2:] if name.startswith('./') else name.lstrip('/')


def sysroot(tar_path, out):
    """The wanted members of the sniper tarball into out/sysroot; symbolic links become copies of their target
    (Windows makes none), resolved inside the sysroot."""
    links, files = {}, set()
    with tarfile.open(tar_path, 'r:gz') as t:
        for m in t:
            name = norm(m.name)
            if m.issym():
                links[name] = m.linkname
            elif m.islnk():   # a hard link names its target from the archive's root
                links[name] = '/' + norm(m.linkname)
            elif m.isfile() and wanted(name):
                dst = out / 'sysroot' / name
                dst.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, open(dst, 'wb') as f:
                    shutil.copyfileobj(src, f)
                files.add(name)

    def resolve(name, depth=0):
        if depth > 16 or name not in links:
            return name
        target = links[name]
        nxt = target.lstrip('/') if target.startswith('/') else posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
        return resolve(nxt, depth + 1)

    need = {n: resolve(n) for n in links if wanted(n)}
    missing = {r for r in need.values() if r not in files}
    if missing:   # a link's target outside the wanted files (libSDL3.so.0 -> libSDL3.so.0.4.18): a second pass
        with tarfile.open(tar_path, 'r:gz') as t:
            for m in t:
                name = norm(m.name)
                if name in missing and m.isfile():
                    dst = out / 'sysroot' / name
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    with t.extractfile(m) as src, open(dst, 'wb') as f:
                        shutil.copyfileobj(src, f)
                    files.add(name)
    for name, real in need.items():
        if real not in files:
            if name.startswith('usr/include/'):
                continue   # a header link to something not in the kit (none is read)
            sys.exit(f'{name} -> {real}: not in the sysroot')
        dst = out / 'sysroot' / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / 'sysroot' / real, dst)
    # copies of extra-only link targets (libSDL3.so.0.4.18) are not needed once the links are files
    for real in missing:
        if not wanted(real):
            (out / 'sysroot' / real).unlink(missing_ok=True)
    lic = out / 'licenses'
    lic.mkdir()
    for pkg in LICENSES:
        src = out / 'sysroot/usr/share/doc' / pkg / 'copyright'
        if src.is_file():
            shutil.copyfile(src, lic / f'{pkg}.txt')
    shutil.rmtree(out / 'sysroot/usr/share', ignore_errors=True)


def zstd(tar_path, out, work):
    """libzstd.a for x86_64 Linux, built with this PC's clang against the kit's sysroot (single-threaded, no legacy
    formats, no assembly: the C decoder), and its headers."""
    with tarfile.open(tar_path, 'r:gz') as t:
        t.extractall(work, filter='data')
    src = work / f'zstd-{ZSTD}/lib'
    clang64 = deps_install.clang64_for(ROOT)   # asked now: a source build may have just installed it
    if clang64 is None:
        sys.exit('no CLANG64 to build zstd with: run tools/setup.py first')
    bin_ = clang64 / 'bin'
    objs = []
    flags = ['--target=x86_64-linux-gnu', f'--sysroot={out / "sysroot"}', '-O2', '-fPIC', '-DZSTD_LEGACY_SUPPORT=0',
             '-DZSTD_DISABLE_ASM', '-DXXH_NAMESPACE=ZSTD_']
    for sub in ('common', 'compress', 'decompress', 'dictBuilder'):
        for c in sorted((src / sub).glob('*.c')):
            o = work / 'obj' / (c.stem + '.o')
            o.parent.mkdir(exist_ok=True)
            subprocess.run([str(bin_ / exe('clang')), *flags, '-c', str(c), '-o', str(o)], check=True)
            objs.append(str(o))
    (out / 'lib').mkdir()
    (out / 'include').mkdir()
    ar = bin_ / exe('llvm-ar')   # CLANG64 / LLVM of a builder package or a source build has it
    subprocess.run([str(ar), 'rcs', str(out / 'lib/libzstd.a'), *objs], check=True)
    for h in ('zstd.h', 'zstd_errors.h', 'zdict.h'):
        shutil.copyfile(src / h, out / 'include' / h)
    shutil.copyfile(work / f'zstd-{ZSTD}/LICENSE', out / 'licenses/zstd.txt')


def build(sniper_tar, zstd_tar, out_zip):
    """The kit from the two pinned downloads (tools/deps.json "linux_kit_sources") into out_zip."""
    out_zip = pathlib.Path(out_zip).resolve()
    with tempfile.TemporaryDirectory(prefix='ultrs-kit-', dir=out_zip.parent) as tmp:
        kit = pathlib.Path(tmp) / 'kit'
        kit.mkdir()
        print('  the Linux kit: the sysroot files ...', flush=True)
        sysroot(sniper_tar, kit)
        print('  the Linux kit: zstd ...', flush=True)
        zstd(zstd_tar, kit, pathlib.Path(tmp) / 'zstd')
        (kit / 'kit.json').write_text(json.dumps({'sniper': SNIPER, 'zstd': ZSTD}, indent=1) + '\n')
        (kit / 'licenses/README.txt').write_text(
            'The UltraTears Linux kit: files of the Steam Runtime 3 "sniper" SDK sysroot (Valve), unchanged, and zstd\n'
            f'{ZSTD} built static. Their licenses are beside this file (glibc LGPL-2.1+, libstdc++ and libgcc GPL-3.0+ with\n'
            'the GCC Runtime Library Exception, SDL3 zlib, Opus and libvpx BSD, zstd BSD). The source code of the sysroot\n'
            f'snapshot {SNIPER} is published by Valve beside it:\n'
            f'https://repo.steampowered.com/steamrt-images-sniper/snapshots/{SNIPER}/\n'
            f'zstd: https://github.com/facebook/zstd/releases/tag/v{ZSTD}\n')
        part = out_zip.with_suffix('.zip.part')
        with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(kit.rglob('*')):
                if p.is_file():
                    z.write(p, p.relative_to(kit).as_posix())
        part.replace(out_zip)
    return out_zip


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sniper', required=True)
    ap.add_argument('--zstd', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    out_zip = build(a.sniper, a.zstd, a.out)
    h = hashlib.sha256(out_zip.read_bytes()).hexdigest()
    print(f'{out_zip.name}: {out_zip.stat().st_size} bytes, sha256 {h}')


if __name__ == '__main__':
    main()
