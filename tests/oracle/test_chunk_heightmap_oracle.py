#!/usr/bin/env python3
"""Session 15c — heightmap predicate-scan oracle.

The C++ test binary builds a chunk with stone arranged in a triangular
footprint within section Y = -4 (world Y ∈ [-60, -50]):

    stone iff (world_y >= -60) and (world_y <= -50)
              and (lx + lz <= world_y - WORLD_BOTTOM_Y)

air otherwise. This produces a non-uniform per-column heightmap pattern
— the multi-column variation the Superflat fixture didn't exercise.

For each column (lx, lz) we enumerate the predicate explicitly
(matching the C++ build path: provider called bottom-up in section-major
Y-Z-X order; std::max tracks topmost qualifying world_y), then assert:

  - All four heightmaps (MOTION_BLOCKING, MOTION_BLOCKING_NO_LEAVES,
    OCEAN_FLOOR, WORLD_SURFACE) unpack to the SAME 256-value array
    (v0.1 predicate is identical across heightmaps per proposal §3.5).

  - For every column index i = lz * 16 + lx (Z-major, X-minor per
    proposal §3.5 and the MC Wiki convention), the unpacked value
    matches `(top_y + 1) - WORLD_BOTTOM_Y` from our per-column scan,
    or 0 if no qualifying block was found.

This is the first cross-check on the `lz * 16 + lx` heightmap layout —
the fixture is uniform so the convention isn't testable empirically.
A wrong (lx * 16 + lz) writer would produce a transposed pattern that
the column-by-column comparison surfaces immediately.
"""

import os
import subprocess
import sys
import tempfile

import nbt.nbt  # type: ignore[import-untyped]
from nbt.nbt import (
    TAG_COMPOUND,
    TAG_LONG_ARRAY,
)

WORLD_BOTTOM_Y = -64
HEIGHTMAP_BITS = 9
HEIGHTMAP_ENTRIES = 256
HEIGHTMAP_LONGS = 37  # ceil(256 / (64 // 9)) = ceil(256 / 7) = 37
HEIGHTMAP_KEYS = (
    "MOTION_BLOCKING",
    "MOTION_BLOCKING_NO_LEAVES",
    "OCEAN_FLOOR",
    "WORLD_SURFACE",
)


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def staircase_predicate(lx, wy, lz):
    """Matches tests/test_chunk_heightmap.cpp's lambda exactly."""
    return (-60 <= wy <= -50) and ((lx + lz) <= (wy - WORLD_BOTTOM_Y))


def expected_top_y(lx, lz):
    """Topmost world_y where the predicate holds for the given (lx, lz),
    or None if no qualifying block exists. Scans top-down to mirror the
    §3.5 per-column algorithm; the writer accumulates via std::max during
    a bottom-up scan, which is mathematically equivalent."""
    for wy in range(-50, -61, -1):
        if staircase_predicate(lx, wy, lz):
            return wy
    return None


def expected_heightmap_value(lx, lz):
    top = expected_top_y(lx, lz)
    if top is None:
        return 0  # sentinel: WORLD_BOTTOM_Y - 1 yields (−65 + 1) − (−64) = 0
    return (top + 1) - WORLD_BOTTOM_Y


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


