# gen_kick_model.py -- generate the boot-kick view model (v_kick) as a Quake 2 MD2.
#
# Builds a marine's right leg as lofted rounded-octagon tubes -- tapered
# thigh with a slight quad bulge, shin with a calf swell, leather boot
# shaft, and a proper boot foot (heel, arch, rounded toe cap, dark sole)
# -- posed over 6 kick frames (windup -> full extension -> retract).
# The skin is a painted 64x64 TGA: olive-drab fatigues with camo mottling,
# worn brown boot leather with a laced front strip, and rubber sole tread,
# matching the marine player model's palette.
#
# The renderer draws MD2s from glcmds, so each loft quad / end cap is
# emitted as a triangle fan with inline normalized UVs; the st/tris lumps
# are still written for format validity. Vertex light normals are the
# nearest entry of id's anorms table (parsed from the reference clone).
#
# Output: baseq2/models/weapons/v_kick/tris.md2 + skin.tga (CMake copies
# the repo baseq2/ tree into build/baseq2 after each game-DLL build).
#
# Author: Len Mudgett

import math
import struct
import sys

from q2gen import ROOT, hash01, nearest_anorm

DIAG = "--diag" in sys.argv     # axis-probe build: colored marker boxes

OUT_DIR = ROOT / "baseq2" / "models" / "weapons" / "v_kick"

SKIN_NAME = b"models/weapons/v_kick/skin.tga"
SKIN_W, SKIN_H = 64, 64
MAX_SKINNAME = 64

RING_N = 8      # points per loft cross-section

# ---------------------------------------------------------------- vec helpers

def vadd(a, b):   return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def vsub(a, b):   return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def vdot(a, b):   return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def vcross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])

def vnorm(a):
    l = math.sqrt(vdot(a, a)) or 1.0
    return (a[0]/l, a[1]/l, a[2]/l)

def rot_pitch(v, deg):
    """Rotate about +Y so that straight-down (0,0,-1) swings toward forward +X."""
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    return (v[0]*c - v[2]*s, v[1], v[0]*s + v[2]*c)

def sepow(u, e):
    """Superellipse shaping: signed |u|^e -- rounds a circle toward a square."""
    return math.copysign(abs(u) ** e, u)

# ---------------------------------------------------------------- geometry
#
# View-model space is the standard Quake entity frame: +X forward, +Y left,
# +Z up, origin at the eye (verified with an axis-probe render: a box at
# +X20 draws dead-center at 20 units; boxes off the X axis never appear).
# The leg hangs from a hip pivot below/right of the eye; everything below
# the knee hangs from the knee. Frame pose = (hip pitch toward forward,
# knee bend backward). A leg lives at the BOTTOM of the screen: the windup
# frames are naturally below the view; the boot enters the frame at
# extension, Duke-style.

HIP = (0.0, -6.0, -19.5)
KNEE_LOCAL = (0.0, 0.0, -18.5)      # knee position in (rotated) thigh space

# (hip pitch, knee bend) per frame; frame 2 is the impact frame (KICK_IMPACT).
# At extension the shin-space boot's toe swings UP, so the sole faces the
# target -- the classic mighty-boot silhouette.
POSES = [
    (35.0,  95.0),      # knee lifting (below view)
    (70.0, 110.0),      # coiled, boot cocked back
    (94.0,   6.0),      # SLAM -- boot up-forward, sole at the target
    (90.0,  20.0),      # recoil begins
    (50.0,  90.0),      # pulling back down
    (20.0,  70.0),      # gone below view
]

# skin regions as (s0, t0, s1, t1) -- see build_skin_tga for the layout
R_PANTS = (0.02, 0.02, 0.48, 0.48)
R_SHAFT = (0.52, 0.02, 0.98, 0.48)
R_FOOT  = (0.52, 0.52, 0.98, 0.98)
R_SOLE  = (0.02, 0.52, 0.48, 0.72)
R_CAP   = (0.06, 0.78, 0.20, 0.95)     # plain leather patch for end caps

def leg_ring(z, r, e=0.8):
    """Cross-section of a leg tube at height z: rounded octagon in X/Y.
    Starts at the back (-X) so the front seam sits mid-region in s."""
    pts, dirs = [], []
    for k in range(RING_N):
        a = -math.pi + 2.0 * math.pi * k / RING_N
        d = (sepow(math.cos(a), e), sepow(math.sin(a), e), 0.0)
        pts.append((r * d[0], r * d[1], z))
        dirs.append(vnorm(d))
    return pts, dirs

