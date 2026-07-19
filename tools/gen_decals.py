# gen_decals.py -- generate the weapon impact-mark textures (Q3-style decals).
#
# Paints 64x64 32-bit TGAs used by the renderer's decal system
# (gl3_decals.c): bullet.tga (dark hole with a chipped rim), scorch.tga
# (soot blast smudge for explosions; the BFG tints it green at draw time),
# energy.tga (blaster burn with a faint ember ring), rail.tga (Q3-style
# blue energy splat: hot pale core, vivid blue ring, charred blue fringe),
# blood.tga (dark crimson hit splatter with streaks and droplets), and
# pool.tga/pool1.tga/pool2.tga (deep-red death pools; the renderer grows
# one, picked at random, over a few seconds -- three variants with lobed
# rims and runner fingers so repeated deaths don't all leave the same
# round blob), and footprint.tga (bloody boot print left by walking
# through a pool; toe toward the image bottom = the walker's forward).
# All alpha-feathered to fully transparent borders so GL_REPEAT sampling
# can't bleed.
#
# Output: baseq2/textures/decals/*.tga (CMake copies the repo baseq2/ tree
# into build/baseq2 after each game-DLL build).
#
# Author: Len Mudgett

import math
import struct

from q2gen import ROOT, hash01

OUT_DIR = ROOT / "baseq2" / "textures" / "decals"
SIZE = 64

# ---------------------------------------------------------------- noise

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
        v = 8 + int(10 * n)
        a = 1.0
    elif r < 0.40:      # crater wall
        v = 16 + int(12 * fbm(px * 0.3, py * 0.3, 11))
        a = 0.95
    else:               # chipped fringe, breaks up with noise (kept VERY
                        # dark: anything paler reads as grey chips on dark
                        # walls -- the whole mark must darken, never lighten)
        v = 22 + int(12 * fbm(px * 0.35, py * 0.35, 12))
        a = max(0.0, 1.0 - (r - 0.40) / 0.38)
        a *= 0.35 + 0.35 * fbm(px * 0.5, py * 0.5, 13)
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
    # dark charred-blue fringe breaking up with noise. Rings fill the
    # quad out to ~0.9 (like bullet's ~0.8) so the stamped radius IS the
    # visible size -- the renderer tints this from glow-blue to black as
    # the mark ages (GL3_DrawDecals DECAL_RAIL branch)
    n = fbm(px * 0.28, py * 0.28, 40)
    r = rr * (1.0 + 0.25 * (n - 0.5))
    if r < 0.20:        # white-hot core
        return 210, 235, 255, 0.95
    if r < 0.60:        # vivid blue ring
        t = (r - 0.20) / 0.40
        return (int(120 - 90 * t + 40 * n), int(170 - 100 * t + 40 * n),
                int(255 - 60 * t), (0.92 - 0.25 * t) * (0.75 + 0.25 * n))
    if r < 0.88:        # charred blue rim
        t = (r - 0.60) / 0.28
        a = (0.65 - 0.45 * t) * (0.5 + 0.5 * n)
        return 16, 26, int(70 - 30 * t), a
    a = max(0.0, 1.0 - (r - 0.88) / 0.12) * 0.25 * n
    return 12, 18, 36, a

def blood(px, py, rr, ang):
    # hit splatter: turbulent blob torn into streaks, satellite droplets
    n = fbm(px * 0.20 + 2 * math.cos(ang * 3), py * 0.20 + 2 * math.sin(ang * 3), 50)
    r = rr * (1.0 + 0.8 * (n - 0.5))
    a = max(0.0, 1.0 - r * 1.25) * (0.35 + 0.9 * n)
    # droplets flung past the main blob
    d = fbm(px * 0.55, py * 0.55, 51)
    if rr > 0.45 and d > 0.72:
        a = max(a, (d - 0.72) * 3.0 * max(0.0, 1.0 - rr))
    a = min(0.92, a * 1.5)
    v = 0.55 + 0.45 * fbm(px * 0.4, py * 0.4, 52)     # wet sheen variation
    return int(105 * v), int(14 * v), int(12 * v), a

