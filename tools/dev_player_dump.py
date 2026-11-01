"""Development: lay out the development extraction as a player's dump (a folder with exefs/ and romfs/ of plain files),
to test tools/setup.py and the RomFS folder mode as a player would meet them.

usage: python tools/dev_player_dump.py --src <extraction: exefs/ and romfs/ with romfs.bin> [--out game]
Game-derived data: written to game/ (ignored by git, where tools/setup.py looks for the dump) or --out, never in git.
"""
import argparse
import json
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True)
    ap.add_argument('--out', default=str(ROOT / 'game'))
    a = ap.parse_args()
    src, out = ROOT / a.src, ROOT / a.out
    (out / 'exefs').mkdir(parents=True, exist_ok=True)
    for f in (src / 'exefs').iterdir():
        shutil.copyfile(f, out / 'exefs' / f.name)
    files = json.loads((src / 'romfs/index.json').read_text())['files']
    done = 0
    with open(src / 'romfs/romfs.bin', 'rb') as img:
        for f in files:
            dst = out / 'romfs' / f['path']
            if dst.is_file() and dst.stat().st_size == f['size']:
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            img.seek(f['offset'])
            dst.write_bytes(img.read(f['size']))
            done += 1
            if done % 20000 == 0:
                print(f'{done} files', flush=True)
    print(f'{len(files)} files in {out / "romfs"} ({done} written)')


if __name__ == '__main__':
    main()
