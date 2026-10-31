"""Build UltraTears from the player's own dump, in one command: every step is checked and can be resumed.

From a Git checkout the build goes to dist\\, or to --output <folder>: the tracked source files are copied there. In a
builder package or a source folder without Git setup runs in place. Build tools this PC lacks are downloaded into
deps\\ from where they are published (tools/deps_install.py, pinned in tools/deps.json); a builder package has them.
usage: python tools/setup.py [--output <build folder>] [--dump <folder with exefs\\ and romfs\\>] [--cpus N]
       python tools/setup.py --check-dump <folder> [--json]   (is it a usable dump? for the launcher's folder screens)
--json (the launcher): events on stdout, one JSON object a line, among the plain log lines: {"ev": "plan", "steps":
[{"id", "weight"}]} (the share of a build from nothing, in %), {"ev": "step", "id", "state": "run" / "done" / "skip",
"secs", "note"}, {"ev": "progress", "label", "done", "total", "eta"} (of the step running), {"ev": "hw", "cases"}
(tools/hw_check.py), {"ev": "ask", "id": "hw"} (answered by a line "yes" / "no" on stdin), {"ev": "error", "id",
"code", "args", "message"}, {"ev": "done", "secs"}; --check-dump: {"ev": "dump", "ok", "code", "args", "path"}.
The dump is game\\ in the checkout/build folder unless --dump names another (remembered for later runs); run it again
to resume, or to rebuild what changed after an update of the port.
The dump: the ExeFS and RomFS of The Legend of Zelda: Tears of the Kingdom with update 1.4.3 applied, as plain files,
extracted by the player from their own console. Encrypted packages (.nsp, .xci, .nca, ...) are refused: the port
decrypts nothing and reads no keys (README.md, Legal). Optional: <dump>\\sysdata\\fonts\\*.bfttf, the console's shared fonts.

Steps (data\\ holds everything made from the dump; it never leaves this PC):
  check     the tools, the dump's version (SHA-256 of the code modules) and its files
  romfs     the RomFS folder packed into one image, data\\romfs (its content hash checked against 1.4.3)
  lift      the game's code translated to C, data\\generated
  compile   the C compiled and linked: UltraTears.exe (objects in build\\)
  shaders   every shader of the game found and translated to SPIR-V, data\\shaders
  textures  the ASTC textures re-encoded for PC graphics cards (BC7 / BC1), data\\textures
  finish    ultratears.cfg
"""
import argparse
import filecmp
import hashlib
import importlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import deps_install  # noqa: E402
import progress  # noqa: E402
import toolchain  # noqa: E402
import public_tree  # noqa: E402
import version  # noqa: E402

DATA = ROOT / 'data'
STATE = DATA / 'setup.json'
# The version the port is made for: TOTK 1.4.3 (facts about the player's files, no game data).
EXEFS = {
    'main': 'e84a90d551fed0824b4b74933b9c6d043c458521363a7cea58b2faca1822dc9e',
    'subsdk0': '767c35bcd7d23e78f9063c69b638290b8b83f91e839beb10799e6576960179b1',
    'sdk': '1d187f29784cd0af46e9e123fc2a62c77b3b07c5a39c8633d90d6c80de64e4ad',
}
ROMFS_FILES = 304243
ROMFS_BYTES = 17026725026
ROMFS_CONTENT = 'b2a2e966a06158e098c03e564f77fd72809bc57275f04eb0725bcde14d522056'
ENCRYPTED = ('.nsp', '.nsz', '.xci', '.xcz', '.nca', '.ncz')
KEEP = {'game', 'data', 'build', 'userdata', 'diag', 'deps', 'dist', 'dev-local'}
IS_LINUX = sys.platform.startswith('linux')
EXE_NAME = 'ultratears' if IS_LINUX else 'UltraTears.exe'
XLATE_NAME = 'ultrs-shaders' if IS_LINUX else 'ultrs-shaders.exe'
# Starts the game with the statistics lines in ultratears.log (UltraTears.exe --debug): what a player runs when asked
# for a performance log.
DEBUG_BAT = 'UltraTears (debug).bat'
DEBUG_BAT_TEXT = '''@echo off
rem UltraTears with the statistics lines in ultratears.log (frame timing, memory): for bug and performance reports.
cd /d "%~dp0"
start "" UltraTears.exe --debug
'''
OUTPUT_OWN = {'setup.bat', 'VERSION.txt', 'THIRD-PARTY-NOTICES.md', 'licenses', 'ultratears.cfg', 'UltraTears.exe',
              'UltraTears.pdb', 'setup-files.txt', DEBUG_BAT,
              'setup.sh', 'ultratears'}


