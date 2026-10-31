"""Translate the exported shaders with eden_xlate (Eden's shader recompiler), 8 shards on at most 8 logical CPUs.

usage: python tools/run_eden_xlate.py work/shaders/v143 [--shards 8] [--only N]
Writes <dir>/eden/<key>.spv and <dir>/eden-results.tsv (key, ok/fail, seconds, spv bytes, message).
"""
import argparse
import ctypes
import os
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--shards', type=int, default=8)
    ap.add_argument('--only', type=int, default=0, help='run shard 0 of N only (a quick check)')
    a = ap.parse_args()
    (a.dir / 'eden').mkdir(exist_ok=True)
    env = dict(os.environ, PATH=r'C:\msys64\clang64\bin' + os.pathsep + os.environ['PATH'])
    exe = ROOT / 'build/eden_xlate/eden_xlate.exe'
    shards = a.only or a.shards
    t0 = time.time()
    procs = []
    for s in (range(1) if a.only else range(shards)):
        if sys.platform == "win32":
            p = subprocess.Popen([str(exe), str(a.dir / "shaders.tsv"), str(a.dir / "raw"), str(a.dir / "eden"), str(s), str(shards)],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, creationflags=0x4 | 0x4000)
            ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t(0xFF))
            ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
        else:
            p = subprocess.Popen([str(exe), str(a.dir / "shaders.tsv"), str(a.dir / "raw"), str(a.dir / "eden"), str(s), str(shards)],
                                 env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        procs.append(p)
    rows = []
    for p in procs:
        out, _ = p.communicate()
        rows += [l for l in out.splitlines() if l.strip()]
        if p.returncode:
            rows.append(f'<shard crashed>\tfail\t0\t0\texit code {p.returncode}')
    with (a.dir / 'eden-results.tsv').open('w') as f:
        f.write('key\tresult\tseconds\tspv_bytes\tmessage\n')
        f.write('\n'.join(sorted(rows)) + '\n')
    ok = sum(r.split('\t')[1] == 'ok' for r in rows)
    print(f'eden: {ok}/{len(rows)} ok, {time.time() - t0:.0f}s wall')


if __name__ == '__main__':
    main()
