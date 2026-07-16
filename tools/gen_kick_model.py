# gen_kick_model.py -- generate the boot-kick view model (v_kick) as a Quake 2 MD2.
#
# Builds a boxy right leg (thigh / shin / boot) posed over 6 kick frames
# (windup -> full extension -> retract) plus a small flat-color TGA skin.
# The renderer draws MD2s from glcmds, so each box face is emitted as a
# 4-vertex triangle fan with inline normalized UVs; the st/tris lumps are
# still written for format validity. Vertex light normals are the nearest
# entry of id's anorms table (parsed from the reference clone).
#
# Output: baseq2/models/weapons/v_kick/tris.md2 + skin.tga (CMake copies
# the repo baseq2/ tree into build/baseq2 after each game-DLL build).
#
# Author: Len Mudgett

import math
import re
import struct
import sys
from pathlib import Path

DIAG = "--diag" in sys.argv     # axis-probe build: colored marker boxes

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "baseq2" / "models" / "weapons" / "v_kick"
ANORMS_H = ROOT / "quake2" / "ref_gl" / "anorms.h"

SKIN_NAME = b"models/weapons/v_kick/skin.tga"
SKIN_W, SKIN_H = 64, 64
MAX_SKINNAME = 64

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

# ---------------------------------------------------------------- anorms

def load_anorms():
    norms = []
    for m in re.finditer(r"\{\s*([-\d.]+)\s*,\s*([-\d.]+)\s*,\s*([-\d.]+)\s*\}",
                         ANORMS_H.read_text()):
        norms.append(tuple(float(g) for g in m.groups()))
    assert len(norms) == 162, f"expected 162 anorms, got {len(norms)}"
    return norms

ANORMS = load_anorms()

def nearest_anorm(n):
    return max(range(162), key=lambda i: vdot(n, ANORMS[i]))

# ---------------------------------------------------------------- geometry
#
# View-model space is the standard Quake entity frame: +X forward, +Y left,
# +Z up, origin at the eye (verified with an axis-probe render: a box at
# +X20 draws dead-center at 20 units; boxes off the X axis never appear).
# The leg hangs from a hip pivot below/right of the eye; the shin+boot hang
# from the knee. Frame pose = (hip pitch toward forward, knee bend backward).
# A leg lives at the BOTTOM of the screen: the windup frames are naturally
# below the view; the boot enters the frame at extension, Duke-style.

HIP = (0.0, -6.0, -19.5)
KNEE_LOCAL = (0.0, 0.0, -16.0)      # knee position in (rotated) thigh space

# (center, half-extents, uv-rect, segment)  -- segment 0 = thigh, 1 = shin/boot
BOXES = [
    ((0.0, 0.0, -8.0),   (3.8, 3.8, 8.0),  (0.55, 0.20, 0.95, 0.45), 0),  # thigh (pants)
    ((0.0, 0.0, -6.5),   (3.0, 3.0, 7.0),  (0.55, 0.20, 0.95, 0.45), 1),  # shin  (pants)
    ((3.5, 0.0, -13.5),  (6.2, 3.4, 2.8),  (0.05, 0.20, 0.45, 0.45), 1),  # boot  (leather)
]

if DIAG:
    # axis probe: static colored boxes, one per axis direction, all frames
    # identical. Skin quadrants: q00 red, q10 green, q01 blue, q11 yellow.
    Q_RED, Q_GREEN, Q_BLUE, Q_YELLOW = ((0.05,0.05,0.45,0.45), (0.55,0.05,0.95,0.45),
                                        (0.05,0.55,0.45,0.95), (0.55,0.55,0.95,0.95))
    BOXES = [
        (( 0.0, -25.0,  -8.0), (2.5, 2.5, 2.5), Q_RED, 0),     # ahead, high
        ((10.0, -25.0, -18.0), (2.5, 2.5, 2.5), Q_GREEN, 0),   # ahead, low, +X side
        ((-10.0, -25.0, -18.0), (2.5, 2.5, 2.5), Q_BLUE, 0),   # ahead, low, -X side
    ]

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

def box_corners(center, half):
    cs = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            for sz in (-1, 1):
                cs.append((center[0]+sx*half[0], center[1]+sy*half[1], center[2]+sz*half[2]))
    return cs   # index = sx*4 + sy*2 + sz with (-1 -> 0, 1 -> 1)

def ci(sx, sy, sz):
    return sx*4 + sy*2 + sz

# each face: (outward normal, 4 corner indices in some cyclic order)
FACES = [
    ((+1, 0, 0), [ci(1,0,0), ci(1,1,0), ci(1,1,1), ci(1,0,1)]),
    ((-1, 0, 0), [ci(0,0,0), ci(0,1,0), ci(0,1,1), ci(0,0,1)]),
    (( 0,+1, 0), [ci(0,1,0), ci(1,1,0), ci(1,1,1), ci(0,1,1)]),
    (( 0,-1, 0), [ci(0,0,0), ci(1,0,0), ci(1,0,1), ci(0,0,1)]),
    (( 0, 0,+1), [ci(0,0,1), ci(1,0,1), ci(1,1,1), ci(0,1,1)]),
    (( 0, 0,-1), [ci(0,0,0), ci(1,0,0), ci(1,1,0), ci(0,1,0)]),
]

