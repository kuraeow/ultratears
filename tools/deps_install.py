"""The build tools of a source build: what is missing on this PC is downloaded into deps/ from where it is published.

tools/deps.json pins every download (URL, size, hash); nothing of it is in git and nothing comes from an UltraTears
release. Each part is used from the PC when it is already there, else installed into deps/:
  - Python 3.14 with lz4, capstone and unicorn: the Python running setup.py when it is 3.14+ and has them, else
    deps/python (python.org's NuGet package, the wheels from PyPI unpacked into it; no pip, no compiler);
  - clang, lld, ninja and the libraries the port links (MSYS2 CLANG64): deps/clang64, ULTRS_CLANG64 or
    C:/msys64/clang64 when it has them, else deps/clang64 from repo.msys2.org;
  - the shader translator ultrs-shaders.exe: built from this source tree (tools/build_xlate.py) whenever deps/shaders
    holds none made from it (deps/shaders/source.txt), with the .NET 10 SDK on PATH or else one downloaded into
    deps/dotnet from Microsoft. A builder package carries a translator stamped with its own source.
deps/installed.json records what was installed from which pins: a changed pin installs that part again.
ULTRS_NO_SYSTEM_TOOLS=1 (testing what a clean PC gets): nothing installed on this PC is used.
"""
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / 'tools/deps.json'
PACKAGES = ('lz4', 'capstone', 'unicorn')
# what the build needs from a CLANG64 folder (bin/..): programs, headers, libraries
CLANG64_NEEDS = ('bin/clang.exe', 'bin/clang++.exe', 'bin/ld.lld.exe', 'bin/ninja.exe', 'bin/llvm-windres.exe',
                 'include/SDL3/SDL.h', 'include/opus/opus.h', 'include/vpx/vpx_decoder.h', 'include/vulkan/vulkan.h',
                 'include/zstd.h', 'lib/libSDL3.a', 'lib/libopus.a', 'lib/libvpx.a')
SYSTEM = not os.environ.get('ULTRS_NO_SYSTEM_TOOLS')
XLATE_SOURCES = ('third_party/shader_translator', 'third_party/maxwell_shader', 'tools/build_xlate.py')


class DepsError(Exception):
    def __init__(self, message, code='tools'):
        """code: for the launcher, which words the failure in the player's language (setup.py --json)."""
        super().__init__(message)
        self.code = code


def say(msg):
    print(msg, flush=True)


def manifest():
    return json.loads(MANIFEST.read_text(encoding='utf-8'))


def key_of(obj):   # what a part was installed from
    return hashlib.sha256(json.dumps(obj, sort_keys=True).encode()).hexdigest()[:16]


def installed(home):
    try:
        return json.loads((home / 'deps/installed.json').read_text())
    except (OSError, ValueError):
        return {}


def mark(home, part, key):
    s = installed(home)
    s[part] = key
    (home / 'deps').mkdir(exist_ok=True)
    (home / 'deps/installed.json').write_text(json.dumps(s, indent=1))


def wipe(home, sub):
    """Remove deps/<sub> of home before reinstalling it (only that folder, never anything outside deps/)."""
    d = (home / 'deps' / sub).resolve()
    if d.parent != (home / 'deps').resolve() or not d.exists():
        return
    shutil.rmtree(d)


# ---------------------------------------------------------------- downloads
# A download that breaks is resumed where it stopped (HTTP Range), a few times per source; a source that sends nothing
# (down, or blocked: repo.msys2.org timed out for a player in Russia, 2026-10-29) is left at once for the next one, and
# tried last for the rest of the run. The mirrors in tools/deps.json serve the same files (their hashes are checked, so
# any mirror will do). ULTRS_MSYS2_MIRROR=<base URL> is tried first for the MSYS2 packages, ULTRS_PYPI_MIRROR for the
# wheels.
TRIES, TIMEOUT = 3, 20
SLOW = set()   # hosts that failed a file TRIES times in this run


