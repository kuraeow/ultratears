"""Compare two translators' SPIR-V after spirv-opt -O on a sample of shaders both translated validly.

usage: python tools/compare_spirv_opt.py work/shaders/v143 [--sample 1000]
Prints the raw and optimized size totals per translator and per stage (instruction words, from the module size).
"""
import argparse
import collections
import ctypes
import pathlib
import random
import subprocess
import tempfile
import threading

OPT = r'C:\VulkanSDK\1.4.363.0\Bin\spirv-opt.exe'


def valid(base, name):
    rows = (base / f'{name}-spirv-val.tsv').read_text().splitlines()[1:]
    return {r.split('\t')[0] for r in rows if r.split('\t')[1] == 'ok'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--sample', type=int, default=1000)
    a = ap.parse_args()
    stage = {l.split('\t')[0]: l.split('\t')[1] for l in (a.dir / 'shaders.tsv').read_text().splitlines()[1:]}
    keys = sorted(valid(a.dir, 'ryujinx') & valid(a.dir, 'eden'))
    random.Random(1).shuffle(keys)
    keys = keys[:a.sample]
    tmp = pathlib.Path(tempfile.mkdtemp(dir=a.dir))
    sizes = collections.defaultdict(lambda: [0, 0, 0])   # (translator, stage) -> raw, optimized, count
    lock = threading.Lock()
    failed = collections.Counter()

    def worker(chunk):
        for key in chunk:
            for name in ('ryujinx', 'eden'):
                src = a.dir / name / f'{key}.spv'
                dst = tmp / f'{name}-{key}.spv'
                if sys.platform == "win32":
                    p = subprocess.Popen([OPT, "-O", "--target-env=vulkan1.3", "--scalar-block-layout", str(src), "-o", str(dst)],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x4 | 0x4000)
                    ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t(0xFF))
                    ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
                else:
                    p = subprocess.Popen([OPT, "-O", "--target-env=vulkan1.3", "--scalar-block-layout", str(src), "-o", str(dst)],
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if p.wait() or not dst.exists():
                    with lock: failed[name] += 1
                    continue
                with lock:
                    s = sizes[(name, stage[key])]
                    s[0] += src.stat().st_size; s[1] += dst.stat().st_size; s[2] += 1
                dst.unlink()
    threads = [threading.Thread(target=worker, args=(keys[i::8],)) for i in range(8)]
    for t in threads: t.start()
    for t in threads: t.join()
    tmp.rmdir()
    print(f'sample: {len(keys)} shaders valid in both; spirv-opt failures: {dict(failed)}')
    for st in ('vs', 'ps', 'cs'):
        r, e = sizes[('ryujinx', st)], sizes[('eden', st)]
        if r[2] and e[2]:
            print(f'{st}: n={r[2]:5d}  raw KB/shader ryujinx {r[0]/r[2]/1e3:6.1f} eden {e[0]/e[2]/1e3:6.1f}   '
                  f'optimized ryujinx {r[1]/r[2]/1e3:6.1f} eden {e[1]/e[2]/1e3:6.1f}  (eden/ryujinx {e[1]/r[1]:.2f})')


if __name__ == '__main__':
    main()
