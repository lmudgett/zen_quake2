# q2gen.py -- shared helpers for the asset generators in this directory.
#
# One home for the pieces every tool was carrying its own copy of: the
# anorms table (MD2 vertex light normals), the deterministic pixel-noise
# hash, the PAK container format (reader AND writer, so the two can't
# drift), and the q2 skin PCX decoder.
#
# Author: Len Mudgett

import math
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------- anorms

_ANORMS = None


def anorms():
    """id's 162 alias-model light normals, parsed from the pristine clone.
    Loaded lazily: tools that never touch MD2s don't need the file."""
    global _ANORMS
    if _ANORMS is None:
        norms = []
        text = (ROOT / "quake2" / "ref_gl" / "anorms.h").read_text()
        for m in re.finditer(
                r"\{\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\}", text):
            norms.append(tuple(float(g) for g in m.groups()))
        assert len(norms) == 162, f"expected 162 anorms, got {len(norms)}"
        _ANORMS = norms
    return _ANORMS


def nearest_anorm(n):
    table = anorms()
    return max(range(162),
               key=lambda i: sum(n[j] * table[i][j] for j in range(3)))

# ---------------------------------------------------------------- noise

def hash01(x, y, salt=0):
    """Deterministic per-pixel noise in [0,1) -- no RNG state to drift."""
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0

# ---------------------------------------------------------------- pak

PAK_MAX_NAME = 56		# name slot in a 64-byte directory entry


def read_pak(path):
    """PAK container -> {lowercased name: bytes}."""
    data = Path(path).read_bytes()
    ident, dirofs, dirlen = struct.unpack_from("<4sii", data, 0)
    assert ident == b"PACK", "not a pak file"
    entries = {}
    for off in range(dirofs, dirofs + dirlen, 64):
        name = data[off:off + PAK_MAX_NAME].split(b"\0")[0] \
            .decode("latin-1").lower()
        fpos, flen = struct.unpack_from("<ii", data, off + PAK_MAX_NAME)
        entries[name] = data[fpos:fpos + flen]
    return entries


def write_pak(path, files):
    """files = iterable of (name, bytes) -> PAK container on disk."""
    entries = []
    data = bytearray(12)            # header placeholder
    for name, raw in files:
        if len(name) >= PAK_MAX_NAME:
            raise ValueError(
                f"path too long for pak ({len(name)} >= {PAK_MAX_NAME}): {name}")
        entries.append((name, len(data), len(raw)))
        data += raw
    dirofs = len(data)
    for name, fpos, flen in entries:
        data += struct.pack("<56sii", name.encode("latin-1"), fpos, flen)
    struct.pack_into("<4sii", data, 0, b"PACK", dirofs, len(entries) * 64)
    Path(path).write_bytes(data)
    return entries

# ---------------------------------------------------------------- pcx

def decode_pcx(data):
    """8-bit single-plane RLE pcx (the q2 skin format) ->
    (width, height, index bytes, 768-byte palette)."""
    xmin, ymin, xmax, ymax = struct.unpack_from("<4H", data, 4)
    w, h = xmax - xmin + 1, ymax - ymin + 1
    pix = bytearray()
    p = 128
    end = len(data) - 769
    while len(pix) < w * h and p < end:
        b = data[p]; p += 1
        if (b & 0xC0) == 0xC0:
            pix += data[p:p + 1] * (b & 0x3F); p += 1
        else:
            pix.append(b)
    return w, h, pix, data[-768:]