def sources(item):
    """The URLs item can be downloaded from, in order."""
    urls = [item['url']]
    for kind, env in (('msys2', 'ULTRS_MSYS2_MIRROR'), ('pypi', 'ULTRS_PYPI_MIRROR')):
        bases = manifest().get('mirrors', {}).get(kind, [])   # bases[0]: the host the pinned URL names
        if not bases or not item['url'].startswith(bases[0] + '/'):
            continue
        path = item['url'][len(bases[0]):]
        if os.environ.get(env):
            bases = [os.environ[env].rstrip('/')] + bases
        urls = [b + path for b in bases]
    urls = list(dict.fromkeys(urls))
    return [u for u in urls if u.split('/')[2] not in SLOW] + [u for u in urls if u.split('/')[2] in SLOW]


def download(url, part, bar):
    """url into part, continuing a partial one; bar counts the bytes."""
    have = part.stat().st_size if part.is_file() else 0
    headers = {'User-Agent': 'UltraTears-setup'}
    if have:
        headers['Range'] = f'bytes={have}-'
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=TIMEOUT) as r:
        if have and r.status != 206:   # the server sent the whole file: start over
            bar.add(-have)
            have = 0
        with open(part, 'ab' if have else 'wb') as out:
            while True:
                block = r.read(1 << 18)
                if not block:
                    break
                out.write(block)
                bar.add(len(block))


def fetch_all(items, cache, label):
    """Every item's file in cache (downloaded unless a good copy is there; sizes and hashes checked), with one
    progress line for all of them."""
    import progress
    cache.mkdir(parents=True, exist_ok=True)
    files, todo = [], []
    for item in items:
        dst = cache / urllib.parse.unquote(item['url'].rsplit('/', 1)[1])
        files.append(dst)
        if not (dst.is_file() and good(dst, item)):
            todo.append((item, dst))
    if not todo:
        return files
    bar = progress.Bar(f'downloading {label}', sum(i.get('size', 0) for i, _ in todo), 'MiB', 2**20, rate=True, live=True)
    for item, dst in todo:
        part = dst.with_name(dst.name + '.part')
        base = bar.done - (part.stat().st_size if part.is_file() else 0)
        bar.add(part.stat().st_size if part.is_file() else 0)
        errors = []
        for url in sources(item):
            host, ok = url.split('/')[2], False
            for _ in range(TRIES):
                before = bar.done
                try:
                    download(url, part, bar)
                    ok = True
                    break
                except Exception as e:   # a timeout, a broken connection, an HTTP error
                    errors.append(f'{host}: {e}')
                    if bar.done == before:   # nothing came at all: this source is down or blocked, not flaky
                        break
                    if isinstance(e, urllib.error.HTTPError) and e.code in (403, 404, 410, 416):
                        part.unlink(missing_ok=True)   # this source does not have it: the next one from the start
                        bar.update(base)
                        break
            if not ok:
                if host not in SLOW:
                    nxt = [u.split('/')[2] for u in sources(item) if u.split('/')[2] not in SLOW | {host}]
                    bar.note(f'{host} does not answer ({errors[-1].split(": ", 1)[-1][:60]})'
                             + (f'; trying {nxt[0]}' if nxt else ''))
                SLOW.add(host)   # tried last for the rest of this run
                continue
            if good(part, item):
                break
            part.unlink(missing_ok=True)   # not the pinned file: try the next source
            bar.update(base)
            errors.append(f'{url.split("/")[2]}: not the pinned file (size or hash differs)')
        else:
            bar.finish()
            raise DepsError(f'could not download {dst.name} from any source:\n    ' + '\n    '.join(errors[-6:]) +
                            '\n  Check the connection (a VPN may help), or download it by hand into ' + str(cache))
        part.replace(dst)
    bar.finish()
    return files


def fetch(item, cache, label):
    return fetch_all([item], cache, label)[0]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def good(path, item):
    if item.get('size') and path.stat().st_size != item['size']:
        return False
    algo = 'sha512' if 'sha512' in item else 'sha256'
    h = hashlib.new(algo)
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest().lower() == item[algo].lower()


