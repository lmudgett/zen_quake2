# gen_decals.py -- generate the weapon impact-mark textures (Q3-style decals).
#
# Paints four 64x64 32-bit TGAs used by the renderer's decal system
# (gl3_decals.c): bullet.tga (dark hole with a chipped rim), scorch.tga
# (soot blast smudge for explosions; the BFG tints it green at draw time),
# energy.tga (blaster burn with a faint ember ring), rail.tga (Q3-style
# blue energy splat: hot pale core, vivid blue ring, charred blue fringe).
# All alpha-feathered to fully transparent borders so GL_REPEAT sampling
# can't bleed.
#
# Output: baseq2/textures/decals/*.tga (CMake copies the repo baseq2/ tree
# into build/baseq2 after each game-DLL build).
#
# Author: Len Mudgett

import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "baseq2" / "textures" / "decals"
SIZE = 64

# ---------------------------------------------------------------- noise

def hash01(x, y, salt=0):
    h = (x * 374761393 + y * 668265263 + salt * 2246822519) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65536.0

def vnoise(x, y, salt=0):
    """Bilinear-smoothed value noise on the integer lattice."""
    xi, yi = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - xi, y - yi
    fx = fx * fx * (3 - 2 * fx)
    fy = fy * fy * (3 - 2 * fy)
    a = hash01(xi, yi, salt);     b = hash01(xi + 1, yi, salt)
    c = hash01(xi, yi + 1, salt); d = hash01(xi + 1, yi + 1, salt)
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy

def fbm(x, y, salt=0):
    return (vnoise(x, y, salt) * 0.55
            + vnoise(x * 2.1, y * 2.1, salt + 1) * 0.30
            + vnoise(x * 4.3, y * 4.3, salt + 2) * 0.15)

def edge_feather(px, py):
    """Force alpha to 0 within 2px of the texture border (GL_REPEAT safe)."""
    m = min(px, py, SIZE - 1 - px, SIZE - 1 - py)
    return 0.0 if m < 2 else min(1.0, (m - 1) / 3.0)

# ---------------------------------------------------------------- painters
# each returns (r, g, b, a) for a pixel; rr = radial distance, 1.0 at edge

def bullet(px, py, rr, ang):
    # noisy crater radius so the hole isn't a perfect circle
    n = fbm(px * 0.22, py * 0.22, 10)
    r = rr * (1.0 + 0.5 * (n - 0.5))
    if r < 0.20:        # punched core
        v = 10 + int(14 * n)
        a = 1.0
    elif r < 0.40:      # crater wall
        v = 26 + int(20 * fbm(px * 0.3, py * 0.3, 11))
        a = 0.95
    else:               # chipped fringe, breaks up with noise (kept dark:
                        # pale rims read as white rings on Q2's dim walls)
        v = 42 + int(24 * fbm(px * 0.35, py * 0.35, 12))
        a = max(0.0, 1.0 - (r - 0.40) / 0.38)
        a *= 0.40 + 0.40 * fbm(px * 0.5, py * 0.5, 13)
    return v, v, int(v * 0.92), a

def scorch(px, py, rr, ang):
    # soot blob: soft falloff broken by turbulence, streaked outward
    n = fbm(px * 0.16 + 3 * math.cos(ang), py * 0.16 + 3 * math.sin(ang), 20)
    a = max(0.0, (1.0 - rr * 1.08)) ** 1.4 * (0.45 + 0.75 * n)
    a = min(0.92, a * 1.6)
    v = 10 + int(26 * fbm(px * 0.4, py * 0.4, 21))
    return v, v, v, a

def energy(px, py, rr, ang):
    n = fbm(px * 0.25, py * 0.25, 30)
    r = rr * (1.0 + 0.35 * (n - 0.5))
    if r < 0.28:        # charred center
        return 14, 10, 7, 0.95
    if r < 0.55:        # ember ring, warm and irregular (dark, not glowing:
                        # decals are unlit, bright texels read as paint)
        t = (r - 0.28) / 0.27
        a = 0.9 - 0.3 * t
        return (int(44 + 52 * (1 - t) * n), int(22 + 24 * (1 - t) * n),
                8, a * (0.6 + 0.4 * n))
    a = max(0.0, 1.0 - (r - 0.55) / 0.35) * (0.4 + 0.6 * n)
    return 18, 13, 9, a * 0.8

def rail(px, py, rr, ang):
    # Quake 3 rail/plasma mark: hot pale-blue core, vivid blue ring,
    # dark charred-blue fringe breaking up with noise
    n = fbm(px * 0.28, py * 0.28, 40)
    r = rr * (1.0 + 0.25 * (n - 0.5))
    if r < 0.16:        # white-hot core
        return 210, 235, 255, 0.95
    if r < 0.45:        # vivid blue ring
        t = (r - 0.16) / 0.29
        return (int(120 - 90 * t + 40 * n), int(170 - 100 * t + 40 * n),
                int(255 - 60 * t), (0.92 - 0.25 * t) * (0.75 + 0.25 * n))
    if r < 0.72:        # charred blue rim
        t = (r - 0.45) / 0.27
        a = (0.65 - 0.45 * t) * (0.5 + 0.5 * n)
        return 16, 26, int(70 - 30 * t), a
    a = max(0.0, 1.0 - (r - 0.72) / 0.2) * 0.25 * n
    return 12, 18, 36, a

# ---------------------------------------------------------------- tga write

def write_tga(name, painter):
    header = struct.pack("<BBB5B4H2B", 0, 0, 2, 0,0,0,0,0, 0, 0, SIZE, SIZE, 32, 8)
    rows = []
    for py in range(SIZE):
        row = b""
        for px in range(SIZE):
            dx, dy = px - (SIZE - 1) / 2.0, py - (SIZE - 1) / 2.0
            rr = math.sqrt(dx * dx + dy * dy) / (SIZE / 2.0)
            ang = math.atan2(dy, dx)
            r, g, b, a = painter(px, py, rr, ang)
            a *= edge_feather(px, py)
            row += bytes((max(0, min(255, b)), max(0, min(255, g)),
                          max(0, min(255, r)), max(0, min(255, int(a * 255)))))
        rows.append(row)
    # TGA type 2 with descriptor bit 5 clear is bottom-up: reverse rows
    data = header + b"".join(reversed(rows))
    (OUT_DIR / name).write_bytes(data)
    print(f"wrote {OUT_DIR / name} ({len(data)} bytes)")

if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_tga("bullet.tga", bullet)
    write_tga("scorch.tga", scorch)
    write_tga("energy.tga", energy)
    write_tga("rail.tga", rail)
