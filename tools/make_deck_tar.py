"""The game for Linux / Steam Deck as one tar, made in the game folder on the PC (tools/build_game.py --platform linux
built linux/ultratears; tools/setup.py --platform linux or both runs this as its step deck): UltraTears-SteamDeck.tar.

usage: python tools/make_deck_tar.py [--out UltraTears-SteamDeck.tar] [--pack data/shaders-deck/translated.pack]
                                     [--keep-debug]
Run in the game folder. A tar, not a zip: it keeps the executable bit, which a zip made on Windows loses. Inside, under
UltraTears/: ultratears (debug information stripped unless --keep-debug; the full one stays in linux/ for reading crash
addresses on the PC), libSDL3.so.0, ultratears.cfg for Linux, and data/ - the RomFS, ExeFS, the BC7 texture cache, the
fonts and the shader pack for the Deck's AMD GPU (--pack; default data/shaders-deck, else data/shaders) as
data/shaders/translated.pack, and data/generated - the game's C, so that the AppImage pointed at this folder on the Deck
updates it without lifting the game again (it brings the builder; the tar has none). Not inside: the PC's objects (built
by another compiler, the Deck's build compiles them anew), the PC's driver shader cache (the Deck builds its own at the
first start), saves. Everything in it was made on this PC from the player's own copy of the game, for their own Deck:
nothing of it is ever published.
"""
import argparse
import io
import os
import pathlib
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import progress  # noqa: E402
from toolchain import CLANG64  # noqa: E402

TOP = 'UltraTears'
# the player's settings that mean the same on the Deck; paths and the display settings are the Deck's own (its screen
# 1280x800, 16:10, fullscreen, 40 fps: as the game's Deck defaults, src/host/boot.c deck_defaults), changed in the
# launcher on the Deck
KEEP = ('lang', 'debug', 'fps_overlay', 'snapshots', 'gpu_marks')
DECK_CFG = ['# Written by tools/make_deck_tar.py: UltraTears for Linux / Steam Deck (paths relative to this folder).',
            'exefs=data/exefs', 'romfs=data/romfs', 'shaderpack=data/shaders/translated.pack',
            'compiled=data/shaders/compiled.bin', 'textures=data/textures', 'saves=userdata', 'fonts=data/fonts',
            'window=fullscreen', 'resolution=1280x800', 'fps=40']


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
    ap.add_argument('--pack', help='the shader pack for the Deck (default: data/shaders-deck, else data/shaders)')
    ap.add_argument('--keep-debug', action='store_true')
    a = ap.parse_args()
    game, sdl = ROOT / 'linux/ultratears', ROOT / 'linux/libSDL3.so.0'
    deck = ROOT / 'data/shaders-deck/translated.pack'
    pack = ROOT / a.pack if a.pack else deck if deck.is_file() else ROOT / 'data/shaders/translated.pack'
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

    files = [(exe, 'ultratears', 0o755), (sdl, 'libSDL3.so.0', 0o755), (pack, 'data/shaders/translated.pack', 0o644)]
    # the launcher for Linux (Play and the settings on the Deck; built by tools/build_launcher.py --platform linux, in a
    # builder package as bin-linux/), the build's record and version (so it opens on Play), the licenses
    launcher = next((p for p in (pathlib.Path(os.environ.get('ULTRS_LINUX_LAUNCHER', '')), ROOT / 'bin-linux/ultratears-launcher')
                     if p.name and p.is_file()), None)
    if launcher:
        files.append((launcher, 'ultratears-launcher', 0o755))
    else:
        print('  no launcher for Linux (bin-linux/ultratears-launcher): the Deck starts the game directly', flush=True)
    for name in ('data/setup.json', 'VERSION.txt', 'LICENSE', 'THIRD-PARTY-NOTICES.md'):
        if (ROOT / name).is_file():
            files.append((ROOT / name, name, 0o644))
    if (ROOT / 'licenses').is_dir():
        files += [(p, p.relative_to(ROOT).as_posix(), 0o644) for p in sorted((ROOT / 'licenses').rglob('*')) if p.is_file()]
    for sub in ('exefs', 'romfs', 'textures', 'fonts', 'generated'):
        base = ROOT / 'data' / sub
        for dirpath, _, names in os.walk(base):
            for n in sorted(names):
                p = pathlib.Path(dirpath) / n
                files.append((p, p.relative_to(ROOT).as_posix(), 0o644))
    out = ROOT / a.out
    tmp = out.with_suffix('.tar.part')
    bar = progress.Bar('packing for the Steam Deck', sum(p.stat().st_size for p, _, _ in files), 'GiB', 2**30, rate=True)
    with tarfile.open(tmp, 'w', format=tarfile.GNU_FORMAT) as t:
        data = deck_cfg()
        info = tarfile.TarInfo(f'{TOP}/ultratears.cfg')
        info.size, info.mode = len(data), 0o644
        t.addfile(info, io.BytesIO(data))
        for path, name, mode in files:
            info = tarfile.TarInfo(f'{TOP}/{name}')
            st = path.stat()
            info.size, info.mtime, info.mode = st.st_size, int(st.st_mtime), mode
            with open(path, 'rb') as f:
                t.addfile(info, f)
            bar.add(st.st_size)
    bar.finish()
    os.replace(tmp, out)
    print(f'{out.name}: {len(files) + 1} files, {out.stat().st_size / 2**30:.1f} GiB', flush=True)


if __name__ == '__main__':
    main()
