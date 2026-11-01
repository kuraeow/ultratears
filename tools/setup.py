"""Build UltraTears from the player's own dump, in one command: every step is checked and can be resumed.

From a Git checkout the build goes to dist\\, or to --output <folder>: the tracked source files are copied there. In a
builder package or a source folder without Git setup runs in place. Build tools this PC lacks are downloaded into
deps\\ from where they are published (tools/deps_install.py, pinned in tools/deps.json); a builder package has them.
usage: python tools/setup.py [--output <build folder>] [--dump <folder with exefs\\ and romfs\\>] [--cpus N] [--full]
                             [--platform windows|linux|both]   (linux: for the Steam Deck; remembered)
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
  graphics  the shaders compiled by this PC's graphics driver (UltraTears --prepare-shaders), data\\shaders\\compiled.bin
  textures  the ASTC textures re-encoded for PC graphics cards (BC7 / BC1), data\\textures
  finish    ultratears.cfg
  deck      --platform linux / both: UltraTears-SteamDeck.tar, the game for the Steam Deck to carry there (the game
            cross-compiled with the Linux kit, deps\\linux, in the compile step; its shaders in the shaders step)
"""
import argparse
import filecmp
import hashlib
import importlib
import json
import os
import pathlib
import re
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
# Each top-level folder of that RomFS: its file count and the first 16 hex digits of the SHA-256 over its files' SHA-256
# (tools/pack_romfs.py): a RomFS that differs says where, so a player can tell a missing update from a mod or a bad copy.
ROMFS_FOLDERS = {
    'AI': (64, 'c035f4f738d81fad'),
    'AISchedule': (5, '1d46453b7aed591e'),
    'AS': (1034, '204fb80cf730fed0'),
    'ActorSystem': (6, '1d3867f33dd70e89'),
    'AnimationEvent': (921, 'bd574aae552a598e'),
    'BSA': (24, '6dab4e2923f6e487'),
    'Bake': (10943, 'c89d7a87ae3a8cf8'),
    'Banc': (137685, '1f986d2d4a47fb95'),
    'CameraAnimation': (193, '9328b7ca4a540be6'),
    'Cave': (3939, '36ee73a65f1cfbaa'),
    'CaveBounding': (219, '8f4d163db50339f2'),
    'Chemical': (16, 'c90428faea06cae8'),
    'Component': (1007, 'ae19ad4dfec2d900'),
    'Cooking': (2, 'f83b3aff9d7de8e6'),
    'Cull': (856, 'ff4a96007701c7ba'),
    'ELink2': (1, '32bdfdb0f7cc7e5e'),
    'Ecosystem': (10, '771b098a08b60d8a'),
    'Effect': (1976, '000f6696e260faef'),
    'Env': (1, '7073ddf5b0110627'),
    'Event': (2619, 'e6e794cb21acbc0e'),
    'Font': (4, '4a11bc7ac1afa62f'),
    'Forest': (20, '0ec3197c86c19657'),
    'Game': (418, '57db42d4059a53a4'),
    'GameBalance': (3, '169acb88adec2cee'),
    'GameData': (2, 'fd3ab9974e326d28'),
    'GameParameter': (27, '72079d643ec0b896'),
    'GrassStats': (2671, 'f7955622d7007a2e'),
    'Lib': (4, '85811e5f7b5632c9'),
    'Life': (14, '5e8eaaa234743ef3'),
    'Logic': (3204, '1078fb82c78f6d2f'),
    'Mals': (15, '9fbf72f91d067dd4'),
    'Model': (13876, 'fdf49084c1e80b9e'),
    'Pack': (15080, 'cb2049efdbf58496'),
    'Perception': (1, '62dd8a6425a656d3'),
    'Phive': (13238, 'b29dbc9a81a4d1a1'),
    'Preload': (175, 'f83a6dc3e8277975'),
    'PrequelSaveData': (1, '133553bd66d2fe2a'),
    'RSDB': (21, 'a96c2b6468d2fd65'),
    'Rumble': (1, '676c205f9520a8a4'),
    'SLink2': (1, 'd70a6075cf23f101'),
    'Scene': (166, 'afa72637d4a694df'),
    'Sequence': (200, '9db5d92da114c41d'),
    'Shader': (13, '76af703897221bb0'),
    'Sound': (23590, '791347a293dade7a'),
    'System': (145, 'fc1726ec5d981e35'),
    'TerrainArc': (10682, '75ddaf9ae639a814'),
    'TexToGo': (29356, '4f8796dfcf721d4c'),
    'UI': (15084, '472b6bb5bacac4dc'),
    'UMii': (1, 'c00943fc8f406fed'),
    'Voice': (13607, '98968775785869cb'),
    'VolumeStats': (1096, '379222973678882b'),
    'WorldMgr': (6, '7dade0ea2eeca301'),
}
ENCRYPTED = ('.nsp', '.nsz', '.xci', '.xcz', '.nca', '.ncz')
KEEP = {'game', 'dump', 'data', 'build', 'userdata', 'diag', 'deps', 'dist', 'release', 'dev-local'}
# Written by builds before the launcher (UltraTears.exe --debug); the launcher's Debug mode (debug=1 in ultratears.cfg)
# replaces it: removed when found.
DEBUG_BAT = 'UltraTears (debug).bat'
OUTPUT_OWN = {'setup.bat', 'VERSION.txt', 'THIRD-PARTY-NOTICES.md', 'licenses', 'ultratears.cfg', 'UltraTears.exe',
              'UltraTears.pdb', 'setup-files.txt', DEBUG_BAT, 'UltraTears Launcher.exe', 'ultratears-launcher', 'libSDL3.so.0'}


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
    """--dump, else the one an earlier --dump gave for this output folder, else the checkout's dump/ (game/ where the
    instructions before 0.2.0 had it)."""
    if requested:
        return requested.resolve()
    try:
        remembered = json.loads((home / 'data/setup.json').read_text()).get('dump')
    except (OSError, ValueError):
        remembered = None
    if remembered and pathlib.Path(remembered).is_dir():
        return pathlib.Path(remembered)
    for candidate in (ROOT / 'dump', ROOT / 'game'):
        if (candidate / 'exefs').is_dir() or (candidate / 'romfs').is_dir():
            return candidate.resolve()
    return (ROOT / 'dump').resolve()


