"""What this PC lacks for the build and the game, as a list of cases for the launcher (and setup.py's console).

Each case: {'code', 'sev', 'args'}. sev 'stop': the game will not run as it is (the player may still build: the launcher
asks "continue anyway"); 'block': the build cannot run (not enough disk space); 'warn': below the recommended, the build
goes on; 'info': a remark. The launcher has the texts for the codes in its languages; args fill them in.

  no_vulkan                       stop   no Vulkan driver at all
  vulkan_old        gpu, version  stop   the best card's driver offers Vulkan below 1.3
  no_shader_object  gpu, layer    stop   no VK_EXT_shader_object (AMD RX 400/500, Vega, Intel HD); layer: the Khronos
                                         shader object layer is installed and the game could run through it
  no_ext_mem_host   gpu           stop   no VK_EXT_external_memory_host
  no_swapchain      gpu           stop   no VK_KHR_swapchain (a compute-only device)
  vram_low          gpu, gb       warn   less than 4 GB of video memory
  ram_low           gb            warn   less than 16 GB of RAM
  cpu_no_avx2                     warn   the code is built for SSE4.2 (slower)
  disk_space        need_gb, free_gb, drive   block
  amd_untested      gpu           info   [temporary] AMD cards are not tested yet (dev-local/docs/issues.md, "#2, #16,
                                         #23"): remove once players with RDNA report that the game runs
usage: python tools/hw_check.py [folder]   (prints the cases as JSON lines; folder: where the build goes, for disk space)
"""
import ctypes
import json
import os
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gpu_check  # noqa: E402

INSTALL_GB = 36            # a build from nothing at its peak (data 27 GB, objects 2.7, tools 0.8, the shader export)
VRAM_GB, RAM_GB = 4, 16
AMD = 0x1002
RANK = {'discrete': 0, 'integrated': 1, 'virtual': 2, 'other': 3, 'CPU': 9}


def gpu_cases():
    details = []
    found = gpu_check.gpus(details=details)
    if found is None:
        return [{'code': 'no_vulkan', 'sev': 'stop', 'args': {}}]
    cards = [(f, d) for f, d in zip(found, details) if f[1] != 'CPU']
    if not cards:
        return [{'code': 'no_vulkan', 'sev': 'stop', 'args': {}}]
    ok = [(f, d) for f, d in cards if not f[3]]
    if ok:   # the game takes a card that has everything: the discrete one first
        (name, kind, api, _), d = min(ok, key=lambda c: RANK.get(c[0][1], 5))
        out = []
        if d['vram'] and d['vram'] < VRAM_GB * 2**30 * 0.95 and kind == 'discrete':
            out.append({'code': 'vram_low', 'sev': 'warn', 'args': {'gpu': name, 'gb': round(d['vram'] / 2**30, 1)}})
        if d['vendor'] == AMD:
            out.append({'code': 'amd_untested', 'sev': 'info', 'args': {'gpu': name}})
        return out
    (name, kind, api, missing), d = min(cards, key=lambda c: (len(c[0][3]), RANK.get(c[0][1], 5)))   # the nearest
    if 'Vulkan 1.3' in missing:
        return [{'code': 'vulkan_old', 'sev': 'stop', 'args': {'gpu': name, 'version': api}}]
    if 'VK_EXT_shader_object' in missing:
        layered = gpu_check.gpus(gpu_check.LAYER) or []
        layer = any(not m for _, k, _, m in layered if k != 'CPU')
        return [{'code': 'no_shader_object', 'sev': 'stop', 'args': {'gpu': name, 'layer': layer}}]
    if 'VK_EXT_external_memory_host' in missing:
        return [{'code': 'no_ext_mem_host', 'sev': 'stop', 'args': {'gpu': name}}]
    return [{'code': 'no_swapchain', 'sev': 'stop', 'args': {'gpu': name}}]


