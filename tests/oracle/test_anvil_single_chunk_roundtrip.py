#!/usr/bin/env python3
"""Tier-2 Anvil oracle: round-trip one chunk through our RegionWriter.

End-to-end:

  1. Read tests/fixtures/real_minecraft_1.21.5_flat_r.0.0.mca; extract
     chunk (0, 0)'s RAW decompressed NBT bytes by reading the location
     table, locating the sector, parsing the length+scheme header, and
     calling zlib.decompress on the compressed payload. This matches
     `tests/dissect/dissect_region.py` and DELIBERATELY avoids
     anvil-parser's NBTFile path (which would re-serialise through its
     own tree and could mask a writer bug). Save to a tempfile.
  2. Invoke $PATH_TO_BINARY --chunk-nbt <tempfile> --out <out_mca>.
  3. Open the output .mca with anvil-parser==0.9.0 — the INDEPENDENT
     reader — extract chunk (0, 0), and assert its logical fields match
     the source chunk (parsed via the same anvil-parser path).

Logical equivalence asserted (per research/anvil_api_proposal.md §3.10):

    DataVersion == 4325
    xPos        == 0
    yPos        == -4
    zPos        == 0
    Status      == "minecraft:full"
    len(sections) == <whatever the source chunk has>

NOT byte-exact: zlib output, padding, and timestamps will differ from
the fixture; the contract is logical-tree-equivalence.
"""

import os
import subprocess
import sys
import tempfile
import zlib

import anvil  # type: ignore[import-untyped]
import nbt.nbt  # type: ignore[import-untyped]

FIXTURE_PATH = "tests/fixtures/real_minecraft_1.21.5_flat_r.0.0.mca"
SECTOR_SIZE = 4096


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def extract_raw_chunk_nbt(region_path, chunk_x, chunk_z):
    """Raw zlib.decompress on the chunk's payload bytes. No anvil-parser
    re-serialisation in the path."""
    with open(region_path, "rb") as f:
        data = f.read()
    entry_index = (chunk_x & 31) + (chunk_z & 31) * 32
    b_off = entry_index * 4
    sector_offset = int.from_bytes(data[b_off:b_off + 3], "big")
    sector_count = data[b_off + 3]
    if sector_offset == 0 and sector_count == 0:
        fail(
            "fixture chunk ({}, {}) is not populated".format(
                chunk_x, chunk_z
            )
        )
    sb = sector_offset * SECTOR_SIZE
    declared_length = int.from_bytes(data[sb:sb + 4], "big")
    scheme = data[sb + 4]
    if scheme != 2:
        fail(
            "fixture chunk ({}, {}) uses compression scheme {}, "
            "expected 2 (zlib)".format(chunk_x, chunk_z, scheme)
        )
    compressed = data[sb + 5:sb + 4 + declared_length]
    return zlib.decompress(compressed)


def parse_source_nbt_from_bytes(raw_bytes):
    import io
    return nbt.nbt.NBTFile(buffer=io.BytesIO(raw_bytes))


def assert_int_field(label, root, key, expected):
    if key not in root:
        fail("{}: root has no {!r}".format(label, key))
    actual = root[key].value
    if actual != expected:
        fail(
            "{}: {} = {!r}, expected {!r}".format(label, key, actual, expected)
        )


def assert_string_field(label, root, key, expected):
    if key not in root:
        fail("{}: root has no {!r}".format(label, key))
    actual = root[key].value
    if actual != expected:
        fail(
            "{}: {} = {!r}, expected {!r}".format(label, key, actual, expected)
        )


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))
    if not os.path.isfile(FIXTURE_PATH):
        fail("fixture missing at {}".format(FIXTURE_PATH))

    nbt_temp = None
    out_temp = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="anvil_chunk_", suffix=".nbt", delete=False
        ) as fh:
            nbt_temp = fh.name
        with tempfile.NamedTemporaryFile(
            prefix="anvil_out_", suffix=".mca", delete=False
        ) as fh:
            out_temp = fh.name

        raw = extract_raw_chunk_nbt(FIXTURE_PATH, 0, 0)
        if not raw:
            fail("extracted source chunk (0,0) is empty")

        with open(nbt_temp, "wb") as fh:
            fh.write(raw)

        proc = subprocess.run(
            [binary, "--chunk-nbt", nbt_temp, "--out", out_temp],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail(
                "writer binary exited {} ; stdout:\n{}\nstderr:\n{}".format(
                    proc.returncode, proc.stdout, proc.stderr
                )
            )

        out_size = os.path.getsize(out_temp)
        if out_size == 0 or out_size % SECTOR_SIZE != 0:
            fail(
                "output {} is {} bytes (must be positive multiple of "
                "{})".format(out_temp, out_size, SECTOR_SIZE)
            )

        src_root = parse_source_nbt_from_bytes(raw)
        out_region = anvil.Region.from_file(out_temp)
        out_root = out_region.chunk_data(0, 0)
        if out_root is None:
            fail("anvil-parser could not read chunk (0,0) from our output")

        assert_int_field("source", src_root, "DataVersion", 4325)
        assert_int_field("output", out_root, "DataVersion", 4325)

        assert_int_field("source", src_root, "xPos", 0)
        assert_int_field("output", out_root, "xPos", 0)

        assert_int_field("source", src_root, "yPos", -4)
        assert_int_field("output", out_root, "yPos", -4)

        assert_int_field("source", src_root, "zPos", 0)
        assert_int_field("output", out_root, "zPos", 0)

        assert_string_field("source", src_root, "Status", "minecraft:full")
        assert_string_field("output", out_root, "Status", "minecraft:full")

        src_sections_len = len(src_root["sections"])
        out_sections_len = len(out_root["sections"])
        if src_sections_len != out_sections_len:
            fail(
                "len(sections) differs: source={}, output={}".format(
                    src_sections_len, out_sections_len
                )
            )

        print("PASS: Anvil writer single-chunk round-trip matches fixture "
              "chunk (0,0)")
        print(
            "  source {} bytes raw NBT, output region {} bytes ({} "
            "sectors)".format(
                len(raw), out_size, out_size // SECTOR_SIZE
            )
        )
        print(
            "  fields verified: DataVersion=4325, xPos=0, yPos=-4, zPos=0, "
            "Status=minecraft:full, len(sections)={}".format(
                src_sections_len
            )
        )
        return 0
    finally:
        for p in (nbt_temp, out_temp):
            if p is not None and os.path.exists(p):
                os.unlink(p)


if __name__ == "__main__":
    sys.exit(main())
