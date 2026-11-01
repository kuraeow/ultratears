"""The port's icon (assets/icon.ico, the author's own drawing) as a PNG for Linux windows: on Windows the exe's icon
resource gives the window its icon, on Linux SDL_SetWindowIcon needs an image (src/host/window_sdl.c, the launcher's
main.cpp). The .ico holds every size as a PNG already; the one asked for is taken as it is.
"""
import pathlib
import struct

ROOT = pathlib.Path(__file__).resolve().parent.parent


def png(size=256):
    """The PNG of that size in assets/icon.ico (bytes)."""
    d = (ROOT / 'assets/icon.ico').read_bytes()
    for i in range(struct.unpack_from('<H', d, 4)[0]):
        w, _, _, _, _, _, n, off = struct.unpack_from('<BBBBHHII', d, 6 + 16 * i)
        if (w or 256) == size and d[off:off + 4] == b'\x89PNG':
            return d[off:off + n]
    raise SystemExit(f'assets/icon.ico has no {size}x{size} PNG')


def c_array(name, data):
    """A C definition of data as `static const unsigned char name[]`."""
    rows = [','.join(str(b) for b in data[k:k + 32]) for k in range(0, len(data), 32)]
    return f'static const unsigned char {name}[] = {{\n' + ',\n'.join(rows) + '};\n'