def build_from_checkout(a):
    home = (a.output or ROOT / 'build').resolve()
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
    if a.full:
        cmd += ['--full']
    if a.platform:
        cmd += ['--platform', a.platform]
    env = dict(os.environ, PYTHONNOUSERSITE='1')
    rc = subprocess.run(cmd, cwd=home, env=env).returncode
    if rc or a.only:
        return rc
    return build_launcher_into(home, py, env)


def build_launcher_into(home, py, env):
    """The launcher beside the game, as a builder package has it (it comes built there): so a game folder made from the
    source is the same as a player's, Play, settings and updates included (tools/build_launcher.py; its intermediate
    files in build/launcher), for this system. With the checkout's own compiler when it has one (a developer's MSYS2):
    an output folder may hold a builder package's compiler, trimmed to what the game reads, without the C++ headers the
    launcher needs; else with the output folder's, which a source build installs whole."""
    linux = os.name != 'nt'
    out = home / ('build-linux/launcher' if linux else 'build/launcher')
    say('\nThe launcher')
    own = toolchain.CLANG64 is not None
    base, python = (ROOT, sys.executable) if own else (home, str(py))
    cmd = [python, str(base / 'tools/build_launcher.py'), '--out', str(out)] + (['--platform', 'linux'] if linux else [])
    if subprocess.run(cmd, cwd=base, env=env).returncode:
        say('  WARNING: the launcher was not built; the game itself is ready (UltraTears.exe / linux/ultratears)')
        return 1
    names = ['ultratears-launcher'] if linux else ['UltraTears Launcher.exe']
    for name in names:
        shutil.copy2(out / name, home / name)
    if linux:   # the SDL3 it loads (rpath $ORIGIN), as the Linux package has it
        shutil.copy2(home / 'linux/libSDL3.so.0', home / 'libSDL3.so.0')
    say(f'  {home / names[0]}')
    return 0


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
    hardware(state.get('platforms', ['windows']))
    if toolchain.XLATE is None:
        raise Fail('the shader translator (deps/shaders) is missing: run setup again to build it')
    if packed(state) and all(ok_module(DATA / 'exefs' / n, h) for n, h in EXEFS.items()):
        return 'Tears of the Kingdom 1.4.3, already taken in (data\\exefs, data\\romfs): the dump is no longer needed'
    check_dump(dump)
    return 'Tears of the Kingdom 1.4.3 (the code modules match)'


