"""Write build/build.ninja for the runtime + the generated game code, and run ninja (MSYS2 CLANG64 clang/ninja).

usage: python tools/build_game.py [--generated data/generated] [--target probe_cpu] [-j N] [--cpus N] [--platform linux]
The ultratears target links UltraTears.exe in the game folder itself (its debug symbols: build/UltraTears.pdb).
--platform linux: the same game for Linux (Steam Deck), cross-compiled here with the same clang against the Linux kit
(LINUX_KIT: the Steam Runtime 3 "sniper" sysroot, a static zstd): objects in build-linux/, the game in
linux/ultratears with libSDL3.so.0 beside it (rpath $ORIGIN).
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
from toolchain import CLANG64, exe, env as tool_env  # noqa: E402
import icon_png  # noqa: E402
import version  # noqa: E402

RUNTIME = ['src/runtime/dispatch.c', 'src/runtime/loader.c', 'src/runtime/platform/windows.c', 'src/runtime/logcost.c', 'src/runtime/envcache.c',
           'src/runtime/fma.c']   # fma.c: correctly rounded fma/fmaf where the CPU has no FMA3 (x86-64-v2)
TARGETS = {'probe_cpu': ['src/probe/probe_cpu.c'],
           'ultratears': ['src/host/boot.c', 'src/host/guest.c', 'src/host/svc.c', 'src/host/threads.c', 'src/host/ipc.c',
                    'src/host/services_lm.c', 'src/host/overrides_oe.c',
                    'src/host/overrides_time.c',
                    'src/host/fs.c', 'src/host/account.c',
                    'src/host/overrides_applet.c', 'src/host/overrides_input.c', 'src/host/mouse_direct.c',
                    'src/host/audio_renderer.c', 'src/host/audio_mixer.c', 'src/host/audio_effects.c',
                    'src/host/audio_reverb.c', 'src/host/opus_codec.c', 'src/host/movie_host.c', 'src/host/hid_feedback.c',
                    'src/host/shared_font.c', 'src/host/swkbd.c', 'src/host/text_entry.c',
                    'src/host/window_sdl.c', 'src/vk/vk.c', 'src/vk/present.c', 'src/vk/fsr.c', 'src/vk/overlay.c', 'src/host/motion_fusion.c',
                    'src/host/deck_imu.c',
                    'src/host/profiler.c', 'src/host/watch.c', 'src/host/frame_rate.c', 'src/host/render_res.c', 'src/host/ui_capture.c',
                    'src/host/ui_aspect.c', 'src/host/ui_prompts.c', 'src/host/zstd_host.c', 'src/host/pgo.c',
                    'src/host/mods.c', 'src/host/restbl.c']}
# The graphics front end (MacNativeGames' NVN layer) with its macOS stand-ins: own include paths, own rule
GFX = ['src/gfx/nvn/hle_nvn.c', 'src/gfx/nvn/nvn_tex.c', 'src/gfx/nvn/totk_nvn_ext.c', 'src/gfx/nvn/nvn_registry.c',
       'src/gfx/nvn/nvn_thunks.gen.c', 'src/gfx/nvn/nvn_dirty_win.c', 'src/gfx/nvn/totk_timeline_win.c',
       'src/gfx/nvn/nvn_names.gen.c', 'src/gfx/backend_null.c', 'src/gfx/gfx_bridge.c',
       'src/gfx/front_win.c', 'src/gfx/nvn/totk_vpool_win.c', 'src/gfx/backend_vk.c', 'src/gfx/vkb_tex.c', 'src/gfx/vkb_shader.c', 'src/gfx/vkb_shbin.c', 'src/gfx/vkb_shpack.c', 'src/gfx/vkb_spv.c', 'src/gfx/vkb_audit.c', 'src/gfx/vkb_copy.c', 'src/gfx/vkb_desc.c', 'src/gfx/vkb_shadow.c', 'src/gfx/vkb_ucap.c', 'src/gfx/vkb_tsr.c', 'src/gfx/vkb_smaa.c',
       'src/gfx/vkb_draw.c', 'src/gfx/tex/nvn_deswizzle.c', 'src/gfx/tex/astc.c', 'src/gfx/vkb_bc7.c',
       'src/gfx/tex/bc7.cpp', 'src/gfx/tex/bc7enc/bc7enc.cpp', 'src/gfx/tex/bc7enc/rgbcx.cpp', 'src/gfx/tex/bc7key.c']
GFX_INCLUDES = '-Isrc/gfx -Isrc/gfx/nvn -Isrc/gfx/nvn/include -Isrc/gfx/compat'
# --wrap: the log's writes go through src/runtime/logcost.c, which times them for the long-frame line; getenv
# answers from src/runtime/envcache.c's table
LIBS = '-static -Wl,--error-limit=0 -Wl,--disable-high-entropy-va -Wl,--wrap=fprintf,--wrap=vfprintf,--wrap=fputs,--wrap=fputc,--wrap=getenv -pthread -ldbghelp -lwinmm -lsynchronization -lshell32 -lopus -lvpx -lSDL3 -lzstd -liconv' \
       ' -luser32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi -ldinput8 -lbcrypt -lc++'
CFLAGS = '-O2 -march=x86-64-v3 -g -gcodeview -fno-strict-aliasing -D_FILE_OFFSET_BITS=64'
GAME_CFLAGS = CFLAGS + ' -w -Isrc/runtime'
# Linux (--platform linux): the kit is deps/linux in a builder, else dev-local/linux-cross (ULTRS_LINUX_KIT overrides).
LINUX_KIT = pathlib.Path(os.environ.get('ULTRS_LINUX_KIT') or next(
    (p for p in (ROOT / 'deps/linux', ROOT / 'dev-local/linux-cross') if (p / 'sysroot').is_dir()), ROOT / 'deps/linux'))
LINUX_RUNTIME = ['src/runtime/dispatch.c', 'src/runtime/loader.c', 'src/runtime/platform/linux.c', 'src/runtime/logcost.c', 'src/runtime/fma.c',
                 'src/runtime/envcache.c']


# The rpath of the Linux programs, as ninja writes it into a command: on Linux ninja runs commands through sh, which
# would put an empty $ORIGIN there and "-rpath" would take the next input as its value (the first object went
# missing from the link); on Windows there is no shell, and quotes would stay in the path.
RPATH = '$$ORIGIN' if os.name == 'nt' else "'$$ORIGIN'"


def ninja_arg(a):
    """One argument in a ninja command, as the command runs: on Linux ninja runs it through sh, on Windows it hands the
    line to the program, which splits it by the C runtime's rules. The kit's path is inside the game folder, and a
    folder with a space in its name ("The Legend of Zelda") split it into two arguments (issue #55). '$' is ninja's."""
    a = a.replace('$', '$$')
    if os.name == 'nt':
        return f'"{a}"' if any(c in a for c in ' \t"') else a
    return "'" + a.replace("'", "'\\''") + "'" if any(c in a for c in ' \t\'"\\;&|()<>*?[]#~`!') else a


def ninja_args(args):
    return ' '.join(ninja_arg(a) for a in args)


def linux_flag_args():
    kit = LINUX_KIT.as_posix()
    return ['--target=x86_64-linux-gnu', f'--sysroot={kit}/sysroot', f'-I{kit}/include']


def linux_flags():   # for a ninja command (quoted); linux_flag_args() for a program run directly
    return ninja_args(linux_flag_args())


def linux_libs():
    # SDL3 shared (beside the game: the sysroot's static one is not position-independent); Opus, libvpx, zstd and
    # libstdc++ static, so the game needs only glibc, the Vulkan loader and that SDL3.
    return (f'{ninja_arg("-L" + LINUX_KIT.as_posix() + "/lib")} -Wl,--wrap=fprintf,--wrap=vfprintf,--wrap=fputs,--wrap=fputc '
            '-Wl,--wrap=getenv,--wrap=setenv,--wrap=unsetenv,--wrap=putenv -lSDL3 '   # src/runtime/envcache.c
            '-l:libopus.a -l:libvpx.a -l:libzstd.a -l:libstdc++.a -lm -lpthread -ldl')


def profile_rt(clang):
    """The link flags of the PGO training build: clang's profile runtime. The game folder's trimmed toolchain has none;
    MSYS2's CLANG64 of the same clang has it (lib/clang/<n>/lib/windows)."""
    for root in (clang.parent, pathlib.Path('C:/msys64/clang64')):
        libs = sorted((root / 'lib/clang').glob('*/lib/windows/libclang_rt.profile-x86_64.a'))
        if libs:
            return ' -u__llvm_profile_runtime ' + ninja_arg(libs[-1].as_posix())
    sys.exit('--pgo gen: no libclang_rt.profile-x86_64.a (the toolchain or C:/msys64/clang64)')


# Profile-guided optimization of the game's code (on by default since 2026-11-06): how often each lifted function ran
# and which way its branches went, counted in a 20-minute training run of the game (--pgo gen, src/host/pgo.c) and
# merged (llvm-profdata merge --sparse: functions that never ran left out). Only the lifted functions' names (their
# addresses), their control-flow hashes and the counts: no bytes of the game. The game's code is the same for every
# player (TOTK 1.4.3, tools/setup.py checks it), so one profile serves all. clang lays out and inlines by it; it
# changes no result. On the Steam Deck the game's work a frame 23-24.6 -> 21-22.6 ms in the dense places
# (dev-local/docs/perf/frame-compare.md 0). Code the training never reached keeps the optimization it has without a
# profile (-pgo-cold-func-opt=default), not the size-first one clang gives cold code. A function whose C changed since
# (another lifter) does not match its hash and is compiled as without. TOTK_PGO=0: without the profile (A/B).
# No .text.hot / .text.unlikely sections (-profile-guided-section-prefix=false): ld.lld groups them across all the
# objects, hot game code first, then ours, then the rest of the game's, and our runtime landed inside the lifted code's
# address range, which turns the virtual cores' asynchronous preemption off (src/host/threads.c ap_on: "host code inside
# the lifted code range"). On the Steam Deck every build with the profile ran without it (2026-11-06), and a game
# thread waited behind a worse-priority core owner up to 5-6 ms at a time instead of 2.3-2.8: the 40-30-40 dips while
# the world streams in. The layout and inlining by the profile stay; only that grouping goes (COFF did not do it).
PGO_PROFILE = ROOT / 'pgo/totk-1.4.3.profdata.zst'
PGO_FLAGS = ' -mllvm -pgo-cold-func-opt=default -mllvm -profile-guided-section-prefix=false'


def pgo_profile(build):
    """The profile unpacked into the build folder (its path), or None: switched off, missing, or no zstd in this
    Python. Rewritten only when it changed: the game objects depend on it, so a new profile compiles them again."""
    if os.environ.get('TOTK_PGO') == '0':
        print('PGO: off (TOTK_PGO=0)', flush=True)
        return None
    if not PGO_PROFILE.is_file():
        return None
    try:
        from compression import zstd
    except ImportError:
        print('PGO: this Python cannot unpack the profile (no compression.zstd): building without it', flush=True)
        return None
    data = zstd.decompress(PGO_PROFILE.read_bytes())
    out = build / 'pgo.profdata'
    if not out.is_file() or out.read_bytes() != data:
        out.write_bytes(data)
    return out


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
    # the window's icon on Linux (src/host/window_sdl.c): the exe's resource gives it on Windows
    write_if_changed(build / 'icon_png.h', '/* Generated by tools/build_game.py from assets/icon.ico. */\n' +
                     icon_png.c_array('ultrs_icon_png', icon_png.png(256)))
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
    r = subprocess.run([str(CLANG64 / exe('clang')), '-march=native', '-dM', '-E', '-x', 'c', os.devnull], capture_output=True,
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
    ap.add_argument('--platform', default='windows' if os.name == 'nt' else 'linux', choices=['windows', 'linux'],
                    help='linux: cross-compile the game for Linux / Steam Deck (the Linux kit, LINUX_KIT)')
    ap.add_argument('--runtime-only', action='store_true', help='compile our runtime only, not the game code (porting)')
    ap.add_argument('--pgo', choices=['gen', 'use'], help='gen: the PGO training build (Windows): the game code counts itself '
                    '(-fprofile-generate, src/host/pgo.c), objects in ../pgo-build, the game as UltraTears-pgo.exe; '
                    'use: the game code optimized by --profile (Windows: UltraTears-pgo.exe; Linux: ../pgo-linux/linux/ultratears)')
    ap.add_argument('--profile', help='--pgo use: the merged profile (llvm-profdata merge of the training counts)')
    a = ap.parse_args()
    linux = a.platform == 'linux'
    if a.pgo and a.target != 'ultratears':
        sys.exit('--pgo: the ultratears target only')
    if a.pgo == 'gen' and linux:
        sys.exit('--pgo gen: Windows only (the Linux kit has no profile runtime; the game code is the same C)')
    if a.pgo == 'use' and not (a.profile and pathlib.Path(a.profile).is_file()):
        sys.exit('--pgo use needs --profile <merged .profdata>')
    if linux and not (LINUX_KIT / 'sysroot').is_dir():
        sys.exit(f'the Linux kit is missing ({LINUX_KIT})')
    if not linux and os.name != 'nt':
        sys.exit('the game for Windows is built on Windows')
    # Linux cross-compiled on Windows: for the Steam Deck, whose Zen 2 has AVX2, FMA and BMI2 (this PC's CPU does not
    # decide); built on Linux: for this machine's CPU, as on Windows
    arch = a.arch if a.arch != 'auto' else 'x86-64-v3' if linux and os.name == 'nt' else cpu_arch()
    cflags, game_cflags = CFLAGS.replace('x86-64-v3', arch), GAME_CFLAGS.replace('x86-64-v3', arch)
    if linux:
        cflags = cflags.replace(' -gcodeview', '') + ' ' + linux_flags()
        game_cflags = game_cflags.replace(' -gcodeview', '') + ' ' + linux_flags()
    if a.pgo == 'gen':   # the counts of the game's code; our runtime is not counted (src/host/pgo.c writes them)
        cflags += ' -DULTRS_PGO_GEN'
        game_cflags += ' -fprofile-generate'
    if a.pgo == 'use':   # functions the training did not reach are compiled as without (warnings: -w)
        game_cflags += ' -fprofile-use=' + ninja_arg(pathlib.Path(a.profile).resolve().as_posix()) + PGO_FLAGS
    if arch != 'x86-64-v3':
        print(f'code for {arch}: this CPU has no AVX2 / FMA / BMI2 (slower than x86-64-v3)', flush=True)
    a.j = a.j or min(a.cpus, jobs_for_ram())
    gen = ROOT / a.generated
    # the PGO builds beside the game folder, not over its own (the training one, and the optimized one until it is checked)
    bd = ('../pgo-build' + ('-linux' if linux else '') + ('-use' if a.pgo == 'use' else '')) if a.pgo else 'build-linux' if linux else 'build'
    lin_dir = '../pgo-linux/linux' if a.pgo else 'linux'
    build = ROOT / bd
    build.mkdir(exist_ok=True)
    profile = pgo_profile(build) if not a.pgo and a.target == 'ultratears' and not a.runtime_only else None
    if profile:
        game_cflags += f' -fprofile-use={bd}/pgo.profdata' + PGO_FLAGS   # relative: a moved game folder rebuilds nothing
        print(f'PGO: the game code is optimized by {rel(PGO_PROFILE)}', flush=True)
    # --no-call-graph-profile-sort: objects built with the profile carry their call graph, and ld.lld orders the
    # functions of the whole program by it, our runtime in among the game's hot code (rt_dispatch between two lifted
    # functions): the same cut through the lifted code's range as the hot/cold sections (PGO_FLAGS), which turns the
    # virtual cores' asynchronous preemption off. The input order keeps the runtime before the game code.
    link = (f'  command = $cc {linux_flags()} --rtlib=libgcc --unwindlib=libgcc -fuse-ld=lld -no-pie -g -Wl,--no-call-graph-profile-sort '
            f'-Wl,-rpath,{RPATH} -o $out @$out.rsp {linux_libs()}' if linux else
            f'  command = $cc -fuse-ld=lld -g{profile_rt(CLANG64) if a.pgo == "gen" else ""} -Wl,--pdb=$pdb -o $out @$out.rsp {LIBS}')
    lines = [
        # the compilers by their full path, not through PATH: on Linux PATH is split at ':', and a game folder named
        # "The Legend of Zelda: Tears of the Kingdom" put clang out of reach (issues #66, #69)
        f'cc = {ninja_arg(str(CLANG64 / exe("clang")))}',
        f'cxx = {ninja_arg(str(CLANG64 / exe("clang++")))}',
        f'builddir = {bd}',   # ninja's own state (.ninja_log, .ninja_deps) beside the objects, not beside the sources
        f'cflags = {cflags} -Wall -Wno-unused-function -I{bd}',   # build/version.h (the game's code: not, so a new version rebuilds nothing of it)
        f'gameflags = {game_cflags}',
        'rule cc',
        '  command = $cc -MD -MF $out.d $cflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        f'gfxflags = {cflags} -w {GFX_INCLUDES}',
        'rule gfxcc',
        '  command = $cc -MD -MF $out.d $gfxflags -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gfxcxx',   # C++ (the BC7 encoder): no exceptions or RTTI, static libc++ at the link
        '  command = $cxx -MD -MF $out.d $gfxflags -std=c++17 -fno-exceptions -fno-rtti'
        + (' -stdlib=libstdc++' if linux else '') + ' -c $in -o $out',   # Linux: the sysroot's libstdc++, linked static
        '  depfile = $out.d',
        '  deps = gcc',
        'rule gamecc',
        '  command = $cc -MD -MF $out.d $gameflags -c $in -o $out',   # runtime headers (cpu.h, simd*.h) are inlined
        '  depfile = $out.d',
        '  deps = gcc',
        'rule rc',
        '  command = llvm-windres --target=pe-x86-64 -I . $in -o $out',
        'rule link',
        link,
        '  rspfile = $out.rsp',
        '  rspfile_content = $in',
        '',
    ]
    objs = []
    for src in (LINUX_RUNTIME if linux else RUNTIME) + TARGETS[a.target]:
        obj = f'{bd}/obj/{src[:-2].replace("/", "_")}.o'
        lines.append(f'build {obj}: cc {src}')
        objs.append(obj)
    if a.target == 'ultratears':
        # the port's icon (assets/icon.ico, the author's own drawing: README.md, Legal) and version in the resources
        print(f'UltraTears {version_files(build)}', flush=True)
        if not linux:
            lines.append(f'build {bd}/obj/ultratears_res.o: rc {bd}/ultratears.rc | assets/icon.ico')
            objs.append(f'{bd}/obj/ultratears_res.o')
        for src in GFX:
            cxx = src.endswith('.cpp')
            obj = f'{bd}/obj/{src.rsplit(".", 1)[0].replace("/", "_")}.o'
            lines.append(f'build {obj}: {"gfxcxx" if cxx else "gfxcc"} {src}')
            objs.append(obj)
    game_objs = []
    for module in ('main', 'subsdk0', 'sdk'):
        for c in sorted((gen / module).glob('*.c')):
            obj = f'{bd}/game/{module}/{c.stem}.o'
            lines.append(f'build {obj}: gamecc {rel(c)}' + (f' | {bd}/pgo.profdata' if profile else ''))
            text = c.read_text(errors='replace')
            # [temporary] a function with an unresolved computed branch is one switch over every
            # instruction; with thousands of labels clang -O1/-O2 takes from minutes to hours per file (main_376:
            # 20+ min at -O1), -O0 15 s. Such functions run unoptimized until their tables resolve.
            # Counted per function: a file's total once sent whole files of small dispatching functions to -O0
            # (main_000 and sdk_130 of 1.4.3: ~630 functions unoptimized for dispatches of 133 and 385 labels).
            if any('L_dispatch:' in f and f.count(chr(10) + 'L_') > 2000 for f in text.split(chr(10) + 'void f_')):
                lines.append('  gameflags = ' + game_cflags.replace('-O2', '-O0'))
            game_objs.append(obj)
    if not game_objs:
        sys.exit(f'no generated C in {gen}: run tools/gen_game.py first')
    if linux:   # the game for Linux in linux/, beside the game folder's data
        out = f'{lin_dir}/ultratears' if a.target == 'ultratears' else f'{bd}/{a.target}'
    else:
        out = ('UltraTears-pgo.exe' if a.pgo else 'UltraTears.exe') if a.target == 'ultratears' else f'build/{a.target}.exe'   # the game: in the game folder
    lines.append(f'build {out}: link {" ".join(objs + game_objs)}')
    if not linux:
        lines.append(f'  pdb = {bd}/{pathlib.Path(out).stem}.pdb')
    lines.append(f'default {" ".join(objs) if a.runtime_only else out}')
    (build / 'build.ninja').write_text('\n'.join(lines) + '\n')
    if linux:   # SDL3 beside the game (rpath $ORIGIN)
        import shutil
        (ROOT / lin_dir).mkdir(parents=True, exist_ok=True)
        # the Linux kit has libSDL3.so.0 itself (its links made copies, tools/make_linux_kit.py); a full sysroot has it
        # as a link to libSDL3.so.0.<version>, which Windows may have extracted as a copy or not at all
        lib = LINUX_KIT / 'sysroot/usr/lib/x86_64-linux-gnu'
        sdl = next((p for p in [lib / 'libSDL3.so.0', *sorted(lib.glob('libSDL3.so.0.*'))]
                    if p.is_file() and p.stat().st_size > 1 << 20), None)
        if not sdl:
            sys.exit(f'libSDL3.so.0 is missing from the Linux kit ({lib}): the game for Linux needs it beside itself')
        shutil.copy2(sdl, ROOT / lin_dir / 'libSDL3.so.0')
    env = tool_env()
    # Another compiler (an update brought another LLVM): ninja sees the same command lines and would keep the objects
    # the old one made, so they go and everything is compiled by the new one.
    cc = subprocess.run([str(CLANG64 / exe('clang')), '--version'], capture_output=True, text=True, env=env).stdout.split('\n')[0]
    stamp = build / 'compiler.txt'
    if stamp.is_file() and stamp.read_text(encoding='utf-8').strip() != cc.strip():
        import shutil
        print(f'another compiler ({cc.strip()}): compiling everything again', flush=True)
        for sub in ('obj', 'game'):
            shutil.rmtree(build / sub, ignore_errors=True)
    stamp.write_text(cc.strip() + '\n', encoding='utf-8')
    print(f'{len(game_objs)} generated files, {len(objs)} runtime files', flush=True)
    # The user works on this PC during builds: ninja starts suspended, gets the first --cpus logical CPUs and
    # below-normal priority, then runs; every clang it starts inherits both, so the other CPUs stay free.
    ninja = [str(CLANG64 / exe('ninja')), '-C', str(ROOT), '-f', f'{bd}/build.ninja', f'-j{a.j}', '-k', '0']
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


def jobs_for_ram():
    """As many clang processes as the memory holds: up to 768 MiB each, in 80 % of the RAM (the
    largest generated files take about that). 8 GB of RAM: 8, a Steam Deck's 16 GB: 16."""
    import hw_check
    ram = hw_check.ram_bytes()
    return max(1, int(ram * 0.8) // (768 << 20)) if ram else 64


def run_limited(cmd, env, mask, on_line=None):
    """cmd at below-normal priority on the CPUs in mask; on_line(line): its output line by line instead of the console.
    Linux: nice 10 and the CPUs set in the child before it starts, which every clang it starts inherits (as PR #42)."""
    pipe = subprocess.PIPE if on_line else None
    if os.name != 'nt':
        def limit():
            os.nice(10)
            cpus = {i for i in range(64) if mask >> i & 1} & os.sched_getaffinity(0)
            if cpus:
                os.sched_setaffinity(0, cpus)
        p = subprocess.Popen(cmd, env=env, preexec_fn=limit, stdout=pipe, stderr=subprocess.STDOUT if on_line else None,
                             text=bool(on_line), errors='replace')
        if on_line:
            for line in p.stdout:
                on_line(line.rstrip('\n'))
        return p.wait()
    import ctypes
    import time
    CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS = 0x4, 0x4000
    # a program the build has just linked (bc7build.exe) may be refused for a few seconds while an antivirus checks it
    # (GitHub #78: "Access denied" from CreateProcess): tried again for up to ~30 s before saying so
    for attempt in range(60):
        try:
            p = subprocess.Popen(cmd, env=env, creationflags=CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, stdout=pipe,
                                 stderr=subprocess.STDOUT if on_line else None, text=bool(on_line), errors='replace')
            break
        except PermissionError:
            if attempt == 59:
                sys.exit(f'Windows does not let {cmd[0]} start (access denied): most likely the antivirus blocks it. '
                         f'Allow the UltraTears folder in the antivirus and start the preparation again.')
            time.sleep(0.5)
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