class Fail(Exception):
    def __init__(self, message, code='failed', **args):
        """code (and args): for the launcher, which words the failure in the player's language (setup.py --json)."""
        super().__init__(message)
        self.code, self.fields = code, args


ASKED = 0.0   # seconds spent waiting for the player's answers: not build time (a build "took 46 min" with 28 of them
              # at the question whether to delete the dump, 2026-10-29)


def ask(question, ask_id=None):
    """The player's answer on a console ('' without one, or on Ctrl+C), lower case. --json: an "ask" event, the answer
    a line on stdin from the launcher."""
    global ASKED
    if progress.JSON and ask_id:
        t0 = time.time()
        progress.emit('ask', id=ask_id)
        line = sys.stdin.readline() if sys.stdin else ''
        ASKED += time.time() - t0
        return line.strip().lower()
    if not (sys.stdin and sys.stdin.isatty()):
        return ''
    t0 = time.time()
    try:
        return input(question).strip().lower()
    except (EOFError, KeyboardInterrupt):
        say('')
        return ''
    finally:
        ASKED += time.time() - t0


def say(msg):
    print(msg, flush=True)


def is_checkout():
    try:
        r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
    except OSError:   # git not installed: a builder package, not a checkout
        return False
    return r.returncode == 0 and pathlib.Path(r.stdout.strip()).resolve() == ROOT


def source_files():
    out = subprocess.run(['git', '-C', str(ROOT), 'ls-files', '-z'], check=True, capture_output=True).stdout
    files = [f for f in out.decode('utf-8').split('\0') if f]
    return [f for f in files if f.split('/')[0] not in public_tree.PRIVATE and (ROOT / f).is_file()]


def sync_source(home):
    """Copy tracked files, including uncommitted edits, while preserving the build folder's own files."""
    if home == ROOT:
        return 0, 0
    home.mkdir(parents=True, exist_ok=True)
    files = source_files()
    if any(f.split('/')[0] in KEEP for f in files):
        raise Fail('a tracked path would overwrite the build folder\'s own data: refusing')
    changed = 0
    for f in files:
        src, dst = ROOT / f, home / f
        if dst.is_file() and filecmp.cmp(src, dst, shallow=False):
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        changed += 1
    listing = home / 'setup-files.txt'
    old = set(listing.read_text(encoding='utf-8').splitlines()) if listing.is_file() else set()
    removed = 0
    for f in sorted(old - set(files)):
        p = home / f
        if f.split('/')[0] not in KEEP | OUTPUT_OWN and p.is_file():
            p.unlink()
            removed += 1
            for d in p.parents:
                if d == home or any(d.iterdir()):
                    break
                d.rmdir()
    listing.write_text('\n'.join(files) + '\n', encoding='utf-8')
    v, commit = version.current()
    (home / 'VERSION.txt').write_text(version.version_txt(v, commit), encoding='utf-8')
    return changed, removed


def own_python(home):
    """The Python the build runs on (3.14 with lz4, capstone, unicorn): this PC's when it will do, else installed into
    home\\deps\\python first."""
    py = deps_install.python_for(home)
    if py is None:
        deps_install.install_python(home)
        py = deps_install.python_for(home)
    return py


