"""Pack the translated shaders (<key>.res.json + <key>.spv, ~43,000 files) into one file the game reads them from.

usage: python tools/pack_shaders.py <translated dir> <pack file> [--jobs N]
       python tools/pack_shaders.py --compress <pack file> [--jobs N]     (an older pack written again, compressed)

Format (src/gfx/vkb_shpack.c reads it), little-endian:
  header (32 bytes): "UTSPACK1", version (u32), count (u32), index offset (u64), 0 (u64)
  the blobs: each shader's .res.json, then its .spv
  index: count records of 64 bytes, by key: key (32 bytes, the SHA-256 the key's hex digits spell), .res.json offset
  (u64), .spv offset (u64), .res.json length (u32), .spv length (u32), stamp (u32: the first four bytes of the SHA-256
  of both, never 0 - which translation it is: the driver's compiled binaries are keyed by it), stored .spv length (u32)
Version 2 (2026-11-11): a .spv is one zstd frame of "stored .spv length" bytes; the .spv length is the module's own.
A stored length of 0 means the .spv is stored as it is, as every .spv of version 1 is (the game reads both versions).
The SPIR-V was 3.4 GiB of a 3.44 GiB pack and packs to a quarter; the stamp is of the module's own bytes, so
compressing a pack keeps the driver's compiled binaries. A Python without compression.zstd writes the .spv as it is.
Written to <pack file>.tmp and moved over the old one when complete.
"""
import argparse
import hashlib
import multiprocessing
import os
import pathlib
import struct
import sys

MAGIC = b'UTSPACK1'
RECORD = '<32sQQIIII'
LEVEL = 9   # 25 % of the SPIR-V at 47 MiB/s a process; level 19 gives 19 % at 2.7 MiB/s (600 shaders, 2026-11-11)

try:
    from compression import zstd
except ImportError:
    zstd = None


def stamp_of(json_bytes, spv):
    return struct.unpack('<I', hashlib.sha256(json_bytes + spv).digest()[:4])[0] | 1


def read_index(f):
    """The records of an open pack: [key, json offset, spv offset, json length, spv length, stamp, stored length]."""
    hdr = f.read(32)
    version, count, at = struct.unpack_from('<IIQ', hdr, 8)
    if hdr[:8] != MAGIC or version not in (1, 2):
        raise SystemExit('not a shader pack (or one of a newer version)')
    f.seek(at)
    index = f.read(count * 64)
    return [list(struct.unpack_from(RECORD, index, i * 64)) for i in range(count)]


def read_spv(f, rec):
    """A record's SPIR-V module, unpacked."""
    f.seek(rec[2])
    if not rec[6]:
        return f.read(rec[4])
    if zstd is None:
        raise SystemExit('this Python has no compression.zstd: it cannot read a compressed shader pack')
    return zstd.decompress(f.read(rec[6]))


def _pack_one(spv):
    z = zstd.compress(spv, LEVEL)
    return z if len(z) < len(spv) else b''   # b'': stored as it is


def write_pack(dst, items, count, jobs, replace=True):
    """items: (key bytes, json bytes, spv bytes, stored) in key order; stored None: the module itself, to be compressed
    here, else its bytes as they go into the file (b'': the module as it is). replace=False: the caller moves the
    returned .tmp file over the pack (rewrite: the old pack is still open while the new one is written)."""
    dst = pathlib.Path(dst)
    tmp = dst.with_name(dst.name + '.tmp')
    index = []
    pool = multiprocessing.Pool(max(1, jobs)) if zstd is not None and jobs > 1 else None

    def stored_of(batch):
        todo = [it[2] for it in batch if it[3] is None]
        done = iter(pool.map(_pack_one, todo, chunksize=16) if pool else
                    [_pack_one(s) if zstd is not None else b'' for s in todo])
        return [next(done) if it[3] is None else it[3] for it in batch]

    try:
        with tmp.open('wb') as f:
            f.write(b'\0' * 32)
            batch = []

            def flush():
                for (key, j, spv, _), z in zip(batch, stored_of(batch)):
                    jo = f.tell()
                    f.write(j)
                    so = f.tell()
                    f.write(z or spv)
                    index.append(struct.pack(RECORD, key, jo, so, len(j), len(spv), stamp_of(j, spv), len(z)))
                batch.clear()

            for it in items:
                batch.append(it)
                if len(batch) == 1024:
                    flush()
            flush()
            at = f.tell()
            f.write(b''.join(index))
            f.seek(0)
            f.write(MAGIC + struct.pack('<IIQQ', 2, count, at, 0))
    finally:
        if pool:
            pool.close()
            pool.join()
    if not replace:
        return tmp
    os.replace(tmp, dst)
    return dst.stat().st_size


def rewrite(path, changed=None, jobs=1):
    """The pack written again, compressed: `changed` ({record number: new SPIR-V}) replaces those modules, a module
    stored as it is gets compressed, a compressed one is copied as it is stored."""
    changed = changed or {}
    with open(path, 'rb') as f:
        recs = read_index(f)

        def items():
            for i, r in enumerate(recs):
                f.seek(r[1])
                j = f.read(r[3])
                if i in changed:
                    yield r[0], j, changed[i], None
                elif r[6]:   # compressed already: its bytes are copied; the module itself only for the stamp
                    f.seek(r[2])
                    z = f.read(r[6])
                    yield r[0], j, read_spv(f, r), z
                else:
                    yield r[0], j, read_spv(f, r), None
        tmp = write_pack(path, items(), len(recs), jobs, replace=False)
    os.replace(tmp, path)
    return os.path.getsize(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src', help='the folder of translated shaders, or --compress')
    ap.add_argument('pack')
    ap.add_argument('--compress', action='store_true', dest='again', help='write an existing pack again, compressed')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    argv = sys.argv[1:]
    if argv and argv[0] == '--compress':
        argv = ['-', argv[1], '--compress'] + argv[2:]
    a = ap.parse_args(argv)
    dst = pathlib.Path(a.pack)
    if zstd is None:
        print('this Python has no compression.zstd: the shaders are packed as they are', flush=True)
    if a.again:
        before = dst.stat().st_size
        size = rewrite(dst, jobs=a.jobs)
        print(f'{dst}: {before / 2**20:.0f} -> {size / 2**20:.0f} MiB')
        return
    src = pathlib.Path(a.src)
    keys = sorted(p.name[:-9] for p in src.glob('*.res.json') if len(p.name) == 64 + 9 and (src / (p.name[:-9] + '.spv')).is_file())
    items = ((bytes.fromhex(key), (src / f'{key}.res.json').read_bytes(), (src / f'{key}.spv').read_bytes(), None) for key in keys)
    size = write_pack(dst, items, len(keys), a.jobs)
    print(f'{len(keys)} shaders packed into {dst} ({size / 2**20:.0f} MiB)')


if __name__ == '__main__':
    main()
