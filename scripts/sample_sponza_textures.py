#!/usr/bin/env python3
"""sample_sponza_textures.py — one-off maintainer script (Session 44).

Produces `research/matcher_quality/sponza_texture_avg_colours.csv` — the 25-row
per-material average-colour table that Tier III of the item #4 metric evaluates
against. This script is committed alongside the CSV; the ~50 MiB PNG textures
are NOT committed (matcher_quality_proposal §3.5.4 CSV-committed-PNGs-unvendored
policy).

## Averaging semantics (fixed, do not change without discussion)

Per matcher_quality_proposal §4(e) recommendation: **average in linear-light**
after sRGB→linear decode, encode back to sRGB-gamma before storing the RGB
value in the CSV, unweighted arithmetic mean over pixels. Fully-transparent
pixels (alpha == 0) are excluded. No pixel-value threshold below that.

Rationale: physically-correct averaging is done in linear-light because sRGB
is a perceptual transfer curve; averaging in gamma-space biases the mean
toward the darker end of the range. The CSV is consumed by baseline.py, which
sRGB-decodes it again to linear before Oklab conversion — round-tripping
through sRGB-gamma is wasteful but matches how the eventual C++ sub-item (e)
implementation is expected to look (see §4(e) Option E-ii).

## Idempotence

Each row's `source_texture_sha256` column pins the exact source texture bytes
this CSV was derived from. Re-running the script against a re-fetched Sponza
archive with the SAME texture bytes produces a byte-identical CSV. If any
source texture SHA changes (upstream mesh update), the affected CSV row's
average colour re-derives; the maintainer commits the delta consciously.

## Fallback

If a Sponza material's map_Kd texture cannot be located OR the file cannot be
decoded, the CSV row falls back to that material's raw MTL Kd value and the
`source_texture` column is set to "mtl_kd" (constraint from Session 44
prompt). No row is skipped; every material in `sponza.mtl` becomes one row.

## Usage

    # 1. Fetch Sponza archive to a scratch dir (~80 MiB, NOT committed):
    curl -o /tmp/sponza.zip \
        https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip
    unzip -d /tmp/sponza_extracted /tmp/sponza.zip

    # 2. Run this script pointing at the extracted textures dir + MTL:
    scripts/sample_sponza_textures.py \
        --mtl     /tmp/sponza_extracted/sponza.mtl \
        --textures /tmp/sponza_extracted/textures \
        --out     research/matcher_quality/sponza_texture_avg_colours.csv

Session 44 fetched the archive from the canonical URL; SHA-256 of the fetched
zip matches the DECISIONS 2026-07-04 Sponza pin `da005cbe...`; the script's
first-run output is what commits.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import os
import re
import sys
from pathlib import Path
from typing import List, Optional, Tuple

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    print("FATAL: Pillow is required (pip install Pillow)", file=sys.stderr)
    raise


# ---------------------------------------------------------------------------
# sRGB math (mirrors research/matcher_quality/predict.py's srgb_to_linear).
# ---------------------------------------------------------------------------
def srgb_to_linear_u8(u: int) -> float:
    c = u / 255.0
    if c <= 0.04045:
        return c / 12.92
    return ((c + 0.055) / 1.055) ** 2.4


def linear_to_srgb_u8(x: float) -> int:
    if x <= 0.0031308:
        v = x * 12.92
    else:
        v = 1.055 * (x ** (1.0 / 2.4)) - 0.055
    return max(0, min(255, round(v * 255.0)))


# ---------------------------------------------------------------------------
# MTL parsing — small custom parser; extracts (name, Kd, map_Kd) per material.
# The MTL uses backslash paths on the textures/ prefix; we normalise.
# ---------------------------------------------------------------------------
NEWMTL_RE  = re.compile(r"^\s*newmtl\s+(\S+)")
KD_RE      = re.compile(r"^\s*Kd\s+([^\s]+)\s+([^\s]+)\s+([^\s]+)")
MAP_KD_RE  = re.compile(r"^\s*map_Kd\s+(\S+)")


def parse_sponza_mtl(path: Path) -> List[dict]:
    """Return list of {name, kd:(r,g,b), map_kd:str-or-None} in MTL order."""
    materials: List[dict] = []
    current: Optional[dict] = None
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            m = NEWMTL_RE.match(line)
            if m:
                if current is not None:
                    materials.append(current)
                current = {"name": m.group(1),
                           "kd": (1.0, 1.0, 1.0),
                           "map_kd": None}
                continue
            if current is None:
                continue
            m = KD_RE.match(line)
            if m:
                current["kd"] = (float(m.group(1)),
                                 float(m.group(2)),
                                 float(m.group(3)))
                continue
            m = MAP_KD_RE.match(line)
            if m:
                # MTL uses backslash paths; take the basename.
                current["map_kd"] = m.group(1).replace("\\", "/").split("/")[-1]
                continue
    if current is not None:
        materials.append(current)
    return materials


# ---------------------------------------------------------------------------
# Texture averaging.
# ---------------------------------------------------------------------------
def average_texture_linear_srgb(path: Path
                                ) -> Tuple[float, float, float, int]:
    """Return (avg_R_linear, avg_G_linear, avg_B_linear, contributing_px_count)
    averaged in linear-light. Fully-transparent pixels are excluded.

    Uses PIL to decode; converts to RGB or RGBA depending on the source mode.
    """
    with Image.open(path) as img:
        mode = img.mode
        if mode in ("P", "LA"):
            img = img.convert("RGBA")
            mode = "RGBA"
        elif mode == "L":
            img = img.convert("RGB")
            mode = "RGB"
        elif mode == "RGB":
            pass
        elif mode == "RGBA":
            pass
        else:
            img = img.convert("RGBA")
            mode = "RGBA"

        px = img.load()
        w, h = img.size

        sum_r = sum_g = sum_b = 0.0
        count = 0
        if mode == "RGBA":
            for y in range(h):
                for x in range(w):
                    r, g, b, a = px[x, y]  # type: ignore[misc]
                    if a == 0:
                        continue
                    sum_r += srgb_to_linear_u8(r)
                    sum_g += srgb_to_linear_u8(g)
                    sum_b += srgb_to_linear_u8(b)
                    count += 1
        else:  # RGB
            for y in range(h):
                for x in range(w):
                    r, g, b = px[x, y]  # type: ignore[misc]
                    sum_r += srgb_to_linear_u8(r)
                    sum_g += srgb_to_linear_u8(g)
                    sum_b += srgb_to_linear_u8(b)
                    count += 1

        if count == 0:
            return (0.0, 0.0, 0.0, 0)
        return (sum_r / count, sum_g / count, sum_b / count, count)


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(65536)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# CSV emission.
# ---------------------------------------------------------------------------
CSV_HEADER = [
    "material_name",
    "avg_r_srgb_gamma_u8",
    "avg_g_srgb_gamma_u8",
    "avg_b_srgb_gamma_u8",
    "source_texture",
    "source_texture_sha256",
    "sample_pixel_count",
]

CSV_LEADIN = """\
# sponza_texture_avg_colours.csv — per-material average sRGB colour, Tier III
# corpus of matcher_quality_proposal §2.3 (v0.2 item #4).
#
# Averaging: unweighted mean of source PNG pixels in LINEAR-LIGHT sRGB
# (physically-correct); result re-encoded to sRGB-gamma uint8 for storage.
# Fully-transparent pixels (alpha == 0) excluded. See scripts/sample_sponza_
# textures.py header for details.
#
# Materials whose map_Kd could not be located OR decoded fall back to the raw
# MTL Kd value with source_texture = "mtl_kd" and source_texture_sha256
# = "n/a"; sample_pixel_count = 0.
#
# Sponza source: https://casual-effects.com/g3d/data10/common/model/
# crytek_sponza/sponza.zip
# Archive SHA-256: da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169
# a27c2770c
# License: CC BY 3.0 (attribution chain: Dabrovic 2002 -> Meinl+Crytek 2010
# -> McGuire 2011 -> objtomc 2026). This CSV is a derivative work under CC
# BY 3.0; PNGs unvendored per matcher_quality_proposal §3.5.4.
"""


def kd_to_gamma_u8(kd_linear_or_srgb: Tuple[float, float, float]
                   ) -> Tuple[int, int, int]:
    """MTL Kd values are already sRGB-gamma; clamp + scale to uint8."""
    return (max(0, min(255, round(kd_linear_or_srgb[0] * 255.0))),
            max(0, min(255, round(kd_linear_or_srgb[1] * 255.0))),
            max(0, min(255, round(kd_linear_or_srgb[2] * 255.0))))


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--mtl", required=True, type=Path,
                   help="Path to sponza.mtl")
    p.add_argument("--textures", required=True, type=Path,
                   help="Path to the extracted textures/ directory")
    p.add_argument("--out", required=True, type=Path,
                   help="Output CSV path")
    args = p.parse_args(argv)

    materials = parse_sponza_mtl(args.mtl)
    print(f"Parsed {len(materials)} materials from {args.mtl}", file=sys.stderr)

    rows = []
    for m in materials:
        name = m["name"]
        map_kd = m["map_kd"]

        if map_kd is None:
            r, g, b = kd_to_gamma_u8(m["kd"])
            rows.append([name, r, g, b, "mtl_kd", "n/a", 0])
            print(f"  {name}: no map_Kd -> mtl_kd ({r},{g},{b})",
                  file=sys.stderr)
            continue

        texture_path = args.textures / map_kd
        if not texture_path.is_file():
            r, g, b = kd_to_gamma_u8(m["kd"])
            rows.append([name, r, g, b, "mtl_kd", "n/a", 0])
            print(f"  {name}: texture {texture_path} missing -> mtl_kd "
                  f"({r},{g},{b})", file=sys.stderr)
            continue

        try:
            avg_r_lin, avg_g_lin, avg_b_lin, count = \
                average_texture_linear_srgb(texture_path)
        except Exception as ex:  # pragma: no cover
            r, g, b = kd_to_gamma_u8(m["kd"])
            rows.append([name, r, g, b, "mtl_kd", "n/a", 0])
            print(f"  {name}: texture decode failed ({ex!r}) -> mtl_kd "
                  f"({r},{g},{b})", file=sys.stderr)
            continue

        r = linear_to_srgb_u8(avg_r_lin)
        g = linear_to_srgb_u8(avg_g_lin)
        b = linear_to_srgb_u8(avg_b_lin)
        sha = sha256_of(texture_path)
        rows.append([name, r, g, b, map_kd, sha, count])
        print(f"  {name}: {map_kd} ({count}px) -> ({r},{g},{b})",
              file=sys.stderr)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.out, "w", newline="") as f:
        f.write(CSV_LEADIN)
        f.write("# columns: " + ",".join(CSV_HEADER) + "\n")
        w = csv.writer(f)
        for row in rows:
            w.writerow(row)

    print(f"\nWrote {len(rows)} rows to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