def choose_dump(home, requested):
    if requested:
        return requested.resolve()
    for candidate in (home / 'game', ROOT / 'game'):
        if (candidate / 'exefs').is_dir() or (candidate / 'romfs').is_dir():
            return candidate.resolve()
    try:   # the dump an earlier --dump gave for this output folder
        remembered = json.loads((home / 'data/setup.json').read_text()).get('dump')
    except (OSError, ValueError):
        remembered = None
    if remembered and pathlib.Path(remembered).is_dir():
        return pathlib.Path(remembered)
    return (home / 'game').resolve()


def build_from_checkout(a):
    home = (a.output or ROOT / 'dist').resolve()
    try:
        changed, removed = sync_source(home)
        say(f'UltraTears source -> {home}: {changed} files updated, {removed} removed')
        py = own_python(home)
    except (Fail, deps_install.DepsError) as e:
        say(f'\nERROR preparing the build folder: {e}')
        return 1
    cmd = [str(py), str(home / 'tools/setup.py'), '--in-place', '--cpus', str(a.cpus),
           '--dump', str(choose_dump(home, a.dump))]
    if a.only:
        cmd += ['--only', a.only]
    if a.keep_shader_work:
        cmd += ['--keep-shader-work']
    if a.json:
        cmd += ['--json']
    return subprocess.run(cmd, cwd=home, env=dict(os.environ, PYTHONNOUSERSITE='1')).returncode


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def tree_hash(paths):
    """A stamp of source files: when it changes, the step that uses them runs again."""
    h = hashlib.sha256()
    for p in sorted(paths):
        h.update(str(p.relative_to(ROOT)).encode() + b'\0' + p.read_bytes())
    return h.hexdigest()[:16]


def run(cmd, what):
    t0 = time.time()
    rc = subprocess.run([str(c) for c in cmd], cwd=ROOT).returncode
    if rc:
        raise Fail(f'{what} failed (exit code {rc}); the lines above say why')
    return time.time() - t0


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state):
    DATA.mkdir(exist_ok=True)
    STATE.write_text(json.dumps(state, indent=1))


# ---------------------------------------------------------------- steps
def step_check(dump, state):
    if sys.version_info < (3, 14):
        raise Fail(f'Python 3.14 or newer is needed (this is {sys.version.split()[0]})')
    for mod in ('lz4', 'capstone', 'unicorn'):
        try:
            __import__(mod)
        except ImportError:
            raise Fail(f'the Python package {mod} is missing from {sys.executable}: delete the deps folder and run '
                       'setup again to reinstall the build tools')
    toolchain.env()
    hardware()
    if toolchain.XLATE is None:
        raise Fail(f'the shader translator deps/shaders/{XLATE_NAME} is missing: run setup again to build it')
    if packed(state) and all(ok_module(DATA / 'exefs' / n, h) for n, h in EXEFS.items()):
        return 'Tears of the Kingdom 1.4.3, already taken in (data\\exefs, data\\romfs): the dump is no longer needed'
    check_dump(dump)
    return 'Tears of the Kingdom 1.4.3 (the code modules match)'


def hardware():
    """What this PC lacks (tools/hw_check.py), before an hour of building: not enough disk space stops the build; a card
    the game cannot run on asks whether to build anyway (ULTRS_SKIP_GPU_CHECK=1: no graphics question)."""
    import hw_check
    cases = hw_check.cases(ROOT)
    if os.environ.get('ULTRS_SKIP_GPU_CHECK'):
        cases = [c for c in cases if c['sev'] != 'stop']
    if progress.JSON:
        progress.emit('hw', cases=cases)
    for c in cases:
        say(f'  {c["sev"]}: {hw_check.text(c)}')
    block = [c for c in cases if c['sev'] == 'block']
    if block:
        raise Fail(hw_check.text(block[0]), block[0]['code'], **block[0]['args'])
    if any(c['sev'] == 'stop' for c in cases) and ask('  Build anyway? [y/N] ', 'hw') not in ('y', 'yes', 'д', 'да'):
        raise Fail('stopped at the hardware check (ULTRS_SKIP_GPU_CHECK=1 builds without asking)', 'hw_declined')
    if not cases:
        say('  hardware: everything the game needs is here')


