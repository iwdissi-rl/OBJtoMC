# chunk_extended_height oracle — Session 63 (v0.4 item #2).
#
# Drives the helper binary (PATH_TO_BINARY) to emit a WorldHeight{-2032,
# 4064} chunk containing one oak_log at world (14, 592, 7), parses it with
# twoolie/NBT, and asserts the parametrised schema against the facts
# established by the Session 61 maintainer capture
# (tests/fixtures/extended_height/chunk_2_-16_h4064.nbt, GROUND_TRUTH_NBT):
#
#   1. yPos == -127 == min_y/16, matching the captured real chunk.
#   2. sections: 254 entries, Y -127..126 contiguous — same span & count
#      as the captured chunk.
#   3. Heightmaps: 4 maps x 52 longs — same long count as the capture.
#   4. 12-bit heightmap encoding: WORLD_SURFACE[column 7*16+14] == 2625
#      == (592 + 1) - (-2032); every other column == 0. 2625 is the SAME
#      value the capture holds at its built column (proven Session 61 by
#      decode-vs-actual-top-block).
#   5. The oak_log sits in section Y=37 at local (14, 592-37*16=0, 7);
#      every other section is single-palette air.

import os
import subprocess
import sys
import tempfile

from nbt import nbt


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def unpack(longs, bits, count):
    per = 64 // bits
    mask = (1 << bits) - 1
    out = []
    for i in range(count):
        word = longs[i // per] & 0xFFFFFFFFFFFFFFFF
        out.append((word >> ((i % per) * bits)) & mask)
    return out


def main():
    binary = os.environ["PATH_TO_BINARY"]
    gt_path = os.environ["GROUND_TRUTH_NBT"]

    with tempfile.TemporaryDirectory() as td:
        out_path = os.path.join(td, "chunk.nbt")
        subprocess.run([binary, "--out", out_path], check=True)
        tag = nbt.NBTFile(buffer=open(out_path, "rb"))

    gt = nbt.NBTFile(buffer=open(gt_path, "rb"))

    # 1. yPos parametrisation, cross-checked against the real chunk.
    if tag["yPos"].value != -127:
        fail("yPos %d != -127" % tag["yPos"].value)
    if gt["yPos"].value != -127:
        fail("ground truth yPos drifted?")

    # 2. Full contiguous section span, same count as the capture.
    ours = sorted(s["Y"].value for s in tag["sections"])
    theirs = sorted(s["Y"].value for s in gt["sections"])
    if ours != list(range(-127, 127)):
        fail("section span %d..%d count %d" % (ours[0], ours[-1], len(ours)))
    if len(ours) != len(theirs):
        fail("section count %d != capture %d" % (len(ours), len(theirs)))

    # 3 + 4. Heightmaps: long count matches capture; 12-bit decode exact.
    for name in ("MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES",
                 "OCEAN_FLOOR", "WORLD_SURFACE"):
        arr = list(tag["Heightmaps"][name].value)
        if len(arr) != 52:
            fail("%s has %d longs, want 52" % (name, len(arr)))
        if len(list(gt["Heightmaps"][name].value)) != 52:
            fail("capture %s long count drifted?" % name)
        vals = unpack(arr, 12, 256)
        want_col = 7 * 16 + 14
        for ci, v in enumerate(vals):
            want = 2625 if ci == want_col else 0
            if v != want:
                fail("%s[%d] = %d, want %d" % (name, ci, v, want))

    # 5. Block placement: oak_log in section 37, everything else air.
    for sec in tag["sections"]:
        sy = sec["Y"].value
        bs = sec["block_states"]
        pal = [p["Name"].value for p in bs["palette"]]
        if sy != 37:
            if pal != ["minecraft:air"] or "data" in bs:
                fail("section %d not single-palette air" % sy)
            continue
        if sorted(pal) != ["minecraft:air", "minecraft:oak_log"]:
            fail("section 37 palette %r" % pal)
        longs = [v & 0xFFFFFFFFFFFFFFFF for v in bs["data"].value]
        bits = max(4, (len(pal) - 1).bit_length())
        vals = unpack(longs, bits, 4096)
        ly = 592 - 37 * 16
        want_idx = ly * 256 + 7 * 16 + 14
        for i, v in enumerate(vals):
            name = pal[v]
            want = "minecraft:oak_log" if i == want_idx else "minecraft:air"
            if name != want:
                fail("section 37 cell %d = %s, want %s" % (i, name, want))

    print("PASS: chunk_extended_height_oracle (yPos, 254-section span, "
          "12-bit/52-long heightmaps, block-exact placement — all matching "
          "the Session 61 ground-truth capture's parameters)")


if __name__ == "__main__":
    main()
