"""Every shader of the game: the NVN shaders in the RomFS (Shader/ packages, SDK and boot packages, effects and UI layouts)
and the few the game modules embed, as (stage, control, code, source) by the runtime's key (SHA-256 of control + code).

From MacNativeGames games/totk/scripts/prepare_shader_seed.py @b75be8c (MIT, (c) 2026 MacNativeGames contributors;
license: third_party/licenses/MacNativeGames.txt): digest, checked, sarc, bnsh_shaders and startup_package unchanged,
sharcb_shaders also skips records that point at themselves; inventory() reads the RomFS through tools/romfs.py (a folder
of plain files or the development romfs.bin) and takes the modules as images (bytes), not as files.
"""
import hashlib
import struct

ZSTD = b'\x28\xb5\x2f\xfd'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError('shader container range outside file')
    return data[offset:offset+size]


def sarc(data):
    if data[:4] != b'SARC':
        raise ValueError('expected SARC')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian+'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise ValueError('invalid SARC header')
    count = struct.unpack_from(endian+'H', data, header+6)[0]
    names = header+12+count*16
    if checked(data, names, 4) != b'SFNT':
        raise ValueError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian+'4I', checked(data, header+12+i*16, 16))
        off = names+(attribute & 0xffffff)*4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo or start+lo < start:
            raise ValueError('invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name.startswith('/') or '\\' in name or any(p in ('', '.', '..') for p in name.split('/')) or name in result:
            raise ValueError('unsafe or duplicate SARC name')
        result[name] = checked(data, start+lo, hi-lo)
    return result


def bnsh_shaders(data):
    """BNSH absolute offsets are relative to each embedded BNSH, not BFSHA."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise ValueError('unsupported BNSH header')
    size = struct.unpack_from('<I', data, 28)[0]
    data = checked(data, 0, size)
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count*64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations+i*64+16)[0]
        checked(data, program, 56)
        for stage, relative in [('vs', 8), ('ps', 40), ('cs', 48)]:
            offset = struct.unpack_from('<Q', data, program+relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset+8, 24))
            control = checked(data, control_off, control_size)
            code = checked(data, code_off, code_size)
            if len(control) < 0x880:
                # The SDK utility BNSH uses an older, shorter control layout.
                # Do not invent bytes to make it look like the game's NVN ABI.
                continue
            control = control[:0x880]   # some are 0x88c; NVN and the runtime's key read 0x880
            c1, length = struct.unpack_from('<II', control, 0x700)
            actual = struct.unpack_from('<I', control, 0x714)[0]
            if actual != {'vs': 0, 'ps': 1, 'cs': 5}[stage] or not 0 <= c1 <= length <= len(code) or not length:
                raise ValueError('invalid NVN shader bounds or stage')
            code = code[:length]
            yield digest(control+code), stage, control, code


def sharcb_shaders(data):
    """agl shader packages (.sharcb, magic BAHS): the records between the header's start and the code pool are
    <u64 code offset in the pool, u32 control size (0x880 or 0x88c), i32 offset of the control from the record>; a record
    may reuse an earlier record's control. Control (its first 0x880 bytes) and code hash to the runtime's key."""
    if len(data) < 32 or data[:4] != b'BAHS':
        raise ValueError('expected an agl shader package')
    start, pool_size, pool = struct.unpack_from('<3I', data, 20)
    if not 32 <= start <= pool or pool + pool_size > len(data):
        raise ValueError('agl shader package layout outside the file')
    seen = set()
    for size in (b'\x80\x08\x00\x00', b'\x8c\x08\x00\x00'):
        at = start
        while (at := data.find(size, at, pool)) >= 0:
            record, at = at - 8, at + 4
            if record < start or record % 4:
                continue
            code_off = struct.unpack_from('<Q', data, record)[0]
            control_at = record + struct.unpack_from('<i', data, record+12)[0]
            # A control's own bytes hold the size words too; read as a record, its control offset is 0 (the "control" would
            # start with the record itself). In 1.4.3 they gave 102 "shaders" that no draw uses: 82 cut short (the
            # translator failed on them, issue #37), 20 with a control made of other bytes.
            if control_at == record or not start <= control_at <= pool - 0x880 or code_off >= pool_size:
                continue
            control = data[control_at:control_at+0x880]
            c1, length = struct.unpack_from('<II', control, 0x700)
            stage = {0: 'vs', 1: 'ps', 5: 'cs'}.get(struct.unpack_from('<I', control, 0x714)[0])
            if not stage or not 0 < length <= pool_size - code_off or c1 > length:
                continue
            code = data[pool+code_off:pool+code_off+length]
            key = digest(control+code)
            if key not in seen:
                seen.add(key)
                yield key, stage, control, code


def startup_package(name):
    """The packages the startup shaders come from; effects and UI layouts carry more."""
    return (name == 'Lib/NintendoSDK/TextureShader.bnsh' or name == 'Pack/Bootup.Nin_NX_NVN.pack.zs' or
            name.startswith('Shader/') and name.endswith('.zs'))


def inventory(rom, modules=None):
    """rom: a tools/romfs.RomFS; modules: {name: image bytes} of the game modules (main, subsdk0, sdk).
    -> shaders {key: (stage, control, code, source path)}, startup (keys translated for the default bind state)."""
    import compression.zstd as zstd
    shaders, startup = {}, set()
    dictionaries = {}
    for raw in sarc(zstd.decompress(rom.read('Pack/ZsDic.pack.zs'))).values():
        d = zstd.ZstdDict(raw)
        dictionaries[d.dict_id] = d

    def decode(raw):
        info = zstd.get_frame_info(raw)
        if info.decompressed_size is None or info.decompressed_size > 512 << 20:
            raise ValueError('unbounded shader package')
        return zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)

    def visit(name, data, keys, depth=0):
        if data[:4] == b'BAHS':   # agl: the engine's own shaders (title screen, post effects, lighting)
            for key, stage, control, code in sharcb_shaders(data):
                shaders.setdefault(key, (stage, control, code, name))
                keys.add(key); startup.add(key)
            return
        if data[:4] == b'SARC':
            if depth > 4:
                return
            for member, raw in sarc(data).items():
                visit(name+'/'+member, decode(raw) if raw[:4] == ZSTD else raw, keys, depth+1)
            return
        # BFSHA and BNSH files, and effect / layout resources, hold complete, independently addressed BNSH files.
        pos = 0
        while True:
            pos = data.find(b'BNSH\0\0\0\0', pos)
            if pos < 0:
                break
            size = struct.unpack_from('<I', checked(data, pos+28, 4))[0]
            for key, stage, control, code in bnsh_shaders(checked(data, pos, size)):
                shaders.setdefault(key, (stage, control, code, name))
                keys.add(key)
            pos += max(size, 8)

    for name in sorted(rom.files):
        raw = rom.read(name)
        # Only containers can hold shaders: compressed files, packages and shader archives.
        if raw[:4] not in (ZSTD, b'SARC', b'FSHA', b'BNSH', b'BAHS') and b'BNSH\0\0\0\0' not in raw:
            continue
        data = decode(raw) if raw[:4] == ZSTD else raw
        keys = set()
        visit(name, data, keys)
        if startup_package(name):
            startup |= keys
    for name, image in sorted((modules or {}).items()):
        keys = set()   # the game module embeds a few BNSH files
        visit('module:'+name, image, keys)
        startup |= keys
    if not startup:
        raise ValueError('no startup shaders discovered')
    return shaders, startup
