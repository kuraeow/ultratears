"""Player mods: what the launcher shows on its Mods page, and the code their patches and cheats change (src/host/mods.c).

usage: python tools/mods_code.py list      the mods, their files, patches and cheats as one JSON object on stdout
       python tools/mods_code.py build     data/mods/code.dll (Linux: code.so) and code.txt, when the patch and cheat
                                           files changed since the last build ("up to date" otherwise)
Run in the game folder. The mods folder is ultratears.cfg's mods_dir (default mods), one folder per mod.

The game's code is translated ahead of time, so an instruction a mod's ExeFS patch (.ips / .pchtxt) or a code cheat
(Atmosphere opcode 0 into main's code) changes needs its function translated again. This builds only those functions
(and those of the same translated file that call them directly: the compiler may have inlined them there) into a
library the game puts in place of its own at start. Every changed instruction is behind a switch, its mod's or its
cheat's, so turning mods and cheats on and off needs no build; only new or changed patch and cheat files do (a few
seconds). An instruction that changes control flow (a branch) cannot be switched inside one translation: such a
function is translated once per combination of its branch-changing switches and picks one when it is called.
Data patches (outside the code) and data cheats need nothing here: the game applies them itself.
"""
import bisect
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'tools/lifter'))

TITLE_ID = '0100F2C0115B6000'
MODULES = {'main': 0x0800000000, 'subsdk0': 0x0804000000, 'sdk': 0x0805000000}   # src/runtime/rt.h
LINUX = os.name != 'nt'
LIB = 'data/mods/code.so' if LINUX else 'data/mods/code.dll'
MANIFEST = 'data/mods/code.txt'
# The runtime functions the library may call (src/host/mods.c IMPORTS); a name outside these fails the build.
IMPORTS = {'rt_dispatch', 'rt_udf', 'rt_brk', 'rt_svc', 'rt_ticks', 'rt_spin_gate', 'fnprof_hit', 'ultrs_keys',
           'totk_zstd_native', 'totk_zstd_multiframe', 'totk_zstd_using_ddict', 'totk_fr_step_ratio30',
           'totk_fr_highfps', 'totk_fr_realtime', 'totk_fr_cloth', 'totk_fr_step_seconds', 'totk_fr_step_ticks60',
           'totk_fr_fps60', 'totk_drs_enabled', 'totk_ability_target_ok', 'totk_shadow_size', 'totk_output_size',
           'totk_scene_size', 'totk_aspect', 'totk_mouse_camera_input', 'totk_mouse_orbit_begin',
           'totk_mouse_orbit_state', 'totk_mouse_orbit_angles', 'totk_mouse_hand_input', 'totk_mouse_load',
           'totk_mouse_camera_activity'}
OWN = {'ultrs_sw', 'g_fnprof', 'g_simd_nanvec'}   # defined in the library's glue


def cfg():
    out = {}
    try:
        for line in (ROOT / 'ultratears.cfg').read_text(encoding='utf-8', errors='replace').splitlines():
            if '=' in line and not line.startswith('#'):
                k, v = line.split('=', 1)
                out[k.strip()] = v
    except OSError:
        pass
    return out


# ---------------------------------------------------------------- the scan (as src/host/mods.c mods_scan)
class Mod:
    def __init__(self, name, root):
        self.name, self.root = name, root
        self.romfs = find_part(root, 'romfs')
        self.cheats = find_part(root, 'cheats')
        ex = find_part(root, 'exefs')
        self.exefs = [ex] if ex else []
        ep = f'{root}/atmosphere/exefs_patches'
        if os.path.isdir(ep):
            self.exefs += [f'{ep}/{d}' for d in sorted(os.listdir(ep)) if os.path.isdir(f'{ep}/{d}')]
        self.enabled = True


def find_part(root, part):
    for form in ('{r}/{p}', '{r}/atmosphere/contents/%s/{p}', '{r}/contents/%s/{p}', '{r}/%s/{p}'):
        for tid in (TITLE_ID, TITLE_ID.lower()):
            p = form.replace('%s', tid).format(r=root, p=part)
            if os.path.isdir(p):
                return p
    return ''


def scan():
    c = cfg()
    top = c.get('mods_dir') or 'mods'
    mods = []
    if os.path.isdir(top):
        mods = [Mod(n, f'{top}/{n}') for n in os.listdir(top) if os.path.isdir(f'{top}/{n}')]
    order = [n for n in (c.get('mods_order') or '').split('|') if n]
    off = set(n for n in (c.get('mods_off') or '').split('|') if n)
    pos = {n: i for i, n in enumerate(order)}
    mods.sort(key=lambda m: (pos.get(m.name, 1 << 20), m.name.encode()))
    for m in mods:
        m.enabled = '|' not in m.name and m.name not in off
    return top, mods, c


def main_build_id():
    return open(ROOT / 'data/exefs/main', 'rb').read(0x60)[0x40:0x60]


