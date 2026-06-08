#!/usr/bin/env python3
"""Tier-3 Anvil oracle: round-trip ALL populated chunks of the MC 1.21.5
fixture through our RegionWriter.

End-to-end:

  1. Read tests/fixtures/real_minecraft_1.21.5_flat_r.0.0.mca, iterate the
     1024-entry location table, and for every populated slot extract:
       - (chunk_x, chunk_z) from the slot index via
             local_x = idx % 32
             local_z = idx // 32
             chunk_x = REGION_X * 32 + local_x
             chunk_z = REGION_Z * 32 + local_z
         (REGION_X = REGION_Z = 0 for this fixture)
       - the chunk's sector start
       - the int32-BE declared length, the compression-scheme byte
       - the compressed payload ((declared_length - 1) bytes after scheme)
       - raw decompressed NBT via zlib.decompress (DELIBERATELY not via
         anvil-parser, to keep the source-of-truth path independent of
         our writer's downstream reader; matches the tier-2 oracle).
     Save each chunk's decompressed bytes to a per-chunk temp file.

  2. Sanity-check: count of populated slots equals
     EXPECTED_POPULATED (960 per Session 8's dissection in
     research/anvil_ground_truth.md); every compression scheme == 2.

  3. Build a tab-separated manifest (chunk_x\tchunk_z\tnbt_path).

  4. Invoke $PATH_TO_FULL_REGION_BINARY --chunks-manifest <manifest>
     --out <out.mca>; relay its stdout/stderr.

  5. Open the produced .mca with anvil-parser==0.9.0. For EVERY source
     chunk, parse the source's raw NBT via twoolie/NBT and the output's
     parsed chunk via anvil-parser, then assert logical equivalence on
     the same set of fields the tier-2 oracle checks:
        DataVersion, xPos, yPos, zPos, Status, len(sections).
     Per-section content comparison is future work.

  6. Print progress every 100 chunks.

  7. Track wall-clock (Python-side, including the C++ subprocess) and
     peak RSS via resource.getrusage(RUSAGE_SELF).ru_maxrss (kilobytes
     on Linux).

  8. PASS summary or first FAIL with explicit per-chunk diagnostics.
"""

import os
import resource
import shutil
import subprocess
import sys
import tempfile
import time
import zlib
from io import BytesIO

import anvil  # type: ignore[import-untyped]
import nbt.nbt  # type: ignore[import-untyped]

FIXTURE_PATH = "tests/fixtures/real_minecraft_1.21.5_flat_r.0.0.mca"
SECTOR_SIZE = 4096
REGION_X = 0
REGION_Z = 0
EXPECTED_POPULATED = 960  # research/anvil_ground_truth.md (Session 8 dissection)


def fail(message):
    print("FAIL: " + message)
    sys.exit(1)


def iterate_fixture_chunks(region_path):
    """Yield (chunk_x, chunk_z, raw_decompressed_nbt_bytes) for every
    populated slot in the location table."""
    with open(region_path, "rb") as f:
        data = f.read()
    if len(data) < 8192:
        fail("fixture is shorter than 8192 bytes ({} bytes)".format(len(data)))
    for idx in range(1024):
        b_off = idx * 4
        sector_offset = int.from_bytes(data[b_off:b_off + 3], "big")
        sector_count = data[b_off + 3]
        if sector_offset == 0 and sector_count == 0:
            continue
        local_x = idx % 32
        local_z = idx // 32
        chunk_x = REGION_X * 32 + local_x
        chunk_z = REGION_Z * 32 + local_z
        sb = sector_offset * SECTOR_SIZE
        if sb + 5 > len(data):
            fail(
                "chunk ({},{}) slot {} sector starts past EOF".format(
                    chunk_x, chunk_z, idx))
        declared_length = int.from_bytes(data[sb:sb + 4], "big")
        scheme = data[sb + 4]
        if scheme != 2:
            fail(
                "chunk ({},{}) compression scheme {} != 2 (zlib)".format(
                    chunk_x, chunk_z, scheme))
        compressed = data[sb + 5:sb + 4 + declared_length]
        try:
            raw = zlib.decompress(compressed)
        except zlib.error as e:
            fail("chunk ({},{}) zlib.decompress raised: {}".format(
                chunk_x, chunk_z, e))
        if not raw:
            fail("chunk ({},{}) decompressed to zero bytes".format(
                chunk_x, chunk_z))
        yield chunk_x, chunk_z, raw


def parse_nbt(raw_bytes):
    return nbt.nbt.NBTFile(buffer=BytesIO(raw_bytes))


