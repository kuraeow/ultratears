"""A/B check of the lifter's C: guest functions as gen.py emits them with registers and flags in the Cpu, against the
same functions with them in C locals (tools/lifter/regs.py, flags.py), run from the same random states by
tools/lifter/ab_harness.c (which says how). Windows, development: reads the module from your dump's ExeFS and writes
the generated C, the image and the harness into --out (game-derived: dev-local/, never committed).

Picks functions with 6 to 600 instructions that the locals path emits differently, no SVC, exclusive or system-register
access, and no patched site; a quarter of them without calls. Prints, per function, the cases that agreed, differed
and faulted on both sides, and the registers that differed in the first few.

Usage: python tools/lifter/ab_check.py --exefs ../release-test/data/exefs [--module main] [--functions 300]
                                       [--cases 48] [--seed 1] [--out dev-local/tmp/ab]
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen   # noqa: E402
import regs   # noqa: E402
from nso import R_AARCH64_RELATIVE   # noqa: E402
from patches import PATCHES, PRE_PATCHES   # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
BASES = {'main': 0x800000000, 'subsdk0': 0x804000000, 'sdk': 0x805000000}   # tools/gen_game.py MODULES
FN = re.compile(r'\bf_([0-9a-f]+)\b')
FORBID = re.compile(r'rt_svc|ldx\d|stx\d|ldxp|stxp|rt_fpcr|tpidr|cntv|cntp|rt_ticks|excl_')


def image(m, base):
    img = bytearray(m.nso.image)
    for off, typ, _sym, add in m.nso.relocs:
        if typ == R_AARCH64_RELATIVE:
            struct.pack_into('<Q', img, off, base + add)
    return bytes(img)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exefs', required=True)
    ap.add_argument('--module', default='main')
    ap.add_argument('--functions', type=int, default=300)
    ap.add_argument('--cases', type=int, default=48)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--out', default=str(ROOT / 'dev-local/tmp/ab'))
    ap.add_argument('--clang', default='C:/msys64/clang64/bin/clang')
    a = ap.parse_args()
    base = BASES[a.module]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    path = str(Path(a.exefs) / a.module)
    old = gen.Module(a.module, path, base, 'c')
    new = gen.Module(a.module, path, base, 'c')
    new.reg_locals, new.local_flags = regs.LOCALS, True
    entries, _ = gen.partition(old, old.boundaries())
    rnd = random.Random(a.seed)
    order = sorted(entries)
    rnd.shuffle(order)
    picked, nocall = [], 0
    for e in order:
        if len(picked) >= a.functions:
            break
        end, labels, tables = entries[e]
        n = (end - e) // 4
        if not 6 <= n <= 600 or any(v == 'all' for v in tables.values()):
            continue
        if any(x in PATCHES or x in PRE_PATCHES for x in range(e, end, 4)):
            continue
        try:
            o = old.emit_function(e, end, labels, tables, {})
            nl = new.emit_function(e, end, labels, tables, {})
        except Exception:
            continue
        to, tn = '\n'.join(o), '\n'.join(nl)
        if to == tn or FORBID.search(to):
            continue
        calls = 'rt_dispatch(c);' in to or re.search(r' f_[0-9a-f]+\(c\);', to)
        if not calls and nocall >= a.functions // 4:
            continue
        nocall += not calls
        picked.append((e, to, tn))
    print(f'{len(picked)} functions picked ({nocall} without calls)', flush=True)
    refs = set()
    for e, to, tn in picked:
        refs |= set(FN.findall(to)) | set(FN.findall(tn))
    tus = {}
    for tag, k in (('o', 1), ('n', 2)):
        text = ['#include "cpu.h"\n#include "simd.h"\n']
        text += [f'void f_{r}(Cpu *c);\n' for r in sorted(refs)]
        for p in picked:
            t = p[k].replace(f'void f_{p[0]:x}(Cpu *c) {{', f'void {tag}_{p[0]:x}(Cpu *c) {{', 1)
            text.append(t + '\n')
        tus[tag] = ''.join(text)
    (out / 'ab_old.c').write_text(tus['o'])
    (out / 'ab_new.c').write_text(tus['n'])
    stubs = ['#include "cpu.h"\nvoid ab_stub(Cpu *c);\n'] + [f'void f_{r}(Cpu *c) {{ ab_stub(c); }}\n' for r in sorted(refs)]
    stubs.append('typedef struct { uint64_t addr; GuestFn old_fn, new_fn; } ab_fn;\n')
    stubs += [f'void o_{e:x}(Cpu *c); void n_{e:x}(Cpu *c);\n' for e, _, _ in picked]
    stubs.append('const ab_fn ab_fns[] = {\n' + ''.join(f'    {{{e:#x}ull, o_{e:x}, n_{e:x}}},\n' for e, _, _ in picked) + '};\n')
    stubs.append(f'const int ab_nfns = {len(picked)};\n')
    (out / 'ab_table.c').write_text(''.join(stubs))
    (out / f'{a.module}.img').write_bytes(image(old, base))
    exe = out / 'ab.exe'
    cmd = [a.clang, '-O2', '-march=x86-64-v3', '-fno-strict-aliasing', '-w', f'-I{ROOT / "src/runtime"}', '-o', str(exe),
           str(ROOT / 'tools/lifter/ab_harness.c'), str(out / 'ab_old.c'), str(out / 'ab_new.c'), str(out / 'ab_table.c'), '-lm']
    print('compiling', flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-4000:])
        sys.exit(1)
    start, bad_fns, hangs, totals = 0, [], [], [0, 0, 0]
    while start < len(picked):
        p = subprocess.run([str(exe), str(out / f'{a.module}.img'), hex(base), str(start), str(a.cases), str(a.seed)],
                           capture_output=True, text=True)
        cur = start
        for line in p.stdout.splitlines():
            if line.startswith('FN '):
                cur = int(line.split()[1])
            elif line.startswith('RESULT'):
                f = line.split()
                ok, bad, faults = int(f[3]), int(f[5]), int(f[7])
                totals[0] += ok; totals[1] += bad; totals[2] += faults
                if bad:
                    bad_fns.append(picked[cur][0])
                    print(f'{picked[cur][0]:#x}: {bad} of {ok + bad} differ', flush=True)
            elif line.startswith('  case'):
                print(line, flush=True)
        if 'DONE' in p.stdout:
            break
        if 'HANG' in p.stdout:
            hangs.append(picked[cur][0])
        else:
            print(f'harness stopped at function {cur} (exit {p.returncode}): {p.stderr[-500:]}', flush=True)
        start = cur + 1
    print(f'cases: {totals[0]} agreed, {totals[1]} differed, {totals[2]} faulted on both; functions that differed: '
          f'{len(bad_fns)}, hung: {len(hangs)}')
    sys.exit(1 if bad_fns else 0)


if __name__ == '__main__':
    main()