def foot_ring(x, zbot, ztop, w, e=0.55):
    """Cross-section of the boot foot at station x: rounded box in Y/Z."""
    zmid, hh = (zbot + ztop) / 2.0, (ztop - zbot) / 2.0
    pts, dirs = [], []
    for k in range(RING_N):
        a = -math.pi / 2.0 + 2.0 * math.pi * k / RING_N   # start at the sole
        d = (0.0, sepow(math.cos(a), e), sepow(math.sin(a), e))
        pts.append((x, w * d[1], zmid + hh * d[2]))
        dirs.append(vnorm(d))
    return pts, dirs

# model accumulators: verts are (local pos, local normal, segment);
# faces are (xyz index fan CW-from-outside, matching uv list)
verts = []      # (pos, nrm, seg)
faces = []

def add_verts(pts, dirs, seg):
    base = len(verts)
    for p, d in zip(pts, dirs):
        verts.append((p, d, seg))
    return base

def wind_cw(idx, uv, n):
    """Order a fan clockwise as seen from outside (id MD2 winding: the
    scene culls GL_FRONT/CCW, so drawn faces are CW from outside)."""
    c0, c1, c2 = verts[idx[0]][0], verts[idx[1]][0], verts[idx[2]][0]
    if vdot(vcross(vsub(c1, c0), vsub(c2, c0)), n) > 0:    # CCW -> flip
        return idx[::-1], uv[::-1]
    return idx, uv

def add_loft(rings, seg, region, region_bottom=None):
    """Skin consecutive rings with quads. s wraps the circumference,
    t runs down the loft. Quads whose outward direction points mostly
    down get region_bottom (the boot sole) when given."""
    bases = [add_verts(p, d, seg) for p, d in rings]
    tt = [i / (len(rings) - 1.0) for i in range(len(rings))]
    for i in range(len(rings) - 1):
        for k in range(RING_N):
            k2 = (k + 1) % RING_N
            idx = [bases[i]+k, bases[i]+k2, bases[i+1]+k2, bases[i+1]+k]
            mid = vnorm(vadd(rings[i][1][k], rings[i][1][k2]))
            reg = region
            if region_bottom and mid[2] < -0.6:
                reg = region_bottom
            s0, t0, s1, t1 = reg
            sk  = s0 + (s1 - s0) * (k / RING_N)
            sk2 = s0 + (s1 - s0) * ((k + 1) / RING_N)
            ta  = t0 + (t1 - t0) * tt[i]
            tb  = t0 + (t1 - t0) * tt[i+1]
            uv = [(sk, ta), (sk2, ta), (sk2, tb), (sk, tb)]
            faces.append(wind_cw(idx, uv, mid))
    return bases

def add_cap(base, outward, region):
    """Close a ring with a single fan polygon over its own vertices."""
    s0, t0, s1, t1 = region
    idx, uv = [], []
    for k in range(RING_N):
        a = -math.pi + 2.0 * math.pi * k / RING_N
        idx.append(base + k)
        uv.append(((s0+s1)/2 + (s1-s0)*0.48*math.cos(a),
                   (t0+t1)/2 + (t1-t0)*0.48*math.sin(a)))
    faces.append(wind_cw(idx, uv, outward))

def add_box(center, half, region, seg):     # --diag probes only
    cs, ds = [], []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                cs.append((center[0]+sx*half[0], center[1]+sy*half[1], center[2]+sz*half[2]))
                ds.append(vnorm((sx, sy, sz)))
    b = add_verts(cs, ds, seg)
    ci = lambda sx, sy, sz: sx*4 + sy*2 + sz
    s0, t0, s1, t1 = region
    uv = [(s0, t0), (s1, t0), (s1, t1), (s0, t1)]
    for n, q in [((+1,0,0), [ci(1,0,0), ci(1,1,0), ci(1,1,1), ci(1,0,1)]),
                 ((-1,0,0), [ci(0,0,0), ci(0,1,0), ci(0,1,1), ci(0,0,1)]),
                 ((0,+1,0), [ci(0,1,0), ci(1,1,0), ci(1,1,1), ci(0,1,1)]),
                 ((0,-1,0), [ci(0,0,0), ci(1,0,0), ci(1,0,1), ci(0,0,1)]),
                 ((0,0,+1), [ci(0,0,1), ci(1,0,1), ci(1,1,1), ci(0,1,1)]),
                 ((0,0,-1), [ci(0,0,0), ci(1,0,0), ci(1,1,0), ci(0,1,0)])]:
        faces.append(wind_cw([b+i for i in q], list(uv), n))

