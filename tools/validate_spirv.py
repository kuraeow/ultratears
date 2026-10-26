"""Validate a translator's SPIR-V output with spirv-val (Vulkan 1.3 rules), on at most 8 logical CPUs.

usage: python tools/validate_spirv.py work/shaders/v143 ryujinx
Writes <dir>/<name>-spirv-val.tsv (key, ok/invalid, first error line).
"""
import ctypes
import pathlib
import subprocess
import sys
import threading

SPIRV_VAL = r'C:\VulkanSDK\1.4.363.0\Bin\spirv-val.exe'


def main():
    base, name = pathlib.Path(sys.argv[1]), sys.argv[2]
    files = sorted((base / name).glob('*.spv'))
    results, lock = [], threading.Lock()

    def worker(chunk):
        for f in chunk:
            if sys.platform == "win32":
                p = subprocess.Popen([SPIRV_VAL, "--target-env", "vulkan1.3", "--scalar-block-layout", str(f)], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, creationflags=0x4 | 0x4000)
                ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t(0xFF))
                ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
            else:
                p = subprocess.Popen([SPIRV_VAL, "--target-env", "vulkan1.3", "--scalar-block-layout", str(f)], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
            out, _ = p.communicate()
            first = next((l for l in out.splitlines() if l.strip()), '')
            with lock:
                results.append((f.stem, 'ok' if p.returncode == 0 else 'invalid', first.replace('\t', ' ')[:200]))
    threads = [threading.Thread(target=worker, args=(files[i::8],)) for i in range(8)]
    for t in threads: t.start()
    for t in threads: t.join()
    with (base / f'{name}-spirv-val.tsv').open('w') as out:
        out.write('key\tresult\tmessage\n')
        for r in sorted(results):
            out.write('\t'.join(r) + '\n')
    bad = [r for r in results if r[1] != 'ok']
    print(f'{name}: {len(results) - len(bad)}/{len(results)} valid')


if __name__ == '__main__':
    main()
