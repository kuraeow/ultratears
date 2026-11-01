"""Copy this builder package into the folder the player chose for the game (the launcher's "Change" of the install
folder): the game is then built there, by the copy. Only the package's own files (package-files.txt, which
make_package.py writes; without it every file outside the player's folders) and the launcher's settings; an unchanged
file is left as it is, so a copy that stopped midway continues.
From an AppImage (Linux) the package's tools stay in it: the folder gets the source and the scripts, and deps/ there
is a link to the AppImage's deps/ (the launcher makes it again at every start: the mount point changes), so a game
folder holds no second copy of Python, LLVM and the Linux kit (~1 GB), and a newer AppImage is the newer builder.
usage: deps\\python\\python.exe tools/install_to.py <folder>   (events as setup.py --json: progress, done, error)
"""
import filecmp
import json
import os
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEEP = {'data', 'build', 'build-linux', 'linux', 'userdata', 'diag', 'game', 'dump', 'update', 'dist', 'release', 'dev-local'}   # the player's, never copied
SETTINGS = 'ultratears-launcher.ini'
# From an AppImage: what stays in it (the tools; the launcher and its SDL3, which the AppImage starts itself)
IN_APPIMAGE = ('deps/', 'ultratears-launcher', 'libSDL3.so.0')


def from_appimage():
    appdir = os.environ.get('APPDIR')
    return bool(appdir) and pathlib.Path(appdir).resolve() == ROOT


def emit(ev, **fields):
    print(json.dumps({'ev': ev, **fields}, ensure_ascii=False), flush=True)


def package_files():
    listing = ROOT / 'package-files.txt'
    if listing.is_file():
        return [f for f in listing.read_text(encoding='utf-8').splitlines() if f and ((ROOT / f).is_file() or (ROOT / f).is_symlink())]
    return sorted(p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*')
                  if p.is_file() and p.relative_to(ROOT).parts[0] not in KEEP and not p.name.endswith(('.log', '.tar')))


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    dest = pathlib.Path(sys.argv[1]).resolve()
    try:
        if dest == ROOT or ROOT in dest.parents:
            raise ValueError(f'{dest} is inside the package itself')
        files = package_files()
        appimage = from_appimage()
        if appimage:
            files = [f for f in files if not (f.startswith(IN_APPIMAGE[0]) or f in IN_APPIMAGE[1:])]
        total, done = sum((ROOT / f).stat().st_size for f in files if not (ROOT / f).is_symlink()), 0
        for f in files:
            src, dst = ROOT / f, dest / f
            if src.is_symlink():   # the Linux package's links (LLVM's clang -> clang-23) stay links, not copies
                link = os.readlink(src)
                if not (dst.is_symlink() and os.readlink(dst) == link):
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    tmp = dst.with_name(dst.name + '.copying')
                    if tmp.is_symlink() or tmp.exists():
                        tmp.unlink()
                    os.symlink(link, tmp)
                    os.replace(tmp, dst)
                continue
            st = src.stat()
            # The same bytes stay as they are, with their time: a package's files carry its commit's time, and a header
            # of the runtime copied anew would make ninja compile every file of the game again at each update.
            same = dst.is_file() and dst.stat().st_size == st.st_size and (
                int(dst.stat().st_mtime) == int(st.st_mtime) or filecmp.cmp(src, dst, shallow=False))
            if same and dst.stat().st_mode != st.st_mode:
                os.chmod(dst, st.st_mode)
            if not same:
                dst.parent.mkdir(parents=True, exist_ok=True)
                tmp = dst.with_name(dst.name + '.copying')
                shutil.copy2(src, tmp)   # with its mode: the Linux package's programs stay executable
                os.replace(tmp, dst)
            done += st.st_size
            emit('progress', label='copy', done=done, total=total, eta=None)
        if appimage:   # deps/ -> the AppImage's (a folder of the older layout had its own copy: replaced)
            link = dest / 'deps'
            if link.is_dir() and not link.is_symlink():
                shutil.rmtree(link)
            for old in IN_APPIMAGE[1:]:
                (dest / old).unlink(missing_ok=True)
            tmp = dest / 'deps.new'
            if tmp.is_symlink() or tmp.exists():
                tmp.unlink()
            os.symlink(ROOT / 'deps', tmp)
            os.replace(tmp, link)
        # From an AppImage ($APPIMAGE, set by its runtime): the AppImage itself into the folder as UltraTears.AppImage, the
        # one program the player starts there (--play: the game), as the Steam Deck's tar has it (tools/make_deck_tar.py)
        image = pathlib.Path(os.environ.get('APPIMAGE', ''))
        if image.name and image.is_file():
            dst = dest / 'UltraTears.AppImage'
            if image.resolve() != dst.resolve() and not (dst.is_file() and filecmp.cmp(image, dst, shallow=False)):
                tmp = dst.with_name(dst.name + '.copying')
                shutil.copy2(image, tmp)
                os.chmod(tmp, 0o755)
                os.replace(tmp, dst)
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