class MemoryStatus(ctypes.Structure):
    _fields_ = [('dwLength', ctypes.c_uint32), ('dwMemoryLoad', ctypes.c_uint32), ('ullTotalPhys', ctypes.c_uint64),
                ('ullAvailPhys', ctypes.c_uint64), ('ullTotalPageFile', ctypes.c_uint64), ('ullAvailPageFile', ctypes.c_uint64),
                ('ullTotalVirtual', ctypes.c_uint64), ('ullAvailVirtual', ctypes.c_uint64), ('ullAvailExtendedVirtual', ctypes.c_uint64)]


def ram_bytes():
    if sys.platform != 'win32':
        try:
            return os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
        except (ValueError, OSError):
            return 0
    m = MemoryStatus(ctypes.sizeof(MemoryStatus))
    return m.ullTotalPhys if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else 0


def avx2():
    if sys.platform != 'win32':   # Linux: the CPU's flags as the kernel reports them
        try:
            return any(line.startswith('flags') and ' avx2' in line for line in open('/proc/cpuinfo'))
        except OSError:
            return True
    return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))   # PF_AVX2_INSTRUCTIONS_AVAILABLE


def folder_size(p):
    total = 0
    for root, _, files in os.walk(p):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def disk_case(folder):
    """Enough free space where the build goes? What is already built there (an update) counts as taken."""
    folder = pathlib.Path(folder)
    have = sum(folder_size(folder / d) for d in ('data', 'build', 'deps') if (folder / d).is_dir())
    need = max(0, INSTALL_GB * 2**30 - have)
    probe = folder
    while not probe.exists() and probe.parent != probe:
        probe = probe.parent
    free = shutil.disk_usage(probe).free
    if free >= need:
        return []
    return [{'code': 'disk_space', 'sev': 'block',
             'args': {'need_gb': round(need / 2**30), 'free_gb': round(free / 2**30), 'drive': str(probe.resolve().drive or probe)}}]


def cases(folder):
    out = gpu_cases()
    ram = ram_bytes()
    if ram and ram < RAM_GB * 2**30 * 0.9:   # a 16 GB PC reports a little less (memory the firmware keeps)
        out.append({'code': 'ram_low', 'sev': 'warn', 'args': {'gb': round(ram / 2**30)}})
    if not avx2():
        out.append({'code': 'cpu_no_avx2', 'sev': 'warn', 'args': {}})
    return out + disk_case(folder)


def text(case):
    """An English line for the console (the launcher has its own texts)."""
    a = case['args']
    return {
        'no_vulkan': 'no Vulkan graphics driver was found: install the current driver for your NVIDIA, AMD or Intel card',
        'vulkan_old': f'the {a.get("gpu")} driver offers Vulkan {a.get("version")}; 1.3 is needed: updating the driver usually helps',
        'no_shader_object': f'{a.get("gpu")} lacks VK_EXT_shader_object (usual for AMD RX 400/500, Vega and Intel HD)' +
                            (': it can run through the installed Khronos shader object layer, slower and with glitches' if a.get('layer') else ''),
        'no_ext_mem_host': f'the {a.get("gpu")} driver lacks VK_EXT_external_memory_host: update the driver',
        'no_swapchain': f'{a.get("gpu")} cannot show pictures (no VK_KHR_swapchain)',
        'vram_low': f'{a.get("gpu")} has {a.get("gb")} GB of video memory; 4 GB or more is recommended',
        'ram_low': f'this PC has {a.get("gb")} GB of RAM; 16 GB or more is recommended',
        'cpu_no_avx2': 'the CPU lacks AVX2: the game is built for SSE4.2 and runs a little slower',
        'disk_space': f'the build needs {a.get("need_gb")} GB on {a.get("drive")}, {a.get("free_gb")} GB is free',
        'amd_untested': f'{a.get("gpu")}: AMD cards are not tested yet; please report how the game runs',
    }.get(case['code'], case['code'])


if __name__ == '__main__':
    for c in cases(sys.argv[1] if len(sys.argv) > 1 else '.'):
        print(json.dumps(c, ensure_ascii=False))
