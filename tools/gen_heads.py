# gen_heads.py -- carve per-monster severed-head models out of the retail MD2s.
#
# Quake 2 gibs every monster into the same generic head2 model; Quake 1 left
# the monster's OWN head lying around. This reads every monster model out of
# pak0.pak, takes its frame-0 (standing) pose, keeps only the triangles whose
# vertices all sit in the top slice of the model -- the head (with a bit of
# neck/shoulder), textured by the monster's real skin and UVs -- and emits it
# as a single-frame head.md2 next to the source model's path. The game's
# ThrowHead swaps it in for the generic head at gib time.
#
# The neck cut is an open rim, but the game only ever spins gib heads around
# YAW, so the hole stays face-down against the floor and is never seen.
#
# Output: baseq2/models/monsters/<dir>/head.md2 (repo tree; CMake copies
# baseq2/ into build/baseq2 after each game-DLL build).
#
# Author: Len Mudgett

import math
import struct
import sys

from q2gen import ROOT, decode_pcx, hash01, nearest_anorm, read_pak

PAK = ROOT / "build" / "baseq2" / "pak0.pak"
OUT_ROOT = ROOT / "baseq2"

MAX_SKINNAME = 64

# fleshy monsters get a stub of spinal column dangling off the neck cut;
# machines obviously don't
SPINE = {"soldier", "infantry", "gunner", "berserk", "gladiatr", "medic",
         "bitch", "insane", "brain", "parasite"}

# starting depth of the cut as a fraction of the model's frame-0 height;
# it grows until the head chunk has substance, so this starts SHALLOW to
# stop before the shoulders join on. Bodies with no real head get deeper
# starts. Cut never grows past 0.6.
DEFAULT_FRAC = 0.10
FRAC_OVERRIDES = {
    "flipper":  0.40,   # a fish: take the front-ish dorsal chunk
    "parasite": 0.35,   # low dog-shaped body
    "boss1":    0.15,
    "boss2":    0.15,
}
MIN_TRIS = 24

# shear parameters: (top_frac, radius budget, depth, min tris). Bodies whose
# head component drags connected spikes/armor along need the tight profile;
# helmeted humanoids need it loose or the trim shells off their faces.
TRIM_DEFAULT = (0.45, 1.5, 3.0, 12)
TRIM_PARAMS = {
    "berserk": (0.30, 1.35, 2.4, 8),
    "gunner":  (0.30, 1.35, 2.4, 8),
    "hover":   (0.30, 1.35, 2.4, 8),
    "mutant":  (0.30, 1.35, 2.4, 8),
    "soldier": (0.50, 1.6, 3.8, 12),    # keep the whole face and chin
}

# minimum head height (units) before the slice may stop growing -- the
# soldier's helmet dome alone passes the generic gate but has NO FACE
NEED_H_OVERRIDES = {
    "soldier": 8.5,
}

# gib readability: heads drawn a bit over life-size read as props, not lumps
HEAD_SCALE = 1.30

# model dirs the GAME references (head_dirs[] in src/game/g_misc.c). If a
# heuristic tweak makes one of these fall into the skip path, the game
# would precache a phantom model and gib an invisible head -- fail LOUDLY
# here instead so the table and the generated files can't drift apart.
REQUIRED = {
    "soldier", "infantry", "gunner", "berserk", "gladiatr", "medic",
    "hover", "float", "parasite", "brain", "bitch", "tank", "insane",
    "boss1", "boss2", "boss3/jorg", "boss3/rider",
}

# ---------------------------------------------------------------- gore

def find_gore_uv(entries, skinname):
    """Scan the monster's own skin for a DARK red patch: the spine and
    base cap sample it, and the alias pipeline renders texels at ~2x
    intensity, so anything brighter than dried blood (or not strongly
    red) comes out salmon/bone in game -- reject those outright and let
    the caller fall back to the darkest patch instead."""
    data = entries.get(skinname.decode("latin-1").lower())
    if not data:
        return None
    try:
        w, h, pix, pal = decode_pcx(data)
    except Exception:
        return None
    best = None
    B = 4
    for by in range(0, h - B, B):
        for bx in range(0, w - B, B):
            rs = gs = bs = 0
            for y in range(by, by + B):
                row = y * w
                for x in range(bx, bx + B):
                    c = pix[row + x] * 3
                    rs += pal[c]; gs += pal[c + 1]; bs += pal[c + 2]
            n = B * B
            r, g, b = rs / n, gs / n, bs / n
            if r < 45 or r > 100:
                continue             # dried-blood dark only
            if max(g, b) > 0.6 * r:
                continue             # strongly red only, no tan/skin tones
            score = r - 1.5 * max(g, b)
            if best is None or score > best[0]:
                best = (score, ((bx + B / 2) / w, (by + B / 2) / h))
    if best and best[0] > 5:
        return best[1]
    return None