def cheat_file(m, bid):
    if not m.cheats:
        return None
    want = bid[:8].hex().upper() + '.txt'
    for n in os.listdir(m.cheats):
        if n.upper() == want.upper() and os.path.isfile(f'{m.cheats}/{n}'):
            return f'{m.cheats}/{n}'
    return None


def patch_files(m):
    out = []
    for d in m.exefs:
        for n in os.listdir(d):
            if n.lower().endswith(('.ips', '.pchtxt')) and os.path.isfile(f'{d}/{n}'):
                out.append(f'{d}/{n}')
    return out


def stamp(mods, bid):
    """FNV-1a 64 of every patch and cheat file of every mod (src/host/mods.c mods_stamp)."""
    keys = []
    for m in mods:
        files = patch_files(m)
        cf = cheat_file(m, bid)
        if cf:
            files.append(cf)
        for f in files:
            st = os.stat(f)
            keys.append(f'{m.name}\x1f{f[len(m.root) + 1:]}\x1f{st.st_size}\x1f{int(st.st_mtime)}\n'.encode())
    h = 14695981039346656037
    for k in sorted(keys):
        for b in k:
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h if keys else 0


# ---------------------------------------------------------------- patch files (as mods.c parse_ips / parse_pchtxt)
def parse_ips(d):
    wide = d[:5] == b'IPS32'
    if not wide and d[:5] != b'PATCH':
        return None
    ob, p, out = (4 if wide else 3), 5, []
    while True:
        if p + ob > len(d):
            return None
        if d[p:p + 4] == b'EEOF' if wide else d[p:p + 3] == b'EOF':
            return out
        off = int.from_bytes(d[p:p + ob], 'big')
        p += ob
        ln = int.from_bytes(d[p:p + 2], 'big')
        p += 2
        if ln == 0:
            cnt = int.from_bytes(d[p:p + 2], 'big')
            data = bytes([d[p + 2]]) * cnt
            p += 3
        else:
            data = d[p:p + ln]
            p += ln
        if off >= 0x100:
            out.append((off - 0x100, data))


