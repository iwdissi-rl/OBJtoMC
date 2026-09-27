#!/usr/bin/env python
"""world_merge — import an objtomc-generated world's blocks into an EXISTING
Minecraft save (e.g. drop a giant tree into a forest, a huge villager into a
village) without touching the surrounding terrain.

Run with the oracle venv python (needs twoolie-NBT + numpy):

  tests/oracle_venv/bin/python studio/world_merge.py SRC_WORLD DST_WORLD \
      --at X,auto,Z [--sink N] [--extend-height auto|off] [--dry-run]

Semantics
  * Only NON-AIR blocks of SRC are written; everything else in DST survives.
  * Anchor is the model's base-centre (centre of footprint, lowest block).
    --at places that anchor at X,Y,Z in DST. Y may be "auto": ground level is
    the median MOTION_BLOCKING_NO_LEAVES heightmap value of the anchor chunk.
  * --sink N buries the base N blocks below the detected/given Y.
  * Touched chunks get Heightmaps deleted and isLightOn=0 so the client
    recomputes both on load. Block entities under overwritten blocks are
    purged. Untouched chunks are byte-preserved.
  * Chunks the DST world has never generated are SKIPPED (counted); the fix
    is to pregenerate the area (e.g. with the Chunky mod) and re-import.
  * If the model tops out above the DST build limit, extend-height=auto
    installs the objtomc_height dimension datapack into DST (same mechanism
    the standalone pipeline uses) and enables it in level.dat.
  * Every region file (and level.dat) is backed up under
    DST/voxforge_backup_<ts>/ before its first modification.

Close the world in Minecraft before importing.
"""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

import numpy as np
from nbt import nbt
from nbt.region import RegionFile, InconceivedChunk

AIR_NAMES = {"minecraft:air", "minecraft:cave_air", "minecraft:void_air"}
VANILLA_MIN_Y = -64
VANILLA_HEIGHT = 384
PACK_FORMAT = 71          # MC 1.21.5 datapack format
MAX_TOP = 2032            # dimension_type: min_y + height <= 2032


def log(msg):
    print(msg, flush=True)


# ---------------------------------------------------------------- bit packing
def decode_indices(longs, npal):
    """block_states data longs -> 4096 palette indices (YZX order)."""
    bits = max(4, (npal - 1).bit_length())
    epl = 64 // bits
    arr = np.asarray(longs, dtype=np.int64).view(np.uint64)
    shifts = np.arange(epl, dtype=np.uint64) * np.uint64(bits)
    mask = np.uint64((1 << bits) - 1)
    out = ((arr[:, None] >> shifts[None, :]) & mask).reshape(-1)
    return out[:4096].astype(np.uint32)


def encode_indices(idx, npal):
    """4096 palette indices -> signed data longs (MC >=1.16 packing)."""
    bits = max(4, (npal - 1).bit_length())
    epl = 64 // bits
    nlongs = (4096 + epl - 1) // epl
    pad = np.zeros(nlongs * epl, dtype=np.uint64)
    pad[:4096] = idx.astype(np.uint64)
    shifts = np.arange(epl, dtype=np.uint64) * np.uint64(bits)
    longs = np.bitwise_or.reduce(pad.reshape(nlongs, epl) << shifts[None, :],
                                 axis=1)
    return longs.view(np.int64).tolist()


# ------------------------------------------------------------ palette registry
def pal_key(entry):
    props = ()
    if "Properties" in entry:
        props = tuple(sorted((k, str(entry["Properties"][k].value))
                             for k in entry["Properties"]))
    return (entry["Name"].value, props)


def make_pal_entry(key):
    name, props = key
    t = nbt.TAG_Compound()
    if props:
        pc = nbt.TAG_Compound()
        pc.name = "Properties"
        for k, v in props:
            pc.tags.append(nbt.TAG_String(name=k, value=v))
        t.tags.append(pc)
    t.tags.append(nbt.TAG_String(name="Name", value=name))
    return t


class Registry:
    """Global block-id space shared between SRC read and DST write."""
    def __init__(self):
        self.key2gid = {("minecraft:air", ()): 0}
        self.keys = [("minecraft:air", ())]

    def gid(self, key):
        g = self.key2gid.get(key)
        if g is None:
            g = len(self.keys)
            self.key2gid[key] = g
            self.keys.append(key)
        return g


