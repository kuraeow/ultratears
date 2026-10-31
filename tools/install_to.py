"""Copy this builder package into the folder the player chose for the game (the launcher's "Change" of the install
folder): the game is then built there, by the copy. Only the package's own files (package-files.txt, which
make_package.py writes; without it every file outside the player's folders) and the launcher's settings; an unchanged
file is left as it is, so a copy that stopped midway continues.
usage: deps\\python\\python.exe tools/install_to.py <folder>   (events as setup.py --json: progress, done, error)
"""
import json
import os
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEEP = {'data', 'build', 'build-linux', 'linux', 'userdata', 'diag', 'game', 'update', 'dist', 'dev-local'}   # the player's, never copied
SETTINGS = 'ultratears-launcher.ini'


def emit(ev, **fields):
    print(json.dumps({'ev': ev, **fields}, ensure_ascii=False), flush=True)


def package_files():
    listing = ROOT / 'package-files.txt'
    if listing.is_file():
        return [f for f in listing.read_text(encoding='utf-8').splitlines() if f and (ROOT / f).is_file()]
    return sorted(p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*')
                  if p.is_file() and p.relative_to(ROOT).parts[0] not in KEEP and not p.name.endswith(('.log', '.tar')))


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    dest = pathlib.Path(sys.argv[1]).resolve()
    try:
        if dest == ROOT or ROOT in dest.parents:
            raise ValueError(f'{dest} is inside the package itself')
        files = package_files()
        total, done = sum((ROOT / f).stat().st_size for f in files), 0
        for f in files:
            src, dst = ROOT / f, dest / f
            st = src.stat()
            if not (dst.is_file() and dst.stat().st_size == st.st_size and int(dst.stat().st_mtime) == int(st.st_mtime)):
                dst.parent.mkdir(parents=True, exist_ok=True)
                tmp = dst.with_name(dst.name + '.copying')
                shutil.copy2(src, tmp)
                os.replace(tmp, dst)
            done += st.st_size
            emit('progress', label='copy', done=done, total=total, eta=None)
        if (ROOT / SETTINGS).is_file():   # the language and the dump, without the install folder (it is that folder now)
            lines = [l for l in (ROOT / SETTINGS).read_text(encoding='utf-8').splitlines() if not l.startswith('install=')]
            (dest / SETTINGS).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    except (OSError, ValueError) as e:
        emit('error', id='tools', code='install_copy', args={'path': str(dest)}, message=str(e))
        return 1
    print(f'{len(files)} files of the builder in {dest}', flush=True)
    emit('done', secs=0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
