"""Fill the BC7 cache from the player's RomFS (tools/bc7build/bc7build.c): every ASTC texture of the TexToGo (.txtg) and
BNTX (.bntx / .bntx.zs: the interface, some effects) files, encoded once.

usage: python tools/bc7_build.py [--romfs data/romfs] [--cache data/textures] [--threads 8] [--quality 2]
                                 [--limit N] [--max-block-area 144]
--max-block-area: ASTC blocks larger than this many texels are left out (decoded when met instead). Blocks larger than
100 texels (the interface's 12x12 maps, 3000x3000) become BC1 when opaque, else BC7 (the user's choice, 2026-10-25).
Runs on --threads logical CPUs at below-normal priority (the rest of the PC stays free), as tools/build_game.py does.
The cache is the player's own derived data: never in git or a build (README.md, Legal).
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64, LINUX_TOOLS, env as tool_env  # noqa: E402
from build_game import cpu_arch, run_limited  # noqa: E402
import progress  # noqa: E402
from romfs import RomFS  # noqa: E402

LINUX = sys.platform == 'linux'
SOURCES_C = ['tools/bc7build/bc7build.c', 'src/gfx/tex/astc.c', 'src/gfx/tex/bc7key.c']
SOURCES_CXX = ['src/gfx/tex/bc7.cpp', 'src/gfx/tex/bc7enc/bc7enc.cpp', 'src/gfx/tex/bc7enc/rgbcx.cpp']


def build_tool(env):
    exe = ROOT / ('build/bc7build' if LINUX else 'build/bc7build.exe')
    newest = max((ROOT / s).stat().st_mtime for s in SOURCES_C + SOURCES_CXX)
    if exe.exists() and exe.stat().st_mtime > newest:
        return exe
    if LINUX:
        cc, cxx_cc = str(LINUX_TOOLS['clang']), str(LINUX_TOOLS['clang++'])
    else:
        cc, cxx_cc = str(CLANG64 / 'clang.exe'), str(CLANG64 / 'clang++.exe')
    objs = []
    arch = cpu_arch()   # as the game: x86-64-v2 on CPUs without AVX2 (it was always v3: illegal instruction there)
    (ROOT / 'build/obj').mkdir(parents=True, exist_ok=True)
    for s in SOURCES_C + SOURCES_CXX:
        cxx = s.endswith('.cpp')
        obj = ROOT / 'build/obj' / ('bc7build_' + s.replace('/', '_').rsplit('.', 1)[0] + '.o')
        cmd = [cxx_cc if cxx else cc, '-O2', f'-march={arch}', '-w', '-c', str(ROOT / s), '-o', str(obj)]
        if cxx:
            cmd += ['-std=c++17', '-fno-exceptions', '-fno-rtti']
        elif LINUX:
            # bc7build.c seeks with the MSVCRT names
            cmd += ['-D_FILE_OFFSET_BITS=64', '-D_fseeki64=fseeko', '-D_ftelli64=ftello']
        subprocess.run(cmd, check=True, env=env)
        objs.append(str(obj))
    if LINUX:
        link = [cxx_cc, '-fuse-ld=lld', '-o', str(exe)] + objs + ['-pthread', '-lzstd']
    else:
        link = [cc, '-fuse-ld=lld', '-o', str(exe)] + objs + ['-static', '-pthread', '-lzstd', '-lc++']
    subprocess.run(link, check=True, env=env)
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs', default='data/romfs')
    ap.add_argument('--cache', default='data/textures')
    ap.add_argument('--threads', type=int, default=8)
    ap.add_argument('--quality', type=int, default=2)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--max-block-area', type=int, default=144)
    a = ap.parse_args()
    rom = RomFS(ROOT / a.romfs)   # a folder of plain files or the development romfs.bin
    files = [dict(path=p, size=n, offset=rom.offset(p)) for p, n in rom.files.items()
             if (p.startswith('TexToGo/') and p.endswith('.txtg')) or p.endswith('.bntx.zs') or p.endswith('.bntx')]
    files.sort(key=lambda f: -f['size'])   # large ones first: the threads finish together
    if a.limit:
        files = files[:a.limit]
    cache = ROOT / a.cache
    cache.mkdir(parents=True, exist_ok=True)
    lst = cache / 'texture-list.txt'
    with open(lst, 'w', encoding='utf-8', newline='\n') as out:
        for f in files:
            out.write(f"{f['offset']} {f['size']} {f['path']}\n")
    env = tool_env()
    exe = build_tool(env)
    print(f'{len(files)} texture files -> {cache}', flush=True)
    t0 = time.time()
    bar = progress.Bar(f're-encoding the textures ({len(files)} files)', sum(f['size'] for f in files), 'MiB', 2**20,
                       rate=True, live=True)

    def line(text):   # bc7build's "<done> / <n> files, <bytes> / <total> bytes, ..." once a second: the progress line
        m = re.match(r'\d+ / \d+ files, (\d+) / (\d+) bytes', text)
        if m:
            bar.update(int(m.group(1)), int(m.group(2)))
        elif text.startswith('done:'):
            bar.update(bar.total, force=True)
            bar.finish()
            print(text, flush=True)
        else:
            bar.note(text.strip())   # a line of its own, the progress line below it ("0:00cache ..." ran together)
    rc = run_limited([str(exe), str(rom.image or rom.root), str(lst), str(cache), str(a.threads), str(a.quality),
                      str(a.max_block_area)], env,
                     (1 << a.threads) - 1, line)
    print(f'{time.time() - t0:.0f} s', flush=True)
    sys.exit(rc)


if __name__ == '__main__':
    main()