def find_dark_uv(entries, skinname):
    """Darkest 4x4 block of the skin: base-cap texels for heads with no
    gory red to borrow (machines)."""
    data = entries.get(skinname.decode("latin-1").lower())
    if not data:
        return None
    try:
        w, h, pix, pal = decode_pcx(data)
    except Exception:
        return None
    best = None
    B = 4
    for by in range(0, h - B, B):
        for bx in range(0, w - B, B):
            tot = 0
            for y in range(by, by + B):
                row = y * w
                for x in range(bx, bx + B):
                    c = pix[row + x] * 3
                    tot += pal[c] + pal[c + 1] + pal[c + 2]
            if best is None or tot < best[0]:
                best = (tot, ((bx + B / 2) / w, (by + B / 2) / h))
    return best[1] if best else None

# ---------------------------------------------------------------- spine

def add_spine(pts, norms, tris, gore_uv, head_h, head_w):
    """Vertebrae stub off the neck cut: exits heading down-and-back, then
    flattens to TRAIL along the floor (heads rest upright and only spin in
    yaw -- a straight-down spine would stilt the head into the air).
    Alternating fat/thin rings read as vertebrae and discs."""
    RING = 6
    radii = [1.15, 0.7, 0.95, 0.62, 0.8]    # first ring fat: buried plug
    L = min(10.0, max(5.5, 1.0 * head_h))
    R = min(1.2, max(0.6, 0.16 * head_w))
    su, sv = gore_uv

    # anchor at the skull base: the bottom region's own centroid, nudged
    # toward the back -- but only within the neck opening's own rear
    # extent, or the spine ends up floating BEHIND the head. The first
    # ring starts buried well up inside the shell so the column visibly
    # emerges from the head instead of hovering under it.
    hmax = max(p[2] for p in pts)
    low = [p for p in pts if p[2] <= 0.35 * hmax] or pts
    lcx = sum(p[0] for p in low) / len(low)
    lcy = sum(p[1] for p in low) / len(low)
    back_extent = lcx - min(p[0] for p in low)
    ax_x = lcx - 0.35 * back_extent
    z0 = 0.42 * hmax

    # stations along the exit curve
    stations = []
    px, pz = ax_x, z0
    a0, a1 = math.radians(75), math.radians(6)
    step = L / (len(radii) - 1)
    for i in range(len(radii)):
        t = i / (len(radii) - 1.0)
        stations.append(((px, lcy, max(pz, 0.30)), radii[i] * R))
        a = a0 + (a1 - a0) * t
        px -= math.cos(a) * step
        pz -= math.sin(a) * step

    def ring_frame(i):
        (x0, _, z0), _ = stations[max(i - 1, 0)]
        (x1, _, z1), _ = stations[min(i + 1, len(stations) - 1)]
        tx, tz = x1 - x0, z1 - z0
        tl = math.hypot(tx, tz) or 1.0
        tx, tz = tx / tl, tz / tl
        # side = +Y; up2 = tangent x side (stays in the xz plane)
        return (0.0, 1.0, 0.0), (tz, 0.0, -tx)

    base = len(pts)
    for i, ((cx, cy, cz), r) in enumerate(stations):
        side, up2 = ring_frame(i)
        for k in range(RING):
            a = 2.0 * math.pi * k / RING
            ca, sa = math.cos(a), math.sin(a)
            out = tuple(side[j] * ca + up2[j] * sa for j in range(3))
            pts.append((cx + out[0] * r, cy + out[1] * r, cz + out[2] * r))
            norms.append(nearest_anorm(out))

    def spine_uv(i, k):
        return (su + (hash01(i, k) - 0.5) * 0.03,
                sv + (hash01(k, i + 7) - 0.5) * 0.03)

    def tri(a, b, c, ia, ib, ic):
        # wind CW seen from outside (scene culls GL_FRONT)
        pa, pb, pc = pts[a], pts[b], pts[c]
        u = tuple(pb[j] - pa[j] for j in range(3))
        w = tuple(pc[j] - pa[j] for j in range(3))
        n = (u[1]*w[2]-u[2]*w[1], u[2]*w[0]-u[0]*w[2], u[0]*w[1]-u[1]*w[0])
        ctr = tuple((pa[j] + pb[j] + pc[j]) / 3.0 - stations[min(ia, len(stations)-1)][0][j]
                    for j in range(3))
        if sum(n[j] * ctr[j] for j in range(3)) > 0:
            a, c = c, a
        return ((a, spine_uv(ia, 0)), (b, spine_uv(ib, 1)), (c, spine_uv(ic, 2)))

    for i in range(len(stations) - 1):
        for k in range(RING):
            k2 = (k + 1) % RING
            v00, v01 = base + i*RING + k, base + i*RING + k2
            v10, v11 = base + (i+1)*RING + k, base + (i+1)*RING + k2
            tris.append(tri(v00, v01, v11, i, i, i + 1))
            tris.append(tri(v00, v11, v10, i, i + 1, i + 1))
    # tip cap
    tipc = stations[-1][0]
    pts.append(tipc)
    norms.append(nearest_anorm((-1.0, 0.0, 0.0)))
    tip = len(pts) - 1
    ilast = len(stations) - 1
    for k in range(RING):
        k2 = (k + 1) % RING
        tris.append(tri(base + ilast*RING + k, base + ilast*RING + k2, tip,
                        ilast, ilast, ilast))
    return pts, norms, tris

