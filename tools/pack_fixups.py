"""Fixes of the translated shaders, made in a shader pack in place (tools/setup.py, step "shaders", after packing).

usage: python tools/pack_fixups.py <pack file> <data folder (romfs, exefs)> [--jobs N]

A shader a fix changes gets the stamp of its new contents (as tools/pack_shaders.py makes it), so the driver's compiled
binaries (data/shaders/compiled.bin) are made again for it alone. Running it again changes nothing. A pack whose
modules are stored as they are (version 1) is written again compressed on the way (pack_shaders.rewrite).

Compute thread groups (2026-11-07): the translation jobs (tools/export_shaders.py) gave no thread-group size, so every
compute shader was translated with LocalSize 1 1 1, while the game dispatches its groups for the size in the NVN
program's control section (+0x748: x, y, z): 247 of TotK's 257 compute shaders run
groups of 32, 64, 16x16 or 8x8 threads. Each group ran its first thread only. The effect simulation moved one particle
in 32 and left the rest where they were born with their first velocity: bomb explosions, gloom, sweat and sparks drew
needles stretched along that velocity (2026-11-07, the user's Switch screenshots against ours). Each compute shader's
OpExecutionMode LocalSize gets the size of its control section. The game's code computes its indices from the group
and thread ids with the size built in, and Maxwell has no other read of it, so the size is all that changes.
"""
import json
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/lifter'))

OP_EXECUTION_MODE, MODE_LOCAL_SIZE = 16, 17


def local_sizes(data):
    """{key: (x, y, z)} of the game's compute shaders, from their NVN control sections."""
    from nso import Nso
    from romfs import RomFS
    from shaders.inventory import inventory
    modules = {name: bytes(Nso(data / 'exefs' / name).image) for name in ('main', 'subsdk0', 'sdk')}
    shaders, _ = inventory(RomFS(data / 'romfs'), modules)
    return {key: struct.unpack_from('<III', control, 0x748)
            for key, (stage, control, _code, _src) in shaders.items() if stage == 'cs'}


def set_local_size(w, size):
    """w: the module's words (a list, changed in place). Returns 1 when its LocalSize changed."""
    p = 5
    while p < len(w):
        op, wc = w[p] & 0xffff, w[p] >> 16
        if not wc:
            return 0
        if op == OP_EXECUTION_MODE and wc == 6 and w[p + 2] == MODE_LOCAL_SIZE:
            if tuple(w[p + 3:p + 6]) == tuple(size):
                return 0
            w[p + 3:p + 6] = list(size)
            return 1
        p += wc
    return 0


def main():
    import pack_shaders
    path, data = sys.argv[1], pathlib.Path(sys.argv[2])
    jobs = int(sys.argv[sys.argv.index('--jobs') + 1]) if '--jobs' in sys.argv else os.cpu_count() or 1
    sizes = local_sizes(data)
    changed, unknown, raw = {}, 0, 0
    with open(path, 'rb') as f:
        recs = pack_shaders.read_index(f)
        for i, (key, jo, so, jl, sl, stamp, stored) in enumerate(recs):
            raw += not stored
            f.seek(jo)
            if json.loads(f.read(jl)).get('stage') != 'cs':
                continue
            size = sizes.get(key.hex())
            if not size or not all(size):
                unknown += 1
                continue
            w = list(struct.unpack(f'<{sl // 4}I', pack_shaders.read_spv(f, recs[i])))
            if set_local_size(w, size):
                changed[i] = struct.pack(f'<{sl // 4}I', *w)
    # The pack is written again when a module changed (its stamp follows its new contents) or when it still holds
    # modules stored as they are (a pack of version 1: compressed here).
    if changed or (raw and pack_shaders.zstd is not None):
        pack_shaders.rewrite(path, changed, jobs)
    print(f'{path}: {len(changed)} compute shaders given their thread-group size'
          + (f' ({unknown} without a size of their own)' if unknown else '')
          + f', {os.path.getsize(path) / 2**20:.0f} MiB')


if __name__ == '__main__':
    main()
