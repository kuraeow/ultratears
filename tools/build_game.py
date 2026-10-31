"""Write build/build.ninja for the runtime + the generated game code, and run ninja.

usage: python tools/build_game.py [--generated data/generated] [--target probe_cpu] [-j N] [--cpus N]
The ultratears target links the game executable in the game folder itself.
Run in the game folder (tools/setup.py does): the generated C is in data/ (derived from the game), objects and
executables go to build/; neither is ever in git.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import CLANG64, LINUX_TOOLS, env as tool_env  # noqa: E402
import version  # noqa: E402

LINUX = sys.platform == 'linux'
RUNTIME = ['src/runtime/dispatch.c', 'src/runtime/loader.c',
           'src/runtime/platform/linux.c' if LINUX else 'src/runtime/platform/windows.c']
TARGETS = {'probe_cpu': ['src/probe/probe_cpu.c'],
           'ultratears': ['src/host/boot.c', 'src/host/guest.c', 'src/host/svc.c', 'src/host/threads.c', 'src/host/ipc.c',
                    'src/host/services_lm.c', 'src/host/overrides_oe.c',
                    'src/host/overrides_time.c',
                    'src/host/fs.c', 'src/host/account.c',
                    'src/host/overrides_applet.c', 'src/host/overrides_input.c',
                    'src/host/audio_renderer.c', 'src/host/audio_mixer.c', 'src/host/audio_effects.c',
                    'src/host/audio_reverb.c', 'src/host/opus_codec.c', 'src/host/movie_host.c', 'src/host/hid_feedback.c',
                    'src/host/shared_font.c',
                    'src/host/window_sdl.c', 'src/vk/vk.c', 'src/vk/present.c', 'src/host/motion_fusion.c',
                    'src/host/profiler.c', 'src/host/watch.c', 'src/host/frame_rate.c']}
# The graphics front end (MacNativeGames' NVN layer) with its macOS stand-ins: own include paths, own rule
GFX = ['src/gfx/nvn/hle_nvn.c', 'src/gfx/nvn/nvn_tex.c', 'src/gfx/nvn/totk_nvn_ext.c', 'src/gfx/nvn/nvn_registry.c',
       'src/gfx/nvn/nvn_thunks.gen.c', 'src/gfx/nvn/nvn_dirty_win.c', 'src/gfx/nvn/totk_timeline_win.c',
       'src/gfx/nvn/nvn_names.gen.c', 'src/gfx/backend_null.c', 'src/gfx/gfx_bridge.c',
       'src/gfx/front_win.c', 'src/gfx/nvn/totk_vpool_win.c', 'src/gfx/backend_vk.c', 'src/gfx/vkb_tex.c', 'src/gfx/vkb_shader.c', 'src/gfx/vkb_shbin.c', 'src/gfx/vkb_shpack.c', 'src/gfx/vkb_spv.c', 'src/gfx/vkb_audit.c', 'src/gfx/vkb_copy.c', 'src/gfx/vkb_desc.c', 'src/gfx/vkb_shadow.c', 'src/gfx/vkb_ucap.c',
       'src/gfx/vkb_draw.c', 'src/gfx/tex/nvn_deswizzle.c', 'src/gfx/tex/astc.c', 'src/gfx/vkb_bc7.c',
       'src/gfx/tex/bc7.cpp', 'src/gfx/tex/bc7enc/bc7enc.cpp', 'src/gfx/tex/bc7enc/rgbcx.cpp', 'src/gfx/tex/bc7key.c']
GFX_INCLUDES = '-Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include -Isrc/gfx/compat'
LIBS = ('-Lbuild/lib -L../../build/third_party/sdl3 -lSDL3 -lopus -lvpx -lzstd -lpthread -ldl -lm -lstdc++' if LINUX else
        '-static -Wl,--error-limit=0 -Wl,--disable-high-entropy-va -pthread -ldbghelp -lwinmm -lsynchronization -lshell32 -lopus -lvpx -lSDL3 -liconv'
        ' -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -ldinput8 -lbcrypt -lc++')
CFLAGS = ('-O2 -march=x86-64-v3 -g -fno-strict-aliasing -D_FILE_OFFSET_BITS=64' if LINUX else
          '-O2 -march=x86-64-v3 -g -gcodeview -fno-strict-aliasing -D_FILE_OFFSET_BITS=64')
CFLAGS += ' -I../../third_party/sdl3/include -I../../third_party/vulkan-headers/include'
GAME_CFLAGS = CFLAGS + ' -w -Isrc/runtime'


def rel(p):
    return pathlib.Path(os.path.relpath(p, ROOT)).as_posix()


def write_if_changed(path, text):   # an unchanged file keeps its time: nothing rebuilds
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')



def unpatch_lifted_sead_allocators(gen):
    """Take out the heap fallbacks that earlier builds injected into the lifted C. 0x802bb74c0 is the stub of
    nn::os::GetTlsValue, not HeapMgr::getCurrentHeap, and the fake heaps they returned crashed the game."""
    hooks = {
        'void f_80006a920(Cpu *c) {': (
            '    if (c->x[1] == 0 || c->x[1] < 0x10000ULL || c->x[1] >= (1ULL << 48)) {\n'
            '        extern void *sead_fallback_heap_alloc(uint64_t size, uint64_t align);\n'
            '        c->x[0] = (uint64_t)(uintptr_t)sead_fallback_heap_alloc(c->x[0], c->x[2]);\n'
            '        return;\n'
            '    }'),
        'void f_8008ac138(Cpu *c) {': (
            '    if (c->x[0] == 0 || c->x[0] < 0x10000ULL || c->x[0] >= (1ULL << 48)) {\n'
            '        extern void *sead_fallback_root_heap(void);\n'
            '        c->x[0] = (uint64_t)(uintptr_t)sead_fallback_root_heap();\n'
            '        return;\n'
            '    }'),
    }
    for module in ('main', 'subsdk0', 'sdk'):
        for c in sorted((gen / module).glob('*.c')):
            text = c.read_text(encoding='utf-8')
            if 'sead_fallback_' not in text:
                continue
            for head, body in hooks.items():
                text = text.replace(head + '\n' + body, head, 1)
            c.write_text(text, encoding='utf-8')


def version_files(build):
    """Write build/version.h and, on Windows, the executable's icon and version resources."""
    v, commit = version.current()
    n = ','.join(str(x) for x in version.numbers(v))
    write_if_changed(build / 'version.h', f'#define ULTRS_VERSION "{v}"\n#define ULTRS_COMMIT "{commit}"\n')
    if not LINUX:
        write_if_changed(build / 'ultratears.rc', f'''1 ICON "assets/icon.ico"
1 VERSIONINFO
FILEVERSION {n}
PRODUCTVERSION {n}
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "UltraTears contributors"
      VALUE "FileDescription", "UltraTears"
      VALUE "FileVersion", "{v} ({commit})"
      VALUE "LegalCopyright", "GPL-3.0-or-later. Not affiliated with Nintendo."
      VALUE "OriginalFilename", "UltraTears.exe"
      VALUE "ProductName", "UltraTears"
      VALUE "ProductVersion", "{v}"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
''')
    return v


