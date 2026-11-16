"""The vertex shader variants to compile ahead, from the MacNativeGames shared shader list (MIT;
https://github.com/ly05010419/MacNativeGames, games/totk/assets/shader_list.tsv, docs/shader-list.md there).

A vertex shader is drawn as a variant of itself (src/gfx/vkb_shader.c vkb_shader_variant): integer attributes read as
raw bits (TOTK's skinned shaders read bone indices that way) and outputs (0, 0, 0, 1) for the fragment inputs it does
not write. Which variant a draw needs depends on the draw (the attribute formats, the fragment shader it is paired
with), so these were compiled at their first draw, on a recording thread: 7-24 at a time where new creatures came into
view, frames of 50-210 ms (2026-11-16). The list names what players met: its V rows give a shader's attribute types
(the integer ones are our integer masks), its R rows a render pipeline's vertex and fragment shader (our pairs). The
game compiles those variants ahead with the shaders themselves (UltraTears --prepare-shaders), and makes a program's
variants when the program is set.

MacNativeGames prunes V rows its own runtime no longer needs (default variants, those that differ only in vertex input
or colour output types), so the list of one revision lacks most integer attribute rows: they are read from every
revision in the file's history. The list holds identifiers only (shader keys: SHA-256 of a shader's NVN control section
and code, as the game computes them; attribute types), no game data; nothing of it goes into the repository. The
builder package carries the result (tools/make_package.py), with the list's license.

usage: python tools/shader_variants.py <MacNativeGames checkout> <out file> [--rev REV]
Output, one line each (the game reads it, vkb_variants.c):
  I <vertex shader key> <integer mask, hex> <signed mask, hex>
  P <vertex shader key> <fragment shader key>
"""
import argparse
import json
import pathlib
import subprocess
import sys

PATH = 'games/totk/assets/shader_list.tsv'


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, encoding='utf-8').stdout


def rows(repo, rev):
    """Each line of the list at rev and in every older revision of it."""
    revs = git(repo, 'log', '--format=%H', rev, '--', PATH).split()
    if not revs:
        sys.exit(f'{repo}: no {PATH} at {rev}')
    seen = set()
    for h in revs:
        for line in git(repo, 'show', f'{h}:{PATH}').splitlines():
            if line not in seen:
                seen.add(line)
                yield line


def build(repo, rev='HEAD'):
    ints, pairs = set(), set()
    for line in rows(repo, rev):
        f = line.split('\t')
        if f[0] == 'V' and len(f) >= 3 and len(f[1]) == 64:
            try:
                attr = json.loads(f[2]).get('attr', {})
            except ValueError:
                continue
            im = sm = 0
            for loc, kind in attr.items():
                if loc.isdigit() and int(loc) < 32 and kind in ('sint', 'uint'):
                    im |= 1 << int(loc)
                    if kind == 'sint':
                        sm |= 1 << int(loc)
            if im:
                ints.add((f[1], im, sm))
        elif f[0] == 'R' and len(f) >= 2:
            parts = f[1].split(':')   # <vertex variant>:<fragment variant>:<the rest>; a variant is <key>-<hash>_b
            if len(parts) >= 2:
                vs, fs = parts[0].split('-')[0], parts[1].split('-')[0]
                if len(vs) == 64 and len(fs) == 64:
                    pairs.add((vs, fs))
    return ints, pairs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('repo', type=pathlib.Path)
    ap.add_argument('out', type=pathlib.Path)
    ap.add_argument('--rev', default='HEAD')
    a = ap.parse_args()
    commit = git(a.repo, 'rev-parse', a.rev).strip()
    ints, pairs = build(a.repo, a.rev)
    tmp = a.out.with_suffix(a.out.suffix + '.tmp')
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'# UltraTears vertex shader variants (tools/shader_variants.py) from the MacNativeGames shader list @ {commit}\n')
        f.write('# (MIT, Copyright (c) 2026 MacNativeGames contributors). Identifiers only, no game data.\n')
        for k, im, sm in sorted(ints):
            f.write(f'I {k} {im:x} {sm:x}\n')
        for vs, fs in sorted(pairs):
            f.write(f'P {vs} {fs}\n')
    tmp.replace(a.out)
    print(f'{a.out}: {len(ints)} integer attribute variants, {len(pairs)} vertex / fragment pairs (list @ {commit[:12]})')


if __name__ == '__main__':
    main()