# ---------------------------------------------------------------- world reads
def iter_region_files(world):
    rdir = Path(world) / "region"
    for p in sorted(rdir.glob("r.*.mca")):
        _, rx, rz, _ = p.name.split(".")
        yield p, int(rx), int(rz)


def load_source(world, reg):
    """Read every non-air block of SRC. Returns dict
    (cx,cz,sy) -> uint32[4096] of gids (0 = air), plus non-air AABB."""
    sections = {}
    lo = [1 << 30] * 3
    hi = [-(1 << 30)] * 3
    pos = np.arange(4096, dtype=np.int64)
    ly, lz, lx = pos >> 8, (pos >> 4) & 15, pos & 15
    nblocks = 0
    for path, rx, rz in iter_region_files(world):
        rf = RegionFile(str(path))
        for m in rf.get_metadata():
            c = rf.get_nbt(m.x, m.z)
            cx, cz = rx * 32 + m.x, rz * 32 + m.z
            for s in c["sections"]:
                if "block_states" not in s:
                    continue
                bs = s["block_states"]
                pal = bs["palette"]
                gids_pal = np.array(
                    [0 if pal_key(p)[0] in AIR_NAMES else reg.gid(pal_key(p))
                     for p in pal], dtype=np.uint32)
                if "data" in bs and len(bs["data"]) > 0:
                    idx = decode_indices(bs["data"].value, len(pal))
                    sect = gids_pal[idx]
                else:
                    sect = np.full(4096, gids_pal[0], dtype=np.uint32)
                nz = np.nonzero(sect)[0]
                if nz.size == 0:
                    continue
                sy = s["Y"].value
                sections[(cx, cz, sy)] = sect
                nblocks += nz.size
                gx = cx * 16 + lx[nz]
                gy = sy * 16 + ly[nz]
                gz = cz * 16 + lz[nz]
                lo = [min(lo[0], int(gx.min())), min(lo[1], int(gy.min())),
                      min(lo[2], int(gz.min()))]
                hi = [max(hi[0], int(gx.max())), max(hi[1], int(gy.max())),
                      max(hi[2], int(gz.max()))]
        rf.close()
    if nblocks == 0:
        raise SystemExit("source world has no blocks")
    return sections, lo, hi, nblocks


# ----------------------------------------------------------- dst world height
def dst_world_height(world):
    """(min_y, height) of DST's overworld, honouring an objtomc_height pack."""
    dj = (Path(world) / "datapacks" / "objtomc_height" / "data" / "minecraft"
          / "dimension_type" / "overworld.json")
    if dj.exists():
        d = json.loads(dj.read_text())
        return int(d["min_y"]), int(d["height"])
    return VANILLA_MIN_Y, VANILLA_HEIGHT