def find_dump(path):
    """The folder with exefs\\ and romfs\\ for what the player gave: the folder itself, one of its subfolders (the
    folder above the dump was dropped) or the folder above it (exefs or romfs itself was dropped). None if none."""
    path = pathlib.Path(path)
    if not path.is_dir():
        return None
    for c in [path, path.parent] + sorted(p for p in path.iterdir() if p.is_dir()):
        if (c / 'exefs').is_dir() and (c / 'romfs').is_dir():
            return c
    return None


def check_dump(dump):
    """Is dump the extracted ExeFS and RomFS of TOTK 1.4.3? Fail with a code for the launcher when not."""
    if not dump.is_dir():
        raise Fail(f'{dump} does not exist: put your extracted game there (game\\exefs and game\\romfs), '
                   'or give its folder with --dump', 'dump_missing', path=str(dump))
    for p in list(dump.iterdir()) + list((dump / 'exefs').glob('*') if (dump / 'exefs').is_dir() else []):
        if p.suffix.lower() in ENCRYPTED:
            raise Fail(f'{p.name}: encrypted packages are not accepted. Give the extracted exefs and romfs folders '
                       '(this project decrypts nothing and reads no keys)', 'dump_encrypted', file=p.name)
    exefs, romfs = dump / 'exefs', dump / 'romfs'
    if not exefs.is_dir() or not romfs.is_dir():
        raise Fail(f'{dump} must hold the folders exefs and romfs (extracted, as plain files)', 'dump_no_folders')
    for name, want in EXEFS.items():
        f = exefs / name
        if not f.is_file():
            raise Fail(f'exefs\\{name} is missing', 'dump_incomplete', file=f'exefs/{name}')
        if sha256(f) != want:
            raise Fail(f'exefs\\{name} is not the one of Tears of the Kingdom 1.4.3. Extract the ExeFS again with '
                       'update 1.4.3 applied (the base game 1.0.0 and other updates are not supported)', 'dump_version')
    if not (romfs / 'Pack/ZsDic.pack.zs').is_file():
        raise Fail(f'{romfs} does not look like the RomFS of Tears of the Kingdom (Pack\\ZsDic.pack.zs is missing)',
                   'dump_not_totk')


def packed(state):
    out = DATA / 'romfs'
    return state.get('romfs') == ROMFS_CONTENT and (out / 'romfs.bin').is_file() and (out / 'runtime-index.bin').is_file()


def ok_module(path, want):
    return path.is_file() and sha256(path) == want


def step_romfs(dump, state, cpus):
    out = DATA / 'romfs'
    if packed(state):
        return 'already packed'
    from pack_romfs import pack
    n, size, content = pack(dump / 'romfs', out, threads=16, progress=say)
    if n != ROMFS_FILES or size < ROMFS_BYTES or content != ROMFS_CONTENT:
        raise Fail(f'the RomFS is not the one of Tears of the Kingdom 1.4.3 ({n} files, expected {ROMFS_FILES}; '
                   'content differs). Extract the RomFS again with update 1.4.3 applied; mods do not go here',
                   'romfs_version')
    state['romfs'] = content
    return f'{n} files, {size / 2**30:.1f} GiB, content verified'


