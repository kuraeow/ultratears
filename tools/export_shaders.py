"""Export every shader of the game (tools/shaders/inventory.py) for offline translation.

usage: python tools/export_shaders.py <romfs> <out> <exefs>
  romfs: a folder of plain files (the player's dump) or the development extraction (romfs.bin + index.json);
  exefs: the NSO modules (main, subsdk0, sdk): they embed engine shaders too (35 in 1.4.3, among them the title
  screen's full-screen passes).
Writes <out>/raw/<key>.ctl (the 0x880-byte NVN control section) and <key>.bin (the code as the game hands it over),
and <out>/jobs.jsonl: one totk_xlate --serve request per shader for the default bind state (as MacNativeGames'
prepare_shader_seed: graphics code without the 0x30-byte NVN header, c1 = the code's tail from the control's c1 offset).
Game-derived data: stays out of git.
"""
import json
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/lifter'))
from nso import Nso  # noqa: E402
from romfs import RomFS  # noqa: E402
from shaders.inventory import inventory  # noqa: E402


def main():
    romfs, out, exefs = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    modules = {name: bytes(Nso(exefs / name).image) for name in ('main', 'subsdk0', 'sdk')}
    raw = out / 'raw'
    code_dir = out / 'code'
    raw.mkdir(parents=True, exist_ok=True)
    code_dir.mkdir(parents=True, exist_ok=True)
    shaders, startup = inventory(RomFS(romfs), modules)
    with (out / 'jobs.jsonl').open('w') as jobs, (out / 'shaders.tsv').open('w') as table:
        table.write('key\tstage\tstartup\tcode_bytes\tsource\n')
        for key, (stage, control, code, source) in sorted(shaders.items()):
            (raw / f'{key}.ctl').write_bytes(control)
            (raw / f'{key}.bin').write_bytes(code)
            c1, length = struct.unpack_from('<II', control, 0x700)
            path = code_dir / f'{key}.code'
            path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
            job = dict(key=key, stage=stage, code=str(path.resolve()), c1=code[c1:length].hex(), depth_minus_w=True,
                       tex={}, img={}, attr={}, fout={})
            jobs.write(json.dumps(dict(job=job, out=str((out / 'translated').resolve()))) + '\n')
            table.write(f'{key}\t{stage}\t{int(key in startup)}\t{length}\t{source}\n')
    print(f'{len(shaders)} shaders exported to {out}')


if __name__ == '__main__':
    main()
