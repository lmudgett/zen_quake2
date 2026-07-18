# gen_flamer_model.py -- generate the flamethrower view model (v_flamer) as a
# Quake 2 MD2, plus its skin and HUD icon.
#
# The gun is lofted rings along +X at the lower right of the view: a boxy
# receiver, a long barrel with a brass pilot collar and flaring soot-black
# nozzle, and a red fuel tank slung underneath. 24 frames: raise (0-4),
# fire with recoil shudder (5-8), idle with a subtle bob (9-19), lower
# (20-23) -- matching Weapon_Flamethrower's Weapon_Generic numbers.
#
# The renderer draws MD2s from glcmds, so every quad/cap is emitted as a
# triangle fan with inline UVs; st/tris lumps are written for validity.
# Icon: baseq2/pics/w_flamer.png (the retexture override path loads loose
# .png for a missing .pcx).
#
# Author: Len Mudgett

import math
import struct

from q2gen import ROOT, hash01, nearest_anorm

OUT_DIR = ROOT / "baseq2" / "models" / "weapons" / "v_flamer"
PICS_DIR = ROOT / "baseq2" / "pics"

SKIN_NAME = b"models/weapons/v_flamer/skin.tga"
SKIN_W, SKIN_H = 64, 64
MAX_SKINNAME = 64
RING_N = 8

# ---------------------------------------------------------------- helpers

def vdot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def vcross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def vsub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def vnorm(a):
    l = math.sqrt(vdot(a, a)) or 1.0
    return (a[0]/l, a[1]/l, a[2]/l)
def sepow(u, e): return math.copysign(abs(u) ** e, u)

# ---------------------------------------------------------------- geometry
#
# View-model space: +X forward, +Y left, +Z up, origin at the eye.
# The whole gun rides at (y -7) with the barrel axis at z -6.5.

GUN_Y, GUN_Z = -7.0, -6.5

# skin regions (s0, t0, s1, t1)
R_METAL  = (0.02, 0.02, 0.48, 0.48)
R_TANK   = (0.52, 0.02, 0.98, 0.48)
R_NOZZLE = (0.02, 0.52, 0.48, 0.98)
R_BRASS  = (0.52, 0.52, 0.98, 0.98)

verts = []      # (pos, outward-normal)
faces = []      # ([idx fan CW-outside], [uv])

def xring(x, cy, cz, r, e=0.85):
    """Ring in the Y/Z plane at station x."""
    pts, dirs = [], []
    for k in range(RING_N):
        a = -math.pi + 2.0 * math.pi * k / RING_N
        d = (0.0, sepow(math.cos(a), e), sepow(math.sin(a), e))
        pts.append((x, cy + r * d[1], cz + r * d[2]))
        dirs.append(vnorm(d))
    return pts, dirs

def add_verts(pts, dirs):
    base = len(verts)
    for p, d in zip(pts, dirs):
        verts.append((p, d))
    return base

def wind_cw(idx, uv, n):
    """Order a fan CW seen from outside. Uses the Newell polygon normal:
    the first three verts of a boxy (low-e superellipse) ring can be
    nearly collinear, making a single cross product flip caps inside out
    (they render as holes)."""
    nx = ny = nz = 0.0
    for i in range(len(idx)):
        a = verts[idx[i]][0]
        b = verts[idx[(i + 1) % len(idx)]][0]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    if vdot((nx, ny, nz), n) > 0:
        return idx[::-1], uv[::-1]
    return idx, uv

def add_loft(rings, region):
    bases = [add_verts(p, d) for p, d in rings]
    tt = [i / (len(rings) - 1.0) for i in range(len(rings))]
    for i in range(len(rings) - 1):
        for k in range(RING_N):
            k2 = (k + 1) % RING_N
            idx = [bases[i]+k, bases[i]+k2, bases[i+1]+k2, bases[i+1]+k]
            mid = vnorm((0.0,
                         rings[i][1][k][1] + rings[i][1][k2][1],
                         rings[i][1][k][2] + rings[i][1][k2][2]))
            s0, t0, s1, t1 = region
            sk  = s0 + (s1 - s0) * (k / RING_N)
            sk2 = s0 + (s1 - s0) * ((k + 1) / RING_N)
            ta  = t0 + (t1 - t0) * tt[i]
            tb  = t0 + (t1 - t0) * tt[i+1]
            uv = [(sk, ta), (sk2, ta), (sk2, tb), (sk, tb)]
            faces.append(wind_cw(idx, uv, mid))
    return bases

def add_cap(base, outward, region):
    s0, t0, s1, t1 = region
    idx, uv = [], []
    for k in range(RING_N):
        a = -math.pi + 2.0 * math.pi * k / RING_N
        idx.append(base + k)
        uv.append(((s0+s1)/2 + (s1-s0)*0.45*math.cos(a),
                   (t0+t1)/2 + (t1-t0)*0.45*math.sin(a)))
    faces.append(wind_cw(idx, uv, outward))