def step_lift(dump, state, cpus):
    exefs = DATA / 'exefs'
    exefs.mkdir(parents=True, exist_ok=True)
    for name in list(EXEFS) + ['main.npdm']:   # the code modules are taken in, so the dump can go after the build
        src = dump / 'exefs' / name
        if src.is_file() and (not (exefs / name).is_file() or (exefs / name).stat().st_size != src.stat().st_size):
            shutil.copyfile(src, exefs / name)
    stamp = tree_hash(list((ROOT / 'tools/lifter').glob('*.py')) + [ROOT / 'tools/gen_game.py'])
    if state.get('lift') == stamp and (DATA / 'generated/main').is_dir():
        return 'up to date'
    run([sys.executable, 'tools/gen_game.py', exefs, DATA / 'generated'], 'translating the code')
    state['lift'] = stamp
    return 'done'


# Ninja rebuilds what is older than its sources by file dates. A newer source ZIP unpacked over the folder (GitHub's
# Download ZIP, no Git) dates every file anew: the whole game compiled again, ~30 min on 6 cores, though a few files
# had changed (2026-10-29). The sources' content and date are recorded after each compile; before the steps, a file
# whose content is unchanged gets its recorded date back, so only what changed is compiled (as after a git pull).
SOURCE_DATES = DATA / 'source-dates.json'
SOURCE_DIRS = ('src', 'tools/bc7build', 'assets')   # assets: the icon compiled into the exe


def compiled_sources():
    for d in SOURCE_DIRS:
        for p in sorted((ROOT / d).rglob('*')):
            if p.is_file():
                yield p


def keep_source_dates():
    """Unchanged sources back to their recorded dates (how many); changed or new ones dated now. A package extracted
    over an older one dates its files by the commit (the zip's times): a changed file older than the object of the last
    build would not be compiled again by ninja, and the game would stay the old one."""
    try:
        old = json.loads(SOURCE_DATES.read_text())
    except (OSError, ValueError):
        return 0
    n = 0
    for p in compiled_sources():
        rec = old.get(p.relative_to(ROOT).as_posix())
        st = p.stat()
        if rec and rec[1] == st.st_mtime_ns:
            continue
        if rec and sha256(p) == rec[0]:
            os.utime(p, ns=(st.st_atime_ns, rec[1]))
            n += 1
        else:
            os.utime(p)   # now: newer than any object built before
    return n


def record_source_dates():
    try:
        old = json.loads(SOURCE_DATES.read_text())
    except (OSError, ValueError):
        old = {}
    new = {}
    for p in compiled_sources():
        rel, mtime = p.relative_to(ROOT).as_posix(), p.stat().st_mtime_ns
        rec = old.get(rel)
        new[rel] = [rec[0] if rec and rec[1] == mtime else sha256(p), mtime]
    DATA.mkdir(exist_ok=True)
    SOURCE_DATES.write_text(json.dumps(new))


def step_compile(dump, state, cpus):
    run([sys.executable, 'tools/build_game.py', '--generated', DATA / 'generated', '--target', 'ultratears', '--cpus', cpus],
        'compiling')
    record_source_dates()
    return EXE_NAME


# The game reads the translated shaders from one file, data/shaders/translated.pack (issue #22: ~57,000 files before).
# They are found in the RomFS and translated in data/shaders/work (code taken out of the game, the translator's jobs and
# output: ~143,000 files), packed, and the work folder is removed (--keep-shader-work keeps it, for development).
# A pack from the same export and translator is up to date; an install from before the pack (data/shaders/ryujinx and
# its neighbours) is packed as it is, without translating again.
KEEP_SHADER_WORK = False
SHADER_OLD = ['raw', 'ryujinx', 'ryujinx-code', 'jobs.jsonl', 'shaders.tsv', 'ryujinx-results.tsv', 'ryujinx-results-new.tsv']


