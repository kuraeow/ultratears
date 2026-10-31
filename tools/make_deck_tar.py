"""The game for Linux / Steam Deck as one tar, made in the game folder on the PC (tools/build_game.py --platform linux
built linux/ultratears; the shaders for the Deck's AMD GPU are in data/shaders-deck): UltraTears-SteamDeck.tar.

usage: python tools/make_deck_tar.py [--out UltraTears-SteamDeck.tar] [--keep-debug]
Run in the game folder. A tar, not a zip: it keeps the executable bit, which a zip made on Windows loses. Inside, under
UltraTears/: ultratears (debug information stripped unless --keep-debug; the full one stays in linux/ for reading crash
addresses on the PC), libSDL3.so.0, ultratears.cfg for Linux, and data/ - the RomFS, ExeFS, the BC7 texture cache, the
fonts and the Deck's shader pack as data/shaders/translated.pack. Not inside: the generated C, the PC's driver shader
cache (the Deck builds its own at the first start), saves. Everything in it was made on this PC from the player's own
copy of the game, for their own Deck: nothing of it is ever published.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64  # noqa: E402

TOP = 'UltraTears'
# the player's settings that mean the same on the Deck; paths and the PC's display settings are written anew
KEEP = ('lang', 'fps', 'vsync', 'debug', 'sharpness', 'scale', 'fps_overlay', 'snapshots')
DECK_CFG = ['# Written by tools/make_deck_tar.py: UltraTears for Linux / Steam Deck (paths relative to this folder).',
            'exefs=data/exefs', 'romfs=data/romfs', 'shaderpack=data/shaders/translated.pack',
            'compiled=data/shaders/compiled.bin', 'textures=data/textures', 'saves=userdata', 'fonts=data/fonts',
            'window=fullscreen', 'resolution=1280x720']


def deck_cfg():
    lines = list(DECK_CFG)
    cfg = ROOT / 'ultratears.cfg'
    if cfg.is_file():
        for line in cfg.read_text(encoding='utf-8', errors='replace').splitlines():
            key = line.split('=', 1)[0]
            if key in KEEP:
                lines.append(line)
    return ('\n'.join(lines) + '\n').encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='UltraTears-SteamDeck.tar')
    ap.add_argument('--keep-debug', action='store_true')
    a = ap.parse_args()
    game, sdl = ROOT / 'linux/ultratears', ROOT / 'linux/libSDL3.so.0'
    pack = ROOT / 'data/shaders-deck/translated.pack'
    for need in (game, sdl, pack, ROOT / 'data/romfs', ROOT / 'data/exefs'):
        if not need.exists():
            sys.exit(f'missing {need.relative_to(ROOT)}: build the game for Linux and its shaders first')
    exe = game
    if not a.keep_debug:   # about a third of the size without the debug information
        exe = ROOT / 'build-linux/ultratears.stripped'
        if not exe.is_file() or exe.stat().st_mtime < game.stat().st_mtime:
            # the builder package's trimmed CLANG64 has no objcopy; the Linux kit is a developer's, so MSYS2's will do
            objcopy = next((p for p in (CLANG64 / 'llvm-objcopy.exe', pathlib.Path('C:/msys64/clang64/bin/llvm-objcopy.exe'))
                            if p.is_file()), None)
            if objcopy is None:
                sys.exit('no llvm-objcopy to strip the debug information: install MSYS2 CLANG64 or pass --keep-debug')
            subprocess.run([str(objcopy), '--strip-debug', str(game), str(exe)], check=True)
    out = ROOT / a.out
    tmp = out.with_suffix('.tar.part')
    files = 0

    def add(path, name, mode):
        nonlocal files
        info = tarfile.TarInfo(f'{TOP}/{name}')
        st = path.stat()
        info.size, info.mtime, info.mode = st.st_size, int(st.st_mtime), mode
        with open(path, 'rb') as f:
            t.addfile(info, f)
        files += 1

    with tarfile.open(tmp, 'w', format=tarfile.GNU_FORMAT) as t:
        add(exe, 'ultratears', 0o755)
        add(sdl, 'libSDL3.so.0', 0o755)
        info = tarfile.TarInfo(f'{TOP}/ultratears.cfg')
        data = deck_cfg()
        info.size, info.mode = len(data), 0o644
        import io
        t.addfile(info, io.BytesIO(data))
        add(pack, 'data/shaders/translated.pack', 0o644)
        for sub in ('exefs', 'romfs', 'textures', 'fonts'):
            base = ROOT / 'data' / sub
            if not base.is_dir():
                continue
            for dirpath, _, names in os.walk(base):
                for n in sorted(names):
                    p = pathlib.Path(dirpath) / n
                    add(p, p.relative_to(ROOT).as_posix(), 0o644)
            print(f'  data/{sub}: done ({files} files so far)', flush=True)
    os.replace(tmp, out)
    print(f'{out.name}: {files} files, {out.stat().st_size / 2**30:.1f} GiB', flush=True)


if __name__ == '__main__':
    main()