def make_pool(variant):
    """Death pool painter. Each variant gets its own asymmetric silhouette:
    an elliptical squash, low-order angular harmonics for lobes, and a few
    narrow runner fingers where the blood streamed out along the floor."""
    salt = 60 + variant * 17
    phase = [hash01(variant, k, salt) * 2 * math.pi for k in range(3)]
    amp = [0.05 + 0.06 * hash01(variant, k, salt + 1) for k in range(3)]
    squash = 0.68 + 0.30 * hash01(variant, 9, salt + 2)   # x/y aspect
    runners = [(hash01(variant, k, salt + 3) * 2 * math.pi,          # angle
                0.16 + 0.22 * hash01(variant, k, salt + 4),          # reach
                0.16 + 0.18 * hash01(variant, k, salt + 5))          # width
               for k in range(2 + variant)]

    def pool(px, py, rr, ang):
        dx = (px - (SIZE - 1) / 2.0) / (SIZE / 2.0)
        dy = (py - (SIZE - 1) / 2.0) / (SIZE / 2.0)
        er = math.sqrt((dx / squash) ** 2 + (dy * squash) ** 2)
        ea = math.atan2(dy * squash, dx / squash)

        # lobed rim radius for this direction
        edge = 0.58
        for k in range(3):
            edge += amp[k] * math.sin((k + 2) * ea + phase[k])
        for ra, reach, width in runners:                   # runner fingers
            d = math.atan2(math.sin(ea - ra), math.cos(ea - ra))
            edge += reach * math.exp(-(d / width) ** 2)
        edge += 0.10 * (fbm(px * 0.16, py * 0.16, salt + 6) - 0.5)
        edge = min(edge, 0.90)

        a = 0.0
        if er < edge:
            fade = min(edge - 0.10, 0.65)                  # feathered rim
            a = 0.95 if er < fade else 0.95 * (1.0 - (er - fade) / (edge - fade))
        elif er < 0.92:
            # satellite droplets flung past the rim
            d = fbm(px * 0.55, py * 0.55, salt + 7)
            if d > 0.74:
                a = min(0.9, (d - 0.74) * 4.0) * max(0.0, 1.0 - er)
        if a <= 0.0:
            return 40, 4, 4, 0.0
        core = 1.0 - 0.35 * max(0.0, 1.0 - er * 1.6)      # darker where deepest
        v = core * (0.85 + 0.3 * fbm(px * 0.3, py * 0.3, salt + 8))
        return int(68 * v), int(7 * v), int(6 * v), a

    return pool

def footprint(px, py, rr, ang):
    # bloody boot print: heel block at the image top, tread-barred sole
    # below, toe toward the image BOTTOM (the renderer maps the walker's
    # forward onto +V, which samples the lower rows). Noise breakup so it
    # reads as blood transfer off a tread, not solid paint.
    cx = (SIZE - 1) / 2.0
    a = 0.0
    if 28 <= py <= 57:          # sole: ball widest, rounded toe taper
        t = (py - 28) / 29.0
        w = 10.5 * (0.78 + 0.55 * math.sin(t * math.pi) ** 0.8)
        if t > 0.75:
            w *= 1.0 - ((t - 0.75) / 0.25) ** 2 * 0.55
        d = abs(px - cx) / max(w, 1e-3)
        if d < 1.0:
            a = 0.92 * (1.0 - d ** 4)
    elif 8 <= py <= 21:         # heel block
        t = (py - 8) / 13.0
        w = 8.5 * (0.7 + 0.5 * math.sin(t * math.pi) ** 0.7)
        d = abs(px - cx) / max(w, 1e-3)
        if d < 1.0:
            a = 0.92 * (1.0 - d ** 4)
    if a > 0.0:
        if (py // 4) % 2 == 0:  # tread bars across the print
            a *= 0.38
        a *= 0.55 + 0.75 * fbm(px * 0.35, py * 0.35, 70)
        a = min(0.95, a)
    # brighter than the dried pools: fresh blood tracked off a boot, and a
    # print is a few thin bars -- at pool darkness (58) they vanished into
    # base floors entirely (user-reported); the tread gaps + noise keep the
    # extra red from reading as the neon-paint failure mode
    v = 0.6 + 0.4 * fbm(px * 0.4, py * 0.4, 71)
    return int(82 * v), int(10 * v), int(8 * v), a

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
    write_tga("blood.tga", blood)
    write_tga("pool.tga", make_pool(0))
    write_tga("pool1.tga", make_pool(1))
    write_tga("pool2.tga", make_pool(2))
    write_tga("footprint.tga", footprint)