def step_shaders(dump, state, cpus):
    out = DATA / 'shaders'
    pack = out / 'translated.pack'
    export = tree_hash([ROOT / 'tools/shaders/inventory.py', ROOT / 'tools/export_shaders.py'])
    xlate = sha256(toolchain.XLATE)[:16]
    if state.get('shaders-pack') == f'{export}/{xlate}' and pack.is_file():
        return 'up to date'
    old = out / 'ryujinx'
    if old.is_dir() and state.get('shaders-xlate') == xlate and not (out / 'work').exists():
        run([sys.executable, 'tools/pack_shaders.py', old, pack], 'packing the shaders')
    else:
        work = out / 'work'
        if state.get('shaders-export') != export or not (work / 'jobs.jsonl').is_file():
            run([sys.executable, 'tools/export_shaders.py', DATA / 'romfs', work, DATA / 'exefs'], 'finding the shaders')
            state['shaders-export'] = export
            save_state(state)
        new_only = ['--new-only'] if state.get('shaders-xlate') == xlate else []
        run([sys.executable, 'tools/run_ryujinx_xlate.py', work, '--workers', min(cpus, 32), '--quiet'] + new_only,
            'translating the shaders')
        state['shaders-xlate'] = xlate
        save_state(state)
        run([sys.executable, 'tools/pack_shaders.py', work / 'translated', pack], 'packing the shaders')
        if not KEEP_SHADER_WORK:
            shutil.rmtree(work)
    for name in SHADER_OLD:   # the layout from before the pack
        p = out / name
        if p.is_dir():
            shutil.rmtree(p)
        elif p.is_file():
            p.unlink()
    state['shaders-export'] = export
    state['shaders-pack'] = f'{export}/{xlate}'
    return 'done'


def step_textures(dump, state, cpus):
    # Up to date when the encoder, its settings and the RomFS are those of the last complete run: it read and checked
    # all 44 000 texture files again on every run (minutes), though it encoded nothing new.
    from bc7_build import SOURCES_C, SOURCES_CXX
    stamp = tree_hash([ROOT / s for s in SOURCES_C + SOURCES_CXX] + [ROOT / 'tools/bc7_build.py']) + '/' + state.get('romfs', '')
    if state.get('textures') == stamp and (DATA / 'textures/bc7.idx').is_file():
        return 'up to date'
    run([sys.executable, 'tools/bc7_build.py', '--romfs', DATA / 'romfs', '--cache', DATA / 'textures', '--threads',
         min(cpus, 64)], 're-encoding the textures')
    state['textures'] = stamp
    return 'done'


def step_finish(dump, state, cpus):
    fonts = dump / 'sysdata/fonts'
    lines = ['# Written by tools/setup.py: where UltraTears finds its data (relative to this folder).',
             'exefs=data/exefs', 'romfs=data/romfs', 'shaderpack=data/shaders/translated.pack', 'compiled=data/shaders/compiled.bin',
             'textures=data/textures',
             'saves=userdata']
    if fonts.is_dir() and list(fonts.glob('*.bfttf')):
        shutil.copytree(fonts, DATA / 'fonts', dirs_exist_ok=True)
    lines.append('fonts=data/fonts')
    (ROOT / 'ultratears.cfg').write_text('\n'.join(lines) + '\n')
    if not IS_LINUX:
        (ROOT / DEBUG_BAT).write_bytes(DEBUG_BAT_TEXT.replace('\n', '\r\n').encode())
    (ROOT / f'build/{EXE_NAME}').unlink(missing_ok=True)   # where builds before 0.1.0 linked it
    if (dump / 'romfs').is_dir():
        say(f'  The game reads data\\romfs and data\\exefs now: {dump} (16 GB) is no longer needed.')
        if dump.resolve() == (ROOT / 'game').resolve():
            if ask('  Delete game\\exefs and game\\romfs to free 16 GB? [y/N] ') in ('y', 'yes', 'д', 'да'):
                for sub in ('exefs', 'romfs'):
                    shutil.rmtree(dump / sub, ignore_errors=True)
                say('  deleted')
    # the package this game was built by (its VERSION.txt line): the launcher offers Play while they match, "Update the
    # game" once an update brought another one; kept here, with the build, whichever launcher looks at the folder
    stamp = ROOT / 'VERSION.txt'
    state['package'] = stamp.read_text(encoding='utf-8').strip() if stamp.is_file() else ''
    return 'ultratears.cfg ready'


