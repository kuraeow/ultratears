"""Development: F9 snapshots summed up for reporting. Each diag/f<N>/ (TOTK_DUMP_KEY=diag, the launcher's "Extended
debugging with snapshots") is reduced to its findings: textures and buffer ranges that differ from game memory, textures
left all zero, skipped draws, errors and warnings. The same finding in several snapshots is counted once.

usage: python tools/dev_triage.py [<game folder or its diag folder>] [--max-issues N]
       python tools/dev_triage.py --self-test
Writes into the diag folder: triage.json (compact, for an assistant), triage.md (for people) and issues/NN-<kind>.md
(drafts, text only: render targets are made from the game and stay on this PC). diag/f<N>/note.txt, if written, is what
the player saw there (one line); it goes into the drafts.
"""
import argparse
import json
import pathlib
import re
import sys
import tempfile

KINDS = {   # in the order they are reported: what each finding means (src/gfx/vkb_tex.c, backend_vk.c)
    'stale-texture': 'The image differs from game memory with no CPU write seen since its upload: draws read other '
                     'contents than the game holds (a missed update).',
    'cpu-over-gpu-texture': 'Game memory differs from a GPU-written texture after the write-back: the CPU wrote over '
                            'the GPU contents.',
    'stale-buffer': 'A buffer range a traced draw reads differs from game memory with no CPU write since its update.',
    'zero-texture': 'A texture the traced draws sample is all zero in game memory and no GPU-written image is over it.',
    'skipped-draw': 'Draws skipped: no program, or no translation of a shader.',
    'error': 'An error line of the renderer during the snapshot.',
    'warning': 'A warning line of the renderer during the snapshot.',
}
PATTERNS = [
    ('stale-texture', re.compile(r'\[vcheck\] texture \d+ format (\S+) (\d+x\d+) layers \d+ at \S+: the image differs')),
    ('cpu-over-gpu-texture', re.compile(r'\[vcheck\] GPU-written texture \d+ format (\S+) (\d+x\d+) at \S+: game memory differs')),
    ('stale-buffer', re.compile(r'\[vcheck\] draw #\d+ (\S+) \S+: the copy differs')),
    ('zero-texture', re.compile(r'\[vcheck\] texture \d+ read from game memory, all zero, .*format (\S+) (\d+x\d+)')),
    ('error', re.compile(r'\[error\] (.*)')),
    ('warning', re.compile(r'\[warning\] (.*)')),
]
DRAW = re.compile(r'\[draw\] frame \d+ .*\| prog \d+ vs (\S+) fs (\S+)')
NUMBERS = re.compile(r'0x[0-9a-fA-F]+|\d+')


def scan(snap):
    """One snapshot folder: its findings as {(kind, key): first line} and its facts."""
    found, draws, last_draw, truncated = {}, 0, None, False
    log = snap / 'f9.log'
    lines = log.read_text(errors='replace').splitlines() if log.is_file() else []
    for line in lines:
        if '[draw] frame ' in line:   # a draw (vs / fs) or a dispatch: the "skipped" line below belongs to it
            m = DRAW.search(line)
            draws += bool(m)
            last_draw = m and f'vs {m[1]} fs {m[2]}'
            continue
        if '[limit]' in line:
            truncated = True
        if '[draw]   skipped:' in line and last_draw:
            found.setdefault(('skipped-draw', last_draw), line)
            continue
        for kind, rx in PATTERNS:
            m = rx.search(line)
            if m:
                key = NUMBERS.sub('#', m[1]) if kind in ('error', 'warning') else ' '.join(m.groups())
                found.setdefault((kind, key), line)
                break
    note = snap / 'note.txt'
    return found, {'frame': int(snap.name[1:]), 'dir': snap.name, 'log_lines': len(lines), 'draws_traced': draws,
                   'truncated': truncated, 'has_log': log.is_file(), 'targets': len(list(snap.glob('*.bmp'))),
                   'note': note.read_text(errors='replace').strip() if note.is_file() else ''}