# ---------------------------------------------------------------- Python
def python_ok(exe):
    """exe is Python 3.14+ with the build's packages."""
    probe = 'import sys; import ' + ', '.join(PACKAGES) + '; sys.exit(0 if sys.version_info >= (3, 14) else 1)'
    try:
        return subprocess.run([str(exe), '-c', probe], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              env=dict(os.environ, PYTHONNOUSERSITE='1')).returncode == 0
    except OSError:
        return False


LINUX = sys.platform.startswith('linux')


def python_key():   # what deps/python is installed from on this system
    m = manifest()
    return key_of([m['linux_host']['python'], m['linux_host']['wheels']] if LINUX else [m['python'], m['wheels']])


def python_for(home):
    """The Python the build runs on: deps/python when installed from the current pins, else this one when it will do,
    else None (install_python must run first)."""
    own = home / ('deps/python/bin/python3' if LINUX else 'deps/python/python.exe')
    if own.is_file() and installed(home).get('python') in (python_key(), 'package'):
        return own
    if SYSTEM and python_ok(sys.executable):
        return pathlib.Path(sys.executable)
    return None


def install_python_linux(home):
    """deps/python on Linux: python-build-standalone's interpreter (as totk-pc-recomp's AppImage carries it) with the
    manylinux wheels unpacked into its site-packages."""
    import tarfile
    lh = manifest()['linux_host']
    own = (home / 'deps/python').resolve()
    if pathlib.Path(sys.executable).resolve().is_relative_to(own):
        raise DepsError('deps/python is outdated but is the Python running setup: start setup.py with another Python')
    say(f'  Python {lh["python"]["version"]} with {", ".join(PACKAGES)} -> deps/python')
    files = fetch_all([lh['python']] + lh['wheels'], home / 'deps/downloads', 'Python')
    wipe(home, 'python')
    with tarfile.open(files[0]) as t:   # install_only: everything under python/
        t.extractall(home / 'deps', filter='data')
    site = own / f'lib/python{lh["python"]["version"].rsplit(".", 1)[0]}/site-packages'
    for whl in files[1:]:
        with zipfile.ZipFile(whl) as z:
            for info in z.infolist():
                rel = pathlib.PurePosixPath(info.filename)
                if '..' in rel.parts or rel.is_absolute():
                    raise DepsError(f'unsafe path in {whl.name}: {info.filename}')
            z.extractall(site)
    for so in site.rglob('*.so*'):   # zip keeps no modes
        so.chmod(0o755)
    if not python_ok(own / 'bin/python3'):
        raise DepsError('the installed deps/python cannot import the build packages')
    mark(home, 'python', python_key())
    for f in files:
        f.unlink(missing_ok=True)


def install_python(home):
    """deps/python: python.org's interpreter with the wheels unpacked into it. Runs on any Python 3."""
    if LINUX:
        return install_python_linux(home)
    m = manifest()
    own = (home / 'deps/python').resolve()
    if pathlib.Path(sys.executable).resolve().is_relative_to(own):
        raise DepsError('deps\\python is outdated but is the Python running setup: start setup.py with another Python')
    cache = home / 'deps/downloads'
    say(f'  Python {m["python"]["version"]} with {", ".join(PACKAGES)} -> deps\\python')
    files = fetch_all([m['python']] + m['wheels'], cache, 'Python')
    wipe(home, 'python')
    with zipfile.ZipFile(files[0]) as z:   # the NuGet package: the interpreter is under tools/
        for info in z.infolist():
            if not info.filename.startswith('tools/') or info.is_dir():
                continue
            rel = pathlib.PurePosixPath(info.filename[len('tools/'):])
            if '..' in rel.parts:
                raise DepsError(f'unsafe path in {files[0].name}: {info.filename}')
            out = own / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(out, 'wb') as dst:
                shutil.copyfileobj(src, dst)
    site = own / 'Lib/site-packages'
    for whl in files[1:]:   # a wheel is a zip of what goes into site-packages
        with zipfile.ZipFile(whl) as z:
            for info in z.infolist():
                rel = pathlib.PurePosixPath(info.filename)
                if '..' in rel.parts or rel.is_absolute():
                    raise DepsError(f'unsafe path in {whl.name}: {info.filename}')
            z.extractall(site)
    if not python_ok(own / 'python.exe'):
        raise DepsError('the installed deps\\python cannot import the build packages')
    mark(home, 'python', key_of([m['python'], m['wheels']]))
    for f in files:
        f.unlink(missing_ok=True)