def add_base_cap(pts, norms, tris, cap_uv):
    """Close the head's open underside: a 12-sector disk shaped by the
    mesh's own bottom silhouette, faced DOWNWARD -- a tilted head shows a
    solid (gory) cross-section instead of a hollow shell."""
    zs = [p[2] for p in pts]
    lowz = 0.35 * max(zs)
    low = [p for p in pts if p[2] <= lowz] or pts
    cx = sum(p[0] for p in low) / len(low)
    cy = sum(p[1] for p in low) / len(low)
    SEG = 12
    radii = [0.0] * SEG
    for p in low:
        a = math.atan2(p[1] - cy, p[0] - cx)
        s = int(((a + math.pi) / (2.0 * math.pi)) * SEG) % SEG
        r = math.hypot(p[0] - cx, p[1] - cy)
        if radii[s] < r:
            radii[s] = r
    for i in range(SEG):        # fill sectors nothing landed in
        if radii[i] == 0.0:
            radii[i] = max(radii[(i - 1) % SEG], radii[(i + 1) % SEG])

    base = len(pts)
    for k in range(SEG):
        a = -math.pi + 2.0 * math.pi * k / SEG
        r = radii[k] * 0.92
        pts.append((cx + math.cos(a) * r, cy + math.sin(a) * r, 0.5))
        norms.append(nearest_anorm((0.0, 0.0, -1.0)))
    pts.append((cx, cy, 0.35))
    norms.append(nearest_anorm((0.0, 0.0, -1.0)))
    ctr = len(pts) - 1

    su, sv = cap_uv

    def uvv(i, k):
        return (su + (hash01(i, k) - 0.5) * 0.04,
                sv + (hash01(k, i + 3) - 0.5) * 0.04)

    for k in range(SEG):
        k2 = (k + 1) % SEG
        a1, b1, c1 = ctr, base + k, base + k2
        pa, pb, pc = pts[a1], pts[b1], pts[c1]
        u = (pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2])
        w = (pc[0]-pa[0], pc[1]-pa[1], pc[2]-pa[2])
        nz = u[0]*w[1] - u[1]*w[0]
        if nz < 0:      # same convention as the spine: RH normal INWARD (up)
            b1, c1 = c1, b1
        tris.append(((a1, uvv(k, 0)), (b1, uvv(k, 1)), (c1, uvv(k, 2))))
    return pts, norms, tris

