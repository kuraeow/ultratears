"""Write build/build.ninja for the runtime + the generated game code, and run ninja (MSYS2 CLANG64 clang/ninja).

usage: python tools/build_game.py [--generated data/generated] [--target probe_cpu] [-j N] [--cpus N]
The ultratears target links UltraTears.exe in the game folder itself (its debug symbols: build/UltraTears.pdb).
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
from toolchain import CLANG64, env as tool_env  # noqa: E402
import version  # noqa: E402

RUNTIME = ['src/runtime/dispatch.c', 'src/runtime/loader.c', 'src/runtime/platform/windows.c']
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
                    'src/host/profiler.c', 'src/host/watch.c',
                    'src/host/keyboard_input.c', 'src/host/mouse_camera.c', 'src/host/camera_v143.c']}
# The graphics front end (MacNativeGames' NVN layer) with its macOS stand-ins: own include paths, own rule
GFX = ['src/gfx/nvn/hle_nvn.c', 'src/gfx/nvn/nvn_tex.c', 'src/gfx/nvn/totk_nvn_ext.c', 'src/gfx/nvn/nvn_registry.c',
       'src/gfx/nvn/nvn_thunks.gen.c', 'src/gfx/nvn/nvn_dirty_win.c', 'src/gfx/nvn/totk_timeline_win.c',
       'src/gfx/nvn/nvn_names.gen.c', 'src/gfx/backend_null.c', 'src/gfx/gfx_bridge.c',
       'src/gfx/front_win.c', 'src/gfx/nvn/totk_vpool_win.c', 'src/gfx/backend_vk.c', 'src/gfx/vkb_tex.c', 'src/gfx/vkb_shader.c', 'src/gfx/vkb_spv.c', 'src/gfx/vkb_audit.c', 'src/gfx/vkb_copy.c', 'src/gfx/vkb_desc.c', 'src/gfx/vkb_shadow.c', 'src/gfx/vkb_ucap.c',
       'src/gfx/vkb_draw.c', 'src/gfx/tex/nvn_deswizzle.c', 'src/gfx/tex/astc.c', 'src/gfx/vkb_bc7.c',
       'src/gfx/tex/bc7.cpp', 'src/gfx/tex/bc7enc/bc7enc.cpp', 'src/gfx/tex/bc7enc/rgbcx.cpp', 'src/gfx/tex/bc7key.c']
GFX_INCLUDES = '-Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include -Isrc/gfx/compat'
LIBS = '-static -Wl,--error-limit=0 -Wl,--disable-high-entropy-va -pthread -ldbghelp -lwinmm -lsynchronization -lshell32 -lopus -lvpx -lSDL3 -liconv' \
       ' -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -ldinput8 -lbcrypt -lc++'
CFLAGS = '-O2 -march=x86-64-v3 -g -gcodeview -fno-strict-aliasing -D_FILE_OFFSET_BITS=64'
GAME_CFLAGS = CFLAGS + ' -w -Isrc/runtime'


def rel(p):
    return pathlib.Path(os.path.relpath(p, ROOT)).as_posix()


def write_if_changed(path, text):   # an unchanged file keeps its time: nothing rebuilds
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def version_files(build):
    """build/version.h (the version and commit, tools/version.py) and the exe's resources: icon and version info."""
    v, commit = version.current()
    n = ','.join(str(x) for x in version.numbers(v))
    write_if_changed(build / 'version.h', f'#define ULTRS_VERSION "{v}"\n#define ULTRS_COMMIT "{commit}"\n')
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--generated', default='data/generated')
    ap.add_argument('--target', default='probe_cpu')
    ap.add_argument('-j', type=int, default=0, help='parallel jobs (default: --cpus)')
    ap.add_argument('--cpus', type=int, default=8, help='logical CPUs the build may use (affinity); the rest stay free for the user')
    a = ap.parse_args()
    a.j = a.j or a.cpus
    gen = ROOT / a.generated
    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    lines = [
        'builddir = build',   # ninja's own state (.ninja_log, .ninja_deps) in build/, not beside the sources
        f'cflags = {CFLAGS} -Wall -Wno-unused-function -Ibuild',   # build/version.h (the game's code: not, so a new version rebuilds nothing of it)
        f'gameflags = {GAME_CFLAGS}',
        'rule cc',
        '  command = clang -MD -MF $out.d $cflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        f'gfxflags = {CFLAGS} -w {GFX_INCLUDES}',
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
        'rule rc',
        '  command = llvm-windres --target=pe-x86-64 -I . $in -o $out',
        'rule link',
        f'  command = clang -fuse-ld=lld -g -Wl,--pdb=$pdb -o $out @$out.rsp {LIBS}',
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
        # the port's icon (assets/icon.ico, the author's own drawing: README.md, Legal) and version in the resources
        print(f'UltraTears {version_files(build)}', flush=True)
        lines.append('build build/obj/ultratears_res.o: rc build/ultratears.rc | assets/icon.ico')
        objs.append('build/obj/ultratears_res.o')
        for src in GFX:
            cxx = src.endswith('.cpp')
            obj = f'build/obj/{src.rsplit(".", 1)[0].replace("/", "_")}.o'
            lines.append(f'build {obj}: {"gfxcxx" if cxx else "gfxcc"} {src}')
            objs.append(obj)
    game_objs = []
    for module in ('main', 'subsdk0', 'sdk'):
        for c in sorted((gen / module).glob('*.c')):
            obj = f'build/game/{module}/{c.stem}.o'
            lines.append(f'build {obj}: gamecc {rel(c)}')
            text = c.read_text(errors='replace')
            # [temporary] a function with an unresolved computed branch is one switch over every
            # instruction; with thousands of labels clang -O1/-O2 takes from minutes to hours per file (main_376:
            # 20+ min at -O1), -O0 15 s. Such functions run unoptimized until their tables resolve.
            if 'L_dispatch:' in text and text.count(chr(10) + 'L_') > 2000:
                lines.append('  gameflags = ' + GAME_CFLAGS.replace('-O2', '-O0'))
            game_objs.append(obj)
    if not game_objs:
        sys.exit(f'no generated C in {gen}: run tools/gen_game.py first')
    exe = 'UltraTears.exe' if a.target == 'ultratears' else f'build/{a.target}.exe'   # the game: in the game folder
    lines.append(f'build {exe}: link {" ".join(objs + game_objs)}')
    lines.append(f'  pdb = build/{pathlib.Path(exe).stem}.pdb')
    lines.append(f'default {exe}')
    (build / 'build.ninja').write_text('\n'.join(lines) + '\n')
    env = tool_env()
    print(f'{len(game_objs)} generated files, {len(objs)} runtime files', flush=True)
    # The user works on this PC during builds: ninja starts suspended, gets the first --cpus logical CPUs and
    # below-normal priority, then runs; every clang it starts inherits both, so the other CPUs stay free.
    ninja = [str(CLANG64 / 'ninja.exe'), '-C', str(ROOT), '-f', 'build/build.ninja', f'-j{a.j}', '-k', '0']
    sys.exit(run_limited(ninja, env, (1 << a.cpus) - 1))


def run_limited(cmd, env, mask):
    import ctypes
    CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS = 0x4, 0x4000
    p = subprocess.Popen(cmd, env=env, creationflags=CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS)
    handle = int(p._handle)
    if not ctypes.windll.kernel32.SetProcessAffinityMask(handle, ctypes.c_size_t(mask)):
        p.kill()
        sys.exit('SetProcessAffinityMask failed')
    ctypes.windll.ntdll.NtResumeProcess(handle)
    return p.wait()


if __name__ == '__main__':
    main()
