"""The Steam Deck's tar, made in the game folder on the PC (tools/setup.py --platform linux or both runs this as its step
deck): UltraTears-SteamDeck.tar, holding UltraTears/ - the game and its data, and the release's AppImage, which is the
builder on the Deck (its launcher copies the source in and links deps/ to its own tools when it builds or updates
there, tools/install_to.py); no builder of its own:

  - what the build makes: linux/ultratears (with its debug information, as linked) and linux/libSDL3.so.0, data/ (the
    ExeFS, the RomFS, the BC7 texture cache, the fonts, the game's C in data/generated, the shader pack for the Deck's
    AMD GPU as data/shaders/translated.pack, data/setup.json as a build on the Deck records it) and ultratears.cfg
    (setup.py cfg_lines, which writes it on the Deck too);
  - UltraTears.AppImage (--appimage), the one program the player starts, as on the Deck (tools/install_to.py copies the
    AppImage into the folder it builds in): the launcher, or with --play the game (Steam's launch option); started
    from a newer release, it updates the folder (src/launcher platform.cpp default_install).

Not inside, as on the Deck before its first update: build-linux/ (the objects; the Deck's own compiler makes them at
its first update, the PC's are another compiler's), the driver's shader cache (made at the first start), saves. A tar,
not a zip: it keeps the modes and the links, which a zip made on Windows loses. Everything made from the game here was
made on this PC from the player's own copy of the game, for their own Deck: nothing of it is ever published.

usage: python tools/make_deck_tar.py --appimage <AppImage> [--pack data/shaders-deck/translated.pack]
                                     [--out UltraTears-SteamDeck.tar]
"""
import argparse
import io
import json
import os
import pathlib
import re
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import progress  # noqa: E402
import setup  # noqa: E402
import version  # noqa: E402

TOP = 'UltraTears'


def deck_state(package):
    """data/setup.json as a build on the Deck records it (an AppImage started in the folder takes it from there): built
    for Linux by the Linux builder `package` (its VERSION.txt line), the shader pack the Deck's (on an NVIDIA PC the one
    made with the AMD gather bias), no PC paths."""
    path = ROOT / 'data/setup.json'
    state = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    if state.get('deck-shaders-pack'):
        state['shaders-pack'] = state['deck-shaders-pack']
    for k in ('deck-shaders-pack', 'deck-tar', 'dump', 'shaders-xlate', 'work-xlate'):
        state.pop(k, None)
    state['platforms'] = ['linux']
    state['package'] = package
    return (json.dumps(state, indent=1) + '\n').encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--appimage', type=pathlib.Path, required=True)
    ap.add_argument('--pack', help='the shader pack for the Deck (default: data/shaders-deck, else data/shaders)')
    ap.add_argument('--out', default=setup.DECK_TAR)
    a = ap.parse_args()
    deck = ROOT / 'data/shaders-deck/translated.pack'
    pack = ROOT / a.pack if a.pack else deck if deck.is_file() else ROOT / 'data/shaders/translated.pack'
    game, sdl = ROOT / 'linux/ultratears', ROOT / 'linux/libSDL3.so.0'
    for need in (game, sdl, pack, ROOT / 'data/romfs', ROOT / 'data/exefs', ROOT / 'data/generated', a.appimage):
        if not need.exists():
            sys.exit(f'missing {need}: build the game for Linux and its shaders first')
    # the AppImage's own release, as its launcher reads it. A release's UltraTears.AppImage is made from the same tag as
    # this folder's package: its VERSION.txt line. A test one carries its version in its name
    # (UltraTears-0.2.0-37-geabddff-test-x86_64.AppImage), the commit in it.
    line = (ROOT / 'VERSION.txt').read_text(encoding='utf-8').strip() if (ROOT / 'VERSION.txt').is_file() else ''
    if a.appimage.name == 'UltraTears.AppImage' and line:
        version_line = line
    else:
        ver = a.appimage.name[len('UltraTears-'):-len('-x86_64.AppImage')]
        m = re.search(r'-g([0-9a-f]{7,})', ver)
        commit = m.group(1) if m else line.rsplit('commit ', 1)[-1].rstrip(')') if 'commit ' in line else version.current()[1]
        version_line = version.version_txt(ver, commit).strip()

    files = [(game, 'linux/ultratears', 0o755), (sdl, 'linux/libSDL3.so.0', 0o644),   # (as build_game.py copies it)
             (pack, 'data/shaders/translated.pack', 0o644), (a.appimage, 'UltraTears.AppImage', 0o755)]
    if setup.SOURCE_DATES.is_file():
        files.append((setup.SOURCE_DATES, 'data/source-dates.json', 0o644))
    for sub in ('exefs', 'romfs', 'textures', 'fonts', 'generated'):
        for dirpath, _, names in os.walk(ROOT / 'data' / sub):
            for n in sorted(names):
                p = pathlib.Path(dirpath) / n
                files.append((p, p.relative_to(ROOT).as_posix(), 0o644))

    total = sum(p.stat().st_size for p, _, _ in files)
    out = ROOT / a.out
    tmp = out.with_suffix('.tar.part')
    bar = progress.Bar('packing for the Steam Deck', total, 'GiB', 2**30, rate=True)
    with tarfile.open(tmp, 'w', format=tarfile.GNU_FORMAT) as t:
        texts = [('ultratears.cfg', ('\n'.join(setup.cfg_lines()) + '\n').encode(), 0o644),
                 ('data/setup.json', deck_state(version_line), 0o644)]
        for name, data, mode in texts:
            info = tarfile.TarInfo(f'{TOP}/{name}')
            info.size, info.mode = len(data), mode
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
    print(f'{out.name}: {version_line}, the game and its data; {out.stat().st_size / 2**30:.1f} GiB', flush=True)


if __name__ == '__main__':
    main()