def triage(diag, max_issues):
    snaps = sorted((p for p in diag.glob('f*') if p.is_dir() and p.name[1:].isdigit()), key=lambda p: int(p.name[1:]))
    findings, facts = {}, []
    for snap in snaps:
        found, fact = scan(snap)
        facts.append(fact)
        for k, line in found.items():
            f = findings.setdefault(k, {'kind': k[0], 'key': k[1], 'snapshots': [], 'evidence': line[:400]})
            f['snapshots'].append(fact['frame'])
    order = list(KINDS)
    ranked = sorted(findings.values(), key=lambda f: (order.index(f['kind']), -len(f['snapshots']), f['key']))
    for n, f in enumerate(ranked, 1):
        f['id'] = n
    version = diag.parent / 'VERSION.txt'
    report = {'version': version.read_text().strip() if version.is_file() else '', 'snapshots': facts, 'findings': ranked}
    (diag / 'triage.json').write_text(json.dumps(report, indent=1))
    md = [f'# F9 triage: {len(facts)} snapshots, {len(ranked)} findings', '',
          '| snapshot | note | log lines | draws traced | targets | cut at the limit |', '|---|---|---|---|---|---|']
    md += [f"| {s['dir']} | {s['note'] or '-'} | {s['log_lines']} | {s['draws_traced']} | {s['targets']} | "
           f"{'yes' if s['truncated'] else ''} |" for s in facts]
    md += ['', '| # | kind | key | snapshots |', '|---|---|---|---|']
    md += [f"| {f['id']} | {f['kind']} | `{f['key']}` | {', '.join(f'f{s}' for s in f['snapshots'])} |" for f in ranked]
    (diag / 'triage.md').write_text('\n'.join(md) + '\n')
    issues = diag / 'issues'
    issues.mkdir(exist_ok=True)
    for old in issues.glob('*.md'):
        old.unlink()
    notes = {s['frame']: s['note'] for s in facts if s['note']}
    for f in ranked[:max_issues]:
        seen = [f'- f{s}: {notes[s]}' for s in f['snapshots'] if s in notes] or ['- (no note: diag/f<N>/note.txt)']
        (issues / f"{f['id']:02d}-{f['kind']}.md").write_text('\n'.join([
            f"# [render] {f['kind']}: {f['key']}", '',
            f"**Snapshots:** {', '.join(f'f{s}' for s in f['snapshots'])} ({len(f['snapshots'])} of {len(facts)})",
            f"**Version:** {report['version'] or '?'}", '', '**What the player saw:**', *seen, '',
            '## What it means', KINDS[f['kind']], '', f"## Evidence (diag/f{f['snapshots'][0]}/f9.log)", '```text',
            f['evidence'], '```', '', '## Reproduce',
            'Turn on "Extended debugging with snapshots" in the launcher, go to the place above, press F9, then run '
            '`python tools/dev_triage.py <game folder>`.', '']))
    return report


def self_test():
    with tempfile.TemporaryDirectory() as t:
        diag = pathlib.Path(t) / 'diag'
        stale = '[totk-gfx] [vk] [vcheck] texture 7 format 0x25 256x256 layers 1 at 0x1: the image differs from game memory in 9 of 99 bytes'
        draw = '[totk-gfx] [vk] [draw] frame 5 indexed prim 4 count 3 inst 1 | rt 1: tex 2 fmt 0x25 8x8 | prog 3 vs aa fs bb | 2 attrs'
        for n, extra in ((10, [draw, '[totk-gfx] [vk] [draw]   skipped: no program / translation']), (20, ['[limit] x'])):
            (diag / f'f{n}').mkdir(parents=True)
            (diag / f'f{n}' / 'f9.log').write_text('\n'.join([stale, *extra]) + '\n')
        (diag / 'f10' / 'note.txt').write_text('screen white with Recall\n')
        r = triage(diag, 10)
        assert [(f['kind'], f['key'], f['snapshots']) for f in r['findings']] == [
            ('stale-texture', '0x25 256x256', [10, 20]), ('skipped-draw', 'vs aa fs bb', [10])], r['findings']
        assert r['snapshots'][1]['truncated'] and r['snapshots'][0]['note'] == 'screen white with Recall'
        assert 'screen white with Recall' in (diag / 'issues' / '01-stale-texture.md').read_text()
    print('self-test ok')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('folder', nargs='?', default='.')
    ap.add_argument('--max-issues', type=int, default=10)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    diag = pathlib.Path(a.folder)
    if (diag / 'diag').is_dir():
        diag = diag / 'diag'
    if not diag.is_dir():
        sys.exit(f'no diag folder at {diag}')
    r = triage(diag, a.max_issues)
    print(f"{len(r['snapshots'])} snapshots, {len(r['findings'])} findings: {diag / 'triage.md'}, {diag / 'triage.json'}, "
          f"{min(len(r['findings']), a.max_issues)} drafts in {diag / 'issues'}")


if __name__ == '__main__':
    main()