# ---------------------------------------------------------------- md2 read

def parse_md2(buf):
    (ident, ver, skinw, skinh, framesize, nskins, nxyz, nst, ntris,
     nglcmds, nframes, ofs_skins, ofs_st, ofs_tris, ofs_frames,
     ofs_glcmds, ofs_end) = struct.unpack_from("<17i", buf, 0)
    assert ident == 0x32504449 and ver == 8, "not an md2"

    skins = [buf[ofs_skins + 64*i: ofs_skins + 64*(i+1)].split(b"\0")[0]
             for i in range(nskins)]

    # cut from a neutral STANDING pose: frame 0 is whatever the animator
    # left first (often a lean or crouch where fists top the head)
    frame = 0
    for f in range(nframes):
        name = buf[ofs_frames + f*framesize + 24: ofs_frames + f*framesize + 40]
        if name.split(b"\0")[0].lower().startswith(b"stand"):
            frame = f
            break
    fofs = ofs_frames + frame * framesize
    scale = struct.unpack_from("<3f", buf, fofs)
    trans = struct.unpack_from("<3f", buf, fofs + 12)
    verts, normi = [], []
    off = fofs + 40
    for i in range(nxyz):
        x, y, z, n = struct.unpack_from("<4B", buf, off + 4*i)
        verts.append((trans[0] + x*scale[0], trans[1] + y*scale[1],
                      trans[2] + z*scale[2]))
        normi.append(n)

    # walk glcmds into a triangle list of ((vertindex, (s,t)) x3)
    tris = []
    p = ofs_glcmds
    while True:
        (cnt,) = struct.unpack_from("<i", buf, p); p += 4
        if cnt == 0:
            break
        fan = cnt < 0
        cnt = abs(cnt)
        vs = []
        for _ in range(cnt):
            s, t, vi = struct.unpack_from("<ffi", buf, p); p += 12
            vs.append((vi, (s, t)))
        if fan:
            for k in range(1, cnt - 1):
                tris.append((vs[0], vs[k], vs[k+1]))
        else:
            for k in range(2, cnt):
                if k & 1:   # strips alternate winding
                    tris.append((vs[k-1], vs[k-2], vs[k]))
                else:
                    tris.append((vs[k-2], vs[k-1], vs[k]))
    return dict(skinw=skinw, skinh=skinh, skins=skins,
                verts=verts, normi=normi, tris=tris)

# ---------------------------------------------------------------- head cut

def components(tris, wkey):
    """Group triangles into connected components. Adjacency runs through
    POSITION-welded vertices (wkey), not raw indices: md2 body parts
    duplicate vertices instead of sharing them, so raw-index connectivity
    shatters a solid head into shards."""
    parent = {}

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for t in tris:
        for vi, _st in t:
            parent.setdefault(wkey[vi], wkey[vi])
        union(wkey[t[0][0]], wkey[t[1][0]])
        union(wkey[t[0][0]], wkey[t[2][0]])
    groups = {}
    for t in tris:
        groups.setdefault(find(wkey[t[0][0]]), []).append(t)
    return groups          # root -> [tris]

