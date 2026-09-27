#!/usr/bin/env python
"""Tier-2 oracle for studio/world_merge.py.

Usage:
  tests/oracle_venv/bin/python tests/test_world_merge.py SRC_WORLD DST_WORLD \
      TX TY TZ

DST_WORLD must already contain the result of
  world_merge.py SRC_WORLD DST_WORLD --at TX,<resolved TY>,TZ
plus the voxforge_backup_* directory the tool wrote. The test rebuilds the
expected overlay independently from SRC and the BACKUP regions and asserts:

  1. every non-air SRC block is present at its offset position in DST
  2. every other block in touched chunks is bit-equal to the backup
  3. untouched chunks inside touched regions are fully preserved
  4. touched chunks have Heightmaps deleted and isLightOn=0

Exit 0 + "ALL PASS" on success.
"""
import glob
import sys
from pathlib import Path

import numpy as np
from nbt.region import RegionFile

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "studio"))
from world_merge import Registry, decode_indices, load_source, pal_key  # noqa: E402

AIR = ("minecraft:air", ())


def section_gids(s, reg):
    bs = s["block_states"]
    pal = bs["palette"]
    gp = np.array([reg.gid(pal_key(p)) for p in pal], dtype=np.uint32)
    if "data" in bs and len(bs["data"]) > 0:
        return gp[decode_indices(bs["data"].value, len(pal))]
    return np.full(4096, gp[0], dtype=np.uint32)


def chunk_gids(c, reg):
    return {s["Y"].value: section_gids(s, reg)
            for s in c["sections"] if "block_states" in s}


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    tx, ty, tz = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])

    reg = Registry()
    sections, lo, hi, nblocks = load_source(src, reg)
    ax, az = (lo[0] + hi[0]) // 2, (lo[2] + hi[2]) // 2
    ox, oy, oz = tx - ax, ty - lo[1], tz - az

    # rebuild expected overlay: (dcx,dcz) -> {sy: (didx, gids)}
    pos = np.arange(4096, dtype=np.int64)
    ly, lz, lx = pos >> 8, (pos >> 4) & 15, pos & 15
    overlay = {}
    for (cx, cz, sy), sect in sections.items():
        nz = np.nonzero(sect)[0]
        gx, gy, gz = cx * 16 + lx[nz] + ox, sy * 16 + ly[nz] + oy, \
            cz * 16 + lz[nz] + oz
        didx = (gy & 15) * 256 + (gz & 15) * 16 + (gx & 15)
        key = np.stack([gx >> 4, gz >> 4, gy >> 4], axis=1)
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        for i, (dcx, dcz, dsy) in enumerate(uniq):
            sel = inv == i
            overlay.setdefault((int(dcx), int(dcz)), {}).setdefault(
                int(dsy), []).append((didx[sel], sect[nz][sel]))

    # blocks in chunks the destination never generated are legitimately
    # skipped by the tool — compute that set independently
    skipped_expected = 0
    for (dcx, dcz), per in overlay.items():
        rp = dst / "region" / f"r.{dcx >> 5}.{dcz >> 5}.mca"
        missing = not rp.exists()
        if not missing:
            rf = RegionFile(str(rp))
            try:
                rf.get_nbt(dcx & 31, dcz & 31)
            except Exception:                       # noqa: BLE001
                missing = True
            rf.close()
        if missing:
            skipped_expected += sum(d.size for parts in per.values()
                                    for d, _ in parts)

    baks = sorted(glob.glob(str(dst / "voxforge_backup_*" / "r.*.mca")))
    assert baks, "no backup regions found"
    placed_checked = mismatched = preserved_bad = untouched_bad = 0
    flags_bad = 0
    for b in baks:
        name = Path(b).name
        rx, rz = int(name.split(".")[1]), int(name.split(".")[2])
        rold = RegionFile(b)
        rnew = RegionFile(str(dst / "region" / name))
        for m in rold.get_metadata():
            cx, cz = rx * 32 + m.x, rz * 32 + m.z
            co = rold.get_nbt(m.x, m.z)
            cn = rnew.get_nbt(m.x, m.z)
            go, gn = chunk_gids(co, reg), chunk_gids(cn, reg)
            per = overlay.get((cx, cz))
            if per is None:                      # untouched chunk
                for sy in set(go) | set(gn):
                    a = go.get(sy, np.zeros(4096, dtype=np.uint32))
                    c_ = gn.get(sy, np.zeros(4096, dtype=np.uint32))
                    untouched_bad += int((a != c_).sum())
                if "Heightmaps" not in cn:
                    flags_bad += 1
                continue
            exp = {sy: g.copy() for sy, g in go.items()}
            for sy, parts in per.items():
                e = exp.setdefault(sy, np.zeros(4096, dtype=np.uint32))
                for didx, gids in parts:
                    e[didx] = gids
                    placed_checked += didx.size
            for sy in set(exp) | set(gn):
                a = exp.get(sy, np.zeros(4096, dtype=np.uint32))
                c_ = gn.get(sy, np.zeros(4096, dtype=np.uint32))
                bad = int((a != c_).sum())
                # split: wrong at our positions vs damage elsewhere
                mask = np.zeros(4096, dtype=bool)
                for didx, _ in per.get(sy, []):
                    mask[didx] = True
                mismatched += int(((a != c_) & mask).sum())
                preserved_bad += bad - int(((a != c_) & mask).sum())
            if "Heightmaps" in cn or ("isLightOn" in cn
                                      and cn["isLightOn"].value != 0):
                flags_bad += 1
        rold.close()
        rnew.close()

    print(f"src blocks: {nblocks:,} | overlay positions checked: "
          f"{placed_checked:,} | in ungenerated chunks (skipped): "
          f"{skipped_expected:,}")
    ok1 = mismatched == 0 and placed_checked + skipped_expected >= nblocks
    ok2 = preserved_bad == 0
    ok3 = untouched_bad == 0
    ok4 = flags_bad == 0
    print(f"1 model blocks placed correctly : "
          f"{'PASS' if ok1 else f'FAIL ({mismatched} wrong)'}")
    print(f"2 terrain in touched chunks     : "
          f"{'PASS' if ok2 else f'FAIL ({preserved_bad} blocks damaged)'}")
    print(f"3 untouched chunks preserved    : "
          f"{'PASS' if ok3 else f'FAIL ({untouched_bad})'}")
    print(f"4 relight/heightmap flags       : "
          f"{'PASS' if ok4 else f'FAIL ({flags_bad} chunks)'}")
    if ok1 and ok2 and ok3 and ok4:
        print("ALL PASS")
        return 0
    print("FAILED")
    return 1


if __name__ == "__main__":
    sys.exit(main())
