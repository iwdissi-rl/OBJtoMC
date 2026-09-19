#!/usr/bin/env python3
"""Session 48 — textured-quad per-voxel texture sampling oracle.

Drives the test_orchestrate_textured_quad helper (PATH_TO_BINARY) against
the tests/fixtures/texture/quad.obj fixture (FIXTURE_OBJ), keeping the
helper's first textured world, then verifies block-exactly with an
independent parser (twoolie/NBT + raw zlib chunk extraction — no
anvil-parser, which cannot decode 1.21+ flat-root chunks):

  The fixture is a 4x4 quad in the XZ plane at y=0.5 with u=x/4, v=z/4 and
  a 2x2 checker map_Kd (top row green|white, bottom row red|blue). With
  voxel_size=1, grid dims {4,1,4}, grid origin (0,0,0) and world_origin
  {0,-60,0}, all 16 voxels fill at blocks (vx, -60, vz):

      chunk (0,0), section Y=-4, ly = -60 - (-64) = 4, lx=vx, lz=vz.

  The helper prints "EXPECT <vx> <vz> <block_name>" for all 16 voxels,
  computed via matcher::match on the four exact quadrant colours — an
  independent prediction path that never touches the texture sampler.

  This oracle asserts:
    1. exactly 16 EXPECT lines;
    2. the block at each of the 16 positions matches its EXPECT name;
    3. every other block in the ly=4 plane (lx or lz > 3) is air;
    4. the ly=3 and ly=5 planes are entirely air (quad occupies one
       voxel row only);
    5. at least 2 distinct non-air block names appear across the 16
       positions (the checker's quadrants must not collapse to a single
       block — texture sampling observably varies the output).
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
    """Returns a 4096-entry list of block names for one section, or a
    single-element list [name] if the section has palette-only encoding
    (no data field → uniform)."""
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
    fixture_obj = os.environ.get("FIXTURE_OBJ")
    if not fixture_obj:
        fail("FIXTURE_OBJ env var is not set")
    if not os.path.isfile(fixture_obj):
        fail("FIXTURE_OBJ={!r} does not exist".format(fixture_obj))

    tmp_root = tempfile.mkdtemp(prefix="orch_texquad_oracle_")
    try:
        world_dir = os.path.join(tmp_root, "world")  # must NOT pre-exist
        proc = subprocess.run(
            [binary, fixture_obj, world_dir],
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
        distinct = set()
        for (vx, vz), want in sorted(expects.items()):
            got = block_at(names, vx, QUAD_LY, vz)
            if got != want:
                fail("block at (lx={}, ly={}, lz={}) = {!r}, EXPECT said "
                     "{!r}".format(vx, QUAD_LY, vz, got, want))
            if want == "minecraft:air":
                fail("EXPECT for ({}, {}) is air — matcher should never "
                     "map a colour to air".format(vx, vz))
            distinct.add(want)

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

        # 5: quadrants must not collapse to one block.
        if len(distinct) < 2:
            fail("all 16 textured voxels matched the single block {!r} — "
                 "texture sampling is not varying the output".format(
                     next(iter(distinct))))

        print("PASS: textured-quad world block-exact against the helper's "
              "matcher-derived EXPECT lines")
        print("  16/16 positions match; {} distinct blocks across the "
              "quadrants: {}".format(
                  len(distinct), ", ".join(sorted(distinct))))
        return 0
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
