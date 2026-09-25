#!/usr/bin/env python
"""world_sampler — extract a renderable voxel sample from a generated world.

Run under tests/oracle_venv (twoolie/NBT). Prints JSON:
  {"palette": [[name, r, g, b], ...],
   "chunks":  [{"cx":.., "cz":.., "v": [[x, y, z, palette_idx], ...]}, ...],
   "total": <full voxel count>, "sampled": <emitted count>}
Voxels are subsampled with a deterministic stride so `sampled` stays under
the cap (default 160k) — enough for an InstancedMesh on any modern GPU.
"""
import json
import struct
import sys
import zlib
from io import BytesIO
from pathlib import Path

from nbt import nbt

CAP = 160_000


def load_palette_csv(path):
    colours = {}
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split(",")
        colours[f[0]] = (int(f[3]), int(f[4]), int(f[5]))
    return colours


def main():
    world = Path(sys.argv[1])
    csv = sys.argv[2]
    colours = load_palette_csv(csv)

    # Pass 1: count voxels to pick the stride.
    entries = []          # (name, x, y, z) accumulated lazily via generator? memory: cap worlds are big
    total = 0

    def chunks_of(region_path):
        data = region_path.read_bytes()
        rx, rz = (int(p) for p in region_path.name.split(".")[1:3])
        for ci in range(1024):
            off = struct.unpack(">I", b"\0" + data[ci * 4:ci * 4 + 3])[0]
            if off == 0:
                continue
            base = off * 4096
            ln = struct.unpack(">I", data[base:base + 4])[0]
            yield nbt.NBTFile(buffer=BytesIO(
                zlib.decompress(data[base + 5:base + 4 + ln])))

    regions = sorted((world / "region").glob("*.mca"))

    # Single pass, collecting per-chunk voxel lists (name-indexed later).
    names = {}
    name_list = []
    chunk_out = []
    for rp in regions:
        for tag in chunks_of(rp):
            cx, cz = tag["xPos"].value, tag["zPos"].value
            vox = []
            for sec in tag["sections"]:
                bs = sec.get("block_states")
                if bs is None or "palette" not in bs:
                    continue
                pal = [p["Name"].value for p in bs["palette"]]
                if pal == ["minecraft:air"]:
                    continue
                y0 = sec["Y"].value * 16
                if "data" not in bs:
                    name = pal[0]
                    if name == "minecraft:air":
                        continue
                    ni = names.setdefault(name, len(names))
                    if ni == len(name_list):
                        name_list.append(name)
                    for i in range(4096):
                        y, r = divmod(i, 256)
                        z, x = divmod(r, 16)
                        vox.append((cx * 16 + x, y0 + y, cz * 16 + z, ni))
                    continue
                longs = [v & 0xFFFFFFFFFFFFFFFF for v in bs["data"].value]
                bits = max(4, (len(pal) - 1).bit_length())
                per = 64 // bits
                mask = (1 << bits) - 1
                for i in range(4096):
                    pi = (longs[i // per] >> ((i % per) * bits)) & mask
                    name = pal[pi]
                    if name == "minecraft:air":
                        continue
                    ni = names.setdefault(name, len(names))
                    if ni == len(name_list):
                        name_list.append(name)
                    y, r = divmod(i, 256)
                    z, x = divmod(r, 16)
                    vox.append((cx * 16 + x, y0 + y, cz * 16 + z, ni))
            if vox:
                total += len(vox)
                chunk_out.append({"cx": cx, "cz": cz, "vox": vox})

    stride = max(1, (total + CAP - 1) // CAP)
    sampled = 0
    out_chunks = []
    for c in chunk_out:
        v = c["vox"][::stride]
        sampled += len(v)
        out_chunks.append({"cx": c["cx"], "cz": c["cz"],
                           "v": [list(t) for t in v]})

    palette = [[n, *colours.get(n, (200, 60, 200))] for n in name_list]
    json.dump({"palette": palette, "chunks": out_chunks,
               "total": total, "sampled": sampled, "stride": stride},
              sys.stdout, separators=(",", ":"))


if __name__ == "__main__":
    main()