def cpu_arch():
    """x86-64-v3 where this CPU has AVX2, FMA and BMI2 (Intel Haswell / 2013, AMD Excavator / Zen and newer), else
    x86-64-v2 (SSE4.2): the code is built on the player's PC, so for its CPU (issue #19: i7-3770, Xeon E3-1230 v2).
    clang -march=native says which features the CPU has."""
    clang = LINUX_TOOLS['clang'] if LINUX else CLANG64 / 'clang.exe'
    r = subprocess.run([str(clang), '-march=native', '-dM', '-E', '-x', 'c', os.devnull], capture_output=True,
                       text=True, env=tool_env())
    if r.returncode:
        print('cannot ask clang about this CPU: building for x86-64-v3', flush=True)
        return 'x86-64-v3'
    have = {w for line in r.stdout.splitlines() for w in line.split()[1:2]}
    return 'x86-64-v3' if {'__AVX2__', '__FMA__', '__BMI2__'} <= have else 'x86-64-v2'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--generated', default='data/generated')
    ap.add_argument('--target', default='probe_cpu')
    ap.add_argument('-j', type=int, default=0, help='parallel jobs (default: --cpus)')
    ap.add_argument('--cpus', type=int, default=8, help='logical CPUs the build may use (affinity); the rest stay free for the user')
    ap.add_argument('--arch', default='auto', choices=['auto', 'x86-64-v3', 'x86-64-v2'],
                    help='instruction set of the code (auto: x86-64-v3 where this CPU has AVX2, FMA and BMI2, else x86-64-v2)')
    a = ap.parse_args()
    arch = cpu_arch() if a.arch == 'auto' else a.arch
    cflags, game_cflags = CFLAGS.replace('x86-64-v3', arch), GAME_CFLAGS.replace('x86-64-v3', arch)
    if arch != 'x86-64-v3':
        print(f'code for {arch}: this CPU has no AVX2 / FMA / BMI2 (slower than x86-64-v3)', flush=True)
    a.j = a.j or a.cpus
    gen = ROOT / a.generated
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    lines = [
        'builddir = build',   # ninja's own state (.ninja_log, .ninja_deps) in build/, not beside the sources
        f'cflags = {cflags} -Wall -Wno-unused-function -Ibuild',   # build/version.h (the game's code: not, so a new version rebuilds nothing of it)
        f'gameflags = {game_cflags}',
        'rule cc',
        '  command = clang -MD -MF $out.d $cflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        f'gfxflags = {cflags} -w {GFX_INCLUDES}',
        'rule gfxcc',
        '  command = clang -MD -MF $out.d $gfxflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gfxcxx',   # C++ (the BC7 encoder): no exceptions or RTTI, static libc++ at the link
        '  command = clang++ -MD -MF $out.d $gfxflags -std=c++17 -fno-exceptions -fno-rtti -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gamecc',
        '  command = clang -MD -MF $out.d $gameflags -c $in -o $out',   # runtime headers (cpu.h, simd*.h) are inlined
        '  depfile = $out.d',
        '  deps = gcc',
        *([] if LINUX else ['rule rc', '  command = llvm-windres --target=pe-x86-64 -I . $in -o $out']),
        'rule link',
        (f'  command = clang -g -o $out @$out.rsp {LIBS}' if LINUX else
         f'  command = clang -g -Wl,--pdb=$pdb -o $out @$out.rsp {LIBS}'),
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '',
    ]
    objs = []
    for src in RUNTIME + TARGETS[a.target]:
        obj = f'build/obj/{src[:-2].replace("/", "_")}.o'
        lines.append(f'build {obj}: cc {src}')
        objs.append(obj)
    if a.target == 'ultratears':
        # Windows resources: the port's icon (assets/icon.ico) and version information.
        print(f'UltraTears {version_files(build)}', flush=True)
        if not LINUX:
            lines.append('build build/obj/ultratears_res.o: rc build/ultratears.rc | assets/icon.ico')
            objs.append('build/obj/ultratears_res.o')
        for src in GFX:
            cxx = src.endswith('.cpp')
            obj = f'build/obj/{src.rsplit(".", 1)[0].replace("/", "_")}.o'
            lines.append(f'build {obj}: {"gfxcxx" if cxx else "gfxcc"} {src}')
            objs.append(obj)
    game_objs = []
    unpatch_lifted_sead_allocators(gen)
    for module in ('main', 'subsdk0', 'sdk'):
        for c in sorted((gen / module).glob('*.c')):
            obj = f'build/game/{module}/{c.stem}.o'
            lines.append(f'build {obj}: gamecc {rel(c)}')
            text = c.read_text(errors='replace')
            # [temporary] a function with an unresolved computed branch is one switch over every
            # instruction; with thousands of labels clang -O1/-O2 takes from minutes to hours per file (main_376:
            # 20+ min at -O1), -O0 15 s. Such functions run unoptimized until their tables resolve.
            if 'L_dispatch:' in text and text.count(chr(10) + 'L_') > 2000:
                lines.append('  gameflags = ' + game_cflags.replace('-O2', '-O0'))
            game_objs.append(obj)
    if not game_objs:
        sys.exit(f'no generated C in {gen}: run tools/gen_game.py first')
    exe = (('ultratears' if LINUX else 'UltraTears.exe') if a.target == 'ultratears' else
           f'build/{a.target}' + ('' if LINUX else '.exe'))   # the game: in the game folder
    lines.append(f'build {exe}: link {" ".join(objs + game_objs)}')
    if not LINUX:
        lines.append(f'  pdb = build/{pathlib.Path(exe).stem}.pdb')
    lines.append(f'default {exe}')
    (build / 'build.ninja').write_text('\n'.join(lines) + '\n')
    env = tool_env()
    print(f'{len(game_objs)} generated files, {len(objs)} runtime files', flush=True)
    # The user works on this PC during builds: ninja starts suspended, gets the first --cpus logical CPUs and
    # below-normal priority, then runs; every clang it starts inherits both, so the other CPUs stay free (Windows;
    # Linux uses ninja's -j limit).
    ninja = (['ninja'] if LINUX else [str(CLANG64 / 'ninja.exe')]) + [
        '-C', str(ROOT), '-f', 'build/build.ninja', f'-j{a.j}', '-k', '0']
    on_line = None
    import progress
    if progress.JSON:   # setup.py --json: ninja's [n/m] as progress events, its lines on to the log
        import re
        bar = progress.Bar('compiling', 0, 'files')
        status = re.compile(r'^\[(\d+)/(\d+)\] ')

        def on_line(line):
            m = status.match(line)
            if m:
                bar.update(int(m.group(1)), int(m.group(2)))
            print(line, flush=True)
    rc = run_limited(ninja, env, (1 << a.cpus) - 1, on_line)
    if on_line:
        bar.finish()
    sys.exit(rc)


def run_limited(cmd, env, mask, on_line=None):
    """cmd at below-normal priority on the CPUs in mask; on_line(line): its output line by line instead of the console."""
    if LINUX:   # (no affinity or priority here)
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE if on_line else None,
                             stderr=subprocess.STDOUT if on_line else None, text=bool(on_line), errors='replace')
        if on_line:
            for line in p.stdout:
                on_line(line.rstrip('\n'))
        return p.wait()
    import ctypes
    CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS = 0x4, 0x4000
    pipe = subprocess.PIPE if on_line else None
    p = subprocess.Popen(cmd, env=env, creationflags=CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, stdout=pipe,
                         stderr=subprocess.STDOUT if on_line else None, text=bool(on_line), errors='replace')
    handle = int(p._handle)
    if not ctypes.windll.kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(mask)):
        p.kill()
        sys.exit('SetProcessAffinityMask failed')
    ctypes.windll.ntdll.NtResumeProcess(handle)
    if on_line:
        for line in p.stdout:
            on_line(line.rstrip('\n'))
    return p.wait()


if __name__ == '__main__':
    main()