def hardware(platforms):
    """What this PC lacks (tools/hw_check.py), before an hour of building: not enough disk space stops the build; a card
    the game cannot run on asks whether to build anyway (ULTRS_SKIP_GPU_CHECK=1: no graphics question)."""
    import hw_check
    cases = hw_check.cases(ROOT, platforms)
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
    folders = {}
    n, size, content = pack(dump / 'romfs', out, threads=16, progress=say, folders=folders)
    if n != ROMFS_FILES or size < ROMFS_BYTES or content != ROMFS_CONTENT:
        diff = [f'{t} ({folders.get(t, (0, ""))[0]} files, expected {want[0]})' for t, want in ROMFS_FOLDERS.items()
                if folders.get(t) != want] + [f'{t} (not in 1.4.3)' for t in folders if t not in ROMFS_FOLDERS]
        raise Fail(f'the RomFS is not the one of Tears of the Kingdom 1.4.3 ({n} files, expected {ROMFS_FILES}; content '
                   f'differs in {len(diff)} of its {len(ROMFS_FOLDERS)} folders: {", ".join(diff[:12])}'
                   f'{", ..." if len(diff) > 12 else ""}). Extract the RomFS again with update 1.4.3 applied; mods do not '
                   'go here', 'romfs_version')
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
    made = []
    if 'windows' in state['platforms']:
        run([sys.executable, 'tools/build_game.py', '--generated', DATA / 'generated', '--target', 'ultratears', '--cpus',
             cpus], 'compiling')
        made.append('UltraTears.exe')
    if 'linux' in state['platforms']:   # the same C for Linux / Steam Deck, against the Linux kit (deps/linux)
        run([sys.executable, 'tools/build_game.py', '--generated', DATA / 'generated', '--target', 'ultratears', '--cpus',
             cpus, '--platform', 'linux'], 'compiling for Linux')
        made.append('linux/ultratears')
    record_source_dates()
    return ', '.join(made)


# The game reads the translated shaders from one file, data/shaders/translated.pack (issue #22: ~57,000 files before).
# They are found in the RomFS and translated in data/shaders/work (code taken out of the game, the translator's jobs and
# output: ~143,000 files), packed, and the work folder is removed (--keep-shader-work keeps it, for development).
# A pack from the same export and translator is up to date; an install from before the pack (data/shaders/ryujinx and
# its neighbours) is packed as it is, without translating again.
KEEP_SHADER_WORK = False
SHADER_OLD = ['raw', 'ryujinx', 'ryujinx-code', 'jobs.jsonl', 'shaders.tsv', 'ryujinx-results.tsv', 'ryujinx-results-new.tsv']


# Gathers on AMD and Intel round the other way from Maxwell: the translator biases their coordinates (TOTK_GATHER_BIAS,
# its QueryHostGatherBiasPrecision; 0 on NVIDIA, 8 on AMD and Intel); a changed card translates again. The Steam Deck's
# GPU is AMD's.
DECK_BIAS = '8'


def host_bias():
    if 'TOTK_GATHER_BIAS' in os.environ:
        return os.environ['TOTK_GATHER_BIAS']
    import hw_check
    return '0' if hw_check.game_gpu_vendor() in (None, 0x10de) else DECK_BIAS