# receiver: boxy block the barrel grows out of
recv = add_loft([xring( 2.0, GUN_Y, GUN_Z, 2.9, e=0.5),
                 xring( 7.0, GUN_Y, GUN_Z, 2.8, e=0.5),
                 xring(13.0, GUN_Y, GUN_Z, 2.3, e=0.6)], R_METAL)
add_cap(recv[0], (-1, 0, 0), R_METAL)
add_cap(recv[-1], (1, 0, 0), R_METAL)

# barrel out to the pilot collar
add_loft([xring(13.0, GUN_Y, GUN_Z, 1.35),
          xring(19.0, GUN_Y, GUN_Z, 1.25),
          xring(23.0, GUN_Y, GUN_Z, 1.2)], R_METAL)

# brass pilot collar -- radii meet the barrel and nozzle exactly, so the
# surface is continuous with no visible ring gaps
add_loft([xring(23.0, GUN_Y, GUN_Z, 1.2),
          xring(23.7, GUN_Y, GUN_Z, 1.6),
          xring(24.6, GUN_Y, GUN_Z, 1.55),
          xring(25.0, GUN_Y, GUN_Z, 1.3)], R_BRASS)

# soot-black nozzle bell, open mouth capped dark
noz = add_loft([xring(25.0, GUN_Y, GUN_Z, 1.3),
                xring(27.5, GUN_Y, GUN_Z, 1.75),
                xring(29.5, GUN_Y, GUN_Z, 2.35)], R_NOZZLE)
add_cap(noz[-1], (1, 0, 0), R_NOZZLE)

# red fuel tank slung under the receiver
tank = add_loft([xring( 4.0, GUN_Y, GUN_Z - 4.2, 2.0, e=0.9),
                 xring( 9.5, GUN_Y, GUN_Z - 4.4, 2.15, e=0.9),
                 xring(15.0, GUN_Y, GUN_Z - 4.2, 1.9, e=0.9)], R_TANK)
add_cap(tank[0], (-1, 0, 0), R_TANK)
add_cap(tank[-1], (1, 0, 0), R_TANK)

NUM_XYZ = len(verts)

# frame offsets: (dx, dz) per frame
def frame_offsets():
    offs = []
    for z in (-13.0, -9.0, -5.5, -2.5, 0.0):        # 0-4 raise
        offs.append((0.0, z))
    for dx, dz in ((-0.9, 0.15), (-0.55, 0.05), (-0.25, 0.0), (0.0, 0.0)):
        offs.append((dx, dz))                        # 5-8 fire recoil
    for i in range(11):                              # 9-19 idle bob
        offs.append((0.0, 0.25 * math.sin(2.0 * math.pi * i / 11.0)))
    for z in (-3.0, -6.5, -10.0, -14.0):             # 20-23 lower
        offs.append((0.0, z))
    return offs

# ---------------------------------------------------------------- md2 write

def build_md2():
    offsets = frame_offsets()
    num_frames = len(offsets)
    framesize = 40 + 4 * NUM_XYZ

    glcmds = b""
    num_glcmds = 0
    st = b""
    tris = b""
    sbase = 0
    num_tris = 0
    for idx, uv in faces:
        glcmds += struct.pack("<i", -len(idx))
        num_glcmds += 1
        for k in range(len(idx)):
            glcmds += struct.pack("<ffi", uv[k][0], uv[k][1], idx[k])
            num_glcmds += 3
        for k in range(len(idx)):
            st += struct.pack("<hh", int(uv[k][0]*SKIN_W), int(uv[k][1]*SKIN_H))
        for k in range(1, len(idx) - 1):
            tris += struct.pack("<3h3h", idx[0], idx[k], idx[k+1],
                                sbase, sbase+k, sbase+k+1)
            num_tris += 1
        sbase += len(idx)
    glcmds += struct.pack("<i", 0)
    num_glcmds += 1

    frames = b""
    for f, (dx, dz) in enumerate(offsets):
        pos = [(p[0]+dx, p[1], p[2]+dz) for p, _d in verts]
        nrm = [nearest_anorm(d) for _p, d in verts]
        mins = [min(p[i] for p in pos) for i in range(3)]
        maxs = [max(p[i] for p in pos) for i in range(3)]
        scale = [max(maxs[i]-mins[i], 1e-5) / 255.0 for i in range(3)]
        frames += struct.pack("<3f3f16s", *scale, *mins, f"flam{f}".encode())
        for p, n in zip(pos, nrm):
            v = [min(255, max(0, round((p[i]-mins[i]) / scale[i]))) for i in range(3)]
            frames += struct.pack("<4B", v[0], v[1], v[2], n)

    skins = SKIN_NAME.ljust(MAX_SKINNAME, b"\0")
    ofs_skins = 68
    ofs_st = ofs_skins + len(skins)
    ofs_tris = ofs_st + len(st)
    ofs_frames = ofs_tris + len(tris)
    ofs_glcmds = ofs_frames + len(frames)
    ofs_end = ofs_glcmds + len(glcmds)

    header = struct.pack("<17i",
        0x32504449, 8, SKIN_W, SKIN_H, framesize,
        1, NUM_XYZ, sbase, num_tris, num_glcmds, num_frames,
        ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_glcmds, ofs_end)
    return header + skins + st + tris + frames + glcmds

