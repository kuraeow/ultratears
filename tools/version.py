"""The port's version: one source, the git tag v<version> on the release commit.

  - a release: the tag on HEAD (tools/make_package.py and tools/public_tree.py refuse an untagged HEAD);
  - a builder package or an output folder made by tools/setup.py has no .git: VERSION.txt at its root says it
    ("UltraTears <version> (commit <hash>)", written by make_package.py / setup.py);
  - a clone: git describe, e.g. 0.1.0-3-gabc1234-dirty between releases;
  - a source ZIP from GitHub (no .git): tools/archive-version.txt, which git archive fills in (.gitattributes:
    export-subst) with the commit and its describe.
tools/build_game.py puts it into the exe (build/version.h: the first log line, the file's properties in Windows).
"""
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAG = re.compile(r'^v(\d+\.\d+\.\d+)$')


def _git(*args):
    try:
        r = subprocess.run(['git', '-C', str(ROOT)] + list(args), capture_output=True, text=True)
    except OSError:   # git not installed (a builder package without VERSION.txt): no version from git
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def release():
    """The version of the tag v<x.y.z> on HEAD, or None."""
    for t in (_git('tag', '--points-at', 'HEAD') or '').split():
        m = TAG.match(t)
        if m:
            return m.group(1)
    return None


def current():
    """(version, commit) of this tree."""
    f = ROOT / 'VERSION.txt'
    if f.is_file():
        m = re.match(r'UltraTears (\S+) \(commit (\w+)\)', f.read_text(encoding='utf-8'))
        if m:
            return m.group(1), m.group(2)
    if _git('rev-parse', 'HEAD') is None:   # no Git here: a source ZIP says what it was made from
        try:
            commit, *desc = (ROOT / 'tools/archive-version.txt').read_text(encoding='utf-8').split()
        except (OSError, ValueError):
            commit, desc = '', []
        if commit and not commit.startswith('$'):
            d = next((x for x in desc if x.startswith('v') and not x.startswith('$')), '')
            return (d[1:] if d else '0.0.0-dev'), commit
    d = _git('describe', '--tags', '--match', 'v[0-9]*', '--dirty')
    commit = _git('rev-parse', '--short', 'HEAD') or 'unknown'
    if d:
        return d[1:], commit
    if _git('rev-parse', 'HEAD'):   # no release tag yet
        dirty = '-dirty' if _git('status', '--porcelain', '--untracked-files=no') else ''
        return f'0.0.0-dev{dirty}', commit
    return 'unknown', 'unknown'


def version_txt(version, commit):
    return f'UltraTears {version} (commit {commit})\n'


def numbers(version):
    """Four numbers for the Windows version resource: x.y.z and the commits since the tag (0.1.0-3-g... -> 0,1,0,3)."""
    m = re.match(r'(\d+)\.(\d+)\.(\d+)(?:-(\d+)-g)?', version)
    return tuple(int(x or 0) for x in m.groups()) if m else (0, 0, 0, 0)
