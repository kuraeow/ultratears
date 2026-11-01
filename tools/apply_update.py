"""Put a new builder package over this one, in place: the launcher's update (src/launcher/update.cpp downloads the zip
and checks its size and SHA-256 first; then runs this with the package's own Python and restarts itself).

Only the package's files change: data\\, build\\, userdata\\, diag\\ and the player's settings stay, so the next build
redoes only what the new version changed (tools/setup.py). A file in use (the running launcher, deps\\python itself) is
renamed to <name>.old-update and the new one put in its place; the launcher deletes those at its next start
(update\\cleanup.txt). Files of the old package that the new one no longer has are removed (package-files.txt, the
listing make_package.py writes).
usage: deps\\python\\python.exe tools/apply_update.py <package.zip> [--root <package folder>]   (events as setup.py --json)
"""
import argparse
import json
import os
import pathlib
import sys
import tarfile
import zipfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEEP = {'data', 'build', 'build-linux', 'linux', 'userdata', 'diag', 'game', 'update'}   # the player's, never the package's
LISTING = 'package-files.txt'


def emit(ev, **fields):
    print(json.dumps({'ev': ev, **fields}, ensure_ascii=False), flush=True)


def crc32(path):
    c = 0
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            c = zlib.crc32(block, c)
    return c


def same(path, info):
    try:
        return path.stat().st_size == info.file_size and crc32(path) == info.CRC
    except OSError:
        return False


def put(path, data, moved):
    """Write path; a file in use is moved aside first (Windows lets a running exe or a loaded DLL be renamed)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.new-update')
    tmp.write_bytes(data)
    try:
        os.replace(tmp, path)
    except PermissionError:
        aside = path.with_name(path.name + '.old-update')
        if aside.exists():
            aside.unlink()
        os.replace(path, aside)
        os.replace(tmp, path)
        moved.append(aside)


def remove(path, moved):
    try:
        path.unlink()
    except PermissionError:
        aside = path.with_name(path.name + '.old-update')
        os.replace(path, aside)
        moved.append(aside)
    for d in path.parents:   # folders left empty
        if d == ROOT or any(d.iterdir()):
            break
        d.rmdir()


class Package:
    """The builder package's files, from the Windows zip or the Linux tar.gz alike: infos (filename, file_size, CRC,
    mode, link: a link's target or None) and read(info)."""
    class Info:
        def __init__(self, filename, size, crc, mode, link, member):
            self.filename, self.file_size, self.CRC, self.mode, self.link, self.member = filename, size, crc, mode, link, member

    def __init__(self, path):
        self.path = path

    def __enter__(self):
        if str(self.path).endswith('.tar.gz'):
            self.t = tarfile.open(self.path, 'r:gz')
            self.z = None
            self.infos = []
            for m in self.t.getmembers():   # the CRC the zip carries: computed here, the files are read anyway
                if m.isfile():
                    data = self.t.extractfile(m).read()
                    self.infos.append(self.Info(m.name, m.size, zlib.crc32(data), m.mode, None, data))
                elif m.issym():
                    self.infos.append(self.Info(m.name, 0, 0, m.mode, m.linkname, None))
        else:
            self.t = None
            self.z = zipfile.ZipFile(self.path)
            self.infos = [self.Info(i.filename, i.file_size, i.CRC, i.external_attr >> 16, None, i)
                          for i in self.z.infolist() if not i.is_dir()]
        return self

    def read(self, info):
        return info.member if self.t else self.z.read(info.member)

    def __exit__(self, *exc):
        (self.t or self.z).close()


def main():
    global ROOT
    ap = argparse.ArgumentParser()
    ap.add_argument('zip', type=pathlib.Path)
    ap.add_argument('--root', type=pathlib.Path, default=ROOT)
    a = ap.parse_args()
    ROOT = a.root.resolve()
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    moved = []
    try:
        with Package(a.zip) as z:
            infos = z.infos
            top = {i.filename.split('/', 1)[0] for i in infos}
            if len(top) != 1 or not any(i.filename.endswith('/tools/setup.py') for i in infos):
                raise ValueError('not an UltraTears builder package')
            prefix = top.pop() + '/'
            files = {}
            for i in infos:
                rel = i.filename[len(prefix):]
                parts = rel.split('/')
                if rel and parts[0] not in KEEP and '..' not in parts and '' not in parts and ':' not in rel and '\\' not in rel:
                    files[rel] = i
            total, done, changed = sum(i.file_size for i in files.values()), 0, 0
            for rel, info in sorted(files.items()):
                dst = ROOT / rel
                if info.link is not None:   # a link of the Linux package (deps/llvm/bin/clang -> clang-23)
                    if not (dst.is_symlink() and os.readlink(dst) == info.link):
                        dst.parent.mkdir(parents=True, exist_ok=True)
                        if dst.is_symlink() or dst.exists():
                            dst.unlink()
                        os.symlink(info.link, dst)
                        changed += 1
                elif not same(dst, info):
                    put(dst, z.read(info), moved)
                    if info.mode & 0o111:   # the Linux package's programs keep their executable bit
                        dst.chmod(info.mode & 0o777)
                    changed += 1
                done += info.file_size
                emit('progress', label='update', done=done, total=total, eta=None)
        old = ROOT / LISTING
        before = set(old.read_text(encoding='utf-8').splitlines()) if old.is_file() else set()
        removed = 0
        for rel in sorted(before - set(files)):
            p = ROOT / rel
            if rel and rel.split('/', 1)[0] not in KEEP and p.is_file():
                remove(p, moved)
                removed += 1
        if LISTING not in files:   # a package from before the listing: keep one for the next update
            old.write_text('\n'.join(sorted(files)) + '\n', encoding='utf-8')
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as e:
        emit('error', id='update', code='update', args={}, message=str(e))
        return 1
    finally:
        if moved:
            cleanup = ROOT / 'update/cleanup.txt'
            cleanup.parent.mkdir(exist_ok=True)
            with open(cleanup, 'a', encoding='utf-8') as f:
                f.write(''.join(f'{p}\n' for p in moved))
    print(f'{changed} files updated, {removed} removed, {len(moved)} in use moved aside', flush=True)
    emit('done', secs=0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
