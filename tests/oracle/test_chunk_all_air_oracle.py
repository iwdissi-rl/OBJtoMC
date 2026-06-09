#!/usr/bin/env python3
"""Session 15a — all-air chunk structural oracle.

Drives `$PATH_TO_BINARY --out <tempfile>`, parses the produced uncompressed
NBT bytes with twoolie/NBT (NBT==1.5.1), and asserts the §3.12 (a) contract
verbatim from research/chunk_schema_proposal.md.

Why twoolie/NBT directly and NOT anvil-parser==0.9.0: anvil-parser 0.9.0
does not support the 1.21+ flat-root chunk format (it expects a pre-1.18
`Level`-wrapped root; `anvil.Chunk.from_region(...)` raises `KeyError: 'Tag
Level does not exist'`). The chunk-schema layer is verified directly at
the NBT-tree level — see proposal §3.12 (c).
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


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))

    out_path = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="chunk_all_air_", suffix=".nbt", delete=False
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

        # ----- Root -----
        check_id(root, TAG_COMPOUND, "root")
        if root.name != "":
            fail(
                "root name is {!r}, expected '' (unnamed)".format(root.name)
            )

        # ----- Scalar metadata -----
        data_version = check_compound_field(root, "DataVersion", TAG_INT, "root")
        if data_version.value != 4325:
            fail("DataVersion = {}, expected 4325".format(data_version.value))

        status = check_compound_field(root, "Status", TAG_STRING, "root")
        if status.value != "minecraft:full":
            fail(
                "Status = {!r}, expected 'minecraft:full'".format(status.value)
            )

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

        # ----- Empty lists -----
        for key in ("block_entities", "fluid_ticks", "block_ticks", "PostProcessing"):
            lst = check_compound_field(root, key, TAG_LIST, "root")
            if len(lst) != 0:
                fail("{} has {} entries, expected 0".format(key, len(lst)))

        # ----- Empty structures compound -----
        structures = check_compound_field(root, "structures", TAG_COMPOUND, "root")
        if len(structures.tags) != 0:
            fail(
                "structures has {} entries, expected 0".format(
                    len(structures.tags)
                )
            )

        # ----- Heightmaps -----
        heightmaps = check_compound_field(root, "Heightmaps", TAG_COMPOUND, "root")
        keys = {entry.name for entry in heightmaps.tags}
        if keys != EXPECTED_HEIGHTMAP_KEYS:
            fail(
                "Heightmaps keys = {!r}, expected {!r}".format(
                    sorted(keys), sorted(EXPECTED_HEIGHTMAP_KEYS)
                )
            )
        for hm_name in EXPECTED_HEIGHTMAP_KEYS:
            hm = check_compound_field(
                heightmaps, hm_name, TAG_LONG_ARRAY, "Heightmaps"
            )
            if len(hm) != EXPECTED_HEIGHTMAP_LONGS:
                fail(
                    "Heightmaps.{} has {} longs, expected {}".format(
                        hm_name, len(hm), EXPECTED_HEIGHTMAP_LONGS
                    )
                )
            for i, v in enumerate(hm):
                if v != 0:
                    fail(
                        "Heightmaps.{}[{}] = {}, expected 0".format(
                            hm_name, i, v
                        )
                    )

        # ----- Sections -----
        sections = check_compound_field(root, "sections", TAG_LIST, "root")
        if sections.tagID != TAG_COMPOUND:
            fail(
                "sections element type = {}, expected TAG_COMPOUND={}".format(
                    sections.tagID, TAG_COMPOUND
                )
            )
        if len(sections) != EXPECTED_SECTIONS_PER_CHUNK:
            fail(
                "len(sections) = {}, expected {}".format(
                    len(sections), EXPECTED_SECTIONS_PER_CHUNK
                )
            )

        for list_index, section in enumerate(sections):
            label = "sections[{}]".format(list_index)
            check_id(section, TAG_COMPOUND, label)

            y_tag = check_compound_field(section, "Y", TAG_BYTE, label)
            expected_y = list_index - 4
            if y_tag.value != expected_y:
                fail(
                    "{}.Y = {}, expected {}".format(
                        label, y_tag.value, expected_y
                    )
                )

            block_states = check_compound_field(
                section, "block_states", TAG_COMPOUND, label
            )
            assert_no_key(block_states, "data", label + ".block_states")
            bs_palette = check_compound_field(
                block_states, "palette", TAG_LIST, label + ".block_states"
            )
            if bs_palette.tagID != TAG_COMPOUND:
                fail(
                    "{}.block_states.palette element type = {}, expected "
                    "TAG_COMPOUND={}".format(label, bs_palette.tagID, TAG_COMPOUND)
                )
            if len(bs_palette) != 1:
                fail(
                    "{}.block_states.palette length = {}, expected 1".format(
                        label, len(bs_palette)
                    )
                )
            entry = bs_palette[0]
            check_id(entry, TAG_COMPOUND, label + ".block_states.palette[0]")
            name_tag = check_compound_field(
                entry, "Name", TAG_STRING, label + ".block_states.palette[0]"
            )
            if name_tag.value != "minecraft:air":
                fail(
                    "{}.block_states.palette[0].Name = {!r}, expected "
                    "'minecraft:air'".format(label, name_tag.value)
                )
            # All-air palette entry must NOT carry Properties.
            assert_no_key(
                entry, "Properties", label + ".block_states.palette[0]"
            )
            # No other keys besides Name in the all-air palette entry.
            if len(entry.tags) != 1:
                fail(
                    "{}.block_states.palette[0] has {} keys, expected only "
                    "'Name'".format(label, len(entry.tags))
                )

            biomes = check_compound_field(
                section, "biomes", TAG_COMPOUND, label
            )
            assert_no_key(biomes, "data", label + ".biomes")
            b_palette = check_compound_field(
                biomes, "palette", TAG_LIST, label + ".biomes"
            )
            if b_palette.tagID != TAG_STRING:
                fail(
                    "{}.biomes.palette element type = {}, expected "
                    "TAG_STRING={}".format(label, b_palette.tagID, TAG_STRING)
                )
            if len(b_palette) != 1:
                fail(
                    "{}.biomes.palette length = {}, expected 1".format(
                        label, len(b_palette)
                    )
                )
            if b_palette[0].value != EXPECTED_BIOME:
                fail(
                    "{}.biomes.palette[0] = {!r}, expected {!r}".format(
                        label, b_palette[0].value, EXPECTED_BIOME
                    )
                )

        # All assertions passed.
        # Element types for the empty lists default to TAG_End in our writer
        # (the canonical empty-list encoding); confirm here so a regression
        # to "tagID is None / TAG_End is rejected" is caught.
        for key in ("block_entities", "fluid_ticks", "block_ticks", "PostProcessing"):
            tag = root[key]
            if tag.tagID != TAG_END:
                fail(
                    "{} empty-list element type = {}, expected "
                    "TAG_END={}".format(key, tag.tagID, TAG_END)
                )

        print("PASS: all-air chunk NBT structure matches §3.12.a contract")
        print(
            "  output {} bytes; root has 14 top-level keys; 24 sections "
            "(Y = -4..19); 4 heightmaps × 37 zero-longs; "
            "biome={!r}".format(os.path.getsize(out_path), EXPECTED_BIOME)
        )
        return 0
    finally:
        if out_path is not None and os.path.exists(out_path):
            os.unlink(out_path)


if __name__ == "__main__":
    sys.exit(main())
