#!/usr/bin/env python3
"""Session 15b — single-stone chunk byte-pattern oracle.

Pins the §3.12.b contract from research/chunk_schema_proposal.md:

  Provider returns stone at (lx=8, wy=-61, lz=8); air everywhere else.

  Section list_index 0 (Y = -4 → world Y -64..-49) must contain the
  stone block at (lx=8, ly=3, lz=8). Within that section:

    - palette has exactly 2 entries:
        palette[0] = {Name: "minecraft:air"}    (no Properties)
        palette[1] = {Name: "minecraft:stone"}  (no Properties)
      Insertion order: air enters first because the section-major scan
      visits (lx=0, ly=0, lz=0) → world Y=-64 → air before reaching the
      stone position.

    - data is TAG_Long_Array of length 256 (bits_per_block = 4,
      4 × 4096 / 64 = 256 longs).

    - Block index for the stone: i = ly*256 + lz*16 + lx
                                   = 3*256 + 8*16 + 8 = 904
      Long index: 904 // 16 = 56
      Slot in long: 904 % 16 = 8
      Bit shift: slot * bits = 8 * 4 = 32
      Stone palette index: 1
      So data[56] = 1 << 32 = 0x0000_0001_0000_0000 = 4 294 967 296.
      Every other long is 0 (air → palette index 0 → contributes 0 bits).

  Sections list_index 1..23 (Y = -3..19): all-air pattern, palette =
  [{Name: "minecraft:air"}] and NO data field — matches §3.12.a.

  Heightmaps remain all-zero in this slice; 15c lands the real
  per-column predicate scan.

The other top-level keys (DataVersion, xPos, yPos, zPos, Status,
LastUpdate, InhabitedTime, isLightOn, block_entities, fluid_ticks,
block_ticks, PostProcessing, structures) are unchanged from §3.12.a
and re-asserted here for safety.

Parser: twoolie/NBT (NBT==1.5.1) directly — anvil-parser==0.9.0 cannot
decode 1.21+ flat-root chunks (proposal §3.12.c).
"""

import os
import subprocess
import sys
import tempfile

import nbt.nbt  # type: ignore[import-untyped]
from nbt.nbt import (
    TAG_BYTE,
    TAG_COMPOUND,
    TAG_END,
    TAG_INT,
    TAG_LIST,
    TAG_LONG,
    TAG_LONG_ARRAY,
    TAG_STRING,
)


EXPECTED_SECTIONS_PER_CHUNK = 24
EXPECTED_HEIGHTMAP_LONGS = 37
EXPECTED_HEIGHTMAP_KEYS = {
    "MOTION_BLOCKING",
    "MOTION_BLOCKING_NO_LEAVES",
    "OCEAN_FLOOR",
    "WORLD_SURFACE",
}
EXPECTED_BIOME = "minecraft:plains"

STONE_LX, STONE_WY, STONE_LZ = 8, -61, 8
STONE_SECTION_LIST_INDEX = 0  # world Y = -61 → section Y = -4 → list_index 0
STONE_LY = STONE_WY - (-4 * 16)  # -61 - (-64) = 3
STONE_BLOCK_INDEX = STONE_LY * 256 + STONE_LZ * 16 + STONE_LX  # 904
STONE_LONG_IDX = STONE_BLOCK_INDEX // 16  # 56
STONE_SLOT = STONE_BLOCK_INDEX % 16       # 8
STONE_BITS = 4
STONE_EXPECTED_LONG = 1 << (STONE_SLOT * STONE_BITS)  # 0x0000_0001_0000_0000


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def check_id(tag, expected_id, label):
    if tag.id != expected_id:
        fail("{}: tag id is {}, expected {}".format(label, tag.id, expected_id))


def check_compound_field(parent, key, expected_id, label):
    if key not in parent:
        fail("{}: missing key {!r}".format(label, key))
    check_id(parent[key], expected_id, "{}.{}".format(label, key))
    return parent[key]


def assert_no_key(parent, key, label):
    if key in parent:
        fail("{}: unexpected key {!r} present".format(label, key))


def assert_air_palette_entry(entry, label):
    check_id(entry, TAG_COMPOUND, label)
    name_tag = check_compound_field(entry, "Name", TAG_STRING, label)
    if name_tag.value != "minecraft:air":
        fail("{}.Name = {!r}, expected 'minecraft:air'".format(
            label, name_tag.value))
    assert_no_key(entry, "Properties", label)
    if len(entry.tags) != 1:
        fail("{} has {} keys, expected only 'Name'".format(
            label, len(entry.tags)))


