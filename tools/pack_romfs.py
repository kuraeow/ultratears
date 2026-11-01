"""Pack the player's RomFS folder (plain files) into one image, as the development extraction has it: romfs.bin (the files
back to back in path order), index.json and runtime-index.bin (tools/make_romfs_index.py).

usage: python tools/pack_romfs.py <romfs folder> <out dir> [--threads 16]
Why one file: opening a file costs ~5 ms cold on Windows (measured on 304 243 files of TOTK 1.4.3: 17 MiB/s one by one,
110 MiB/s with 16 threads, one image read 446 MiB/s), so the game and the build tools read one image instead of
hundreds of thousands of files. The folder is read with --threads readers, in order, a batch at a time.
Prints the content hash: SHA-256 over the SHA-256 of every file in path order (tools/setup.py compares it with the
version the port is made for). Game-derived data: stays out of git.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from romfs import RomFS  # noqa: E402

BATCH = 2000
ALIGN = 16


def pack(folder, out, threads=16, progress=print, folders=None):
    """-> (file count, bytes, content hash). Writes <out>/romfs.bin, index.json, runtime-index.bin. folders (a dict):
    filled with each top-level folder's (file count, the first 16 hex digits of the SHA-256 over its files' SHA-256), so
    a RomFS that differs from the expected one can say where (tools/setup.py ROMFS_FOLDERS).
    The console shows a progress line (tools/progress.py); progress is kept for callers and no longer used."""
    import progress as bars
    with bars.Ticker('listing the RomFS files'):
        rom = RomFS(folder)
    if rom.image is not None:
        raise ValueError(f'{folder} is already an image')
    names = sorted(rom.files)   # byte order of the paths, as the runtime's index wants
    out.mkdir(parents=True, exist_ok=True)
    rows, total, offset = [], sum(rom.files.values()), 0
    content = hashlib.sha256()
    bar = bars.Bar(f'packing the RomFS ({len(names)} files)', total, 'GiB', 2**30, rate=True)

    def read(p):
        data = rom.read(p)
        return data, hashlib.sha256(data).digest()

    part = out / 'romfs.bin.part'
    with open(part, 'wb') as img, ThreadPoolExecutor(threads) as ex:
        for b in range(0, len(names), BATCH):
            batch = names[b:b + BATCH]
            for p, (data, digest) in zip(batch, ex.map(read, batch)):
                if len(data) != rom.files[p]:
                    raise ValueError(f'{p}: size changed while packing')
                pad = -offset % ALIGN
                if pad:
                    img.write(b'\0' * pad)
                    offset += pad
                rows.append(dict(path=p, offset=offset, size=len(data)))
                img.write(data)
                offset += len(data)
                content.update(digest)
                if folders is not None:
                    top = p.split('/', 1)[0]
                    n, h = folders.get(top) or (0, hashlib.sha256())
                    h.update(digest)
                    folders[top] = (n + 1, h)
                bar.add(len(data))
    bar.finish()
    (out / 'romfs.bin').unlink(missing_ok=True)
    part.rename(out / 'romfs.bin')
    (out / 'index.json').write_text(json.dumps(dict(status='pass', source='folder', content_sha256=content.hexdigest(),
                                                    files=rows)))
    subprocess.run([sys.executable, str(ROOT / 'tools/make_romfs_index.py'), str(out)], check=True,
                   stdout=subprocess.DEVNULL)
    if folders is not None:
        for top, (n, h) in folders.items():
            folders[top] = (n, h.hexdigest()[:16])
    return len(rows), offset, content.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder', type=pathlib.Path)
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('--threads', type=int, default=16)
    a = ap.parse_args()
    t0 = time.time()
    n, size, h = pack(a.folder, a.out, a.threads)
    print(f'{n} files, {size / 2**30:.1f} GiB, content {h}, {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