def shader_packs(state):
    """(folder, gather bias, state key) of each pack this build makes: the PC's for Windows; for the Deck the PC's when
    its card is AMD or Intel, or the only one when the build is for the Deck alone, else one of its own in shaders-deck:
    the Deck gets a pack ready to play, it translates nothing (dev-local/docs/linux-deck.md, 2026-11-01)."""
    plats, host = state['platforms'], host_bias()
    if os.name != 'nt':   # built on Linux (a Steam Deck, a Linux PC): for this machine's own card
        return [(DATA / 'shaders', host, 'shaders')]
    if 'windows' not in plats:
        return [(DATA / 'shaders', DECK_BIAS, 'shaders')]
    packs = [(DATA / 'shaders', host, 'shaders')]
    if 'linux' in plats and host != DECK_BIAS:
        packs.append((DATA / 'shaders-deck', DECK_BIAS, 'deck-shaders'))
    return packs


def deck_pack(state):
    return shader_packs(state)[-1][0] / 'translated.pack'


def step_shaders(dump, state, cpus):
    export = tree_hash([ROOT / 'tools/shaders/inventory.py', ROOT / 'tools/export_shaders.py'])
    # the translator by the sources it is built from (deps/shaders/source.txt, tools/build_xlate.py): the Windows and the
    # Linux build of one source are the same translation, so a Steam Deck's folder made on a PC keeps its pack
    stamp = toolchain.XLATE.parent / 'source.txt'
    xlate_id = 'src-' + stamp.read_text().strip() if stamp.is_file() else sha256(toolchain.XLATE)[:16]
    work = DATA / 'shaders/work'
    made = []
    keep_bias = os.environ.get('TOTK_GATHER_BIAS')
    try:
        for out, bias, key in shader_packs(state):
            pack = out / 'translated.pack'
            xlate = xlate_id + ('' if bias == '0' else '-g' + bias)
            if state.get(f'{key}-pack') == f'{export}/{xlate}' and pack.is_file():
                continue
            os.environ['TOTK_GATHER_BIAS'] = bias
            old = out / 'ryujinx'
            if key == 'shaders' and old.is_dir() and state.get('shaders-xlate') == xlate and not work.exists():
                run([sys.executable, 'tools/pack_shaders.py', old, pack], 'packing the shaders')
            else:
                if state.get('shaders-export') != export or not (work / 'jobs.jsonl').is_file():
                    run([sys.executable, 'tools/export_shaders.py', DATA / 'romfs', work, DATA / 'exefs'], 'finding the shaders')
                    state['shaders-export'] = export
                    state.pop('work-xlate', None)
                    save_state(state)
                # the work folder holds one translation: of this bias when it is the one made last
                new_only = ['--new-only'] if state.get('work-xlate') == xlate else []
                run([sys.executable, 'tools/run_ryujinx_xlate.py', work, '--workers', min(cpus, 32), '--quiet'] + new_only,
                    'translating the shaders')
                state['work-xlate'] = xlate
                if key == 'shaders':
                    state['shaders-xlate'] = xlate
                save_state(state)
                out.mkdir(parents=True, exist_ok=True)
                run([sys.executable, 'tools/pack_shaders.py', work / 'translated', pack], 'packing the shaders')
            state[f'{key}-pack'] = f'{export}/{xlate}'
            save_state(state)
            made.append(out.name)
    finally:
        if keep_bias is None:
            os.environ.pop('TOTK_GATHER_BIAS', None)
        else:
            os.environ['TOTK_GATHER_BIAS'] = keep_bias
    if work.exists() and not KEEP_SHADER_WORK:
        shutil.rmtree(work)
        state.pop('work-xlate', None)
    for name in SHADER_OLD:   # the layout from before the pack
        p = DATA / 'shaders' / name
        if p.is_dir():
            shutil.rmtree(p)
        elif p.is_file():
            p.unlink()
    if 'deck-shaders' not in [k for _, _, k in shader_packs(state)]:   # the Deck uses the PC's pack now
        shutil.rmtree(DATA / 'shaders-deck', ignore_errors=True)
        state.pop('deck-shaders-pack', None)
    state['shaders-export'] = export
    return 'done: ' + ', '.join(made) if made else 'up to date'


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