def parse_pchtxt(t):
    shift, on, le, bid, out = 0, True, False, '', []
    for line in t.splitlines():
        line = line.split('//', 1)[0].strip()
        if not line:
            continue
        low = line.lower()
        if low.startswith('@nsobid-'):
            bid = re.match(r'[0-9a-fA-F]*', line[8:]).group(0)
        elif low.startswith('@flag'):
            rest = low[5:].strip()
            if rest.startswith('offset_shift'):
                shift = int(rest[12:].strip(), 0)
            elif rest.startswith('le'):
                le = True
            elif rest.startswith('be'):
                le = False
        elif low.startswith('@little-endian'):
            le = True
        elif low.startswith('@big-endian'):
            le = False
        elif low.startswith('@enabled'):
            on = True
        elif low.startswith('@disabled'):
            on = False
        elif low.startswith('@stop'):
            break
        elif line.startswith('@'):
            continue
        elif on:
            m = re.match(r'([0-9a-fA-F]+)\s+(.*)', line)
            if not m:
                continue
            off, val = int(m.group(1), 16), m.group(2).strip()
            if val.startswith('"'):
                data = val[1:].split('"', 1)[0].encode()
            else:
                hx = re.match(r'[0-9a-fA-F]*', val).group(0)
                data = bytes.fromhex(hx[:len(hx) // 2 * 2])
                if le:
                    data = data[::-1]
            img = off + shift - 0x100
            if data and img >= 0:
                out.append((img, data))
    return bid, out


def bid_match(hexs, bid):
    if not 16 <= len(hexs) <= 64:
        return False
    full = bid.hex()
    return full[:len(hexs)] == hexs.lower() and set(full[len(hexs):]) <= {'0'}


def mod_writes(m, bids):
    """{module: [(offset, bytes)]} of mod m's patch files, and the files for no module here."""
    out, other = {}, 0
    for f in patch_files(m):
        d = open(f, 'rb').read()
        if f.lower().endswith('.ips'):
            w, hexs = parse_ips(d), os.path.basename(f)[:-4]
        else:
            hexs, w = parse_pchtxt(d.decode('utf-8', 'replace'))
        if w is None:
            continue
        mod = next((n for n, b in bids.items() if bid_match(hexs, b)), None)
        if mod is None:
            other += 1
            continue
        out.setdefault(mod, []).extend(w)
    return out, other


# ---------------------------------------------------------------- cheats (as mods.c cheats_file / op_words)
def op_words(op, i):
    w = op[i]
    t, width = w >> 28, (w >> 24) & 15
    if t in (0, 1):
        return 4 if width == 8 else 3
    if t == 2 or t == 8:
        return 1
    if t == 3:
        return 1 if (w >> 24) & 15 else 2
    if t in (4, 6):
        return 3
    if t in (5, 7):
        return 2
    if t == 9:
        return (3 if width == 8 else 2) if (w >> 8) & 15 else 1
    if t == 0xA:
        return 2 if (w >> 8) & 15 in (2, 4, 5) else 1
    if t == 0xC:
        sub = (w >> 24) & 15
        if sub == 0:
            x, tw = (w >> 8) & 15, (w >> 20) & 15
            return 2 if x in (0, 2) else (3 if tw == 8 else 2) if x == 4 else 1
        return 1 if sub <= 3 else 0
    return 0


def parse_cheats(text, modname):
    cheats, cur = [], None
    for line in text.splitlines():
        line = line.strip()
        if line[:1] in ('[', '{'):
            close = ']' if line[0] == '[' else '}'
            if close not in line[1:]:
                continue
            name = line[1:line.index(close, 1)]
            if 'TID=' in name or not name:
                cur = None
                continue
            name = name.encode('utf-8')[:127].decode('utf-8', 'ignore')
            same = sum(1 for c in cheats if c['name'] == name or c['name'].startswith(name + '#'))
            if same:
                name = f'{name}#{same + 1}'
            cur = {'name': name, 'master': close == '}', 'op': [], 'bad': False}
            cur['id'] = fnv32(f'{modname}/{name}'.encode())
            cheats.append(cur)
            continue
        if cur is None:
            continue
        for tok in line.split():
            try:
                cur['op'].append(int(tok, 16) & 0xFFFFFFFF)
            except ValueError:
                cur['bad'] = True
                break
    return cheats


def fnv32(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


def classify(c, text_end, code_end, image):
    """A cheat's kind: 'data' (the cheat VM runs it), 'code' with its writes into main's code [(offset, word)] (up to
    code_end: the code and the padding after it, where cheats put code caves) and the buttons it waits for (0: none),
    'restore' (it writes back the game's own code: not needed, switching a cheat off restores it), or 'unsupported'
    with why: 'opcode' (one the port does not run), 'write' (a code write of another width) or 'mixed' (code writes
    with other opcodes). The offset register of a store (0TMR) is taken as 0, as cheats that set none expect."""
    op, i, writes, keys, in_else, code, other = c['op'], 0, [], 0, False, False, False
    while i < len(op):
        n = op_words(op, i)
        if not n or i + n > len(op):
            return 'unsupported', 'opcode', [], 0
        w = op[i]
        t = w >> 28
        if t == 0 and (w >> 20) & 15 == 0 and ((w & 0xFF) << 32 | op[i + 1]) < code_end:
            code = True
            if not in_else:
                width = (w >> 24) & 15
                off = (w & 0xFF) << 32 | op[i + 1]
                val = (op[i + 2] << 32 | op[i + 3]) if width == 8 else op[i + 2]
                if width not in (1, 2, 4, 8):
                    return 'unsupported', 'write', [], 0
                for k in range(width):   # merged into the original words
                    writes.append((off + k, (val >> (8 * k)) & 0xFF))
        elif t == 8:
            keys = w & 0x0FFFFFFF
        elif t == 2:
            in_else = (w >> 24) & 15 == 1
        else:
            other = True
        i += n
    if not code:
        return 'data', '', [], 0
    if other:
        return 'unsupported', 'mixed', [], 0
    words = {}
    for off, b in writes:
        a = off & ~3
        if a not in words:
            words[a] = bytearray(image[a:a + 4])
        words[a][off - a] = b
    out = sorted((a, struct.unpack('<I', bytes(v))[0]) for a, v in words.items())
    if all(a < text_end and w == struct.unpack_from('<I', image, a)[0] for a, w in out):
        return 'restore', 'restore', [], 0
    return 'code', '', out, keys


def main_code():
    """main's code end, the end of the padding after it (its read-only data starts there) and its image."""
    from nso import Nso
    n = Nso(str(ROOT / 'data/exefs/main'))
    return n.text[0] + n.text[1], n.rodata[0], n.image


# ---------------------------------------------------------------- list
def romfs_index():
    """The RomFS paths (data/romfs/runtime-index.bin, TOTKRFS1) as a set, and their lower-case spelling."""
    p = ROOT / 'data/romfs/runtime-index.bin'
    names = set()
    if p.is_file():
        d = p.read_bytes()
        count, strings = struct.unpack_from('<QQ', d, 8)
        for i in range(count):
            _, _, off, ln = struct.unpack_from('<QQII', d, 24 + i * 24)
            names.add(d[strings + off:strings + off + ln - 1].decode('utf-8', 'replace'))
    elif (ROOT / 'data/romfs').is_dir():
        base = ROOT / 'data/romfs'
        for dp, _, fs in os.walk(base):
            for f in fs:
                names.add(os.path.relpath(os.path.join(dp, f), base).replace(os.sep, '/'))
    return names, {n.lower(): n for n in names}


def cmd_list():
    top, mods, c = scan()
    bid = main_build_id()
    names, lower = romfs_index()
    taken = {}
    exe_text = None
    out = []
    for m in mods:
        e = {'name': m.name, 'enabled': m.enabled, 'romfs': bool(m.romfs), 'exefs': bool(m.exefs), 'cheats': [],
             'replaced': 0, 'added': 0, 'other_version': 0, 'overridden': 0, 'overridden_by': '', 'patch_files': 0,
             'other_build': 0, 'problem': ''}
        if '|' in m.name:
            e['problem'] = 'bad_name'
        if m.romfs:
            for dp, _, fs in os.walk(m.romfs):
                for f in fs:
                    rel = os.path.relpath(os.path.join(dp, f), m.romfs).replace(os.sep, '/')
                    rel = lower.get(rel.lower(), rel)
                    if rel in taken and m.enabled:
                        e['overridden'] += 1
                        e['overridden_by'] = e['overridden_by'] or taken[rel]
                        continue
                    if m.enabled:
                        taken[rel] = m.name
                    if rel in names:
                        e['replaced'] += 1
                    else:
                        e['added'] += 1
                        if re.search(r'\.Product\.\d', rel):
                            e['other_version'] += 1
        if m.exefs:
            bids = {'main': bid}
            w, other = mod_writes(m, bids)
            e['patch_files'] = len(patch_files(m)) - other
            e['other_build'] = other
        cf = cheat_file(m, bid)
        if cf:
            if exe_text is None:
                exe_text = main_code()
            for ch in parse_cheats(open(cf, encoding='utf-8', errors='replace').read(), m.name):
                kind, why, _, keys = ('unsupported', 'unreadable', [], 0) if ch['bad'] else classify(ch, *exe_text)
                if not ch['op'] or ch['op'] == [0x20000000]:   # a name alone, or a folder's start (20000000)
                    kind = 'heading'
                elif ch['op'] == [0x20000001]:   # a folder's end: nothing to show
                    continue
                e['cheats'].append({'id': f"{ch['id']:08x}", 'name': ch['name'], 'master': ch['master'], 'kind': kind,
                                    'why': why, 'keys': keys})
        if not (m.romfs or m.exefs or m.cheats):
            e['problem'] = e['problem'] or 'empty'
        out.append(e)
    on = [x for x in (c.get('cheats_on') or '').replace(' ', '').split(',') if x]
    state = 'none'
    if any(x['patch_files'] for x in out) or any(ch['kind'] == 'code' for x in out for ch in x['cheats']):
        state = 'ok' if built_ok(mods, bid) else 'stale'
    print(json.dumps({'dir': top, 'mods': out, 'cheats_on': on, 'code': state, 'build_id': bid[:8].hex().upper()},
                     ensure_ascii=False))


def exe_line():
    exe = ROOT / ('linux/ultratears' if LINUX else 'UltraTears.exe')
    st = exe.stat()
    return f'exe {st.st_size} {int(st.st_mtime)}'


def built_ok(mods, bid):
    try:
        txt = (ROOT / MANIFEST).read_text(encoding='utf-8')
    except OSError:
        return False
    lines = txt.splitlines()
    want = f'stamp {stamp(mods, bid):016x}'
    order = [l.split(' ', 3)[3] for l in lines if l.startswith('sw ') and l.split(' ')[2] == 'mod']
    names = [m.name for m in mods]
    pos = [names.index(n) for n in order if n in names]
    return want in lines and exe_line() in lines and pos == sorted(pos) and (ROOT / LIB).is_file()


# ---------------------------------------------------------------- build
def cmd_build():
    t0 = time.time()
    top, mods, _ = scan()
    bid = main_build_id()
    (ROOT / 'data/mods').mkdir(parents=True, exist_ok=True)
    if built_ok(mods, bid):
        print('up to date')
        return 0
    st = stamp(mods, bid)
    if not st:   # no patch or cheat files: nothing to build, and no library to load
        for p in (ROOT / LIB, ROOT / MANIFEST):
            p.unlink(missing_ok=True)
        print('no code patches or code cheats')
        return 0
    from nso import Nso
    bids = {name: open(ROOT / f'data/exefs/{name}', 'rb').read(0x60)[0x40:0x60] for name in MODULES
            if (ROOT / f'data/exefs/{name}').is_file()}
    # every changed word: {module: {offset: [(switch, word, keys, mod)]}}, top mod first, cheats before patches
    sites, switches, notes = {}, [], []
    images = {}

    def image(name):
        if name not in images:
            n = Nso(str(ROOT / f'data/exefs/{name}'))
            images[name] = (n.text[0], n.text[0] + n.text[1], n.rodata[0], n.image)
        return images[name]

    # a site: (switch, new word, buttons, who, the code cave its writer put after the code {offset: word})
    for m in mods:
        cf = cheat_file(m, bid)
        if not cf:
            continue
        lo, hi, ro, img = image('main')
        for ch in parse_cheats(open(cf, encoding='utf-8', errors='replace').read(), m.name):
            if ch['bad'] or not ch['op']:
                continue
            kind, why, words, keys = classify(ch, hi, ro, img)
            if kind != 'code':
                continue
            sw = len(switches)
            switches.append(f"sw {sw} cheat {ch['id']:08x} {m.name}")
            cave = {a: w for a, w in words if a >= hi}
            for off, word in words:
                if off < hi:
                    sites.setdefault('main', {}).setdefault(off, []).append((sw, word, keys, f"{m.name}: {ch['name']}", cave))
    for m in mods:
        w, _ = mod_writes(m, bids)
        sw = None
        for name, writes in w.items():
            lo, hi, ro, img = image(name)
            words = {}
            for off, data in writes:
                for k, b in enumerate(data):
                    a = off + k
                    if lo <= a < ro:
                        words.setdefault(a & ~3, bytearray(img[a & ~3:(a & ~3) + 4]))[a & 3] = b
            if words and sw is None:
                sw = len(switches)
                switches.append(f'sw {sw} mod {m.name}')
            cave = {a: struct.unpack('<I', bytes(v))[0] for a, v in words.items() if a >= hi}
            for a, v in words.items():
                word = struct.unpack('<I', bytes(v))[0]
                if a < hi and word != struct.unpack_from('<I', img, a)[0]:
                    sites.setdefault(name, {}).setdefault(a, []).append((sw, word, 0, m.name, cave))
    if not sites:
        write_manifest(st, switches, [])
        (ROOT / LIB).unlink(missing_ok=True)
        print('no code patches or code cheats')
        return 0
    c_text, fns, built_notes = lift(sites, image)
    notes += built_notes
    if not fns:
        for n in notes:
            print('note:', n)
        print('ERROR: none of the code changes could be built', file=sys.stderr)
        return 1
    ok = compile_lib(c_text, fns, len(switches))
    if not ok:
        return 1
    write_manifest(st, switches, fns)
    for n in notes:
        print('note:', n)
    print(f'built {len(fns)} functions for {sum(len(v) for v in sites.values())} changed instructions, '
          f'{len(switches)} switches, in {time.time() - t0:.1f} s')
    return 0


def write_manifest(st, switches, fns):
    lines = [f'stamp {st:016x}', exe_line()] + switches
    tmp = ROOT / (MANIFEST + '.new')
    tmp.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, ROOT / MANIFEST)


def entries_of(name):
    t = (ROOT / f'data/generated/{name}/{name}_table.c').read_text()
    return sorted(int(x, 16) for x in re.findall(r'^void f_([0-9a-f]+)\(Cpu \*c\);', t, re.M))


def chunk_index(name):
    """[(first, last, file)] of the translated files of module name, by address (cached beside the library)."""
    gen_dir = ROOT / f'data/generated/{name}'
    table = gen_dir / f'{name}_table.c'
    cache = ROOT / f'data/mods/{name}.chunks'
    key = f'{table.stat().st_size} {int(table.stat().st_mtime)}'
    if cache.is_file():
        lines = cache.read_text().splitlines()
        if lines and lines[0] == key:
            return [(int(a, 16), int(b, 16), f) for a, b, f in (l.split(' ') for l in lines[1:])]
    out = []
    for f in sorted(gen_dir.glob(f'{name}_[0-9]*.c')):
        d = f.read_bytes()
        defs = [int(x, 16) for x in re.findall(rb'^void f_([0-9a-f]+)\(Cpu \*c\) \{', d, re.M)]
        if defs:
            out.append((min(defs), max(defs), f.name))
    cache.write_text('\n'.join([key] + [f'{a:x} {b:x} {f}' for a, b, f in out]) + '\n')
    return out


def callers_in_file(path, targets):
    """The functions of a translated file that call any of targets directly, again for those, until none is new."""
    d = path.read_text(errors='replace')
    bodies = {}
    for m in re.finditer(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', d, re.M):
        bodies[int(m.group(1), 16)] = m.start()
    starts = sorted(bodies.values())
    text = {a: d[s:starts[bisect.bisect_right(starts, s)] if bisect.bisect_right(starts, s) < len(starts) else len(d)]
            for a, s in bodies.items()}
    found, todo = set(), set(targets)
    while todo:
        t = todo.pop()
        pat = f'f_{t:x}(c)'
        for a, body in text.items():
            if a not in found and a not in targets and a != t and pat in body:
                found.add(a)
                todo.add(a)
    return found


def is_plain(m, word, a):
    try:
        ins = m.lifter.lift(word, a)
    except Exception:
        return False
    return ins.kind in ('op', 'asm')


def is_cave(m, word, a, ro):
    try:
        ins = m.lifter.lift(word, a)
    except Exception:
        return False
    return ins.kind in ('b', 'bl') and m.hi <= ins.target < ro


def cave_code(m, site, word, cave, f, end, ro, tag, labels):
    """C for a branch at site into a code cave (code a cheat or patch wrote into the padding after the game's code):
    the cave's instructions inline, its branches back into function [f, end) as gotos to its labels (added to labels).
    cave: {module offset: word} of what the writer put there; elsewhere the padding is the module's own."""
    ins = m.lifter.lift(word, site)
    code = [f'c->x[30] = {site + 4:#x}ull;'] if ins.kind == 'bl' else []
    pending, a, n = set(), ins.target, 0

    def goto(x):
        if m.hi <= x < ro:
            pending.add(x)
            return f'goto LC_{tag}_{x:x};'
        if f <= x < end:
            labels.add(x)
            return f'goto L_{x:x};'
        raise ValueError(f'its code cave jumps to {x:#x}, outside the function it was entered from')

    while True:
        if not m.hi <= a < ro or n > 512:
            raise ValueError('its code cave runs past the padding after the code')
        off = a - m.base
        w = cave.get(off, struct.unpack_from('<I', m.nso.image, off)[0])
        i = m.lifter.lift(w, a)
        code.append(f'LC_{tag}_{a:x}:;')
        done = False
        if i.kind in ('op', 'asm'):
            code.append(m.c_of(w, a))
        elif i.kind == 'bcond':
            code.append(f'if ({i.cond}) {{ {goto(i.target)} }}')
        elif i.kind == 'b':
            code.append(goto(i.target))
            done = True
        elif i.kind == 'ret' and ins.kind == 'bl':
            code.append(goto(site + 4))
            done = True
        elif i.kind == 'bl' and m.in_text(i.target):
            code.append(f'c->x[30] = {a + 4:#x}ull; f_{i.target:x}(c);')
        else:
            raise ValueError(f'its code cave has an instruction the port cannot inline at {a:#x}')
        pending.discard(a)
        if done and not any(p > a for p in pending):
            return ' '.join(code)
        a += 4
        n += 1


def lift(sites, image):
    import gen
    import patches
    import regs
    out, fns, notes = [], [], []
    keys_cond = lambda sw, keys: f'(ultrs_sw[{sw}] && (ultrs_keys() & {keys:#x}ull) == {keys:#x}ull)' if keys \
        else f'ultrs_sw[{sw}]'
    for name, ss in sites.items():
        base = MODULES[name]
        m = gen.Module(name, str(ROOT / f'data/exefs/{name}'), base, 'c')
        m.reg_locals, m.local_flags = regs.LOCALS, True
        entries = entries_of(name)
        entry_set = set(entries)
        chunks = chunk_index(name)
        firsts = [c[0] for c in chunks]
        by_fn = {}
        for off, alts in ss.items():
            a = base + off
            i = bisect.bisect_right(entries, a) - 1
            if i < 0 or not m.in_text(a):
                notes.append(f'{alts[0][3]}: {a:#x} is outside the translated code')
                continue
            by_fn.setdefault(entries[i], {})[a] = alts
        # the functions a changed function may be inlined into: callers in its translated file
        extra = set()
        for f in by_fn:
            k = bisect.bisect_right(firsts, f) - 1
            if k >= 0 and chunks[k][0] <= f <= chunks[k][1]:
                extra |= callers_in_file(ROOT / f'data/generated/{name}' / chunks[k][2], set(by_fn))
        todo = sorted(set(by_fn) | extra)
        ro = base + image(name)[2]
        for f in todo:
            end = entries[bisect.bisect_right(entries, f)] if bisect.bisect_right(entries, f) < len(entries) else m.hi
            fsites = by_fn.get(f, {})
            # each change: 'plain' (one instruction for another), 'cave' (a branch into code its writer put after the
            # game's code, inlined here) or 'branch' (control flow changes: built into a variant of the function)
            kind = {}
            for a, alts in fsites.items():
                for x in alts:
                    kind[(a, x[0])] = 'branch' if not is_plain(m, m.word(a), a) else 'plain' if is_plain(m, x[1], a) \
                        else 'cave' if is_cave(m, x[1], a, ro) else 'branch'
            branchy = sorted({sw for (a, sw), k in kind.items() if k == 'branch'})
            keyed = {x[0]: x[2] for alts in fsites.values() for x in alts}
            if len(branchy) > 3:
                notes.append(f'{f:#x}: more than 3 mods or cheats change its branches: theirs left out')
                branchy = []
                fsites = {a: [x for x in alts if kind[(a, x[0])] != 'branch'] for a, alts in fsites.items()}
            variants = []
            for v in range(1 << len(branchy)):
                on = {sw for j, sw in enumerate(branchy) if v >> j & 1}
                orig, saved, extra_labels = {}, {}, set()
                try:
                    for a, alts in fsites.items():
                        switched = [x for x in alts if kind[(a, x[0])] != 'branch']
                        static = next((x for x in alts if x[0] in on and kind[(a, x[0])] == 'branch'), None)
                        saved[a] = (patches.PATCHES.pop(a, None), patches.PRE_PATCHES.pop(a, None))
                        if saved[a] != (None, None) and v == 0:
                            notes.append(f'{alts[0][3]}: replaces a change of the port at {a:#x}')
                        if static is not None:   # a branch change of a switch this variant has on: built in
                            orig[a] = m.word(a)
                            m.words[(a - m.lo) // 4] = static[1]
                            m.cache.pop(a, None)
                            if not is_plain(m, static[1], a):
                                continue
                        if switched:
                            chain = ''
                            for x in switched:
                                body = m.c_of(x[1], a) if kind[(a, x[0])] == 'plain' else \
                                    cave_code(m, a, x[1], x[4], f, end, ro, f'{a:x}_{x[0]}', extra_labels)
                                chain += f'if ({keys_cond(x[0], keyed.get(x[0], 0))}) {{ {body} }} else '
                            code = f'{chain}{{ {m.c_of(m.word(a), a)} }}'
                            patches.PATCHES[a] = (lambda c: lambda g, at: c)(code)
                    labels, ext, tables = m.scan(f, end)
                    if ext - entry_set:
                        raise ValueError(f'jumps to {min(ext - entry_set):#x}, which is not a function of the game')
                    m._fb = None
                    lines = m.emit_function(f, end, set(labels) | extra_labels, tables, {})
                    fb = m._fb
                except (Exception, SystemExit) as e:
                    notes.append(f'{f:#x}: cannot be built ({e}): the changes in it are left out')
                    lines = None
                for a, (p, pre) in saved.items():
                    patches.PATCHES.pop(a, None)
                    if p:
                        patches.PATCHES[a] = p
                    if pre:
                        patches.PRE_PATCHES[a] = pre
                for a, w in orig.items():
                    m.words[(a - m.lo) // 4] = w
                    m.cache.pop(a, None)
                if lines is None:
                    variants = None
                    break
                variants.append((lines, fb))
            if variants is None:
                continue
            fn, fbn = gen.fname(f), gen.fbname(f)
            if len(variants) == 1:
                lines, fb = variants[0]
                out += lines + (fb or [])
            else:
                for v, (lines, fb) in enumerate(variants):
                    lines = [lines[0].replace(f'void {fn}(', f'void {fn}_v{v}(')] + lines[1:]
                    body = '\n'.join(lines + (fb or [])).replace(f'{fbn}(', f'{fbn}_v{v}(')
                    out.append(body)
                sel = ' | '.join(f'({keys_cond(sw, keyed.get(sw, 0))} ? {1 << j} : 0)' for j, sw in enumerate(branchy))
                out.append(f'void {fn}(Cpu *c) {{\n    unsigned v = {sel};\n' +
                           ''.join(f'    if (v == {v}) {{ __attribute__((musttail)) return {fn}_v{v}(c); }}\n'
                                   for v in range(len(variants) - 1)) +
                           f'    __attribute__((musttail)) return {fn}_v{len(variants) - 1}(c);\n}}')
            fns.append(f)
    text = '\n'.join(out)
    names = sorted(set(re.findall(r'\b((?:f|fb)_[0-9a-f]+(?:_v\d+)?)\(', text)))
    head = ['#include "cpu.h"', '#include "simd.h"', 'extern uint8_t ultrs_sw[];', 'uint64_t ultrs_keys(void);']
    head += [f'void {n}(Cpu *c);' for n in names]
    return '\n'.join(head) + '\n' + text + '\n', fns, notes


def compile_lib(c_text, fns, nsw):
    from toolchain import CLANG64, exe, env as tool_env
    from build_game import cpu_arch, linux_flag_args
    work = ROOT / 'data/mods/build'
    work.mkdir(parents=True, exist_ok=True)
    arch = cpu_arch()
    cc = str(CLANG64 / exe('clang'))
    flags = ['-O2', f'-march={arch}', '-fno-strict-aliasing', '-w', '-I', str(ROOT / 'src/runtime')]
    if LINUX:   # the game folder's Linux kit, as the game's own build: a Steam Deck has no system headers or libraries
        flags += ['-fPIC', *linux_flag_args()]
    e = tool_env()
    # below the game and the desktop, as the game's own build (tools/build_game.py)
    nice = {} if LINUX else {'creationflags': 0x4000}   # BELOW_NORMAL_PRIORITY_CLASS

    def run(cmd):
        r = subprocess.run(cmd, capture_output=True, text=True, env=e, **nice)
        if r.returncode:
            print(r.stdout + r.stderr, file=sys.stderr)
            print('ERROR: ' + ' '.join(map(str, cmd[:3])) + ' failed', file=sys.stderr)
            return False
        return True

    # one file per function, compiled in parallel (a few of the game's functions are ~1 MB of C) and kept by the hash of
    # their text: a new mod compiles only what changed
    import concurrent.futures
    import hashlib
    starts = [mm.start() for mm in re.finditer(r'^void (?:f|fb)_[0-9a-f]+(?:_v\d+)?\(Cpu \*c\) \{', c_text, re.M)]
    head = c_text[:starts[0]] if starts else c_text
    units = [c_text[s:t] for s, t in zip(starts, starts[1:] + [len(c_text)])]
    cache = work / 'obj'
    cache.mkdir(exist_ok=True)
    jobs = []
    for k, u in enumerate(units):
        uflags = flags
        if 'L_dispatch:' in u and u.count('\nL_') > 2000:   # as build_game.py: such switches take clang hours at -O2
            uflags = [f if f != '-O2' else '-O0' for f in flags]
        key = hashlib.sha1((' '.join(uflags) + '\0' + head + u).encode()).hexdigest()[:20]
        obj = cache / f'{key}.o'
        if not obj.is_file():
            src = cache / f'{key}.c'
            src.write_text(head + u, newline='\n')
            jobs.append(([cc, *uflags, '-c', str(src), '-o', str(obj)], src))
        units[k] = obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, min(8, (os.cpu_count() or 2) // 2))) as pool:
        results = list(pool.map(lambda j: run(j[0]), jobs))
    for _, src in jobs:
        src.unlink(missing_ok=True)
    if not all(results):
        return False
    used = {o.name for o in units}
    for old in cache.glob('*.o'):   # objects no unit uses any more
        if old.name not in used:
            old.unlink(missing_ok=True)
    # the game's functions it calls: those it names and does not define; the runtime's: every one it may call (a
    # name the lifter's own replacements use that is not in IMPORTS fails the link below)
    defined = set(re.findall(r'^void ((?:f|fb)_[0-9a-f]+(?:_v\d+)?)\(Cpu \*c\) \{', c_text, re.M))
    ext = sorted(set(re.findall(r'\b(f_[0-9a-f]+)\(', c_text)) - defined)
    imps = sorted(IMPORTS)
    exp = '' if LINUX else '__declspec(dllexport) '
    g = ['#include <stdint.h>', 'typedef struct Cpu Cpu;',
         '' if LINUX else 'int __stdcall ultrs_dll_entry(void *h, unsigned reason, void *p) { return 1; }',
         f'{exp}uint8_t ultrs_sw[{max(nsw, 1)}];', f'{exp}int g_simd_nanvec;', 'volatile int g_fnprof;',
         f'{exp}uint64_t ultrs_fn_count = {len(fns)};',
         f'{exp}uint64_t ultrs_fn_addr[] = {{{", ".join(f"{a:#x}ull" for a in fns)}}};']
    g += [f'void f_{a:x}(Cpu *c);' for a in fns]
    g += [f'{exp}void *ultrs_fn_ptr[] = {{{", ".join(f"(void *)f_{a:x}" for a in fns)}}};',
          f'{exp}uint64_t ultrs_ext_count = {len(ext)};',
          f'{exp}uint64_t ultrs_ext_addr[] = {{{", ".join(f"0x{u[2:]}ull" for u in ext) or "0"}}};',
          f'{exp}void *ultrs_ext_ptr[{max(len(ext), 1)}];',
          f'{exp}uint64_t ultrs_imp_count = {len(imps)};',
          f'{exp}const char *ultrs_imp_name[] = {{{", ".join(json.dumps(u) for u in imps) or "0"}}};',
          f'{exp}void *ultrs_imp_ptr[{max(len(imps), 1)}];']
    # the game's functions and the runtime's: a jump through the slot the game fills at load (any signature)
    for k, u in enumerate(ext):
        g.append(f'__asm__(".globl {u}\\n{u}:\\n    jmp *ultrs_ext_ptr+{8 * k}(%rip)\\n");')
    for k, u in enumerate(imps):
        g.append(f'__asm__(".globl {u}\\n{u}:\\n    jmp *ultrs_imp_ptr+{8 * k}(%rip)\\n");')
    (work / 'glue.c').write_text('\n'.join(g) + '\n', newline='\n')
    if not run([cc, *flags, '-c', str(work / 'glue.c'), '-o', str(work / 'glue.o')]):
        return False
    objs = [str(o) for o in units] + [str(work / 'glue.o')]
    if arch != 'x86-64-v3':   # correctly rounded fma where the CPU has no FMA3, as the game (build_game.py)
        if not run([cc, *flags, '-c', str(ROOT / 'src/runtime/fma.c'), '-o', str(work / 'fma.o')]):
            return False
        objs.append(str(work / 'fma.o'))
    tmp = ROOT / (LIB + '.new')
    link = [cc, '-shared', '-fuse-ld=lld', '-nostartfiles', '-o', str(tmp), *objs]
    # no C runtime start-up of its own, nothing here needs one: the game's toolchain has none for libraries on Windows,
    # and the players' trimmed Linux kit has only the start files of a program (crt1.o, crtbegin.o), not a library's
    # Linux: -Bsymbolic, the glue's jumps through ultrs_ext_ptr / ultrs_imp_ptr (exported for the game's dlsym) are
    # %rip-relative, which lld takes only against a symbol bound inside the library
    link += ([*linux_flag_args(), '--rtlib=libgcc', '--unwindlib=libgcc', '-Wl,-Bsymbolic', '-lm'] if LINUX else
             ['-Wl,--entry,ultrs_dll_entry', '-static-libgcc'])
    if not run(link):
        return False
    try:
        os.replace(tmp, ROOT / LIB)
    except OSError as x:
        print(f'ERROR: {LIB} is in use (the game is running?): {x}', file=sys.stderr)
        return False
    return True


def main():
    os.chdir(ROOT)
    if len(sys.argv) != 2 or sys.argv[1] not in ('list', 'build'):
        sys.exit(__doc__)
    if sys.argv[1] == 'list':
        cmd_list()
        return 0
    return cmd_build()


if __name__ == '__main__':
    sys.exit(main())