# ---------------------------------------------------------------- CLANG64
def clang64_ok(prefix):
    return prefix is not None and all((prefix / n).is_file() for n in CLANG64_NEEDS)


LLVM_NEEDS = ('bin/clang', 'bin/clang++', 'bin/ld.lld', 'bin/ninja', 'bin/llvm-ar', 'bin/llvm-objcopy')


def llvm_key():
    lh = manifest()['linux_host']
    return key_of([lh['llvm'], lh['ninja']])


def llvm_for(home):
    """Linux: the LLVM folder to build with (its bin/..), or None: deps/llvm, else ULTRS_LLVM (a bin folder with clang,
    lld, ninja, llvm-ar and llvm-objcopy; development)."""
    own = home / 'deps/llvm'
    if installed(home).get('clang64') in (llvm_key(), 'package') and (own / 'bin/clang').is_file():
        return own
    c = os.environ.get('ULTRS_LLVM') if SYSTEM else None
    if c and all((pathlib.Path(c).parent / n).exists() for n in LLVM_NEEDS):
        return pathlib.Path(c).parent
    return None


def install_llvm(home):
    """deps/llvm on Linux: the parts of LLVM's release the build uses (as totk-pc-recomp's make_release_linux.py takes
    them: clang, lld, llvm-ar, llvm-objcopy, the clang resource headers; 2 GB downloaded once, ~150 MB kept), ninja,
    and LLVM's license (its binary release carries none)."""
    import tarfile
    lh = manifest()['linux_host']
    llvm = lh['llvm']
    say(f'  LLVM {llvm["version"]} (clang, lld) and ninja {lh["ninja"]["version"]} -> deps/llvm '
        f'({llvm["size"] / 2**30:.1f} GiB download, once)')
    files = fetch_all([llvm, lh['ninja'], {**llvm['license'], 'url': llvm['license']['url']}], home / 'deps/downloads',
                      'LLVM')
    wipe(home, 'llvm')
    own = home / 'deps/llvm'
    own.mkdir(parents=True)
    top = f'LLVM-{llvm["version"]}-Linux-X64/'
    import progress
    with tarfile.open(files[0]) as t, progress.Ticker('unpacking LLVM'):
        for info in t:
            if not info.name.startswith(top):
                continue
            rel = info.name[len(top):]
            if not any(rel == m or rel.startswith(m + '/') for m in llvm['members']):
                continue
            info.name = rel
            if info.islnk() and info.linkname.startswith(top):   # a hard link names its target from the archive's root
                info.linkname = info.linkname[len(top):]
            t.extract(info, own, filter='data')
    with zipfile.ZipFile(files[1]) as z:
        z.extract('ninja', own / 'bin')
    (own / 'bin/ninja').chmod(0o755)
    shutil.copyfile(files[2], own / 'LICENSE.TXT')
    if not all((own / n).exists() for n in LLVM_NEEDS):
        raise DepsError('the installed deps/llvm lacks part of the toolchain')
    mark(home, 'clang64', llvm_key())
    for f in files:
        f.unlink(missing_ok=True)


def clang64_for(home):
    """The CLANG64 folder to build with (its bin/..), or None. Linux: llvm_for."""
    if LINUX:
        return llvm_for(home)
    m = manifest()
    own = home / 'deps/clang64'
    if installed(home).get('clang64') in (key_of(m['msys2']), 'package') and (own / 'bin/clang.exe').is_file():
        return own
    for c in (os.environ.get('ULTRS_CLANG64'), 'C:/msys64/clang64/bin') if SYSTEM else ():
        if c and clang64_ok(pathlib.Path(c).parent):
            return pathlib.Path(c).parent
    return None