def cfg_lines():
    """ultratears.cfg's own lines: where the game finds its data, relative to its folder. The same for every build (the
    Steam Deck's tar, tools/make_deck_tar.py, has them as a build on the Deck writes them)."""
    return ['# Written by tools/setup.py: where UltraTears finds its data (relative to this folder).',
            'exefs=data/exefs', 'romfs=data/romfs', 'shaderpack=data/shaders/translated.pack', 'compiled=data/shaders/compiled.bin',
            'textures=data/textures', 'saves=userdata', 'fonts=data/fonts']


def step_finish(dump, state, cpus):
    fonts = dump / 'sysdata/fonts'
    lines = cfg_lines()
    if fonts.is_dir() and list(fonts.glob('*.bfttf')):
        shutil.copytree(fonts, DATA / 'fonts', dirs_exist_ok=True)
    # Linux: UltraTears.AppImage is what the player starts (--play: the game); the start scripts of 2026-11-01 go
    for old in ('UltraTears.sh', 'UltraTears-Launcher.sh'):
        (ROOT / old).unlink(missing_ok=True)
    # the player's settings (fps=, window=, vsync=, lang=: the launcher's Play screen, or written by hand) are kept:
    # an update must not reset them
    cfg = ROOT / 'ultratears.cfg'
    ours = {l.split('=', 1)[0] for l in lines if '=' in l}
    if cfg.is_file():
        for l in cfg.read_text(encoding='utf-8', errors='replace').splitlines():
            if '=' in l and not l.startswith('#') and l.split('=', 1)[0] not in ours:
                lines.append(l)
    cfg.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    (ROOT / DEBUG_BAT).unlink(missing_ok=True)
    (ROOT / 'build/UltraTears.exe').unlink(missing_ok=True)   # where builds before 0.1.0 linked it
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


PREPARED = re.compile(r'prepare-shaders: (\d+) of (\d+)$')


def prepare_shaders(game, say_line=None):
    """The game compiles every shader with this machine's graphics driver into data/shaders/compiled.bin
    (UltraTears --prepare-shaders, src/gfx/vkb_shader.c), so the first play has no compile stalls. A driver crash kills
    it: the next run skips the shaders it was compiling (prepare-skip.txt), so it runs again, up to 3 times. Returns
    the last line it printed and its exit code."""
    last, rc = '', 0
    for _ in range(3):
        p = subprocess.Popen([str(game), '--prepare-shaders'], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding='utf-8', errors='replace')
        bar = None
        for line in p.stdout:
            line = line.rstrip()
            m = PREPARED.search(line)
            if m:
                if not bar:
                    bar = progress.Bar('compiling the shaders for this graphics card', int(m.group(2)), 'shaders')
                bar.update(int(m.group(1)))
            elif line.startswith('prepare-shaders:'):
                last = line[len('prepare-shaders:'):].strip()
                (say_line or say)('  ' + last)
        if bar:
            bar.finish()
        rc = p.wait()
        if not rc:
            break
        say(f'  the game stopped while compiling the shaders (exit code {rc}): again, without the shaders it was on')
    return last, rc


def step_graphics(dump, state, cpus):
    """The shaders compiled for this PC's graphics card (the game of this platform). The Steam Deck's tar is prepared
    on the Deck: its launcher does it before the first play (another driver)."""
    game = ROOT / ('UltraTears.exe' if os.name == 'nt' else 'linux/ultratears')
    if not game.is_file():
        return 'no game for this PC'
    last, rc = prepare_shaders(game)
    if last.startswith('ready'):   # every shader compiled before (an update that changed none)
        return 'up to date'
    if rc:   # not fatal: what is not compiled now is compiled while playing
        say(f'  WARNING: the shaders are not all prepared (exit code {rc}); the game compiles the rest while it runs')
        return 'partly: compiled while playing'
    return last or 'done'