def assert_logical_equivalence(chunk_x, chunk_z, src_root, out_root):
    for key in ("DataVersion", "xPos", "yPos", "zPos", "Status"):
        if key not in src_root:
            fail("chunk ({},{}) source NBT missing key {!r}".format(
                chunk_x, chunk_z, key))
        if key not in out_root:
            fail("chunk ({},{}) output NBT missing key {!r}".format(
                chunk_x, chunk_z, key))
        src_val = src_root[key].value
        out_val = out_root[key].value
        if src_val != out_val:
            fail(
                "chunk ({},{}) field {!r} differs: "
                "source={!r}, output={!r}".format(
                    chunk_x, chunk_z, key, src_val, out_val))

    if "sections" not in src_root:
        fail("chunk ({},{}) source NBT has no 'sections'".format(
            chunk_x, chunk_z))
    if "sections" not in out_root:
        fail("chunk ({},{}) output NBT has no 'sections'".format(
            chunk_x, chunk_z))
    src_n = len(src_root["sections"])
    out_n = len(out_root["sections"])
    if src_n != out_n:
        fail(
            "chunk ({},{}) len(sections) differs: source={}, output={}".format(
                chunk_x, chunk_z, src_n, out_n))


def main():
    binary = os.environ.get("PATH_TO_FULL_REGION_BINARY")
    if not binary:
        fail("PATH_TO_FULL_REGION_BINARY env var is not set")
    if not os.path.isfile(binary):
        fail("PATH_TO_FULL_REGION_BINARY={!r} does not exist".format(binary))
    if not os.path.isfile(FIXTURE_PATH):
        fail("fixture missing at {}".format(FIXTURE_PATH))

    tmp_root = tempfile.mkdtemp(prefix="anvil_full_region_")
    try:
        wall_start = time.monotonic()

        chunks_dir = os.path.join(tmp_root, "chunks")
        os.makedirs(chunks_dir)
        manifest_path = os.path.join(tmp_root, "manifest.tsv")
        out_path = os.path.join(tmp_root, "roundtrip.mca")

        sources = []  # (chunk_x, chunk_z, raw_bytes, nbt_path)
        for chunk_x, chunk_z, raw in iterate_fixture_chunks(FIXTURE_PATH):
            nbt_path = os.path.join(
                chunks_dir, "chunk_{}_{}.nbt".format(chunk_x, chunk_z))
            with open(nbt_path, "wb") as fh:
                fh.write(raw)
            sources.append((chunk_x, chunk_z, raw, nbt_path))

        if len(sources) != EXPECTED_POPULATED:
            fail(
                "extracted {} populated chunks, expected {} per "
                "research/anvil_ground_truth.md".format(
                    len(sources), EXPECTED_POPULATED))

        with open(manifest_path, "w") as fh:
            for chunk_x, chunk_z, _, nbt_path in sources:
                fh.write("{}\t{}\t{}\n".format(chunk_x, chunk_z, nbt_path))

        proc = subprocess.run(
            [binary,
             "--chunks-manifest", manifest_path,
             "--out", out_path],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            fail(
                "writer binary exited {} ; stdout:\n{}\nstderr:\n{}".format(
                    proc.returncode, proc.stdout, proc.stderr))

        print("---- C++ binary stdout ----")
        sys.stdout.write(proc.stdout)
        if not proc.stdout.endswith("\n"):
            print()
        print("---- end C++ binary stdout ----")

        out_size = os.path.getsize(out_path)
        if out_size == 0 or out_size % SECTOR_SIZE != 0:
            fail("output {} is {} bytes (must be positive multiple of "
                 "{})".format(out_path, out_size, SECTOR_SIZE))

        out_region = anvil.Region.from_file(out_path)

        for i, (chunk_x, chunk_z, raw, _) in enumerate(sources, start=1):
            src_root = parse_nbt(raw)
            out_root = out_region.chunk_data(chunk_x, chunk_z)
            if out_root is None:
                fail("anvil-parser could not read chunk ({},{}) from our "
                     "output".format(chunk_x, chunk_z))
            assert_logical_equivalence(chunk_x, chunk_z, src_root, out_root)
            if i % 100 == 0:
                print("[{}/{}] verified".format(i, len(sources)))

        wall_end = time.monotonic()
        elapsed_s = wall_end - wall_start

        ru = resource.getrusage(resource.RUSAGE_SELF)
        peak_rss_kb = ru.ru_maxrss  # KB on Linux

        print()
        print("PASS: Anvil writer full-region round-trip verified across "
              "all {} chunks".format(len(sources)))
        print("  output region size : {} bytes ({} sectors of {})".format(
            out_size, out_size // SECTOR_SIZE, SECTOR_SIZE))
        print("  wall-clock total   : {:.3f} s "
              "(Python-side, incl. C++ subprocess + per-chunk verify)".format(
                  elapsed_s))
        print("  peak RSS (oracle)  : {} KB ({:.1f} MiB)".format(
            peak_rss_kb, peak_rss_kb / 1024.0))
        print("  fixture chunks     : {} populated".format(len(sources)))
        return 0
    finally:
        shutil.rmtree(tmp_root, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