def wind_cw(corners, idx, n):
    """Order the quad clockwise as seen from outside (id MD2 winding:
    the scene culls GL_FRONT/CCW, so drawn faces are CW from outside)."""
    c0, c1, c2 = corners[idx[0]], corners[idx[1]], corners[idx[2]]
    if vdot(vcross(vsub(c1, c0), vsub(c2, c0)), n) > 0:    # CCW -> flip
        return [idx[0], idx[3], idx[2], idx[1]]
    return list(idx)

# Build the static topology once (vertex positions are per frame, indices fixed).
# xyz vertex i of box b = b*8 + corner.  Face UVs map each quad to its uv-rect.
faces = []          # (xyz indices CW, uv pairs, base normal, segment)
for b, (center, half, (s0, t0, s1, t1), seg) in enumerate(BOXES):
    corners = box_corners(center, half)
    uv = [(s0, t0), (s1, t0), (s1, t1), (s0, t1)]
    for n, idx in FACES:
        order = wind_cw(corners, idx, n)
        faces.append(([b*8 + i for i in order], uv, n, seg))

NUM_XYZ = len(BOXES) * 8

def frame_verts(theta, phi):
    """World-space positions + light normals for all 24 verts of one pose."""
    knee = vadd(HIP, rot_pitch(KNEE_LOCAL, theta))
    pos, nrm_rot = [], []
    for b, (center, half, _uv, seg) in enumerate(BOXES):
        if DIAG:
            for corner in box_corners(center, half):
                pos.append(corner)
                nrm_rot.append(nearest_anorm(vnorm(vsub(corner, center))))
            continue
        ang = theta if seg == 0 else theta - phi
        base = HIP if seg == 0 else knee
        for corner in box_corners(center, half):
            local = vsub(corner, center)
            pos.append(vadd(base, rot_pitch(corner, ang)))
            nrm_rot.append(nearest_anorm(vnorm(rot_pitch(local, ang))))
    return pos, nrm_rot

# ---------------------------------------------------------------- md2 write

def build_md2():
    num_frames = len(POSES)
    num_tris = len(faces) * 2
    num_st = len(faces) * 4
    framesize = 40 + 4 * NUM_XYZ

    # glcmds: one 4-vertex fan per face + terminator
    glcmds = b""
    num_glcmds = 0
    for idx, uv, _n, _seg in faces:
        glcmds += struct.pack("<i", -4)
        num_glcmds += 1
        for k in range(4):
            glcmds += struct.pack("<ffi", uv[k][0], uv[k][1], idx[k])
            num_glcmds += 3
    glcmds += struct.pack("<i", 0)
    num_glcmds += 1

    # st + tris lumps (unused by the GL renderer but must validate)
    st = b""
    tris = b""
    for f, (idx, uv, _n, _seg) in enumerate(faces):
        for k in range(4):
            st += struct.pack("<hh", int(uv[k][0]*SKIN_W), int(uv[k][1]*SKIN_H))
        s = f * 4
        tris += struct.pack("<3h3h", idx[0], idx[1], idx[2], s, s+1, s+2)
        tris += struct.pack("<3h3h", idx[0], idx[2], idx[3], s, s+2, s+3)

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

def build_skin_tga():
    """64x64 uncompressed 24-bit TGA: left half boot leather, right half pants.
    Column-keyed colors so file row order can't matter."""
    header = struct.pack("<BBB5B4H2B", 0, 0, 2, 0,0,0,0,0, 0, 0, SKIN_W, SKIN_H, 24, 0)
    if DIAG:
        # quadrant colors (BGR): q00 red, q10 green, q01 blue, q11 yellow
        img = b""
        for y in range(SKIN_H):
            for x in range(SKIN_W):
                img += bytes({(0,0):(0,0,255), (1,0):(0,255,0),
                              (0,1):(255,0,0), (1,1):(0,255,255)}[(x//32, y//32)])
        return header + img
    boot = (30, 50, 75)     # BGR: worn leather brown
    pants = (40, 62, 55)    # BGR: olive drab
    row = b""
    for x in range(SKIN_W):
        base = boot if x < 32 else pants
        # faint vertical banding so big flat faces aren't dead flat
        d = 6 if (x // 4) % 2 else 0
        row += bytes(max(0, c - d) for c in base)
    return header + row * SKIN_H

if __name__ == "__main__":
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    md2 = build_md2()
    (OUT_DIR / "tris.md2").write_bytes(md2)
    (OUT_DIR / "skin.tga").write_bytes(build_skin_tga())
    print(f"wrote {OUT_DIR / 'tris.md2'} ({len(md2)} bytes) + skin.tga")