# ---------------------------------------------------------------- skin

def build_skin_tga():
    header = struct.pack("<BBB5B4H2B", 0, 0, 2, 0,0,0,0,0, 0, 0, SKIN_W, SKIN_H, 24, 0)
    img = [[(60, 60, 62)] * SKIN_W for _ in range(SKIN_H)]

    def fill(region, fn):
        x0 = int(region[0]*SKIN_W); y0 = int(region[1]*SKIN_H)
        x1 = int(region[2]*SKIN_W); y1 = int(region[3]*SKIN_H)
        for y in range(y0, y1):
            for x in range(x0, x1):
                u = (x - x0) / max(1, x1 - x0 - 1)
                v = (y - y0) / max(1, y1 - y0 - 1)
                img[y][x] = fn(x, y, u, v)

    def metal(x, y, u, v):
        # dark-texel lesson: the alias pipeline runs intensity x2 +
        # overbright, so gunmetal must be painted well below mid-grey
        d = 0.85 + 0.3 * hash01(x, y, 1)
        band = 0.85 if (y % 7) == 3 else 1.0        # machining lines
        g = int(52 * d * band)
        return (g, g, g + 3)

    def tank(x, y, u, v):
        d = 0.8 + 0.35 * hash01(x, y, 2)
        d *= 1.0 - 0.25 * v                          # grimier toward the base
        if hash01(x//2, y//2, 3) < 0.06:
            d *= 0.55                                # scuffs
        return (int(120 * d), int(28 * d), int(22 * d))

    def nozzle(x, y, u, v):
        d = 0.7 + 0.5 * hash01(x, y, 4)
        heat = max(0.0, 1.0 - abs(u - 0.5) * 3.0)    # heat-blued streak
        return (int(30 * d + 14 * heat), int(26 * d + 6 * heat),
                int(28 * d + 20 * heat))

    def brass(x, y, u, v):
        d = 0.8 + 0.3 * hash01(x, y, 5)
        return (int(104 * d), int(78 * d), int(36 * d))

    fill(R_METAL, metal)
    fill(R_TANK, tank)
    fill(R_NOZZLE, nozzle)
    fill(R_BRASS, brass)

    data = header
    for y in range(SKIN_H - 1, -1, -1):              # bottom-up
        for x in range(SKIN_W):
            r, g, b = img[y][x]
            data += bytes((b, g, r))
    return data

# ---------------------------------------------------------------- icon

def build_icon():
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (24, 24), (28, 28, 30, 255))
    dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, 23, 23], outline=(70, 70, 74, 255))
    # layered flame: red base, orange body, yellow core
    dr.polygon([(12, 2), (18, 10), (16, 12), (19, 16), (12, 22),
                (5, 16), (8, 12), (6, 10)], fill=(178, 44, 16, 255))
    dr.polygon([(12, 5), (16, 11), (14, 13), (16, 16), (12, 20),
                (8, 16), (10, 13), (8, 11)], fill=(236, 118, 20, 255))
    dr.polygon([(12, 9), (14, 13), (12, 18), (10, 13)], fill=(255, 216, 96, 255))
    return im

def build_fuel_icon():
    """a_fuel: a red jerrycan with a small flame over the spout."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (24, 24), (28, 28, 30, 255))
    dr = ImageDraw.Draw(im)
    dr.rectangle([0, 0, 23, 23], outline=(70, 70, 74, 255))
    # canister body + cap
    dr.rectangle([5, 9, 18, 21], fill=(150, 36, 26, 255),
                 outline=(60, 16, 12, 255))
    dr.rectangle([13, 6, 17, 9], fill=(110, 28, 20, 255))
    dr.line([7, 12, 16, 19], fill=(110, 26, 18, 255), width=2)   # X brace
    dr.line([16, 12, 7, 19], fill=(110, 26, 18, 255), width=2)
    # flame licking off the spout
    dr.polygon([(15, 1), (18, 5), (15, 7), (12, 5)], fill=(236, 118, 20, 255))
    dr.polygon([(15, 3), (16, 5), (15, 6), (14, 5)], fill=(255, 216, 96, 255))
    return im

if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    PICS_DIR.mkdir(parents=True, exist_ok=True)
    md2 = build_md2()
    (OUT_DIR / "tris.md2").write_bytes(md2)
    print(f"wrote {OUT_DIR / 'tris.md2'} ({len(md2)} bytes, {NUM_XYZ} verts, "
          f"{len(faces)} faces, 24 frames)")
    skin = build_skin_tga()
    (OUT_DIR / "skin.tga").write_bytes(skin)
    print(f"wrote {OUT_DIR / 'skin.tga'} ({len(skin)} bytes)")
    build_icon().save(PICS_DIR / "w_flamer.png")
    print(f"wrote {PICS_DIR / 'w_flamer.png'}")
    build_fuel_icon().save(PICS_DIR / "a_fuel.png")
    print(f"wrote {PICS_DIR / 'a_fuel.png'}")
