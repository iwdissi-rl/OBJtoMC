#!/usr/bin/env python3
"""Session 51 — emissive-quad per-voxel emissive texture sampling oracle.

Drives the test_orchestrate_emissive_quad helper (PATH_TO_BINARY) against
tests/fixtures/texture/emissive.glb (FIXTURE_EMISSIVE_GLB) and quad.glb
(FIXTURE_QUAD_GLB), keeping the helper's first emissive world, then
verifies block-exactly with an independent parser (twoolie/NBT + raw zlib
chunk extraction — no anvil-parser, which cannot decode 1.21+ flat-root
chunks):

  emissive.glb is the 4x4 checker quad with emissiveFactor [1,1,1] and a
  2x1 emissiveTexture (left texel white, right black). With voxel_size=1,
  grid dims {4,1,4}, grid origin (0,0,0) and world_origin {0,-60,0}, all
  16 voxels fill at blocks (vx, -60, vz): chunk (0,0), section Y=-4,
  ly=4, lx=vx, lz=vz.

  The helper prints "EXPECT <vx> <vz> <block_name>" for all 16 voxels,
  computed via matcher::match on the exact per-voxel materials (checker
  quadrant kd; ke {1,1,1} for vx<2, {0,0,0} for vx>=2) — an independent
  prediction path that never touches the texture sampler.

  This oracle asserts:
    1. exactly 16 EXPECT lines;
    2. the block at each of the 16 positions matches its EXPECT name;
    3. every other block in the ly=4 plane (lx or lz > 3) is air;
    4. the ly=3 and ly=5 planes are entirely air;
    5. the left half (vx<2) collapses to ONE block name (the matcher's
       emissive branch ignores kd, so all 8 voxels share the white-
       emissive target) and that name never appears in the right half —
       the emissive mask observably splits the output;
    6. the right half (vx>=2) has >= 2 distinct names (blue|white
       checker quadrants still vary the opaque branch).
"""

import io
import os
import subprocess
import sys
import tempfile
import shutil
import zlib

import nbt.nbt  # type: ignore[import-untyped]

SECTOR_SIZE = 4096
QUAD_SECTION_Y = -4
QUAD_LY = 4  # world y=-60 → -60 - (-64)


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def extract_raw_chunk_nbt(region_path, chunk_x, chunk_z):
    with open(region_path, "rb") as f:
        data = f.read()
    entry_index = (chunk_x & 31) + (chunk_z & 31) * 32
    b_off = entry_index * 4
    sector_offset = int.from_bytes(data[b_off:b_off + 3], "big")
    sector_count = data[b_off + 3]
    if sector_offset == 0 and sector_count == 0:
        fail("chunk ({}, {}) is not populated".format(chunk_x, chunk_z))
    sb = sector_offset * SECTOR_SIZE
    declared_length = int.from_bytes(data[sb:sb + 4], "big")
    scheme = data[sb + 4]
    if scheme != 2:
        fail("chunk ({}, {}) uses compression scheme {}, expected 2 "
             "(zlib)".format(chunk_x, chunk_z, scheme))
    compressed = data[sb + 5:sb + 4 + declared_length]
    return zlib.decompress(compressed)


def unpack_lsb_first_nonstraddling(longs, bits, num_entries):
    entries_per_long = 64 // bits
    mask = (1 << bits) - 1
    out = []
    for i in range(num_entries):
        long_idx = i // entries_per_long
        slot = i % entries_per_long
        val_u64 = longs[long_idx] & 0xFFFFFFFFFFFFFFFF
        out.append((val_u64 >> (slot * bits)) & mask)
    return out


