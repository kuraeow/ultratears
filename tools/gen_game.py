"""Lift the three TOTK code modules to C (tools/lifter/gen.py), in parallel.

usage: python tools/gen_game.py <exefs dir> <output dir>
Bases must match src/runtime/rt.h (MAIN_BASE, SUBSDK0_BASE, SDK_BASE).
"""
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
MODULES = {'main': 0x0800000000, 'subsdk0': 0x0804000000, 'sdk': 0x0805000000}
# Windows exit codes of a lifter that crashed (issue #30: all three ended with 3221226505 and an empty summary)
CRASHES = {0xC0000005: 'access violation in native code (unicorn / capstone / lz4 or Python itself)',
           0xC0000409: 'fast fail: Python or a native module aborted (a fatal error, a failed allocation, a stack check)',
           0xC00000FD: 'stack overflow', 0xC0000017: 'out of memory', 0xC000001D: 'illegal instruction (the CPU lacks a feature)'}


def main():
    exefs, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    t0 = time.time()
    procs = []
    for name, base in MODULES.items():
        (out / name).mkdir(parents=True, exist_ok=True)
        log = open(out / f'{name}.log', 'w')
        procs.append((name, log, subprocess.Popen(
            [sys.executable, '-X', 'faulthandler', str(ROOT / 'tools/lifter/gen.py'), name, str(exefs / name), hex(base), str(out / name),
             '--simd', 'c'], stdout=log, stderr=subprocess.STDOUT)))
    # one progress line: the lifters' "progress <functions done>/<functions>" lines (none while they analyse)
    sys.path.insert(0, str(ROOT / 'tools'))
    import progress
    bar = progress.Bar('translating the code', 0, 'functions')
    while any(p.poll() is None for _, _, p in procs):
        done = total = known = 0
        for name, _, p in procs:
            try:
                tail = (out / f'{name}.log').read_bytes()[-400:].decode(errors='replace')
            except OSError:
                continue
            m = re.findall(r'progress (\d+)/(\d+)', tail)
            if m:
                done, total, known = done + int(m[-1][0]), total + int(m[-1][1]), known + 1
            elif p.poll() is not None:
                known += 1
        # a percentage only once every lifter has counted its functions (main analyses for ~40 s first)
        bar.update(done, total) if known == len(procs) else bar.update(0, 0)
        time.sleep(0.5)
    bar.finish()
    failed = False
    for name, log, p in procs:
        code = p.wait()
        log.close()
        lines = (out / f'{name}.log').read_text(errors='replace').splitlines()
        summary = [l for l in lines if 'progress' not in l]
        print(f'{name}: exit {code}; ' + ' | '.join(summary[-2:]))
        if code != 0:   # a crash leaves no summary: what it was doing last, and what the code means
            if code & 0xFFFFFFFF in CRASHES:
                print(f'  {name}: {CRASHES[code & 0xFFFFFFFF]} ({code & 0xFFFFFFFF:#x})')
            for l in lines[-6:]:
                print(f'  {name} | {l}')
            print(f'  {name}: the whole log is {out / f"{name}.log"}')
        failed |= code != 0
    print(f'{time.time() - t0:.0f}s')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