if DIAG:
    # axis probe: static colored boxes, one per axis direction, all frames
    # identical. Skin quadrants: q00 red, q10 green, q01 blue, q11 yellow.
    add_box(( 0.0, -25.0,  -8.0), (2.5, 2.5, 2.5), (0.05, 0.05, 0.45, 0.45), 0)
    add_box((10.0, -25.0, -18.0), (2.5, 2.5, 2.5), (0.55, 0.05, 0.95, 0.45), 0)
    add_box((-10.0, -25.0, -18.0), (2.5, 2.5, 2.5), (0.05, 0.55, 0.45, 0.95), 0)
else:
    # thigh (hip space): quad bulge up top, tapering into the knee; runs a
    # touch past the knee pivot so the joint stays closed when it bends
    thigh = add_loft([leg_ring(  0.5, 4.2),
                      leg_ring( -6.5, 4.45),
                      leg_ring(-13.5, 3.7),
                      leg_ring(-20.3, 3.25)], 0, R_PANTS)
    add_cap(thigh[0], (0, 0, 1), R_PANTS)

    # shin (knee space): starts above the pivot (tucks into the thigh),
    # calf swell, then tapers toward the ankle
    shin = add_loft([leg_ring(  2.8, 3.15),
                     leg_ring( -1.2, 3.5),
                     leg_ring( -5.2, 3.1),
                     leg_ring( -8.0, 2.8)], 1, R_PANTS)
    add_cap(shin[-1], (0, 0, -1), R_CAP)

    # boot shaft (knee space): leather collar slightly proud of the shin,
    # laced down the front, meeting the foot at the ankle
    shaft = add_loft([leg_ring( -7.5, 3.0, e=0.7),
                      leg_ring(-10.8, 2.8, e=0.7),
                      leg_ring(-14.2, 2.55, e=0.7)], 1, R_SHAFT)
    add_cap(shaft[0], (0, 0, 1), R_CAP)

    # boot foot (knee space): heel -> instep -> rounded toe cap, lofted
    # along +X with the sole picked out in rubber. The toe reaches ~+9 so
    # the kick's reach matches the old model.
    foot_rings = [foot_ring(-3.5, -18.9, -14.3, 2.7),
                  foot_ring(-1.2, -19.1, -13.7, 3.1),   # ankle rise
                  foot_ring( 2.2, -19.1, -14.6, 3.2),   # instep
                  foot_ring( 5.6, -19.1, -15.7, 3.1),
                  foot_ring( 8.6, -19.0, -16.6, 2.7),
                  foot_ring(10.6, -18.3, -17.4, 1.8)]   # toe cap rounds off
    foot = add_loft(foot_rings, 1, R_FOOT, region_bottom=R_SOLE)
    add_cap(foot[0], (-1, 0, 0), R_CAP)                 # heel
    add_cap(foot[-1], (1, 0, 0), R_CAP)                 # toe

NUM_XYZ = len(verts)

def frame_verts(theta, phi):
    """World-space positions + light normals for all verts of one pose."""
    knee = vadd(HIP, rot_pitch(KNEE_LOCAL, theta))
    pos, nrm = [], []
    for p, d, seg in verts:
        if DIAG:
            pos.append(p)
            nrm.append(nearest_anorm(d))
            continue
        ang = theta if seg == 0 else theta - phi
        base = HIP if seg == 0 else knee
        pos.append(vadd(base, rot_pitch(p, ang)))
        nrm.append(nearest_anorm(vnorm(rot_pitch(d, ang))))
    return pos, nrm

# ---------------------------------------------------------------- md2 write

def build_md2():
    num_frames = len(POSES)
    num_tris = sum(len(idx) - 2 for idx, _uv in faces)
    num_st = sum(len(idx) for idx, _uv in faces)
    framesize = 40 + 4 * NUM_XYZ

    # glcmds: one fan per face + terminator
    glcmds = b""
    num_glcmds = 0
    for idx, uv in faces:
        glcmds += struct.pack("<i", -len(idx))
        num_glcmds += 1
        for k in range(len(idx)):
            glcmds += struct.pack("<ffi", uv[k][0], uv[k][1], idx[k])
            num_glcmds += 3
    glcmds += struct.pack("<i", 0)
    num_glcmds += 1

    # st + tris lumps (unused by the GL renderer but must validate)
    st = b""
    tris = b""
    sbase = 0
    for idx, uv in faces:
        for k in range(len(idx)):
            st += struct.pack("<hh", int(uv[k][0]*SKIN_W), int(uv[k][1]*SKIN_H))
        for k in range(1, len(idx) - 1):
            tris += struct.pack("<3h3h", idx[0], idx[k], idx[k+1],
                                sbase, sbase+k, sbase+k+1)
        sbase += len(idx)

    frames = b""
    for f, (theta, phi) in enumerate(POSES):
        pos, nrm = frame_verts(theta, phi)
        mins = [min(p[i] for p in pos) for i in range(3)]
        maxs = [max(p[i] for p in pos) for i in range(3)]
        scale = [max(maxs[i]-mins[i], 1e-5) / 255.0 for i in range(3)]
        frames += struct.pack("<3f3f16s", *scale, *mins, f"kick{f}".encode())
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
        0x32504449, 8,                      # "IDP2", ALIAS_VERSION
        SKIN_W, SKIN_H, framesize,
        1, NUM_XYZ, num_st, num_tris, num_glcmds, num_frames,
        ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_glcmds, ofs_end)

    return header + skins + st + tris + frames + glcmds