def section_block_names(section):
    block_states = section["block_states"]
    palette = [entry["Name"].value for entry in block_states["palette"]]
    if "data" not in block_states:
        if len(palette) != 1:
            fail("section without data has palette length {}".format(
                len(palette)))
        return palette
    bits = max(4, (len(palette) - 1).bit_length())
    data = list(block_states["data"])
    expected_longs = (4096 + (64 // bits) - 1) // (64 // bits)
    if len(data) != expected_longs:
        fail("section data has {} longs, expected {} (bits={}, palette "
             "len={})".format(len(data), expected_longs, bits, len(palette)))
    indices = unpack_lsb_first_nonstraddling(data, bits, 4096)
    for idx in indices:
        if idx >= len(palette):
            fail("palette index {} out of range (palette len {})".format(
                idx, len(palette)))
    return [palette[i] for i in indices]


def block_at(names, lx, ly, lz):
    if len(names) == 1:
        return names[0]
    return names[ly * 256 + lz * 16 + lx]


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))
    emissive_glb = os.environ.get("FIXTURE_EMISSIVE_GLB")
    if not emissive_glb or not os.path.isfile(emissive_glb):
        fail("FIXTURE_EMISSIVE_GLB={!r} is not a file".format(emissive_glb))
    quad_glb = os.environ.get("FIXTURE_QUAD_GLB")
    if not quad_glb or not os.path.isfile(quad_glb):
        fail("FIXTURE_QUAD_GLB={!r} is not a file".format(quad_glb))

    tmp_root = tempfile.mkdtemp(prefix="orch_emiquad_oracle_")
    try:
        world_dir = os.path.join(tmp_root, "world")  # must NOT pre-exist
        proc = subprocess.run(
            [binary, emissive_glb, quad_glb, world_dir],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail("helper exited {} ; stdout:\n{}\nstderr:\n{}".format(
                proc.returncode, proc.stdout, proc.stderr))

        expects = {}
        for line in proc.stdout.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[0] == "EXPECT":
                vx, vz, name = int(parts[1]), int(parts[2]), parts[3]
                expects[(vx, vz)] = name
        if len(expects) != 16:
            fail("parsed {} EXPECT lines, expected 16; stdout:\n{}".format(
                len(expects), proc.stdout))

        region_path = os.path.join(world_dir, "region", "r.0.0.mca")
        if not os.path.isfile(region_path):
            fail("helper did not write {}".format(region_path))

        raw = extract_raw_chunk_nbt(region_path, 0, 0)
        root = nbt.nbt.NBTFile(buffer=io.BytesIO(raw))

        target = None
        for section in root["sections"]:
            if section["Y"].value == QUAD_SECTION_Y:
                target = section
                break
        if target is None:
            fail("no section with Y={} found".format(QUAD_SECTION_Y))
        names = section_block_names(target)

        # 1+2: the 16 quad positions match the helper's EXPECT names.
        left_names = set()
        right_names = set()
        for (vx, vz), want in sorted(expects.items()):
            got = block_at(names, vx, QUAD_LY, vz)
            if got != want:
                fail("block at (lx={}, ly={}, lz={}) = {!r}, EXPECT said "
                     "{!r}".format(vx, QUAD_LY, vz, got, want))
            if want == "minecraft:air":
                fail("EXPECT for ({}, {}) is air — matcher should never "
                     "map a colour to air".format(vx, vz))
            (left_names if vx < 2 else right_names).add(want)

        # 3: rest of the ly=4 plane is air.
        for lz in range(16):
            for lx in range(16):
                if lx < 4 and lz < 4:
                    continue
                got = block_at(names, lx, QUAD_LY, lz)
                if got != "minecraft:air":
                    fail("block outside quad at (lx={}, ly=4, lz={}) = {!r}, "
                         "expected air".format(lx, lz, got))

        # 4: planes above and below are air.
        for ly in (QUAD_LY - 1, QUAD_LY + 1):
            for lz in range(16):
                for lx in range(16):
                    got = block_at(names, lx, ly, lz)
                    if got != "minecraft:air":
                        fail("block at (lx={}, ly={}, lz={}) = {!r}, expected "
                             "air".format(lx, ly, lz, got))

        # 5: emissive half is one block name, absent from the right half.
        if len(left_names) != 1:
            fail("left (emissive) half has {} distinct names {!r}, expected "
                 "exactly 1 (emissive branch ignores kd)".format(
                     len(left_names), sorted(left_names)))
        emissive_block = next(iter(left_names))
        if emissive_block in right_names:
            fail("emissive block {!r} also appears in the non-emissive "
                 "half — the mask is not splitting the output".format(
                     emissive_block))

        # 6: non-emissive half still varies with the checker.
        if len(right_names) < 2:
            fail("right (non-emissive) half collapsed to {!r} — kd "
                 "sampling regressed".format(sorted(right_names)))

        print("PASS: emissive-quad world block-exact against the helper's "
              "matcher-derived EXPECT lines")
        print("  16/16 positions match; emissive half = {}; non-emissive "
              "half = {}".format(emissive_block,
                                 ", ".join(sorted(right_names))))
        return 0
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