def extract_head(m, frac, key):
    verts = m["verts"]
    zs = [v[2] for v in verts]
    zmin, zmax = min(zs), max(zs)
    height = zmax - zmin
    ax = sum(v[0] for v in verts) / len(verts)
    ay = sum(v[1] for v in verts) / len(verts)
    need_h = NEED_H_OVERRIDES.get(key,
        max(4.0, 0.10 * height))        # a head, not a helmet-top sliver

    def rxy(v):
        return math.hypot(v[0] - ax, v[1] - ay)

    rlim = 0.35 * max(rxy(v) for v in verts)    # "on-axis" radius
    wkey = {i: (round(v[0] / 0.6), round(v[1] / 0.6), round(v[2] / 0.6))
            for i, v in enumerate(verts)}

    # slice from the top, shallow first. The head is the connected chunk
    # under the CROWN -- the topmost vertex sitting near the body's axis;
    # raised fists and rifle muzzles enter the slice too, but as separate
    # off-axis islands, and growth stops before the shoulders merge on.
    keep = None
    while frac <= 0.6:
        zcut = zmax - frac * height
        cand = [t for t in m["tris"]
                if all(verts[vi][2] >= zcut for vi, _st in t)]
        if cand:
            groups = components(cand, wkey)

            def usable(c):
                cverts = {vi for t in c for vi, _st in t}
                czs = [verts[vi][2] for vi in cverts]
                ch = max(czs) - min(czs)
                if len(c) < MIN_TRIS or ch < need_h:
                    return False
                # heads are roughly round; a shoulder-spike or backpack
                # slab is several times wider than tall -- reject it
                cw = max(max(verts[vi][0] for vi in cverts)
                         - min(verts[vi][0] for vi in cverts),
                         max(verts[vi][1] for vi in cverts)
                         - min(verts[vi][1] for vi in cverts))
                return cw <= 2.6 * ch

            # crown-seeded picks, walking DOWN the on-axis verts: the
            # topmost central thing can be an antenna tip or a shoulder
            # spike whose component never reads as a head -- try the next
            # seed below it. Biggest on-axis component is the last resort.
            seeds = sorted({vi for t in cand for vi, _st in t
                            if rxy(verts[vi]) <= rlim},
                           key=lambda vi: -verts[vi][2])
            ranked = []
            seen_roots = set()
            for vi in seeds[:64]:
                for root, c in groups.items():
                    if root in seen_roots:
                        continue
                    if any(wkey[v] == wkey[vi] for t in c for v, _st in t):
                        seen_roots.add(root)
                        ranked.append(c)
                        break
            for c in sorted(groups.values(), key=len, reverse=True):
                cverts = {vi for t in c for vi, _st in t}
                cx = sum(verts[vi][0] for vi in cverts) / len(cverts)
                cy = sum(verts[vi][1] for vi in cverts) / len(cverts)
                if math.hypot(cx - ax, cy - ay) <= rlim and c not in ranked:
                    ranked.append(c)
            for c in ranked:
                if usable(c):
                    keep = c
                    break
        if keep:
            break
        frac += 0.03
    if not keep:
        return None

    # trim the pick down to JUST the head. The top of the component is
    # pure skull, so its radius sets the budget: anything hanging wider
    # (shoulder caps, packs) or deeper (collar, chest) gets sheared off.
    cverts = {vi for t in keep for vi, _st in t}
    czmax = max(verts[vi][2] for vi in cverts)
    czmin = min(verts[vi][2] for vi in cverts)
    top_frac, budget, depth, mintris = TRIM_PARAMS.get(key, TRIM_DEFAULT)
    topv = [vi for vi in cverts
            if verts[vi][2] >= czmax - top_frac * max(czmax - czmin, 1.0)]
    hx = sum(verts[vi][0] for vi in topv) / len(topv)
    hy = sum(verts[vi][1] for vi in topv) / len(topv)
    rtop = max(math.hypot(verts[vi][0] - hx, verts[vi][1] - hy)
               for vi in topv) or 1.0
    zfloor = czmax - depth * rtop
    trimmed = [t for t in keep
               if all(math.hypot(verts[vi][0] - hx, verts[vi][1] - hy)
                      <= budget * rtop and verts[vi][2] >= zfloor
                      for vi, _st in t)]
    if len(trimmed) >= mintris:
        keep = trimmed

    used = sorted({vi for t in keep for vi, _st in t})
    remap = {vi: i for i, vi in enumerate(used)}
    pts = [m["verts"][vi] for vi in used]
    cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2.0
    cy = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2.0
    zbase = min(p[2] for p in pts)
    # center in x/y (it spins around yaw), base on the floor at z=0,
    # and draw a touch over life-size so the gib reads as a head
    pts = [((p[0] - cx) * HEAD_SCALE, (p[1] - cy) * HEAD_SCALE,
            (p[2] - zbase) * HEAD_SCALE) for p in pts]
    norms = [m["normi"][vi] for vi in used]
    tris = [tuple((remap[vi], st) for vi, st in t) for t in keep]
    return pts, norms, tris

# ---------------------------------------------------------------- md2 write