def assert_all_air_section(section, list_index):
    label = "sections[{}]".format(list_index)
    check_id(section, TAG_COMPOUND, label)

    y_tag = check_compound_field(section, "Y", TAG_BYTE, label)
    expected_y = list_index - 4
    if y_tag.value != expected_y:
        fail("{}.Y = {}, expected {}".format(label, y_tag.value, expected_y))

    block_states = check_compound_field(
        section, "block_states", TAG_COMPOUND, label)
    assert_no_key(block_states, "data", label + ".block_states")
    bs_palette = check_compound_field(
        block_states, "palette", TAG_LIST, label + ".block_states")
    if bs_palette.tagID != TAG_COMPOUND:
        fail("{}.block_states.palette element type = {}, expected "
             "TAG_COMPOUND={}".format(label, bs_palette.tagID, TAG_COMPOUND))
    if len(bs_palette) != 1:
        fail("{}.block_states.palette length = {}, expected 1".format(
            label, len(bs_palette)))
    assert_air_palette_entry(
        bs_palette[0], label + ".block_states.palette[0]")

    biomes = check_compound_field(section, "biomes", TAG_COMPOUND, label)
    assert_no_key(biomes, "data", label + ".biomes")
    b_palette = check_compound_field(
        biomes, "palette", TAG_LIST, label + ".biomes")
    if b_palette.tagID != TAG_STRING:
        fail("{}.biomes.palette element type = {}, expected "
             "TAG_STRING={}".format(label, b_palette.tagID, TAG_STRING))
    if len(b_palette) != 1:
        fail("{}.biomes.palette length = {}, expected 1".format(
            label, len(b_palette)))
    if b_palette[0].value != EXPECTED_BIOME:
        fail("{}.biomes.palette[0] = {!r}, expected {!r}".format(
            label, b_palette[0].value, EXPECTED_BIOME))