def install_clang64(home):
    """deps/clang64 from the MSYS2 packages (needs Python 3.14: tarfile reads .tar.zst). Linux: install_llvm."""
    if LINUX:
        return install_llvm(home)
    import tarfile
    m = manifest()
    cache = home / 'deps/downloads'
    total = sum(p['size'] for p in m['msys2'])
    say(f'  clang / lld / ninja and the libraries ({len(m["msys2"])} MSYS2 packages, {total / 2**20:.0f} MiB) -> deps\\clang64')
    files = fetch_all(m['msys2'], cache, 'clang')
    wipe(home, 'clang64')
    import progress
    bar = progress.Bar('unpacking', len(files), 'packages')
    own = home / 'deps/clang64'
    own.mkdir(parents=True)
    def where(name):   # clang64/<rel> -> its place in deps/clang64 (None: package metadata, .PKGINFO etc.)
        if not name.startswith('clang64/') or name == 'clang64/':
            return None
        rel = pathlib.PurePosixPath(name[len('clang64/'):])
        if rel.is_absolute() or '..' in rel.parts:
            raise DepsError(f'unsafe path in an MSYS2 package: {name}')
        return own / rel
    for f in files:
        with tarfile.open(f, 'r:zst') as t:
            for info in t:
                dst = where(info.name)
                if dst is None:
                    continue
                if info.isdir():
                    dst.mkdir(parents=True, exist_ok=True)
                elif info.isfile():
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    with t.extractfile(info) as src, open(dst, 'wb') as out:
                        shutil.copyfileobj(src, out)
                elif info.islnk() or info.issym():   # hard links (clang.exe / clang++.exe ...): a copy of the target
                    target = where(info.linkname) if info.islnk() else dst.parent / info.linkname
                    if target is None or not target.resolve().is_relative_to(own.resolve()):
                        raise DepsError(f'unsafe link in {f.name}: {info.name} -> {info.linkname}')
                    if target.is_file():
                        dst.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(target, dst)
        bar.add(1)
    bar.finish()
    if not clang64_ok(own):
        raise DepsError('the installed deps\\clang64 lacks part of the toolchain')
    mark(home, 'clang64', key_of(m['msys2']))
    for f in files:
        f.unlink(missing_ok=True)


# ---------------------------------------------------------------- shader translator
def xlate_stamp(root=ROOT):
    """The sources the translator is built from (tools/build_xlate.py writes this next to it)."""
    h = hashlib.sha256()
    files = []
    for s in XLATE_SOURCES:
        p = root / s
        files += [p] if p.is_file() else [f for f in p.rglob('*') if f.is_file() and not {'bin', 'obj'} & set(f.relative_to(p).parts)]
    # by the path as text: Windows sorts paths without case, Linux with it (one source, two stamps until 2026-11-01)
    for f in sorted(files, key=lambda f: f.relative_to(root).as_posix()):
        h.update(f.relative_to(root).as_posix().encode() + b'\0' + f.read_bytes().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:16]


def xlate_ok(home):
    """A builder package carries the translator built from its commit, without the translator's sources (the link to
    them is in THIRD-PARTY-NOTICES.md): taken as it is. A source tree: rebuilt whenever its sources changed."""
    exe = home / 'deps/shaders' / ('ultrs-shaders' if LINUX else 'ultrs-shaders.exe')
    stamp = home / 'deps/shaders/source.txt'
    if installed(home).get('shaders') == 'package':
        return exe.is_file()
    return exe.is_file() and stamp.is_file() and stamp.read_text().strip() == xlate_stamp(home)


def system_dotnet():
    if not SYSTEM:
        return None
    try:
        r = subprocess.run(['dotnet', '--list-sdks'], capture_output=True, text=True)
    except OSError:
        return None
    return 'dotnet' if r.returncode == 0 and any(l.split('.')[0].strip().isdigit() and int(l.split('.')[0]) >= 10
                                                for l in r.stdout.splitlines()) else None