DECK_TAR = 'UltraTears-SteamDeck.tar'


def step_deck(dump, state, cpus):
    """A build for Linux / Steam Deck: UltraTears-SteamDeck.tar in this folder, which the player carries to the Deck
    (tools/make_deck_tar.py): the game and its data, and the release's AppImage, which is the builder there (its
    launcher builds and updates the game in that folder with the tools inside it). Made again when anything in it is
    newer."""
    tar = ROOT / DECK_TAR
    if 'linux' not in state['platforms'] or os.name != 'nt':   # on Linux the game is built where it is played
        return 'not for the Steam Deck'
    pack = deck_pack(state)
    appimage = deps_install.linux_package(ROOT)
    if appimage is None:   # a build from the source without ULTRS_LINUX_PACKAGE (an AppImage): the game is built, no tar
        say('  WARNING: no AppImage (ULTRS_LINUX_PACKAGE=<folder with UltraTears.AppImage>): the Steam Deck\'s '
            'tar is not made; the game for Linux is built (linux/ultratears)')
        return 'no AppImage: no tar'
    inputs =[ROOT / 'linux/ultratears', ROOT / 'linux/libSDL3.so.0', pack, appimage,
              DATA / 'romfs/romfs.bin', DATA / 'textures/bc7.idx']
    key = f'{pack}|{appimage.name}'
    if tar.is_file() and state.get('deck-tar') == key and all(p.stat().st_mtime <= tar.stat().st_mtime for p in inputs if p.exists()):
        return 'up to date'
    run([sys.executable, 'tools/make_deck_tar.py', '--pack', pack.relative_to(ROOT), '--appimage', appimage],
        'packing the game for the Steam Deck')
    state['deck-tar'] = key
    return f'{DECK_TAR}, {tar.stat().st_size / 2**30:.1f} GiB'


STEPS = [('check', step_check), ('romfs', step_romfs), ('lift', step_lift), ('compile', step_compile),
         ('shaders', step_shaders), ('graphics', step_graphics), ('textures', step_textures), ('finish', step_finish),
         ('deck', step_deck)]


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
    ap.add_argument('--full', action='store_true',
                    help='forget what was built and make every step again (the RomFS too while the dump is there)')
    ap.add_argument('--platform', choices=list(PLATFORMS),
                    help='what to build: the game for this PC (windows), for the Steam Deck (linux: a tar to carry to it) '
                         'or both (default: as the last build, else windows; remembered)')
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
    state = load_state()
    if a.platform:
        state['platforms'] = PLATFORMS[a.platform]
    state.setdefault('platforms', ['windows'])
    if os.name != 'nt':   # Linux builds the game for Linux, here
        # The tools' temporary files in the game's folder (temp/): on a Steam Deck /tmp is memory and /home small
        # (totk-pc-recomp's installer puts its temp folder beside the game for the same reason)
        tmp = ROOT / 'temp'
        tmp.mkdir(exist_ok=True)
        os.environ['TMPDIR'] = str(tmp)
        if a.platform and a.platform != 'linux':
            fail_tools(deps_install.DepsError('on Linux UltraTears is built for Linux: --platform linux'))
        state['platforms'] = ['linux']
    if progress.JSON and not os.environ.get('ULTRATEARS_SETUP_OWN_PYTHON'):
        progress.emit('plan', steps=[{'id': s, 'weight': w} for s, w in step_weights(state['platforms']).items()])
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
        deps_install.ensure_tools(ROOT, linux='linux' in state['platforms'])
    except deps_install.DepsError as e:
        fail_tools(e)
    if progress.JSON:
        progress.emit('step', id='tools', state='done', secs=round(time.time() - t_tools))
    awake()
    importlib.reload(toolchain)
    if a.dump:
        state['dump'] = str(a.dump.resolve())
    if not state.get('dump'):
        state['dump'] = str(ROOT / 'game')
    dump = pathlib.Path(state['dump'])
    if a.full:
        full_rebuild(state, dump)
    save_state(state)
    # a build of everything (the first, or --full) is timed for the launcher's estimates on this PC (full_secs)
    from_nothing = not a.only and (a.full or not any(state.get(k) for k in ('lift', 'shaders-pack', 'textures')))
    say(f'UltraTears setup: dump {dump}, {a.cpus} CPUs, for {" and ".join(state["platforms"])}')
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
            # the step had nothing to do (an update, a resume, no Deck)
            skipped = result in ('up to date', 'already packed', 'not for the Steam Deck', 'no game for this PC', 'no AppImage: no tar')
            progress.emit('step', id=name, state='skip' if skipped else 'done', secs=round(secs), note=result or '')
    if from_nothing:
        state['full_secs'], state['full_cpus'] = round(time.time() - t_all - ASKED), a.cpus
        save_state(state)
    say(f'\nDone in {progress.clock(time.time() - t_all - ASKED)}.' +
        (f' Start the game: UltraTears.exe in {ROOT}.' if 'windows' in state['platforms'] else '') +
        (f' For the Steam Deck: carry {ROOT / DECK_TAR} to it and unpack it there.' if 'linux' in state['platforms'] and
         os.name == 'nt' else '') +
        (f' Start the game: ultratears-launcher or linux/ultratears in {ROOT}.' if os.name != 'nt' else ''))
    if progress.JSON:
        progress.emit('done', secs=round(time.time() - t_all - ASKED))