# ---------------------------------------------------------------- skin paint

def px_rect(region):
    s0, t0, s1, t1 = region
    return int(s0*SKIN_W), int(t0*SKIN_H), int(s1*SKIN_W), int(t1*SKIN_H)

def build_skin_tga():
    """64x64 uncompressed 24-bit TGA, painted to match the marine:
    olive-drab fatigues with camo mottling (pants), worn brown leather
    with a laced front strip (boot shaft), scuffed leather with a toe-cap
    seam (foot), and near-black rubber tread (sole)."""
    header = struct.pack("<BBB5B4H2B", 0, 0, 2, 0,0,0,0,0, 0, 0, SKIN_W, SKIN_H, 24, 0)
    img = [[(60, 42, 26)] * SKIN_W for _ in range(SKIN_H)]    # leather backdrop
                                                              # (mip-bleed safe); y=0 top

    def fill(region, fn):
        x0, y0, x1, y1 = px_rect(region)
        for y in range(y0, y1):
            for x in range(x0, x1):
                u = (x - x0) / max(1, x1 - x0 - 1)
                v = (y - y0) / max(1, y1 - y0 - 1)
                img[y][x] = fn(x, y, u, v)

    if DIAG:
        quad = {(0,0): (255,0,0), (1,0): (0,255,0), (0,1): (0,0,255), (1,1): (255,255,0)}
        fill((0, 0, 1, 1), lambda x, y, u, v: quad[(x//32, y//32)])
    else:
        def pants(x, y, u, v):
            base = (96, 92, 58)                       # olive drab
            n = hash01(x//3, y//3, 1)                 # camo blotches
            if n < 0.22:   base = (70, 68, 44)
            elif n > 0.86: base = (108, 104, 68)
            d = 1.0 - 0.18 * abs(math.sin(u * math.pi * 3.0))   # fabric folds
            d *= 1.0 - 0.25 * v                       # darker toward the boot
            return tuple(int(c * d) for c in base)

        def leather(x, y, u, v, laces):
            base = (76, 52, 32)                       # worn brown leather
            d = 1.0 - 0.14 * abs(math.sin(u * math.pi * 4.0))
            d *= 0.9 + 0.2 * hash01(x, y, 2)          # grain
            c = [int(cc * d) for cc in base]
            if laces and 0.40 < u < 0.60:             # front lace strip
                c = [44, 30, 20]                      # recessed tongue
                ph = (v * 10.0) % 1.0
                if abs(ph - 0.5) < 0.22:              # crisscross laces
                    c = [150, 122, 82]
            return tuple(c)

        def foot_leather(x, y, u, v):
            c = list(leather(x, y, u, v, False))
            # toe-cap stitch seam across the loft's last quarter
            if 0.72 < v < 0.78:
                c = [max(0, cc - 26) for cc in c]
            if v > 0.78:                              # scuffed toe
                s = 1.0 + 0.25 * hash01(x, y, 3)
                c = [min(255, int(cc * s)) for cc in c]
            return tuple(c)

        def sole(x, y, u, v):
            base = 40 if (x // 3) % 2 else 26         # tread bands
            return (base, base - 4, base - 6)

        fill(R_PANTS, pants)
        fill(R_SHAFT, lambda x, y, u, v: leather(x, y, u, v, True))
        fill(R_FOOT, foot_leather)
        fill(R_SOLE, sole)
        fill(R_CAP, lambda x, y, u, v: leather(x, y, u, v, False))

    # TGA image type 2 with descriptor 0 is bottom-up: emit rows reversed
    out = b""
    for y in range(SKIN_H - 1, -1, -1):
        for x in range(SKIN_W):
            r, g, b = img[y][x]
            out += bytes((b, g, r))
    return header + out

if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    md2 = build_md2()
    (OUT_DIR / "tris.md2").write_bytes(md2)
    (OUT_DIR / "skin.tga").write_bytes(build_skin_tga())
    print(f"wrote {OUT_DIR / 'tris.md2'} ({len(md2)} bytes, {NUM_XYZ} verts, "
          f"{len(faces)} faces) + skin.tga")