def assert_stone_section(section):
    label = "sections[0]"  # the stone is in section Y=-4 = list_index 0
    check_id(section, TAG_COMPOUND, label)

    y_tag = check_compound_field(section, "Y", TAG_BYTE, label)
    if y_tag.value != -4:
        fail("{}.Y = {}, expected -4".format(label, y_tag.value))

    block_states = check_compound_field(
        section, "block_states", TAG_COMPOUND, label)

    # Palette: [air, stone], no Properties on either.
    palette = check_compound_field(
        block_states, "palette", TAG_LIST, label + ".block_states")
    if palette.tagID != TAG_COMPOUND:
        fail("{}.block_states.palette element type = {}, expected "
             "TAG_COMPOUND={}".format(label, palette.tagID, TAG_COMPOUND))
    if len(palette) != 2:
        fail("{}.block_states.palette length = {}, expected 2".format(
            label, len(palette)))
    assert_air_palette_entry(palette[0], label + ".block_states.palette[0]")
    stone_entry = palette[1]
    check_id(stone_entry, TAG_COMPOUND,
             label + ".block_states.palette[1]")
    stone_name = check_compound_field(
        stone_entry, "Name", TAG_STRING,
        label + ".block_states.palette[1]")
    if stone_name.value != "minecraft:stone":
        fail("{}.block_states.palette[1].Name = {!r}, expected "
             "'minecraft:stone'".format(label, stone_name.value))
    assert_no_key(stone_entry, "Properties",
                  label + ".block_states.palette[1]")
    if len(stone_entry.tags) != 1:
        fail("{}.block_states.palette[1] has {} keys, expected only "
             "'Name'".format(label, len(stone_entry.tags)))

    # data: present, TAG_Long_Array of length 256, all zeros except data[56].
    data = check_compound_field(
        block_states, "data", TAG_LONG_ARRAY, label + ".block_states")
    if len(data) != 256:
        fail("{}.block_states.data length = {}, expected 256".format(
            label, len(data)))
    for i, v in enumerate(data):
        expected = STONE_EXPECTED_LONG if i == STONE_LONG_IDX else 0
        if v != expected:
            fail(
                "{}.block_states.data[{}] = 0x{:016x} ({}), "
                "expected 0x{:016x} ({})".format(
                    label, i, v & 0xFFFFFFFFFFFFFFFF, v,
                    expected & 0xFFFFFFFFFFFFFFFF, expected))

    biomes = check_compound_field(section, "biomes", TAG_COMPOUND, label)
    assert_no_key(biomes, "data", label + ".biomes")
    b_palette = check_compound_field(
        biomes, "palette", TAG_LIST, label + ".biomes")
    if b_palette.tagID != TAG_STRING:
        fail("{}.biomes.palette element type = {}, expected "
             "TAG_STRING={}".format(label, b_palette.tagID, TAG_STRING))
    if len(b_palette) != 1:
        fail("{}.biomes.palette length = {}, expected 1".format(
            label, len(b_palette)))
    if b_palette[0].value != EXPECTED_BIOME:
        fail("{}.biomes.palette[0] = {!r}, expected {!r}".format(
            label, b_palette[0].value, EXPECTED_BIOME))


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))

    out_path = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="chunk_single_stone_", suffix=".nbt", delete=False
        ) as fh:
            out_path = fh.name

        proc = subprocess.run(
            [binary, "--out", out_path],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail(
                "writer binary exited {} ; stdout:\n{}\nstderr:\n{}".format(
                    proc.returncode, proc.stdout, proc.stderr
                )
            )
        if os.path.getsize(out_path) == 0:
            fail("writer binary produced empty output")

        with open(out_path, "rb") as fh:
            root = nbt.nbt.NBTFile(buffer=fh)

        # ----- Root scalars (re-assert from §3.12.a contract for safety) ----
        check_id(root, TAG_COMPOUND, "root")
        if root.name != "":
            fail("root name is {!r}, expected '' (unnamed)".format(root.name))

        data_version = check_compound_field(root, "DataVersion", TAG_INT, "root")
        if data_version.value != 4325:
            fail("DataVersion = {}, expected 4325".format(data_version.value))

        status = check_compound_field(root, "Status", TAG_STRING, "root")
        if status.value != "minecraft:full":
            fail("Status = {!r}, expected 'minecraft:full'".format(status.value))

        for key, expected in (("xPos", 0), ("yPos", -4), ("zPos", 0)):
            tag = check_compound_field(root, key, TAG_INT, "root")
            if tag.value != expected:
                fail("{} = {}, expected {}".format(key, tag.value, expected))

        for key in ("LastUpdate", "InhabitedTime"):
            tag = check_compound_field(root, key, TAG_LONG, "root")
            if tag.value != 0:
                fail("{} = {}, expected 0".format(key, tag.value))

        is_light_on = check_compound_field(root, "isLightOn", TAG_BYTE, "root")
        if is_light_on.value != 0:
            fail("isLightOn = {}, expected 0".format(is_light_on.value))

        for key in ("block_entities", "fluid_ticks", "block_ticks", "PostProcessing"):
            lst = check_compound_field(root, key, TAG_LIST, "root")
            if len(lst) != 0:
                fail("{} has {} entries, expected 0".format(key, len(lst)))
            if lst.tagID != TAG_END:
                fail("{} empty-list element type = {}, expected "
                     "TAG_END={}".format(key, lst.tagID, TAG_END))

        structures = check_compound_field(root, "structures", TAG_COMPOUND, "root")
        if len(structures.tags) != 0:
            fail("structures has {} entries, expected 0".format(
                len(structures.tags)))

        # ----- Heightmaps: still all-zero in 15b (15c lands real values) -----
        heightmaps = check_compound_field(root, "Heightmaps", TAG_COMPOUND, "root")
        keys = {entry.name for entry in heightmaps.tags}
        if keys != EXPECTED_HEIGHTMAP_KEYS:
            fail("Heightmaps keys = {!r}, expected {!r}".format(
                sorted(keys), sorted(EXPECTED_HEIGHTMAP_KEYS)))
        for hm_name in EXPECTED_HEIGHTMAP_KEYS:
            hm = check_compound_field(
                heightmaps, hm_name, TAG_LONG_ARRAY, "Heightmaps")
            if len(hm) != EXPECTED_HEIGHTMAP_LONGS:
                fail("Heightmaps.{} has {} longs, expected {}".format(
                    hm_name, len(hm), EXPECTED_HEIGHTMAP_LONGS))
            for i, v in enumerate(hm):
                if v != 0:
                    fail("Heightmaps.{}[{}] = {}, expected 0 (15b still "
                         "emits zero heightmaps; 15c lands real scan)".format(
                             hm_name, i, v))

        # ----- Sections -----
        sections = check_compound_field(root, "sections", TAG_LIST, "root")
        if sections.tagID != TAG_COMPOUND:
            fail("sections element type = {}, expected TAG_COMPOUND={}".format(
                sections.tagID, TAG_COMPOUND))
        if len(sections) != EXPECTED_SECTIONS_PER_CHUNK:
            fail("len(sections) = {}, expected {}".format(
                len(sections), EXPECTED_SECTIONS_PER_CHUNK))

        # Section 0 (Y=-4) is the stone-bearing section. The remaining 23
        # are all-air.
        assert_stone_section(sections[STONE_SECTION_LIST_INDEX])
        for list_index in range(EXPECTED_SECTIONS_PER_CHUNK):
            if list_index == STONE_SECTION_LIST_INDEX:
                continue
            assert_all_air_section(sections[list_index], list_index)

        print("PASS: single-stone chunk byte-pattern matches §3.12.b contract")
        print("  output {} bytes; stone at (lx={}, wy={}, lz={}) → block_index "
              "{}, long_idx {}, slot {}, bits {} → "
              "data[{}] = 0x{:016x} (4 bytes verified zero elsewhere across "
              "256 longs in section 0; remaining 23 sections all-air)".format(
                  os.path.getsize(out_path), STONE_LX, STONE_WY, STONE_LZ,
                  STONE_BLOCK_INDEX, STONE_LONG_IDX, STONE_SLOT, STONE_BITS,
                  STONE_LONG_IDX, STONE_EXPECTED_LONG))
        return 0
    finally:
        if out_path is not None and os.path.exists(out_path):
            os.unlink(out_path)


if __name__ == "__main__":
    sys.exit(main())