STEPS = [('check', step_check), ('romfs', step_romfs), ('lift', step_lift), ('compile', step_compile),
         ('shaders', step_shaders), ('textures', step_textures), ('finish', step_finish)]


def no_quick_edit():
    """Windows' console stops a program whose output it is selecting (QuickEdit: a click into the window starts a
    selection, the title says "Select"): a player's build sat for 5 minutes at 0 % CPU in the RomFS step
    (2026-10-29). Off while setup runs, for this console and the tools it starts; the player's setting comes back at exit."""
    if os.name != 'nt':
        return
    import atexit
    import ctypes
    k32 = ctypes.windll.kernel32
    handle = k32.GetStdHandle(-10)   # STD_INPUT_HANDLE
    mode = ctypes.c_uint32()
    if not handle or not k32.GetConsoleMode(handle, ctypes.byref(mode)):
        return   # not a console (a pipe, a file)
    QUICK_EDIT, EXTENDED = 0x40, 0x80
    if k32.SetConsoleMode(handle, (mode.value & ~QUICK_EDIT) | EXTENDED):
        atexit.register(k32.SetConsoleMode, handle, mode.value)


def main():
    ap = argparse.ArgumentParser(description='Build UltraTears from your own dump of the game.')
    ap.add_argument('--output', type=pathlib.Path,
                    help='build folder when run from a Git checkout (default: dist here)')
    ap.add_argument('--dump', type=pathlib.Path,
                    help='the folder with exefs\\ and romfs\\ (default: game\\ here; remembered for later runs)')
    ap.add_argument('--cpus', type=int, default=os.cpu_count() or 4,
                    help='logical CPUs the build may use (default: all; it runs at low priority)')
    ap.add_argument('--only', choices=[s for s, _ in STEPS], help='run one step only')
    ap.add_argument('--keep-shader-work', action='store_true', help='keep data/shaders/work after packing (development)')
    ap.add_argument('--in-place', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--json', action='store_true', help='events for the launcher on stdout (see the top of this file)')
    ap.add_argument('--check-dump', type=pathlib.Path, metavar='FOLDER', help='only say whether FOLDER is a usable dump')
    a = ap.parse_args()
    global KEEP_SHADER_WORK
    KEEP_SHADER_WORK = a.keep_shader_work
    if a.json:   # the tools setup starts read it too (tools/progress.py); UTF-8 whatever the console's code page
        os.environ['ULTRS_JSON'] = '1'
        os.environ['PYTHONIOENCODING'] = 'utf-8'
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        progress.JSON = True
    if a.check_dump:
        sys.exit(check_dump_only(a.check_dump))
    no_quick_edit()
    if is_checkout() and not a.in_place:
        sys.exit(build_from_checkout(a))
    if a.output:
        ap.error('--output is used only when setup.py is run from a Git checkout')
    # The build runs on a Python 3.14 with the build's packages (deps_install.python_for), whichever Python started
    # setup: a source folder without Git (GitHub's Download ZIP) went on with the player's own, which may lack them or
    # be a version they do not install on. Then the rest of the tools, installed when this PC lacks them.
    if progress.JSON and not os.environ.get('ULTRATEARS_SETUP_OWN_PYTHON'):
        progress.emit('plan', steps=[{'id': s, 'weight': w} for s, w in STEP_WEIGHTS.items()])
        progress.emit('step', id='tools', state='run')
    t_tools = time.time()
    try:
        py = own_python(ROOT)
    except deps_install.DepsError as e:
        fail_tools(e)
    if pathlib.Path(sys.executable).resolve() != py.resolve() and not os.environ.get('ULTRATEARS_SETUP_OWN_PYTHON'):
        env = dict(os.environ, PYTHONNOUSERSITE='1', ULTRATEARS_SETUP_OWN_PYTHON='1')
        sys.exit(subprocess.run([str(py), str(ROOT / 'tools/setup.py')] + sys.argv[1:], cwd=ROOT, env=env).returncode)
    try:
        deps_install.ensure_tools(ROOT)
    except deps_install.DepsError as e:
        fail_tools(e)
    if progress.JSON:
        progress.emit('step', id='tools', state='done', secs=round(time.time() - t_tools))
    awake()
    importlib.reload(toolchain)
    if hasattr(os, 'nice'):   # (Linux: the build below the desktop's priority)
        try:
            os.nice(10)
        except OSError:
            pass
    state = load_state()
    if a.dump:
        state['dump'] = str(a.dump.resolve())
    if not state.get('dump'):
        state['dump'] = str(ROOT / 'game')
    dump = pathlib.Path(state['dump'])
    say(f'UltraTears setup: dump {dump}, {a.cpus} CPUs')
    t_all = time.time()
    kept = keep_source_dates()
    if kept:
        say(f'{kept} unchanged source files keep their dates (only changed ones are compiled again)')
    for i, (name, fn) in enumerate(STEPS, 1):
        if a.only and name != a.only and name != 'check':
            continue
        say(f'[{i}/{len(STEPS)}] {name}' + (f'  ({progress.clock(time.time() - t_all - ASKED)} since the start)' if i > 1 else ''))
        if progress.JSON:
            progress.emit('step', id=name, state='run')
        t0, asked0 = time.time(), ASKED
        try:
            result = fn(dump, state) if name == 'check' else fn(dump, state, a.cpus)
        except Fail as e:
            say(f'\nERROR in step "{name}": {e}')
            if progress.JSON:
                progress.emit('error', id=name, code=e.code, args=e.fields, message=str(e))
            save_state(state)
            sys.exit(1)
        save_state(state)
        secs = time.time() - t0 - (ASKED - asked0)
        say(f'  {result or "ok"} ({progress.clock(secs)})')
        if progress.JSON:
            skipped = result in ('up to date', 'already packed')   # the step had nothing to do (an update, a resume)
            progress.emit('step', id=name, state='skip' if skipped else 'done', secs=round(secs), note=result or '')
    say(f'\nDone in {progress.clock(time.time() - t_all - ASKED)}. Start the game: {EXE_NAME} in {ROOT}')
    if progress.JSON:
        progress.emit('done', secs=round(time.time() - t_all - ASKED))


# The share of each step in a build from nothing, in % (a clean 0.2.0 package on 8 CPUs, 2026-10-30: romfs 2:19, lift
# 1:19, compile 15:39, shaders 2:54, textures 10:08; tools: a source build's first download). The launcher's overall bar
# weighs each step's own progress by these; steps already done count in full.
STEP_WEIGHTS = {'tools': 2, 'check': 1, 'romfs': 7, 'lift': 4, 'compile': 47, 'shaders': 9, 'textures': 30, 'finish': 0}


def fail_tools(e):
    say(f'\nERROR preparing the build tools: {e}')
    if progress.JSON:
        progress.emit('error', id='tools', code='tools', args={}, message=str(e))
    sys.exit(1)


def awake():
    """The PC does not go to sleep while setup runs (an hour of building on a laptop); the display may still turn off."""
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001)   # ES_CONTINUOUS | ES_SYSTEM_REQUIRED


def check_dump_only(path):
    """--check-dump: whether path (or the folder above or below it) holds a usable dump, without building."""
    path = pathlib.Path(path).resolve()
    found = find_dump(path)
    try:
        check_dump(found or pathlib.Path(path))
        ok, code, fields, message = True, 'ok', {}, 'Tears of the Kingdom 1.4.3'
    except Fail as e:
        ok, code, fields, message = False, e.code, e.fields, str(e)
    if progress.JSON:
        progress.emit('dump', ok=ok, code=code, args=fields, path=str(found or path), message=message)
    else:
        say(f'{found or path}: {message}')
    return 0 if ok else 1


if __name__ == '__main__':
    main()