def build_xlate(home):
    sdk = manifest()['linux_host']['dotnet'] if LINUX else manifest()['dotnet']
    dotnet = system_dotnet()
    if dotnet is None:
        own = home / 'deps/dotnet'
        prog = own / ('dotnet' if LINUX else 'dotnet.exe')
        if installed(home).get('dotnet') != key_of(sdk) or not prog.is_file():
            say(f'  .NET SDK {sdk["version"]} (to build the shader translator) -> deps/dotnet')
            z = fetch(sdk, home / 'deps/downloads', '.NET SDK')
            wipe(home, 'dotnet')
            import progress
            with progress.Ticker('unpacking .NET SDK'):
                if LINUX:
                    import tarfile
                    with tarfile.open(z) as t:
                        t.extractall(own, filter='data')
                else:
                    with zipfile.ZipFile(z) as zf:
                        zf.extractall(own)
            mark(home, 'dotnet', key_of(sdk))
            z.unlink(missing_ok=True)
        dotnet = str(prog)
    say('  building the shader translator from this source -> deps\\shaders (a few minutes the first time)')
    import progress
    with progress.Ticker('building the shader translator'):
        r = subprocess.run([sys.executable, str(home / 'tools/build_xlate.py'), '--dotnet', dotnet, '--out', str(home / 'deps/shaders'),
                            '--cache', str(home / 'deps/dotnet-cache')], cwd=home, capture_output=True, text=True)
    rc = r.returncode
    if rc:
        print(r.stdout[-4000:] + r.stderr[-4000:], flush=True)
    if rc or not xlate_ok(home):
        raise DepsError('building the shader translator failed; the lines above say why')


# ---------------------------------------------------------------- the launcher's libraries
def launcher_libs(home=ROOT):
    """deps/launcher/<lib>/: Dear ImGui, nanosvg and stb_image, the files tools/build_launcher.py compiles in, each
    downloaded from its pinned upstream commit (tools/deps.json "launcher": size and SHA-256 checked) unless a good
    copy is there. Only the developer builds the launcher; a release carries it built."""
    out = home / 'deps/launcher'
    for lib, spec in manifest()['launcher'].items():
        for item in spec['files']:
            dst = out / lib / item['path']
            if dst.is_file() and good(dst, item):
                continue
            got = fetch(item, out / 'downloads' / lib / pathlib.PurePosixPath(item['path']).parent, f'{lib} {spec["version"]}')
            dst.parent.mkdir(parents=True, exist_ok=True)
            os.replace(got, dst)
    shutil.rmtree(out / 'downloads', ignore_errors=True)
    return out


# ---------------------------------------------------------------- the Linux kit
# A build for Linux / Steam Deck (setup.py --platform linux or both) needs the Linux kit in deps/linux: the files of
# Valve's Steam Runtime "sniper" sysroot a compile and link of the game read, and a static zstd (tools/make_linux_kit.py,
# ~18 MB). A builder package carries it; a build from the source makes it here from the pinned sysroot (1.2 GB, once)
# and zstd's source (tools/deps.json "linux_kit_sources"). ULTRS_LINUX_KIT_ZIP=<a kit zip> installs that one instead.
def linux_kit_for(home):
    """deps/linux when it is the package's, made from the current pins or from a given zip (a developer's: kept until
    deps/linux is deleted), else None."""
    key = installed(home).get('linux_kit')
    want = key_of(manifest()['linux_kit_sources'])
    if (home / 'deps/linux/sysroot').is_dir() and key in (want, 'package', 'local'):
        return home / 'deps/linux'
    return None


