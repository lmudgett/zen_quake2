# gen_fly_sprite.py -- generate the corpse-flies sprite (Unreal-style).
#
# EF_FLIES corpses draw a swarm of these instead of id's black particle
# dots (cl_fx.c CL_AddFlies). Two 64x64 RGBA TGA frames on a 5x5-unit
# SP2 billboard, painted as a top-down fly: big dull-red eyes, brown
# thorax, striped tapered abdomen, six legs, and two translucent veined
# wings -- frame 0 wings swept back in a V, frame 1 wings beaten into a
# broad motion-blur fan; the client alternates frames for the wingbeat.
# The sprite draws on the blended path so wing alpha survives (the
# alpha-test path would cut it).
#
# Output: baseq2/sprites/s_fly.sp2 + fly_0.tga + fly_1.tga
#
# Author: Len Mudgett

import math
import struct

from q2gen import ROOT, hash01

OUT_DIR = ROOT / "baseq2" / "sprites"
SIZE = 64
MAX_SKINNAME = 64

def in_ellipse(px, py, cx, cy, rx, ry, rot=0.0):
    """Distance in ellipse-normalized units (<1 = inside)."""
    dx, dy = px - cx, py - cy
    c, s = math.cos(rot), math.sin(rot)
    ex, ey = dx * c + dy * s, -dx * s + dy * c
    return math.sqrt((ex / rx) ** 2 + (ey / ry) ** 2)

def seg_dist(px, py, x0, y0, x1, y1):
    """Distance from a pixel to a line segment (for the legs)."""
    vx, vy = x1 - x0, y1 - y0
    wx, wy = px - x0, py - y0
    t = max(0.0, min(1.0, (wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6)))
    dx, dy = wx - t * vx, wy - t * vy
    return math.sqrt(dx * dx + dy * dy)

# six legs, three per side, rooted on the thorax: (root, knee, foot)
LEGS = [
    ((38, 27), (44, 21), (49, 19)),     # front
    ((36, 25), (36, 18), (33, 14)),     # middle
    ((32, 26), (26, 20), (21, 16)),     # hind
]
LEGS = LEGS + [((x0, 64 - y0), (x1, 64 - y1), (x2, 64 - y2))
               for (x0, y0), (x1, y1), (x2, y2) in LEGS]

def paint_fly(px, py, blur_wings):
    """RGBA for one pixel. Top-down fly facing +x: head right, abdomen left."""
    r = g = b = a = 0

    def put(nr, ng, nb, na):
        nonlocal r, g, b, a
        if na > a:
            r, g, b, a = nr, ng, nb, na

    # NOTE: the sprite path multiplies by gl_intensity (2) and the alias
    # 2x overbright, then gamma -- texels must stay VERY dark or the fly
    # renders as a pale blob (first pass shipped grey-white flies). The
    # shape has to come from silhouette and alpha, not brightness.

    # wings first (body and legs draw over them where they overlap).
    # visibility comes from ALPHA, not brightness -- alpha survives the
    # intensity/overbright pipeline, bright texels wash out to grey
    if blur_wings:
        # beaten: a broad fan each side plus a fainter trailing ghost
        for side in (-1, 1):
            d = in_ellipse(px, py, 29, 32 + side * 9, 15.0, 6.5, rot=side * 0.15)
            if d < 1.0:
                if d > 0.85:            # faint rim so the fan has an edge
                    put(18, 20, 24, 110)
                else:
                    put(46, 49, 55, int(105 * (1.0 - d) + 45))
            d = in_ellipse(px, py, 24, 32 + side * 12, 13.0, 5.0, rot=side * 0.55)
            if d < 1.0 and a < 60:
                put(40, 42, 48, int(65 * (1.0 - d) + 20))
    else:
        # at rest: two long wings rooted at the thorax and swept back past
        # the abdomen in a V, with a dark outline and leading-edge vein
        for side in (-1, 1):
            d = in_ellipse(px, py, 22, 32 + side * 10.5, 13.5, 4.4, rot=-side * 0.55)
            if d < 1.0:
                if d > 0.80:            # outline rim
                    put(16, 18, 22, 150)
                else:                   # translucent membrane
                    put(52, 57, 64, int(85 * (1.0 - d) + 60))
            # one long vein down the wing's leading edge
            dv = in_ellipse(px, py, 23, 32 + side * 8.5, 12.0, 1.0, rot=-side * 0.55)
            if dv < 1.0 and a < 140:
                put(22, 24, 28, 135)

    # legs: thin dark jointed lines, over the wings, under the body
    for root, knee, foot in LEGS:
        if (seg_dist(px, py, *root, *knee) < 1.0
                or seg_dist(px, py, *knee, *foot) < 0.8):
            put(8, 6, 5, 230)

    # abdomen (left): fat tapered oval, segment stripes, bottle-green sheen
    d = in_ellipse(px, py, 20, 32, 12.0, 7.2)
    if d < 1.0:
        v = 7 + int(7 * (1.0 - d))
        if ((px + 1) // 3) % 2:
            v -= 4                      # segment stripes
        sheen = int(4 * hash01(px, py, 1))
        # faint green iridescence on the back (upper edge)
        gr = 3 if abs(py - 29) < 3 else 0
        put(v + sheen, v - 1 + sheen // 2 + gr, v - 3, 255)

    # thorax (middle): brownish hump
    d = in_ellipse(px, py, 35, 32, 7.0, 6.4)
    if d < 1.0:
        v = 11 + int(6 * (1.0 - d))
        put(v + 2, v - 2, v - 4, 255)

    # head (right)
    d = in_ellipse(px, py, 45, 32, 4.6, 4.2)
    if d < 1.0:
        put(11, 7, 5, 255)

    # big dull-red compound eyes wrapping the head sides
    for side in (-1, 1):
        d = in_ellipse(px, py, 46.5, 32 + side * 3.4, 2.9, 2.7)
        if d < 1.0:
            v = 1.0 - 0.5 * d
            put(int(60 * v), int(13 * v), int(9 * v), 255)

    return r, g, b, a

def write_tga(name, blur_wings):
    header = struct.pack("<BBB5B4H2B", 0, 0, 2, 0,0,0,0,0, 0, 0, SIZE, SIZE, 32, 8)
    rows = []
    for py in range(SIZE):
        row = b""
        for px in range(SIZE):
            r, g, b, a = paint_fly(px, py, blur_wings)
            row += bytes((b, g, r, a))
        rows.append(row)
    data = header + b"".join(reversed(rows))    # bottom-up TGA
    (OUT_DIR / name).write_bytes(data)
    print(f"wrote {OUT_DIR / name} ({len(data)} bytes)")

def write_sp2():
    frames = b""
    for i in range(2):
        name = f"sprites/fly_{i}.tga".encode().ljust(MAX_SKINNAME, b"\0")
        # 5x5 world units, centered on the entity origin
        frames += struct.pack("<4i", 5, 5, 2, 2) + name
    sp2 = struct.pack("<3i", 0x32534449, 2, 2) + frames    # "IDS2", version, count
    (OUT_DIR / "s_fly.sp2").write_bytes(sp2)
    print(f"wrote {OUT_DIR / 's_fly.sp2'} ({len(sp2)} bytes)")

if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    write_tga("fly_0.tga", False)
    write_tga("fly_1.tga", True)
    write_sp2()
