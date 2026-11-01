"""Pack the translated shaders (<key>.res.json + <key>.spv, ~57,000 files) into one file the game reads them from.

usage: python tools/pack_shaders.py <translated dir> <pack file>

Format (src/gfx/vkb_shpack.c reads it), little-endian:
  header (32 bytes): "UTSPACK1", version 1 (u32), count (u32), index offset (u64), 0 (u64)
  the blobs: each shader's .res.json, then its .spv
  index: count records of 64 bytes, by key: key (32 bytes, the SHA-256 the key's hex digits spell), .res.json offset
  (u64), .spv offset (u64), .res.json length (u32), .spv length (u32), stamp (u32: the first four bytes of the SHA-256
  of both, never 0 - which translation it is: the driver's compiled binaries are keyed by it), 0 (u32)
Written to <pack file>.tmp and moved over the old one when complete.
"""
import hashlib
import os
import pathlib
import struct
import sys

MAGIC = b'UTSPACK1'


def main():
    src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    keys = sorted(p.name[:-9] for p in src.glob('*.res.json') if len(p.name) == 64 + 9 and (src / (p.name[:-9] + '.spv')).is_file())
    tmp = dst.with_name(dst.name + '.tmp')
    index = []
    with tmp.open('wb') as f:
        f.write(b'\0' * 32)
        for key in keys:
            j = (src / f'{key}.res.json').read_bytes()
            s = (src / f'{key}.spv').read_bytes()
            stamp = struct.unpack('<I', hashlib.sha256(j + s).digest()[:4])[0] | 1
            jo = f.tell()
            f.write(j)
            so = f.tell()
            f.write(s)
            index.append(struct.pack('<32sQQIIII', bytes.fromhex(key), jo, so, len(j), len(s), stamp, 0))
        at = f.tell()
        f.write(b''.join(index))
        f.seek(0)
        f.write(MAGIC + struct.pack('<IIQQ', 1, len(keys), at, 0))
    os.replace(tmp, dst)
    print(f'{len(keys)} shaders packed into {dst} ({dst.stat().st_size / 2**20:.0f} MiB)')


if __name__ == '__main__':
    main()