# The share of each step in a build from nothing, in % (a clean 0.2.0 package on 8 CPUs, 2026-10-30: romfs 2:19, lift
# 1:19, compile 15:39, shaders 2:54, textures 10:08; tools: a source build's first download). The launcher's overall bar
# weighs each step's own progress by these; steps already done count in full.
STEP_WEIGHTS = {'tools': 2, 'check': 1, 'romfs': 7, 'lift': 4, 'compile': 47, 'shaders': 9, 'textures': 30, 'finish': 0,
                'graphics': 4}   # graphics: not measured yet (driver compile of ~28 700 shaders on all CPUs)
PLATFORMS = {'windows': ['windows'], 'linux': ['linux'], 'both': ['windows', 'linux']}


def step_weights(platforms):
    """STEP_WEIGHTS for what is built: the game compiled once per platform, the Deck's shaders translated as well on a PC
    that is not an AMD or Intel one, the tar of ~25 GB written (~3 min)."""
    w = dict(STEP_WEIGHTS)
    w['compile'] *= len(platforms)
    if 'linux' in platforms:
        w['shaders'] *= 2 if 'windows' in platforms else 1
        w['deck'] = 6
    return w


def full_rebuild(state, dump):
    """--full (the launcher's "Rebuild completely"): nothing built before is trusted. The records of every step go, and
    what the steps made: objects, the translated code, the shaders (with the game's compiled.bin), the textures; every
    step then runs as on a first build. The RomFS image is packed again from the dump while the dump is there; a
    player who deleted it keeps the packed one (there is nothing to pack it from). Saves and settings stay."""
    keep_romfs = not (dump / 'romfs').is_dir()
    for k in list(state):
        if k not in ('dump', 'platforms') and not (keep_romfs and k == 'romfs'):
            del state[k]
    root = ROOT.resolve()
    for p in (ROOT / 'build/obj', ROOT / 'build/game', ROOT / 'build-linux/obj', ROOT / 'build-linux/game',
              DATA / 'generated', DATA / 'shaders', DATA / 'shaders-deck', DATA / 'textures'):
        if p.is_dir() and root in p.resolve().parents:
            shutil.rmtree(p)
    SOURCE_DATES.unlink(missing_ok=True)
    save_state(state)
    say('Full rebuild: everything built before is made again' +
        (' (the RomFS stays as packed: the dump is gone)' if keep_romfs else ''))


def fail_tools(e):
    say(f'\nERROR preparing the build tools: {e}')
    if progress.JSON:
        progress.emit('error', id='tools', code=getattr(e, 'code', 'tools'), args={}, message=str(e))
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
