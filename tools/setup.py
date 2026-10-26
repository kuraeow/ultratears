"""Build UltraTears from the player's own dump, in one command: every step is checked and can be resumed.

From a Git checkout the build goes to dist\\, or to --output <folder>. The tracked source files are copied there and
missing build tools are downloaded from the latest GitHub builder release. In a builder package setup runs in place.
usage: python tools/setup.py [--output <build folder>] [--dump <folder with exefs\\ and romfs\\>] [--cpus N]
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
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
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
GITHUB_RELEASES = 'https://api.github.com/repos/kuraeow/ultratears/releases?per_page=20'
KEEP = {'game', 'data', 'build', 'userdata', 'diag', 'deps', 'dist', 'dev-local'}
IS_LINUX = sys.platform.startswith("linux")
EXE_NAME = "UltraTears" if IS_LINUX else "UltraTears.exe"
XLATE_NAME = "ultrs-shaders" if IS_LINUX else "ultrs-shaders.exe"
OUTPUT_OWN = {'setup.bat', 'setup.sh', 'VERSION.txt', 'THIRD-PARTY-NOTICES.md', 'licenses', 'ultratears.cfg',
              'UltraTears', 'UltraTears.exe', 'UltraTears.pdb', 'setup-files.txt'}


class Fail(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def is_checkout():
    r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--show-toplevel'], capture_output=True, text=True)
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


def deps_ready(home):
    if IS_LINUX:
        for tool in ("clang", "clang++", "ninja"):
            if not shutil.which(tool) and not (pathlib.Path("/usr/bin") / tool).is_file():
                return False
        packages = subprocess.run([sys.executable, "-c", "import lz4, capstone, unicorn"],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return packages.returncode == 0
    py = home / 'deps/python/python.exe'
    programs = (py, home / 'deps/clang64/bin/clang.exe', home / 'deps/clang64/bin/clang++.exe',
                home / 'deps/clang64/bin/lld.exe', home / 'deps/clang64/bin/ninja.exe',
                home / 'deps/shaders/ultrs-shaders.exe')
    if not all(p.is_file() for p in programs):
        return False
    packages = subprocess.run([str(py), '-c', 'import lz4, capstone, unicorn'], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, env=dict(os.environ, PYTHONNOUSERSITE='1'))
    return packages.returncode == 0


def builder_asset():
    request = urllib.request.Request(GITHUB_RELEASES, headers={'Accept': 'application/vnd.github+json',
                                                               'User-Agent': 'UltraTears-setup'})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            releases = json.load(response)
    except Exception as e:
        raise Fail(f'could not query GitHub releases for the build tools: {e}') from e
    for release in releases:
        for asset in release.get('assets', []):
            name = asset.get('name', '')
            if name.startswith('UltraTears-builder-') and name.endswith('-windows-x86_64.zip'):
                return name, asset['browser_download_url']
    raise Fail('no UltraTears builder package was found in the GitHub releases')


def download(url, path):
    request = urllib.request.Request(url, headers={'User-Agent': 'UltraTears-setup'})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, open(path, 'wb') as out:
            total = int(response.headers.get('Content-Length', 0))
            done, shown = 0, -1
            while True:
                block = response.read(1 << 20)
                if not block:
                    break
                out.write(block)
                done += len(block)
                mark = done // (64 << 20)
                if mark != shown:
                    shown = mark
                    suffix = f' / {total / 2**20:.0f} MiB' if total else ''
                    say(f'  downloading tools: {done / 2**20:.0f} MiB{suffix}')
    except Exception as e:
        path.unlink(missing_ok=True)
        raise Fail(f'could not download the build tools: {e}') from e


def install_deps(home):
    if deps_ready(home):
        return
    if IS_LINUX:
        missing_tools = [tool for tool in ("clang", "clang++", "ninja")
                         if not shutil.which(tool) and not (pathlib.Path("/usr/bin") / tool).is_file()]
        if missing_tools:
            raise Fail(f"missing system build tools: {', '.join(missing_tools)}. Install clang, lld, ninja via your package manager.")
        missing_pkgs = []
        for pkg in ("lz4", "capstone", "unicorn"):
            try:
                __import__(pkg)
            except ImportError:
                missing_pkgs.append(pkg)
        if missing_pkgs:
            say(f"  installing missing python packages: {', '.join(missing_pkgs)}")
            r = subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", *missing_pkgs])
            if r.returncode != 0:
                raise Fail(f"failed to install python packages: {' '.join(missing_pkgs)}. Run: pip install {' '.join(missing_pkgs)}")
        return
    name, url = builder_asset()
    cache = ROOT / 'dev-local/downloads'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    if not archive.is_file():
        say(f'Build tools are missing; downloading {name} once.')
        partial = archive.with_suffix(archive.suffix + '.part')
        download(url, partial)
        partial.replace(archive)
    say(f'  installing tools in {home / "deps"}')
    try:
        with zipfile.ZipFile(archive) as zf:
            for info in zf.infolist():
                parts = pathlib.PurePosixPath(info.filename).parts
                indexes = [i for i, p in enumerate(parts) if p in ('deps', 'licenses')]
                if not indexes:
                    continue
                rel = pathlib.Path(*parts[indexes[0]:])
                if '..' in rel.parts:
                    raise Fail(f'unsafe path in the builder archive: {info.filename}')
                dst = home / rel
                if info.is_dir():
                    dst.mkdir(parents=True, exist_ok=True)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    try:
                        with zf.open(info) as src, open(dst, 'wb') as out:
                            shutil.copyfileobj(src, out)
                    except PermissionError:
                        # An in-place repair may be running with deps/python/python.exe, whose EXE/DLLs Windows locks.
                        if not dst.is_file():
                            raise
    except (OSError, zipfile.BadZipFile) as e:
        archive.unlink(missing_ok=True)
        raise Fail(f'could not unpack the build tools: {e}') from e
    if not deps_ready(home):
        raise Fail(f'{name} did not contain all required build tools')


def choose_dump(home, requested):
    if requested:
        return requested.resolve()
    for candidate in (home / 'game', ROOT / 'game'):
        if (candidate / 'exefs').is_dir() or (candidate / 'romfs').is_dir():
            return candidate.resolve()
    return (home / 'game').resolve()


def build_from_checkout(a):
    home = (a.output or ROOT / 'dist').resolve()
    try:
        changed, removed = sync_source(home)
        say(f'UltraTears source -> {home}: {changed} files updated, {removed} removed')
        install_deps(home)
        for xlate_cand in ("ultrs-shaders", "ultrs-shaders.exe"):
            local_xlate = ROOT / f"dev-local/build/shaders/{xlate_cand}"
            target_xlate = home / f"deps/shaders/{xlate_cand}"
            if local_xlate.is_file() and not filecmp.cmp(local_xlate, target_xlate, shallow=False):
                target_xlate.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(local_xlate, target_xlate)
                say("  installed the locally built shader translator")
    except Fail as e:
        say(f"\nERROR preparing the build folder: {e}")
        return 1
    py_exec = sys.executable if IS_LINUX else str(home / "deps/python/python.exe")
    cmd = [py_exec, str(home / "tools/setup.py"), "--in-place", "--cpus", str(a.cpus),
           "--dump", str(choose_dump(home, a.dump))]
    if a.only:
        cmd += ["--only", a.only]
    env = os.environ.copy()
    if not IS_LINUX:
        env["PYTHONNOUSERSITE"] = "1"
    return subprocess.run(cmd, cwd=home, env=env).returncode


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
            raise Fail(f'the Python package {mod} is missing: python -m pip install lz4 capstone unicorn')
    toolchain.env()
    if toolchain.XLATE is None:
        raise Fail(f"the shader translator {XLATE_NAME} is missing: build it with python tools/build_xlate.py "
                   "(needs the .NET 10 SDK) or take deps/shaders from the builder package")
    if packed(state) and all(ok_module(DATA / 'exefs' / n, h) for n, h in EXEFS.items()):
        return 'Tears of the Kingdom 1.4.3, already taken in (data\\exefs, data\\romfs): the dump is no longer needed'
    if not dump.is_dir():
        raise Fail(f'{dump} does not exist: put your extracted game there (game\\exefs and game\\romfs), '
                   'or give its folder with --dump')
    for p in list(dump.iterdir()) + list((dump / 'exefs').glob('*') if (dump / 'exefs').is_dir() else []):
        if p.suffix.lower() in ENCRYPTED:
            raise Fail(f'{p.name}: encrypted packages are not accepted. Give the extracted exefs and romfs folders '
                       '(this project decrypts nothing and reads no keys)')
    exefs, romfs = dump / 'exefs', dump / 'romfs'
    if not exefs.is_dir() or not romfs.is_dir():
        raise Fail(f'{dump} must hold the folders exefs and romfs (extracted, as plain files)')
    for name, want in EXEFS.items():
        f = exefs / name
        if not f.is_file():
            raise Fail(f'exefs\\{name} is missing')
        if sha256(f) != want:
            raise Fail(f'exefs\\{name} is not the one of Tears of the Kingdom 1.4.3. Extract the ExeFS again with '
                       'update 1.4.3 applied (the base game 1.0.0 and other updates are not supported)')
    if not (romfs / 'Pack/ZsDic.pack.zs').is_file():
        raise Fail(f'{romfs} does not look like the RomFS of Tears of the Kingdom (Pack\\ZsDic.pack.zs is missing)')
    return 'Tears of the Kingdom 1.4.3 (the code modules match)'


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
                   'content differs). Extract the RomFS again with update 1.4.3 applied; mods do not go here')
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


def step_compile(dump, state, cpus):
    run([sys.executable, 'tools/build_game.py', '--generated', DATA / 'generated', '--target', 'ultratears', '--cpus', cpus],
        'compiling')
    return EXE_NAME


def step_shaders(dump, state, cpus):
    out = DATA / 'shaders'
    stamp = tree_hash([ROOT / 'tools/shaders/inventory.py', ROOT / 'tools/export_shaders.py'])
    if state.get('shaders-export') != stamp or not (out / 'jobs.jsonl').is_file():
        run([sys.executable, 'tools/export_shaders.py', DATA / 'romfs', out, DATA / 'exefs'], 'finding the shaders')
        state['shaders-export'] = stamp
        save_state(state)
    xlate = sha256(toolchain.XLATE)[:16]
    new_only = ['--new-only'] if state.get('shaders-xlate') == xlate else []
    run([sys.executable, 'tools/run_ryujinx_xlate.py', out, '--workers', min(cpus, 32), '--quiet'] + new_only,
        'translating the shaders')
    state['shaders-xlate'] = xlate
    return 'done'


def step_textures(dump, state, cpus):
    run([sys.executable, 'tools/bc7_build.py', '--romfs', DATA / 'romfs', '--cache', DATA / 'textures', '--threads',
         min(cpus, 64)], 're-encoding the textures')
    return 'done'


def step_finish(dump, state, cpus):
    fonts = dump / 'sysdata/fonts'
    lines = ['# Written by tools/setup.py: where UltraTears finds its data (relative to this folder).',
             'exefs=data/exefs', 'romfs=data/romfs', 'shaders=data/shaders/ryujinx', 'textures=data/textures',
             'saves=userdata']
    if fonts.is_dir() and list(fonts.glob('*.bfttf')):
        shutil.copytree(fonts, DATA / 'fonts', dirs_exist_ok=True)
    lines.append('fonts=data/fonts')
    (ROOT / 'ultratears.cfg').write_text('\n'.join(lines) + '\n')
    (ROOT / f"build/{EXE_NAME}").unlink(missing_ok=True)   # where builds before 0.1.0 linked it
    if (dump / 'romfs').is_dir():
        say(f'  The game reads data\\romfs and data\\exefs now: {dump} (16 GB) is no longer needed.')
        if sys.stdin.isatty() and dump.resolve() == (ROOT / 'game').resolve():
            answer = input('  Delete game\\exefs and game\\romfs to free 16 GB? [y/N] ').strip().lower()
            if answer in ('y', 'yes', 'д', 'да'):
                for sub in ('exefs', 'romfs'):
                    shutil.rmtree(dump / sub, ignore_errors=True)
                say('  deleted')
    return 'ultratears.cfg ready'


STEPS = [('check', step_check), ('romfs', step_romfs), ('lift', step_lift), ('compile', step_compile),
         ('shaders', step_shaders), ('textures', step_textures), ('finish', step_finish)]


def main():
    ap = argparse.ArgumentParser(description='Build UltraTears from your own dump of the game.')
    ap.add_argument('--output', type=pathlib.Path,
                    help='build folder when run from a Git checkout (default: dist here)')
    ap.add_argument('--dump', type=pathlib.Path,
                    help='the folder with exefs\\ and romfs\\ (default: game\\ here; remembered for later runs)')
    ap.add_argument('--cpus', type=int, default=os.cpu_count() or 4,
                    help='logical CPUs the build may use (default: all; it runs at low priority)')
    ap.add_argument('--only', choices=[s for s, _ in STEPS], help='run one step only')
    ap.add_argument('--in-place', action='store_true', help=argparse.SUPPRESS)
    a = ap.parse_args()
    if is_checkout() and not a.in_place:
        sys.exit(build_from_checkout(a))
    if a.output:
        ap.error('--output is used only when setup.py is run from a Git checkout')
    if not deps_ready(ROOT):
        try:
            install_deps(ROOT)
        except Fail as e:
            say(f'\nERROR preparing the build tools: {e}')
            sys.exit(1)
        importlib.reload(toolchain)
    if hasattr(os, "nice"):
        try:
            os.nice(10)
        except Exception:
            pass
    state = load_state()
    if a.dump:
        state['dump'] = str(a.dump.resolve())
    if not state.get('dump'):
        state['dump'] = str(ROOT / 'game')
    dump = pathlib.Path(state['dump'])
    say(f'UltraTears setup: dump {dump}, {a.cpus} CPUs')
    t_all = time.time()
    for i, (name, fn) in enumerate(STEPS, 1):
        if a.only and name != a.only and name != 'check':
            continue
        say(f'[{i}/{len(STEPS)}] {name}')
        t0 = time.time()
        try:
            result = fn(dump, state) if name == 'check' else fn(dump, state, a.cpus)
        except Fail as e:
            say(f'\nERROR in step "{name}": {e}')
            save_state(state)
            sys.exit(1)
        save_state(state)
        say(f'  {result or "ok"} ({time.time() - t0:.0f} s)')
    say(f"\nDone in {(time.time() - t_all) / 60:.0f} min. Start the game: {EXE_NAME} in {ROOT}")


if __name__ == '__main__':
    main()