def install_linux_kit(home):
    src = manifest()['linux_kit_sources']
    local = os.environ.get('ULTRS_LINUX_KIT_ZIP')
    if not local and (home / 'package-files.txt').is_file():
        # a builder package came with it: gone from it means a damaged package, not a 1.2 GB download to make it again
        raise DepsError('the Linux kit (deps\\linux) is missing from this builder package: download the release again '
                        'and extract it over this folder', 'linux_kit_missing')
    if local:
        path = pathlib.Path(local)
        say(f'  the Linux kit {path} -> deps\\linux')
    else:
        total = src['sniper']['size'] + src['zstd']['size']
        say(f'  the Linux kit: Steam Runtime sysroot {src["sniper"]["version"]} and zstd {src["zstd"]["version"]} '
            f'({total / 2**20:.0f} MiB, once) -> deps\\linux')
        cache = home / 'deps/downloads'
        sniper, zst = fetch_all([src['sniper'], src['zstd']], cache, 'the Linux kit')
        import make_linux_kit   # after CLANG64 is installed: it builds zstd with it
        path = make_linux_kit.build(sniper, zst, cache / 'ultrs-linux-kit.zip')
    wipe(home, 'linux')
    own = (home / 'deps/linux').resolve()
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            rel = pathlib.PurePosixPath(info.filename)
            if '..' in rel.parts or rel.is_absolute():
                raise DepsError(f'unsafe path in {path.name}: {info.filename}')
        z.extractall(own)
    if not (own / 'sysroot/usr/include/stdio.h').is_file():
        raise DepsError('the installed deps\\linux lacks the sysroot')
    mark(home, 'linux_kit', 'local' if local else key_of(src))
    if not local:
        for f in (path, sniper, zst):
            f.unlink(missing_ok=True)


# ---------------------------------------------------------------- the AppImage (for the Steam Deck's tar)
# The tar a Windows build makes for the Steam Deck holds the game, its data and the release's AppImage (the builder on
# the Deck). The Windows builder package names it with its hash in deps/linux-package.json (tools/make_package.py); it is
# downloaded once, when a build is for the Deck. ULTRS_LINUX_PACKAGE=<folder>: the newest AppImage there instead
# (development: release/).
LINUX_PACKAGE = 'deps/linux-package.json'


def linux_package(home):
    """The AppImage of this release, which the Steam Deck's tar carries (the builder on the Deck: its launcher builds and
    updates there with the tools inside it), downloaded into deps/downloads when not there yet. None in a build from the
    source with no AppImage at hand (ULTRS_LINUX_PACKAGE unset, or a folder without one): the game is built for Linux
    all the same and only the tar is not made (setup.py step deck) - the AppImage of a release is made after such a
    build (release.md), so requiring one here was circular."""
    local = os.environ.get('ULTRS_LINUX_PACKAGE')
    if local:
        d = pathlib.Path(local)
        images = list(d.glob('UltraTears.AppImage')) + list(d.glob('UltraTears-*-x86_64.AppImage'))
        return max(images, key=lambda p: p.stat().st_mtime, default=None)
    pins = home / LINUX_PACKAGE
    if not pins.is_file():
        return None
    item = json.loads(pins.read_text(encoding='utf-8'))['appimage']
    if not (home / 'deps/downloads' / item['name']).is_file():
        say(f'  the AppImage of this release ({item["size"] / 2**20:.0f} MiB, once) -> deps\\downloads')
    image, = fetch_all([item], home / 'deps/downloads', 'the AppImage')
    return image


# ---------------------------------------------------------------- entry points
def ensure_tools(home, linux=False):
    """Run on the build's Python (3.14+): CLANG64 and the translator, installed when missing; linux: the Linux kit too,
    and on Windows a release package's AppImage for the Steam Deck's tar (downloaded before an hour of building, not
    after)."""
    if clang64_for(home) is None:
        install_clang64(home)
    if not xlate_ok(home):
        build_xlate(home)
    if (linux or LINUX) and linux_kit_for(home) is None:   # on Linux the kit is the sysroot of every build
        install_linux_kit(home)
    if linux and not LINUX:
        linux_package(home)


def ready(home):
    return python_for(home) is not None and clang64_for(home) is not None and xlate_ok(home)