def detect_ground(world, tx, tz, min_y, height):
    """Median MOTION_BLOCKING_NO_LEAVES surface Y of the chunk holding
    (tx,tz); returns first-air Y, or None if the chunk isn't generated."""
    cx, cz = tx >> 4, tz >> 4
    path = Path(world) / "region" / f"r.{cx >> 5}.{cz >> 5}.mca"
    if not path.exists():
        return None
    rf = RegionFile(str(path))
    try:
        c = rf.get_nbt(cx & 31, cz & 31)
    except InconceivedChunk:
        rf.close()
        return None
    rf.close()
    hm = c.get("Heightmaps")
    for key in ("MOTION_BLOCKING_NO_LEAVES", "MOTION_BLOCKING",
                "WORLD_SURFACE"):
        if hm is not None and key in hm and len(hm[key]) > 0:
            # The stored bit width follows the world height AT WRITE TIME,
            # which can predate a height extension — infer it from the long
            # count instead (prefer the current-height width if it fits).
            nlongs = len(hm[key])
            cand = [b for b in range(4, 16)
                    if -(-256 // (64 // b)) == nlongs]
            if not cand:
                continue
            want = (height + 1).bit_length()
            bits = want if want in cand else max(cand)
            vals = decode_indices(hm[key].value, 1 << bits)[:256]
            vals = vals[vals > 0]
            if vals.size:
                return min_y + int(np.median(vals))
    # fallback: scan the anchor column top-down
    lxz = ((tz & 15) * 16) + (tx & 15)
    best = None
    for s in c["sections"]:
        if "block_states" not in s:
            continue
        bs = s["block_states"]
        pal = bs["palette"]
        if "data" in bs and len(bs["data"]) > 0:
            idx = decode_indices(bs["data"].value, len(pal))
        else:
            idx = np.zeros(4096, dtype=np.uint32)
        solid = np.array([pal_key(p)[0] not in AIR_NAMES for p in pal])
        col = idx[lxz::256]                      # the 16 Ys of this column
        occ = np.nonzero(solid[col])[0]
        if occ.size:
            y = s["Y"].value * 16 + int(occ.max()) + 1
            best = y if best is None else max(best, y)
    return best


# ------------------------------------------------------------- height extend
def extend_dst_height(world, need_top, min_y):
    height = ((need_top - min_y + 16 + 15) // 16) * 16
    height = min(height, MAX_TOP - min_y)
    if min_y + height <= need_top:
        raise SystemExit(f"model top y={need_top} exceeds the datapack "
                         f"ceiling ({MAX_TOP - 1}); use a smaller size")
    dp = Path(world) / "datapacks" / "objtomc_height"
    dj = dp / "data" / "minecraft" / "dimension_type"
    dj.mkdir(parents=True, exist_ok=True)
    (dp / "pack.mcmeta").write_text(json.dumps({"pack": {
        "pack_format": PACK_FORMAT,
        "description": f"objtomc extended world height (min_y {min_y}, "
                       f"height {height}). Removing this datapack corrupts "
                       f"the world."}}, indent=2) + "\n")
    (dj / "overworld.json").write_text(json.dumps({
        "ambient_light": 0, "bed_works": True, "coordinate_scale": 1,
        "effects": "minecraft:overworld", "has_ceiling": False,
        "has_raids": True, "has_skylight": True, "height": height,
        "infiniburn": "#minecraft:infiniburn_overworld",
        "logical_height": height, "min_y": min_y,
        "monster_spawn_block_light_limit": 0,
        "monster_spawn_light_level": {"type": "minecraft:uniform",
                                      "max_inclusive": 7, "min_inclusive": 0},
        "natural": True, "piglin_safe": False,
        "respawn_anchor_works": False, "ultrawarm": False},
        indent=2, sort_keys=True) + "\n")
    lvl = nbt.NBTFile(str(Path(world) / "level.dat"))
    enabled = lvl["Data"]["DataPacks"]["Enabled"]
    names = [str(t) for t in enabled]
    if "file/objtomc_height" not in names:
        pos = names.index("vanilla") + 1 if "vanilla" in names else len(names)
        enabled.tags.insert(pos, nbt.TAG_String(value="file/objtomc_height"))
        lvl.write_file(str(Path(world) / "level.dat"))
    return height


# ------------------------------------------------------------------ dst write
def blank_section(sy, template_biomes):
    s = nbt.TAG_Compound()
    s.tags.append(nbt.TAG_Byte(name="Y", value=sy))
    bs = nbt.TAG_Compound()
    bs.name = "block_states"
    pl = nbt.TAG_List(name="palette", type=nbt.TAG_Compound)
    pl.tags.append(make_pal_entry(("minecraft:air", ())))
    bs.tags.append(pl)
    s.tags.append(bs)
    bio = nbt.TAG_Compound()
    bio.name = "biomes"
    bpl = nbt.TAG_List(name="palette", type=nbt.TAG_String)
    bpl.tags.append(nbt.TAG_String(value=template_biomes))
    bio.tags.append(bpl)
    s.tags.append(bio)
    return s


def merge_into_chunk(c, per_sect, reg):
    """Overlay {sy: (didx, gids)} into chunk NBT `c`. Returns blocks placed
    and #block-entities purged."""
    secs = {s["Y"].value: s for s in c["sections"]}
    placed_mask = {}                       # sy -> bool[4096] of our writes
    placed = 0
    template_biome = "minecraft:plains"
    for s in c["sections"]:
        if "biomes" in s and len(s["biomes"]["palette"]) > 0:
            template_biome = s["biomes"]["palette"][0].value
            break
    for sy, (didx, gids) in sorted(per_sect.items()):
        s = secs.get(sy)
        if s is None:
            s = blank_section(sy, template_biome)
            c["sections"].tags.append(s)
            c["sections"].tags.sort(key=lambda t: t["Y"].value)
            secs[sy] = s
        if "block_states" not in s:
            bs = nbt.TAG_Compound()
            bs.name = "block_states"
            pl = nbt.TAG_List(name="palette", type=nbt.TAG_Compound)
            pl.tags.append(make_pal_entry(("minecraft:air", ())))
            bs.tags.append(pl)
            s.tags.append(bs)
        bs = s["block_states"]
        pal = bs["palette"]
        pal_keys = [pal_key(p) for p in pal]
        key2local = {k: i for i, k in enumerate(pal_keys)}
        if "data" in bs and len(bs["data"]) > 0:
            idx = decode_indices(bs["data"].value, len(pal))
        else:
            idx = np.zeros(4096, dtype=np.uint32)
        # extend the local palette with any gid we are about to place
        gid2local = {}
        for g in np.unique(gids):
            key = reg.keys[int(g)]
            if key not in key2local:
                key2local[key] = len(pal_keys)
                pal_keys.append(key)
                pal.tags.append(make_pal_entry(key))
            gid2local[int(g)] = key2local[key]
        lut = np.zeros(max(gid2local) + 1, dtype=np.uint32)
        for g, l in gid2local.items():
            lut[g] = l
        idx[didx] = lut[gids]
        placed += didx.size
        m = placed_mask.setdefault(sy, np.zeros(4096, dtype=bool))
        m[didx] = True
        # re-encode
        if "data" in bs:
            bs.tags.remove(bs["data"])
        if len(pal) > 1:
            da = nbt.TAG_Long_Array(name="data")
            da.value = encode_indices(idx, len(pal))
            bs.tags.append(da)
    # purge block entities we buried
    purged = 0
    if "block_entities" in c:
        bel = c["block_entities"]
        keep = []
        for be in bel.tags:
            x, y, z = be["x"].value, be["y"].value, be["z"].value
            m = placed_mask.get(y >> 4)
            if m is not None and m[(y & 15) * 256 + (z & 15) * 16 + (x & 15)]:
                purged += 1
            else:
                keep.append(be)
        if purged:
            bel.tags = keep
    # force light + heightmap recompute in the client
    if "Heightmaps" in c:
        c.tags.remove(c["Heightmaps"])
    if "isLightOn" in c:
        c["isLightOn"].value = 0
    return placed, purged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--at", required=True,
                    help="X,Y,Z target for the model's base-centre; Y may be "
                         "'auto' (terrain surface at the anchor chunk)")
    ap.add_argument("--sink", type=int, default=0,
                    help="bury the base N blocks (default 0)")
    ap.add_argument("--extend-height", choices=["auto", "off"],
                    default="auto")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    src, dst = Path(a.src), Path(a.dst)
    if not (src / "level.dat").exists():
        raise SystemExit(f"not a world: {src}")
    if not (dst / "level.dat").exists():
        raise SystemExit(f"not a world: {dst}")

    xs, ys, zs = [t.strip() for t in a.at.split(",")]
    tx, tz = int(xs), int(zs)

    reg = Registry()
    log(f"[merge] reading source blocks from {src.name} ...")
    sections, lo, hi, nblocks = load_source(src, reg)
    log(f"[merge] source: {nblocks:,} blocks, AABB "
        f"{hi[0]-lo[0]+1}x{hi[1]-lo[1]+1}x{hi[2]-lo[2]+1}, "
        f"{len(reg.keys)-1} block types")

    min_y, height = dst_world_height(dst)
    log(f"[merge] destination height range: y {min_y}..{min_y+height-1}")

    if ys == "auto":
        ty = detect_ground(dst, tx, tz, min_y, height)
        if ty is None:
            raise SystemExit(f"anchor chunk at x={tx} z={tz} is not "
                             f"generated in {dst.name}; pick generated "
                             f"coords or pregenerate (Chunky)")
        log(f"[merge] ground detected at y={ty} (median surface of anchor "
            f"chunk)")
    else:
        ty = int(ys)
    ty -= a.sink

    ax = (lo[0] + hi[0]) // 2
    az = (lo[2] + hi[2]) // 2
    ox, oy, oz = tx - ax, ty - lo[1], tz - az
    top = hi[1] + oy
    log(f"[merge] placing base-centre at ({tx},{ty},{tz}) -> world span "
        f"x {lo[0]+ox}..{hi[0]+ox}, y {ty}..{top}, z {lo[2]+oz}..{hi[2]+oz}")

    if top >= min_y + height:
        if a.extend_height == "auto":
            height = extend_dst_height(dst, top, min_y)
            log(f"[merge] extended {dst.name} world height to {height} "
                f"(datapack objtomc_height installed + enabled)")
        else:
            raise SystemExit(f"model top y={top} exceeds build limit "
                             f"{min_y+height-1}; rerun with --extend-height "
                             f"auto or a smaller size")
    below = 0

    # ---- group source blocks into destination sections
    log("[merge] regrouping into destination chunks ...")
    pos = np.arange(4096, dtype=np.int64)
    ly, lz, lx = pos >> 8, (pos >> 4) & 15, pos & 15
    buckets = {}                                     # (dcx,dcz,sy)->[(didx,g)]
    for (cx, cz, sy), sect in sections.items():
        nz = np.nonzero(sect)[0]
        gx = cx * 16 + lx[nz] + ox
        gy = sy * 16 + ly[nz] + oy
        gz = cz * 16 + lz[nz] + oz
        ok = gy >= min_y
        below += int(nz.size - ok.sum())
        if not ok.all():
            nz, gx, gy, gz = nz[ok], gx[ok], gy[ok], gz[ok]
        if nz.size == 0:
            continue
        g = sect[nz]
        dk = np.stack([gx >> 4, gz >> 4, gy >> 4], axis=1)
        didx = (gy & 15) * 256 + (gz & 15) * 16 + (gx & 15)
        uniq, inv = np.unique(dk, axis=0, return_inverse=True)
        for i, (dcx, dcz, dsy) in enumerate(uniq):
            sel = inv == i
            buckets.setdefault((int(dcx), int(dcz), int(dsy)),
                               []).append((didx[sel], g[sel]))
    if below:
        log(f"[merge] warning: {below:,} blocks below the world floor were "
            f"cropped")

    chunks = {}
    for (dcx, dcz, dsy), parts in buckets.items():
        didx = np.concatenate([p[0] for p in parts])
        gids = np.concatenate([p[1] for p in parts])
        chunks.setdefault((dcx, dcz), {})[dsy] = (didx, gids)
    log(f"[merge] {len(chunks)} destination chunks to modify")
    if a.dry_run:
        log(":: DRY RUN — no files written")
        return

    # ---- apply, region by region, with backups
    bdir = dst / f"voxforge_backup_{time.strftime('%Y%m%d_%H%M%S')}"
    by_region = {}
    for (dcx, dcz), per in chunks.items():
        by_region.setdefault((dcx >> 5, dcz >> 5), {})[(dcx, dcz)] = per

    placed = purged = skipped_chunks = skipped_blocks = done = 0
    total = len(chunks)
    for (rx, rz), cmap in sorted(by_region.items()):
        rpath = dst / "region" / f"r.{rx}.{rz}.mca"
        if not rpath.exists():
            skipped_chunks += len(cmap)
            skipped_blocks += sum(d.size for per in cmap.values()
                                  for d, _ in per.values())
            done += len(cmap)
            log(f"[merge] region r.{rx}.{rz} not generated — skipped "
                f"{len(cmap)} chunks")
            continue
        bdir.mkdir(exist_ok=True)
        shutil.copy2(rpath, bdir / rpath.name)
        rf = RegionFile(str(rpath))
        for (dcx, dcz), per in sorted(cmap.items()):
            try:
                c = rf.get_nbt(dcx & 31, dcz & 31)
            except InconceivedChunk:
                skipped_chunks += 1
                skipped_blocks += sum(d.size for d, _ in per.values())
                done += 1
                continue
            p, q = merge_into_chunk(c, per, reg)
            rf.write_chunk(dcx & 31, dcz & 31, c)
            placed += p
            purged += q
            done += 1
            if done % 64 == 0 or done == total:
                log(f"[merge] chunk {done}/{total} — {placed:,} blocks "
                    f"placed")
        rf.close()

    if bdir.exists():
        shutil.copy2(dst / "level.dat", bdir / "level.dat")
        log(f"[merge] backup of touched regions + level.dat: {bdir.name}/ "
            f"(copy back to undo)")
    msg = (f":: IMPORT COMPLETE — {placed:,} blocks into "
           f"{total - skipped_chunks} chunks of {dst.name} at "
           f"({tx},{ty},{tz})")
    if skipped_chunks:
        msg += (f" | {skipped_chunks} ungenerated chunks skipped "
                f"({skipped_blocks:,} blocks) — pregenerate with Chunky and "
                f"re-import")
    if purged:
        msg += f" | {purged} buried block entities removed"
    log(msg)


if __name__ == "__main__":
    main()