def build_head_md2(m, pts, norms, tris):
    skinw, skinh = m["skinw"], m["skinh"]
    nxyz = len(pts)

    glcmds = b""
    num_glcmds = 0
    st_lump = b""
    tris_lump = b""
    sbase = 0
    for t in tris:
        glcmds += struct.pack("<i", -3)     # one fan per triangle
        num_glcmds += 1
        for vi, (s, tt) in t:
            glcmds += struct.pack("<ffi", s, tt, vi)
            num_glcmds += 3
            st_lump += struct.pack("<hh",
                min(skinw - 1, max(0, int(s * skinw))),
                min(skinh - 1, max(0, int(tt * skinh))))
        tris_lump += struct.pack("<3h3h",
            t[0][0], t[1][0], t[2][0], sbase, sbase + 1, sbase + 2)
        sbase += 3
    glcmds += struct.pack("<i", 0)
    num_glcmds += 1

    mins = [min(p[i] for p in pts) for i in range(3)]
    maxs = [max(p[i] for p in pts) for i in range(3)]
    scale = [max(maxs[i] - mins[i], 1e-5) / 255.0 for i in range(3)]
    frame = struct.pack("<3f3f16s", *scale, *mins, b"head")
    for p, n in zip(pts, norms):
        v = [min(255, max(0, round((p[i] - mins[i]) / scale[i]))) for i in range(3)]
        frame += struct.pack("<4B", v[0], v[1], v[2], n)

    skins = b"".join(s.ljust(MAX_SKINNAME, b"\0") for s in m["skins"])
    framesize = 40 + 4 * nxyz

    ofs_skins = 68
    ofs_st = ofs_skins + len(skins)
    ofs_tris = ofs_st + len(st_lump)
    ofs_frames = ofs_tris + len(tris_lump)
    ofs_glcmds = ofs_frames + len(frame)
    ofs_end = ofs_glcmds + len(glcmds)

    header = struct.pack("<17i",
        0x32504449, 8, skinw, skinh, framesize,
        len(m["skins"]), nxyz, sbase, len(tris), num_glcmds, 1,
        ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_glcmds, ofs_end)
    return header + skins + st_lump + tris_lump + frame + glcmds

# ---------------------------------------------------------------- main

def main():
    # clear stale outputs first: a model that stops yielding a head must
    # not leave last run's file behind
    for old in (OUT_ROOT / "models" / "monsters").glob("**/head.md2"):
        old.unlink()
    entries = read_pak(PAK)
    models = sorted(n for n in entries
                    if n.startswith("models/monsters/") and n.endswith("/tris.md2"))
    made = 0
    done = set()
    for name in models:
        subdir = name[len("models/monsters/"):-len("/tris.md2")]
        key = subdir.split("/")[0]
        frac = FRAC_OVERRIDES.get(key, DEFAULT_FRAC)
        try:
            m = parse_md2(entries[name])
        except Exception as e:
            print(f"SKIP {name}: {e}")
            continue
        cut = extract_head(m, frac, key)
        if not cut:
            print(f"SKIP {name}: no usable head cut")
            continue
        pts, norms, tris = cut
        h = max(p[2] for p in pts)
        w = max(max(abs(p[0]), abs(p[1])) for p in pts) * 2
        spine = ""
        cap_uv = None
        if m["skins"]:
            gore = find_gore_uv(entries, m["skins"][0])
            if key in SPINE and gore:
                pts, norms, tris = add_spine (pts, norms, tris, gore, h, w)
                spine = ", spine"
            cap_uv = gore if gore else find_dark_uv(entries, m["skins"][0])
        pts, norms, tris = add_base_cap(pts, norms, tris, cap_uv or (0.5, 0.5))
        out = OUT_ROOT / "models" / "monsters" / subdir / "head.md2"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(build_head_md2(m, pts, norms, tris))
        print(f"wrote {out.relative_to(ROOT)}  ({len(tris)} tris, "
              f"{h:.0f}u tall, {w:.0f}u wide{spine})")
        made += 1
        done.add(subdir)
    print(f"{made}/{len(models)} heads generated")
    missing = REQUIRED - done
    if missing:
        sys.exit(f"FAIL: the game's head_dirs[] table references heads that "
                 f"did not generate: {sorted(missing)} -- fix the cut "
                 f"heuristics or remove them from g_misc.c")

if __name__ == "__main__":
    main()
