"""Translate the exported shaders with totk_xlate --serve (Ryujinx), in parallel on at most 8 logical CPUs.

usage: python tools/run_ryujinx_xlate.py data/shaders [--workers 8]
Writes <dir>/translated/<key>.spv (+ .res.json) and <dir>/translate-results.tsv (key, ok/fail, seconds, spv bytes, message).
"""
import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import XLATE  # noqa: E402
import progress  # noqa: E402


def start_pinned(cmd, mask, env):
    if sys.platform == "win32":
        CREATE_SUSPENDED, BELOW_NORMAL = 0x4, 0x4000
        p = subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             text=True, creationflags=CREATE_SUSPENDED | BELOW_NORMAL)
        ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t(mask))
        ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
        return p
    def preexec():
        try:
            os.nice(10)
            if hasattr(os, "sched_setaffinity"):
                cpus = {i for i in range(64) if (mask & (1 << i))}
                if cpus:
                    os.sched_setaffinity(0, cpus)
        except Exception:
            pass
    return subprocess.Popen(cmd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, preexec_fn=preexec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--new-only', action='store_true', help='translate only keys without a .spv')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    lines = (a.dir / 'jobs.jsonl').read_text().splitlines()
    if a.new_only:   # only shaders without a translation yet (the comparison's outputs stay)
        lines = [l for l in lines if not (a.dir / 'translated' / (json.loads(l)['job']['key'] + '.spv')).exists()]
    (a.dir / 'translated').mkdir(exist_ok=True)
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
               DOTNET_CLI_HOME=str(ROOT / 'third_party/dotnet-home'))
    xlate_name = "ultrs-shaders" if sys.platform.startswith("linux") else "ultrs-shaders.exe"
    if XLATE is None:
        sys.exit(f"the shader translator {xlate_name} is missing: python tools/build_xlate.py")
    cmd = [str(XLATE)]   # tools/build_xlate.py, or the builder package's deps/shaders
    results, lock = [], threading.Lock()
    shards = [lines[i::a.workers] for i in range(a.workers)]
    t0 = time.time()
    bar = progress.Bar('translating the shaders', len(lines), 'shaders') if a.quiet else None

    def worker(shard):
        p = start_pinned(cmd + ['--serve'], (1 << a.workers) - 1, env)
        for line in shard:
            key = json.loads(line)['job']['key']
            s = time.perf_counter()
            p.stdin.write(line + '\n'); p.stdin.flush()
            reply = p.stdout.readline().strip()
            dt = time.perf_counter() - s
            spv = a.dir / 'translated' / f'{key}.spv'
            ok = reply.startswith('ok ')
            size = spv.stat().st_size if ok and spv.exists() else 0
            msg = '' if ok else (reply[len('fail ' + key) + 1:] if reply.startswith('fail') else reply or 'no reply')
            with lock:
                results.append((key, 'ok' if ok else 'fail', dt, size, msg.replace('\t', ' ')[:300]))
                if bar:
                    bar.update(len(results))
                elif len(results) % 2000 == 0:
                    print(f'{len(results)}/{len(lines)} {time.time() - t0:.0f}s', flush=True)
        p.stdin.close(); p.wait()

    threads = [threading.Thread(target=worker, args=(s,)) for s in shards]
    for t in threads: t.start()
    for t in threads: t.join()
    if bar:
        bar.finish()
    with (a.dir / ('translate-results-new.tsv' if a.new_only else 'translate-results.tsv')).open('w') as f:   # --new-only keeps the full table
        f.write('key\tresult\tseconds\tspv_bytes\tmessage\n')
        for r in sorted(results):
            f.write(f'{r[0]}\t{r[1]}\t{r[2]:.4f}\t{r[3]}\t{r[4]}\n')
    ok = sum(r[1] == 'ok' for r in results)
    print(f'shaders translated: {ok}/{len(results)} ok, {time.time() - t0:.0f}s wall, {sum(r[2] for r in results):.0f}s busy')


if __name__ == '__main__':
    main()
