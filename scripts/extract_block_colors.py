#!/usr/bin/env python3
"""Extract per-block average colours from a vanilla MC 1.21.5 client jar.

Output: ``data/block_palette_db.csv`` (one row per placeable full-cube block).

Inputs
------
* ``--jar PATH``  vanilla Minecraft Java Edition 1.21.5 client jar (typically
  found at ``~/.minecraft/versions/1.21.5/1.21.5.jar`` on Linux). Mojang EULA
  permits personal/research extraction from a jar you legally own. This
  script reads PNGs from the jar but **never redistributes them** — the only
  derivative we commit is the per-block 6-byte average colour written to
  the output CSV.
* ``--blocks-json PATH``  defaults to
  ``third_party/minecraft-data/pc/1.21.5/blocks.json`` (vendored MIT data).

Outputs
-------
* ``--out PATH``  defaults to ``data/block_palette_db.csv``. The CSV's
  header comment lines record provenance:

      # mc_version: 1.21.5
      # prismarinejs_sha: <full SHA from VERSION.txt>
      # extractor_commit: <git rev-parse HEAD or "uncommitted">
      # extractor_pillow_version: <Pillow.__version__>
      # generated: <ISO 8601 UTC timestamp>
      # columns: name,opacity,luminance,r,g,b,is_full_cube,texture_path

Dependencies
------------
* Python 3.10+ (uses ``zoneinfo``, structural pattern matching is NOT used).
* **Pillow** for PNG decode + sRGB → linear conversion. Install via::

      python3 -m pip install --user Pillow

  The Pillow version used at extract time is recorded in the CSV header so
  a future maintainer can detect PNG-decode edge-case drift (proposal §3.1
  "Maintainer fallback").

Invocation
----------
::

    python3 scripts/extract_block_colors.py \
        --jar ~/.minecraft/versions/1.21.5/1.21.5.jar \
        --out data/block_palette_db.csv

The script reads ~700 PNGs from the jar, computes per-block averages, and
writes a ~50 KiB CSV. End-to-end runtime is seconds. The script is
**idempotent**: running twice with the same ``--jar`` and the same
PrismarineJS pin produces a byte-identical CSV (modulo PIL version
differences in PNG decode edge cases). The CSV's final line includes a
self-SHA-256 marker computed over the body (excluding the marker line)
that a future maintainer can sanity-check.

Multi-texture block resolution (per OPEN_QUESTIONS.md 2026-06-20)
----------------------------------------------------------------
Many full-cube blocks ship with multiple PNGs (grass_block_top vs
grass_block_side; oak_log vs oak_log_top; etc.). For v0.1 we pick the
"most-visible-from-top-down-voxel-view" texture per the lookup order:

  1. ``block/<name>.png``                  (single-texture blocks)
  2. ``block/<name>_top.png``              (top-distinguished blocks)
  3. ``block/<name>_side.png``             (side-distinguished fallback)

If none of (1)-(3) hit, six documented family-alias fallback patterns
(see ``OPEN_QUESTIONS.md`` 2026-06-20 "Extractor texture-resolution
fallbacks") are tried in order:

  Pattern 1: ``<name>_wood`` -> ``<name>_log.png``      (wood face uses log side)
  Pattern 2: ``waxed_<name>`` -> ``<name>`` lookup      (waxed copper shares texture)
  Pattern 3: ``infested_<name>`` -> ``<name>`` lookup   (infested shares host stone)
  Pattern 4: ``smooth_<name>`` -> ``<name>_top.png`` / ``<name>.png``
  Pattern 5: ``<name>_hyphae`` -> ``<name>_stem.png``   (nether wood face uses stem side)
  Pattern 6: explicit name remap                        (magma_block, dried_kelp_block, sticky_piston)
  Pattern 7: ``<name>_front.png``                       (dispenser/dropper-style)

If none of the patterns match, an anomaly path picks the first
alphabetical ``block/<name>_*.png`` match. Stderr emits ``info: ...``
for each fallback used and ``warning: ...`` for anomaly picks or
unrecoverable exclusions. The resolved path is recorded in the CSV's
``texture_path`` column so a future matcher can re-extract a different
face if needed.

Tint application (per palette_db_proposal.md §3.7)
--------------------------------------------------
The chunk-schema v0.1 lock binds every build to ``minecraft:plains`` per
DECISIONS.md 2026-06-09. The extractor pre-multiplies tint-applicable
textures by the plains-biome defaults at extract time:

* grass tint  ``(0x91, 0xBD, 0x59)`` -> ``grass_block`` (via top texture)
* foliage tint ``(0x77, 0xAB, 0x2F)`` -> oak/jungle/dark_oak/acacia/mangrove leaves
* constant tints from ``tints.json`` -> birch_leaves, spruce_leaves

Tint multiplication happens in linear-RGB space, then sRGB-encoded for
storage. Output CSV stores sRGB-uint8; the C++ loader gamma-decodes on
parse per proposal §3.4.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import io
import json
import math
import pathlib
import re
import subprocess
import sys
import zipfile
from typing import Iterable

# -----------------------------------------------------------------------------
# Exclusions, allow-lists, and tint defaults are all encoded here so that
# regenerating the CSV is a deterministic function of the inputs.
# -----------------------------------------------------------------------------

# Air & non-block entities — handled by the chunk schema layer, not the matcher.
EXCLUDE_AIR = {"air", "cave_air", "void_air"}

# Fluids — v0.1 chunk schema does not emit fluid-tick handling.
EXCLUDE_FLUIDS = {"water", "lava", "bubble_column"}

# Debug / admin — never placeable by the matcher.
# `test_block` is a 1.21.5 dev/debug-only block whose only PNGs are
# variant-state indicators (`test_block_accept.png`, `_fail.png`, `_log.png`,
# `_start.png`); not a regular placeable block, so it joins the admin list.
EXCLUDE_ADMIN = {
    "barrier", "light", "structure_void", "structure_block",
    "jigsaw", "command_block", "chain_command_block",
    "repeating_command_block", "debug_stick", "test_block",
}

# Partial-block name suffixes. Names matching any of these are NOT full cubes
# regardless of what ``boundingBox`` claims (proposal §3.6).
PARTIAL_SUFFIXES = (
    "_slab", "_stairs", "_wall", "_fence", "_fence_gate", "_pane", "_bars",
    "_trapdoor", "_door", "_button", "_pressure_plate", "_carpet", "_candle",
    "_layer", "_sapling", "_vine", "_kelp", "_plant", "_seagrass", "_rod",
    "_torch", "_bed", "_sign", "_banner", "_head", "_skull", "_chain",
    "_wire", "_rail", "_track",
)

# Substring-match partial blocks (don't match suffix neatly).
PARTIAL_NAME_SUBSTRINGS = (
    "flower_pot", "scaffolding", "cake", "snow", "lever", "tripwire",
    "ladder", "iron_bars",
)

# Transparent solids that fail filterLight==15 but ARE full cubes (proposal §3.6).
FULL_CUBE_ALLOWLIST = {
    "glass", "ice", "packed_ice", "blue_ice", "frosted_ice",
    "slime_block", "honey_block",
    # Stained / tinted glass — each is also a full cube.
    "white_stained_glass", "orange_stained_glass", "magenta_stained_glass",
    "light_blue_stained_glass", "yellow_stained_glass", "lime_stained_glass",
    "pink_stained_glass", "gray_stained_glass", "light_gray_stained_glass",
    "cyan_stained_glass", "purple_stained_glass", "blue_stained_glass",
    "brown_stained_glass", "green_stained_glass", "red_stained_glass",
    "black_stained_glass", "tinted_glass",
}

# Pattern 6 — explicit name remaps for blocks whose texture filename diverges
# from the block name and isn't covered by primary-face lookup nor a family
# prefix/suffix fallback. Each value is a filename under
# `assets/minecraft/textures/block/` chosen as the "most visible from a
# top-down voxelisation viewpoint". Verified to exist in 1.21.5 (above).
EXPLICIT_TEXTURE_REMAP = {
    # `magma_block` ships only `magma.png` (animated 16x48; avg colour over all
    # frames is the orange-red lava tone — see scripts ground-truth comment).
    "magma_block": "magma.png",
    # `dried_kelp_block` has top/bottom/side variants; side is the most
    # representative face for matcher purposes.
    "dried_kelp_block": "dried_kelp_side.png",
    # `sticky_piston` ships only `sticky_piston_top` + `piston_*`; piston_side
    # (the body) is more representative than the small sticky head on top.
    "sticky_piston": "piston_side.png",
    # `smooth_quartz` strips to `quartz`, but the unsmoothed base block is
    # actually called `quartz_block` (MC naming quirk: `smooth_sandstone` ->
    # `sandstone` works because the base IS `sandstone`, but `quartz_block` is
    # the only block in this family with the `_block` suffix). Pattern 4 misses
    # for this one block; the explicit remap recovers it.
    "smooth_quartz": "quartz_block_top.png",
}

# Plains-biome default tints in sRGB-uint8 (proposal §3.7).
PLAINS_GRASS_TINT = (0x91, 0xBD, 0x59)
PLAINS_FOLIAGE_TINT = (0x77, 0xAB, 0x2F)

# Which full-cube blocks get which tint applied at extract time.
GRASS_TINTED_BLOCKS = {"grass_block"}
FOLIAGE_TINTED_BLOCKS = {
    "oak_leaves", "jungle_leaves", "dark_oak_leaves",
    "acacia_leaves", "mangrove_leaves",
}
# birch_leaves and spruce_leaves use constant tints from tints.json (loaded below).


def srgb_to_linear(c: float) -> float:
    """Standard sRGB -> linear-RGB transfer function. c is in [0, 1]."""
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb(c: float) -> float:
    """Inverse transfer; c is in [0, 1]."""
    return 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1.0 / 2.4)) - 0.055


def is_full_cube(name: str, block: dict) -> bool:
    """Proposal §3.6 derivation."""
    if name in FULL_CUBE_ALLOWLIST:
        return True
    if any(name.endswith(s) for s in PARTIAL_SUFFIXES):
        return False
    if any(s in name for s in PARTIAL_NAME_SUBSTRINGS):
        return False
    return block.get("boundingBox") == "block" and block.get("filterLight") == 15


def is_excluded(name: str) -> bool:
    return name in EXCLUDE_AIR or name in EXCLUDE_FLUIDS or name in EXCLUDE_ADMIN


def resolve_texture(
    jar_nameset: set[str], jar_namelist: list[str], block_name: str
) -> tuple[str | None, int | None]:
    """Find the PNG path for ``block_name``.

    Returns ``(path, pattern_id)`` where ``pattern_id`` is:

    * ``None`` for a direct primary-face hit (``<name>.png`` / ``_top`` / ``_side``)
      or when nothing matched (``path`` is also ``None`` in that case).
    * ``1``-``7`` for one of the documented family-alias fallback patterns
      (see OPEN_QUESTIONS.md 2026-06-20 "Extractor texture-resolution fallbacks").
    * ``8`` for the anomaly first-alphabetical match.

    Caller emits stderr ``info:`` / ``warning:`` lines based on ``pattern_id``.
    """
    prefix = "assets/minecraft/textures/block/"

    def try_primary(name: str) -> str | None:
        """Direct primary-face lookup. Returns the first matching path or None."""
        for suffix in ("", "_top", "_side"):
            c = f"{prefix}{name}{suffix}.png"
            if c in jar_nameset:
                return c
        return None

    # Direct lookup (proposal + OPEN_QUESTIONS.md 2026-06-20 default order).
    direct = try_primary(block_name)
    if direct is not None:
        return (direct, None)

    # Pattern 6 — explicit name remap. Checked before family-prefix patterns
    # so an entry like `magma_block` doesn't get hijacked by a future family rule.
    if block_name in EXPLICIT_TEXTURE_REMAP:
        c = prefix + EXPLICIT_TEXTURE_REMAP[block_name]
        if c in jar_nameset:
            return (c, 6)

    # Pattern 1 — *_wood / stripped_*_wood -> *_log / stripped_*_log.
    # The "wood" model uses the log's side texture on all six faces.
    if block_name.endswith("_wood"):
        base = block_name[: -len("_wood")]
        c = f"{prefix}{base}_log.png"
        if c in jar_nameset:
            return (c, 1)

    # Pattern 5 — *_hyphae / stripped_*_hyphae -> *_stem / stripped_*_stem.
    # Same scheme as wood->log, but for nether woods (warped, crimson).
    if block_name.endswith("_hyphae"):
        base = block_name[: -len("_hyphae")]
        c = f"{prefix}{base}_stem.png"
        if c in jar_nameset:
            return (c, 5)

    # Pattern 2 — waxed_* -> unwaxed primary lookup (single level, no recursion).
    # Waxed copper variants share textures verbatim with their unwaxed
    # counterparts (the wax coating is a state, not a texture).
    if block_name.startswith("waxed_"):
        unwaxed = block_name[len("waxed_") :]
        c = try_primary(unwaxed)
        if c is not None:
            return (c, 2)

    # Pattern 3 — infested_* -> host stone variant.
    # The silverfish-hideout block reuses the host stone's texture.
    if block_name.startswith("infested_"):
        host = block_name[len("infested_") :]
        c = try_primary(host)
        if c is not None:
            return (c, 3)

    # Pattern 4 — smooth_* -> base _top first, then base.
    # Smooth variants use the unsmoothed block's *top* face (which lacks
    # the "frame" pattern), e.g. smooth_sandstone uses sandstone_top.
    if block_name.startswith("smooth_"):
        base = block_name[len("smooth_") :]
        for suffix in ("_top", ""):
            c = f"{prefix}{base}{suffix}.png"
            if c in jar_nameset:
                return (c, 4)

    # Pattern 7 — dispenser/dropper-style: prefer _side (if a future MC version
    # ships one), then _front. In 1.21.5 only `_front.png` exists for these two
    # blocks (the actual `_side` texture lives in `furnace_side.png` via the
    # block model JSON, which the extractor doesn't parse); the directive
    # accommodates future MC versions that may add `<name>_side.png`.
    c_front = f"{prefix}{block_name}_front.png"
    if c_front in jar_nameset:
        return (c_front, 7)

    # Anomaly path: first alphabetical ``block_name_*.png`` match.
    pattern = re.compile(rf"^{re.escape(prefix + block_name)}_[^/]+\.png$")
    matches = sorted(p for p in jar_namelist if pattern.match(p))
    if matches:
        return (matches[0], 8)
    return (None, None)


def constant_tints_from_json(tints_json: dict) -> dict[str, tuple[int, int, int]]:
    """Decode the ``constant`` group of tints.json into a per-block-name tint.
    Colours are stored as signed int24 in tints.json; reinterpret as unsigned.
    """
    out: dict[str, tuple[int, int, int]] = {}
    group = tints_json.get("constant", {}).get("data", [])
    for entry in group:
        color = entry["color"]
        if color < 0:
            color &= 0xFFFFFF
        r, g, b = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
        for key in entry["keys"]:
            if isinstance(key, str):
                out[key] = (r, g, b)
    return out


def average_color_with_tint(
    png_bytes: bytes, tint: tuple[int, int, int] | None
) -> tuple[int, int, int]:
    """Compute the mean RGB over non-transparent pixels (alpha > 0).
    If ``tint`` is provided, multiply each pixel's linear-RGB by the tint's
    linear-RGB before averaging, then sRGB-encode the result.
    """
    from PIL import Image  # type: ignore

    with Image.open(io.BytesIO(png_bytes)) as im:
        im = im.convert("RGBA")
        w, h = im.size
        pixels = im.load()
        sum_r, sum_g, sum_b, n = 0.0, 0.0, 0.0, 0
        if tint is not None:
            tr, tg, tb = (srgb_to_linear(c / 255.0) for c in tint)
        for y in range(h):
            for x in range(w):
                r, g, b, a = pixels[x, y]
                if a == 0:
                    continue
                if tint is not None:
                    lr = srgb_to_linear(r / 255.0) * tr
                    lg = srgb_to_linear(g / 255.0) * tg
                    lb = srgb_to_linear(b / 255.0) * tb
                    r = max(0, min(255, round(linear_to_srgb(lr) * 255.0)))
                    g = max(0, min(255, round(linear_to_srgb(lg) * 255.0)))
                    b = max(0, min(255, round(linear_to_srgb(lb) * 255.0)))
                sum_r += r
                sum_g += g
                sum_b += b
                n += 1
        if n == 0:
            return (0, 0, 0)
        return (round(sum_r / n), round(sum_g / n), round(sum_b / n))


def pick_tint(name: str, constants: dict[str, tuple[int, int, int]]
              ) -> tuple[int, int, int] | None:
    if name in GRASS_TINTED_BLOCKS:
        return PLAINS_GRASS_TINT
    if name in FOLIAGE_TINTED_BLOCKS:
        return PLAINS_FOLIAGE_TINT
    return constants.get(name)


def git_head_sha(repo_root: pathlib.Path) -> str:
    try:
        out = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True,
        )
        return out.stdout.strip()
    except Exception:
        return "uncommitted"


def read_pin_sha(version_txt: pathlib.Path) -> str:
    for line in version_txt.read_text().splitlines():
        m = re.match(r"^git-sha:\s+([0-9a-f]{40})", line)
        if m:
            return m.group(1)
    return "unknown"


def main() -> int:
    repo_root = pathlib.Path(__file__).resolve().parent.parent
    default_blocks = repo_root / "third_party/minecraft-data/pc/1.21.5/blocks.json"
    default_tints = repo_root / "third_party/minecraft-data/pc/1.21.5/tints.json"
    default_out = repo_root / "data/block_palette_db.csv"

    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--jar", required=True, type=pathlib.Path,
                        help="vanilla MC 1.21.5 client jar (read-only)")
    parser.add_argument("--blocks-json", default=default_blocks, type=pathlib.Path)
    parser.add_argument("--tints-json", default=default_tints, type=pathlib.Path)
    parser.add_argument("--out", default=default_out, type=pathlib.Path)
    args = parser.parse_args()

    if not args.jar.exists():
        sys.stderr.write(f"error: jar not found at {args.jar}\n")
        return 2
    if not args.blocks_json.exists():
        sys.stderr.write(f"error: blocks.json not found at {args.blocks_json}\n")
        return 2

    blocks = json.loads(args.blocks_json.read_text())
    tints = json.loads(args.tints_json.read_text())
    constants = constant_tints_from_json(tints)

    pin_sha = read_pin_sha(repo_root / "third_party/minecraft-data/VERSION.txt")
    head_sha = git_head_sha(repo_root)
    try:
        import PIL  # type: ignore
        pillow_version = PIL.__version__
    except ImportError:
        sys.stderr.write("error: Pillow not installed (pip install Pillow)\n")
        return 2

    counts = {
        "examined": 0, "excl_air": 0, "excl_fluid": 0, "excl_admin": 0,
        "excl_partial": 0, "excl_no_texture": 0, "rows_written": 0,
    }
    fallback_counts = {p: 0 for p in (1, 2, 3, 4, 5, 6, 7, 8)}

    rows: list[tuple[str, int, int, int, int, int, bool, str]] = []
    with zipfile.ZipFile(args.jar) as jar:
        jar_namelist = jar.namelist()
        jar_nameset = set(jar_namelist)
        for block in sorted(blocks, key=lambda b: b["id"]):
            name = block["name"]
            counts["examined"] += 1
            if name in EXCLUDE_AIR:
                counts["excl_air"] += 1
                continue
            if name in EXCLUDE_FLUIDS:
                counts["excl_fluid"] += 1
                continue
            if name in EXCLUDE_ADMIN:
                counts["excl_admin"] += 1
                continue
            if not is_full_cube(name, block):
                counts["excl_partial"] += 1
                continue
            tex_path, pattern_id = resolve_texture(jar_nameset, jar_namelist, name)
            if tex_path is None:
                counts["excl_no_texture"] += 1
                sys.stderr.write(
                    f"warning: no texture found for {name!r}; excluding\n"
                )
                continue
            if pattern_id is not None:
                fallback_counts[pattern_id] += 1
                if pattern_id == 8:
                    sys.stderr.write(
                        f"warning: anomaly texture-pick for {name!r}: chose {tex_path!r}\n"
                    )
                else:
                    sys.stderr.write(
                        f"info: {name!r} resolved via fallback pattern {pattern_id}: {tex_path}\n"
                    )
            png_bytes = jar.read(tex_path)
            tint = pick_tint(name, constants)
            r, g, b = average_color_with_tint(png_bytes, tint)
            # tex_path is recorded relative to the jar root for portability.
            rows.append(
                (f"minecraft:{name}", block["filterLight"], block["emitLight"],
                 r, g, b, True, tex_path)
            )
            counts["rows_written"] += 1

    generated = _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    header_lines = [
        "# mc_version: 1.21.5",
        f"# prismarinejs_sha: {pin_sha}",
        f"# extractor_commit: {head_sha}",
        f"# extractor_pillow_version: {pillow_version}",
        f"# generated: {generated}",
        "# columns: name,opacity,luminance,r,g,b,is_full_cube,texture_path",
    ]
    body_lines = [
        f"{name},{op},{lu},{r},{g},{b},{1 if fc else 0},{tp}"
        for (name, op, lu, r, g, b, fc, tp) in rows
    ]

    # The CSV file is rows sorted by minecraft-data id (insertion order above)
    # preceded by header lines. `body_sha` is computed over the data rows only
    # so it is byte-stable across re-runs (the `# generated:` timestamp in the
    # header changes per run by design — it's an audit-trail field, not part
    # of the catalog identity).
    file_text = "\n".join(header_lines + body_lines) + "\n"
    body_sha = hashlib.sha256(("\n".join(body_lines) + "\n").encode()).hexdigest()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(file_text)

    fb_summary = ", ".join(
        f"p{p}={n}" for p, n in fallback_counts.items() if n > 0
    ) or "none"
    print(
        f"extracted {counts['rows_written']} blocks (examined {counts['examined']}); "
        f"excluded: air={counts['excl_air']}, fluid={counts['excl_fluid']}, "
        f"admin={counts['excl_admin']}, partial={counts['excl_partial']}, "
        f"no_texture={counts['excl_no_texture']}\n"
        f"fallback patterns used: {fb_summary}\n"
        f"output: {args.out}\n"
        f"sha256(body): {body_sha}",
        file=sys.stdout,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
