# gen_pak.py -- pack the port's NEW assets into a release pak.
#
# The repo-root baseq2/ tree is the canonical manifest of every asset this
# port adds on top of retail (generated models, decal/sprite textures, HUD
# icons, ...). This packs that whole tree into build/baseq2/pak1.pak (the
# engine auto-loads pak0..pak9) and writes a manifest listing beside it.
#
# CAUTION for development: the engine searches paks BEFORE loose files, so
# a stale pak1.pak SHADOWS freshly copied loose assets. Only generate this
# for a release; delete build/baseq2/pak1.pak to go back to loose-file dev.
#
# Author: Len Mudgett

import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "baseq2"
OUT = ROOT / "build" / "baseq2" / "pak1.pak"
MANIFEST = ROOT / "build" / "baseq2" / "pak1_manifest.txt"

MAX_NAME = 56


def main():
    files = sorted(p for p in SRC.rglob("*") if p.is_file())
    if not files:
        sys.exit(f"no assets under {SRC}")

    entries = []
    data = bytearray(12)            # header placeholder
    for p in files:
        name = p.relative_to(SRC).as_posix().lower()
        raw = p.read_bytes()
        if len(name) >= MAX_NAME:
            sys.exit(f"path too long for pak ({len(name)} >= {MAX_NAME}): {name}")
        entries.append((name, len(data), len(raw)))
        data += raw

    dirofs = len(data)
    for name, fpos, flen in entries:
        data += struct.pack("<56sii", name.encode("latin-1"), fpos, flen)
    struct.pack_into("<4sii", data, 0, b"PACK", dirofs, len(entries) * 64)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(data)
    MANIFEST.write_text(
        "# assets added by this port, packed into pak1.pak\n"
        + "\n".join(f"{flen:>9}  {name}" for name, _f, flen in entries) + "\n")

    total = sum(flen for _n, _f, flen in entries)
    print(f"wrote {OUT}  ({len(entries)} files, {total/1024:.0f} KB)")
    print(f"wrote {MANIFEST}")
    print("NOTE: paks shadow loose files -- delete pak1.pak for loose-file dev.")


if __name__ == "__main__":
    main()