def main():
    binary = os.environ.get("PATH_TO_BINARY")
    if not binary:
        fail("PATH_TO_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_BINARY={!r} does not exist".format(binary))

    # Pre-compute the expected heightmap across all 256 columns.
    expected = [0] * HEIGHTMAP_ENTRIES
    for lz in range(16):
        for lx in range(16):
            expected[lz * 16 + lx] = expected_heightmap_value(lx, lz)
    nonzero_count = sum(1 for v in expected if v != 0)
    max_value = max(expected)
    if nonzero_count == 0 or nonzero_count == HEIGHTMAP_ENTRIES:
        fail("staircase predicate degenerate: expected pattern has {} "
             "non-zero columns; the test loses its multi-column "
             "discriminating power".format(nonzero_count))

    out_path = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix="chunk_heightmap_", suffix=".nbt", delete=False
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
                    proc.returncode, proc.stdout, proc.stderr))
        if os.path.getsize(out_path) == 0:
            fail("writer binary produced empty output")

        with open(out_path, "rb") as fh:
            root = nbt.nbt.NBTFile(buffer=fh)

        if "Heightmaps" not in root:
            fail("root has no 'Heightmaps' key")
        heightmaps = root["Heightmaps"]
        if heightmaps.id != TAG_COMPOUND:
            fail("Heightmaps tag id is {}, expected TAG_COMPOUND={}".format(
                heightmaps.id, TAG_COMPOUND))

        present_keys = {entry.name for entry in heightmaps.tags}
        expected_keys = set(HEIGHTMAP_KEYS)
        if present_keys != expected_keys:
            fail("Heightmaps keys = {!r}, expected {!r}".format(
                sorted(present_keys), sorted(expected_keys)))

        # Unpack each heightmap; assert per-column value matches expected.
        # All four must be identical (v0.1 collapsed predicate per §3.5).
        unpacked_per_heightmap = {}
        for hm_name in HEIGHTMAP_KEYS:
            hm = heightmaps[hm_name]
            if hm.id != TAG_LONG_ARRAY:
                fail("Heightmaps.{} tag id is {}, expected "
                     "TAG_LONG_ARRAY={}".format(hm_name, hm.id, TAG_LONG_ARRAY))
            if len(hm) != HEIGHTMAP_LONGS:
                fail("Heightmaps.{} has {} longs, expected {}".format(
                    hm_name, len(hm), HEIGHTMAP_LONGS))

            unpacked = unpack_lsb_first_nonstraddling(
                list(hm), HEIGHTMAP_BITS, HEIGHTMAP_ENTRIES)
            unpacked_per_heightmap[hm_name] = unpacked

            for col_idx, got in enumerate(unpacked):
                lx = col_idx % 16
                lz = col_idx // 16
                want = expected[col_idx]
                if got != want:
                    fail("Heightmaps.{}[col={}] (lx={}, lz={}) = {}, "
                         "expected {} — note: if this looks like a "
                         "transposed pattern, the writer may be using "
                         "(lx * 16 + lz) instead of (lz * 16 + lx) per "
                         "§3.5".format(hm_name, col_idx, lx, lz, got, want))

        # All four heightmaps must be byte-identical (same predicate).
        ref = unpacked_per_heightmap[HEIGHTMAP_KEYS[0]]
        for hm_name in HEIGHTMAP_KEYS[1:]:
            other = unpacked_per_heightmap[hm_name]
            if other != ref:
                # Find first differing column for the diagnostic.
                for i in range(HEIGHTMAP_ENTRIES):
                    if other[i] != ref[i]:
                        fail("Heightmaps.{} differs from Heightmaps.{} at "
                             "col={}: {} vs {}".format(
                                 hm_name, HEIGHTMAP_KEYS[0], i,
                                 other[i], ref[i]))
                fail("Heightmaps.{} differs from Heightmaps.{} but "
                     "couldn't locate the column".format(
                         hm_name, HEIGHTMAP_KEYS[0]))

        print("PASS: heightmap predicate scan matches staircase pattern "
              "across all 256 columns × 4 heightmaps")
        print("  output {} bytes; staircase predicate produced {} non-zero "
              "columns and {} all-air columns; max heightmap value = {} "
              "(corresponding to top_y = -50, value (-50+1)-(-64) = 15)"
              .format(os.path.getsize(out_path), nonzero_count,
                      HEIGHTMAP_ENTRIES - nonzero_count, max_value))
        print("  4/4 heightmaps unpacked identically (v0.1 collapsed "
              "predicate per §3.5); (lz*16+lx) layout cross-checked "
              "against a non-uniform pattern for the first time")
        return 0
    finally:
        if out_path is not None and os.path.exists(out_path):
            os.unlink(out_path)


if __name__ == "__main__":
    sys.exit(main())
