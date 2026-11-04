#!/usr/bin/env python3
"""Consolidated matcher prediction script (v0.2 item #4).

This is the SINGLE MAINTAINED reproduction of `src/matcher.cpp`'s algorithm
in Python. Every scoring change in item #4's sub-items (a), (b), (c) updates
this file FIRST (predict.py re-prediction committed BEFORE the implementation
commit per proposal §3.6), then updates matcher.cpp + matcher_gpu.cu to
match. Every `baseline.py` run consults this module for its Tier-I/II/III
material→pick mapping. Every ctest whose acceptable set flips consults this
module for the new set.

Replaces the three near-copy scratch scripts:
  research/scratch/cornell_box/predict.py     (Session 29)
  research/scratch/transparent/predict.py     (Session 32)
  research/scratch/scale/predict.py           (Session 35)
The scratch copies stay UNTOUCHED as historical record; new work uses this
file.

Algorithm mirrored verbatim from src/matcher.cpp:
  1. sRGB-gamma → linear (IEC 61966-2-1 piecewise; clamped at 0 on negatives,
     no upper clamp — Cornell light Ke = (17, 12, 4) exceeds 1).
  2. linear → Oklab (Björn Ottosson 2020; 18 canonical coefficients).
  3. Three-branch dispatch:
       max(ke) > emission_threshold (0.5)     → EMISSIVE  (pool = lum > 0)
       d < opacity_threshold (0.5)            → TRANSPARENT (pool = op < 15)
       otherwise                              → OPAQUE   (full palette)
     Target Oklab derived from material.ke for emissive branch;
     from material.kd for the other two.
  4. Score = perceptual + branch-specific proximity bonus:
       emissive:      score = oklab_distance + 0.05 * |max_ke*15 - lum|
       transparent:   score = oklab_distance + 0.05 * |d*15    - opacity|
       opaque:        score = oklab_distance
  5. Tie-break key (LEX ascending): (score, opacity_order, luminance_order,
                                     name)
       opacity_order   = (transparent) ? +opacity : -opacity
       luminance_order = (emissive)    ? -luminance : +luminance
       name comparison = std::string alphabetical
     This mirrors src/matcher.cpp TieBreakKey::operator< line 148.

Notes:
- This module is IMPORTED by baseline.py; it does not have side effects at
  import time. Run as a script to print human-readable predictions for the
  canonical fixture materials (behaviour parity with the scratch scripts it
  replaces).
- Weights (emission_threshold, opacity_threshold, proximity weights) are
  MatcherOptions::default_options() values from src/matcher.cpp line 158.
  These are the MATCHER's knobs; DO NOT confuse with the METRIC's weights
  in baseline.py (which are frozen per §2.4.1).
"""
from __future__ import annotations

import csv
import math
import os
from dataclasses import dataclass, field
from typing import Iterable, List, Optional, Sequence, Tuple

PROJECT_ROOT = os.environ.get("OBJTOMC_ROOT", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
PALETTE_CSV = os.path.join(PROJECT_ROOT, "data", "block_palette_db.csv")

# MatcherOptions::default_options() from src/matcher.cpp line 158.
EMISSION_THRESHOLD = 0.5
OPACITY_THRESHOLD  = 0.5
LUMINANCE_PROXIMITY_WEIGHT = 0.05
OPACITY_PROXIMITY_WEIGHT   = 0.05


# ---------------------------------------------------------------------------
# Color math
# ---------------------------------------------------------------------------
def srgb_to_linear_channel(c: float) -> float:
    """IEC 61966-2-1 sRGB → linear, verbatim from src/matcher.cpp line 30."""
    if c < 0:
        return 0.0
    if c <= 0.04045:
        return c / 12.92
    return ((c + 0.055) / 1.055) ** 2.4


def srgb_to_linear(rgb: Sequence[float]) -> Tuple[float, float, float]:
    return (srgb_to_linear_channel(rgb[0]),
            srgb_to_linear_channel(rgb[1]),
            srgb_to_linear_channel(rgb[2]))


def linear_to_oklab(r: float, g: float, b: float) -> Tuple[float, float, float]:
    """Björn Ottosson 2020, coefficients verbatim from src/matcher.cpp line 56."""
    l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b
    m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b
    s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b
    # cbrt with sign preservation (Python's ** loses sign on negatives).
    l_ = math.copysign(abs(l) ** (1.0 / 3.0), l)
    m_ = math.copysign(abs(m) ** (1.0 / 3.0), m)
    s_ = math.copysign(abs(s) ** (1.0 / 3.0), s)
    L  = 0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_
    a  = 1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_
    bb = 0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
    return (L, a, bb)


def material_color_to_oklab(srgb_gamma: Sequence[float]) -> Tuple[float, float, float]:
    """Combined srgb_to_linear + linear_to_oklab per src/matcher.cpp line 79."""
    return linear_to_oklab(*srgb_to_linear(srgb_gamma))


def oklab_distance(p: Tuple[float, float, float],
                   q: Tuple[float, float, float]) -> float:
    dL = p[0] - q[0]
    da = p[1] - q[1]
    db = p[2] - q[2]
    return math.sqrt(dL * dL + da * da + db * db)


# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------
@dataclass
class PaletteEntry:
    name: str
    opacity: int      # 0..15 — MC filterLight (light propagation, NOT visual translucency)
    luminance: int    # 0..15 — MC emitLight
    r: int            # sRGB uint8
    g: int
    b: int
    is_full_cube: bool
    texture_path: str
    renders_translucent: bool = False  # Session 46 (c′): visual-translucency ruler
    oklab: Tuple[float, float, float] = field(default=(0.0, 0.0, 0.0))


def load_palette(path: str = PALETTE_CSV) -> List[PaletteEntry]:
    """Load `data/block_palette_db.csv`, filter to full-cube, pre-convert Oklab.

    Mirrors `src/palette.cpp`'s CSV load + gamma-decode + `Matcher` sidecar
    Oklab pre-conversion. Session 46 schema_version 2 adds `renders_translucent`
    as the 9th column.
    """
    entries: List[PaletteEntry] = []
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            try:
                name = row[0]
                opacity = int(row[1])
                luminance = int(row[2])
                r = int(row[3]); g = int(row[4]); b = int(row[5])
                is_full_cube = row[6] in ("1", "true", "True")
                texture_path = row[7] if len(row) > 7 else ""
                rt_str = row[8].strip().lower() if len(row) > 8 else "false"
                renders_translucent = rt_str in ("1", "true")
            except (IndexError, ValueError):
                continue
            if not is_full_cube:
                continue
            # CSV r/g/b are sRGB uint8; palette.cpp decodes to linear at load
            # time then Matcher pre-converts to Oklab. We match that here.
            lin = srgb_to_linear((r / 255.0, g / 255.0, b / 255.0))
            ok  = linear_to_oklab(*lin)
            entries.append(PaletteEntry(
                name=name, opacity=opacity, luminance=luminance,
                r=r, g=g, b=b, is_full_cube=is_full_cube,
                texture_path=texture_path,
                renders_translucent=renders_translucent,
                oklab=ok,
            ))
    return entries


# ---------------------------------------------------------------------------
# Material + Branch + Matcher
# ---------------------------------------------------------------------------
@dataclass
class Material:
    name: str
    kd: Tuple[float, float, float] = (0.5, 0.5, 0.5)
    ke: Tuple[float, float, float] = (0.0, 0.0, 0.0)
    d: float = 1.0


BRANCH_EMISSIVE    = "emissive"
BRANCH_TRANSPARENT = "transparent"
BRANCH_OPAQUE      = "opaque"


def branch_for(m: Material) -> str:
    """src/matcher.cpp branch dispatch line 244."""
    if max(m.ke) > EMISSION_THRESHOLD:
        return BRANCH_EMISSIVE
    if m.d < OPACITY_THRESHOLD:
        return BRANCH_TRANSPARENT
    return BRANCH_OPAQUE


def score_and_key(m: Material, e: PaletteEntry,
                  branch: str, target_ok: Tuple[float, float, float]
                  ) -> Optional[Tuple[float, int, int, str]]:
    """Return the TieBreakKey tuple for (m, e) or None if e is filtered out
    of this branch's pool. Mirrors src/matcher.cpp lines 264-303 exactly,
    including the Session 46 (c′) two-pool partition on transparent + opaque
    branches per research/translucency_pool_partitioning_proposal.md §4.
    """
    # Pool filter (emissive unchanged; transparent + opaque partitioned by
    # renders_translucent per Session 46 c′).
    if branch == BRANCH_EMISSIVE    and e.luminance == 0:            return None
    if branch == BRANCH_TRANSPARENT and not e.renders_translucent:    return None
    if branch == BRANCH_OPAQUE      and e.renders_translucent:       return None

    perceptual = oklab_distance(target_ok, e.oklab)
    score = perceptual
    if branch == BRANCH_EMISSIVE:
        score += LUMINANCE_PROXIMITY_WEIGHT * abs(max(m.ke) * 15.0 - e.luminance)
    elif branch == BRANCH_TRANSPARENT:
        score += OPACITY_PROXIMITY_WEIGHT   * abs(m.d      * 15.0 - e.opacity)

    # Orientation per src/matcher.cpp line 293-300.
    opacity_order   = e.opacity   if branch == BRANCH_TRANSPARENT else -e.opacity
    luminance_order = -e.luminance if branch == BRANCH_EMISSIVE   else  e.luminance

    return (score, opacity_order, luminance_order, e.name)


def match(m: Material, palette: List[PaletteEntry]) -> PaletteEntry:
    """The consolidated matcher: choose the palette entry with the minimum
    tie-break key for material m. Byte-parity target = src/matcher.cpp.
    """
    branch = branch_for(m)
    if branch == BRANCH_EMISSIVE:
        target = material_color_to_oklab(m.ke)
    else:
        target = material_color_to_oklab(m.kd)

    best_key: Optional[Tuple[float, int, int, str]] = None
    best_entry: Optional[PaletteEntry] = None
    for e in palette:
        key = score_and_key(m, e, branch, target)
        if key is None:
            continue
        if best_key is None or key < best_key:
            best_key = key
            best_entry = e
    if best_entry is None:
        raise RuntimeError(
            f"empty candidate pool for branch {branch} on material {m.name!r}")
    return best_entry


def top_k(m: Material, palette: List[PaletteEntry], k: int = 5
          ) -> Tuple[str, Tuple[float, float, float],
                     List[Tuple[float, int, int, str]]]:
    """Full ranked top-K under the branch's pool + score."""
    branch = branch_for(m)
    if branch == BRANCH_EMISSIVE:
        target = material_color_to_oklab(m.ke)
    else:
        target = material_color_to_oklab(m.kd)
    ranked: List[Tuple[float, int, int, str]] = []
    for e in palette:
        key = score_and_key(m, e, branch, target)
        if key is not None:
            ranked.append(key)
    ranked.sort()
    return branch, target, ranked[:k]


# ---------------------------------------------------------------------------
# Canonical Tier-I materials (the 8 maintainer-verified fixture picks).
# Values verbatim from tests/test_matcher_cornell_box_acceptable_set.cpp
# and tests/test_matcher_transparent_acceptable_set.cpp.
# ---------------------------------------------------------------------------
CANONICALS: List[Material] = [
    Material("cornell_leftWall",  (0.63, 0.065, 0.05), (0.0, 0.0, 0.0), 1.0),
    Material("cornell_rightWall", (0.14, 0.45,  0.091),(0.0, 0.0, 0.0), 1.0),
    Material("cornell_white",     (0.725,0.71,  0.68), (0.0, 0.0, 0.0), 1.0),
    Material("cornell_light",     (0.78, 0.78,  0.78), (17.0, 12.0, 4.0), 1.0),
    Material("clear_glass",       (0.90, 0.90, 0.90),  (0.0, 0.0, 0.0), 0.30),
    Material("red_glass",         (0.60, 0.20, 0.20),  (0.0, 0.0, 0.0), 0.30),
    Material("opaque_red",        (0.80, 0.10, 0.10),  (0.0, 0.0, 0.0), 1.0),
    Material("opaque_white",      (0.90, 0.90, 0.90),  (0.0, 0.0, 0.0), 1.0),
]


# ---------------------------------------------------------------------------
# 2128 synthetic sweep (Tier-II corpus for baseline.py).
# Ported verbatim from tests/test_matcher_cpu_gpu_equality.cpp
# `build_synthetic_batch` lines 36-79. Order preserved so indexing matches
# the ctest's material vector.
# ---------------------------------------------------------------------------
def build_synthetic_batch() -> List[Material]:
    mats: List[Material] = []
    # 12x12x12 = 1728 opaque.
    for r in range(12):
        for g in range(12):
            for b in range(12):
                m = Material(
                    name=f"opaque_{r}_{g}_{b}",
                    kd=(r / 11.0, g / 11.0, b / 11.0),
                    ke=(0.0, 0.0, 0.0),
                    d=1.0,
                )
                mats.append(m)
    # 200 emissive.
    for i in range(200):
        hue = i / 200.0
        boost = 0.6 + (i % 20) * 0.02
        mats.append(Material(
            name=f"emit_{i}",
            kd=(0.4 + 0.3 * hue, 0.5, 0.4 + 0.3 * (1.0 - hue)),
            ke=(boost * (1.0 - hue), boost * 0.5, boost * hue),
            d=1.0,
        ))
    # 200 transparent.
    for i in range(200):
        hue = i / 200.0
        mats.append(Material(
            name=f"trans_{i}",
            kd=(hue, 0.4 + 0.3 * (1.0 - hue), 1.0 - hue),
            ke=(0.0, 0.0, 0.0),
            d=0.15 + 0.3 * hue,
        ))
    return mats


# ---------------------------------------------------------------------------
# CLI: predict picks for the 8 canonicals (parity with scratch predict.py's
# output shape). Behavioural parity, not byte-parity — the scratch scripts
# printed slightly different formatting; this consolidated version is the
# authoritative going-forward output.
# ---------------------------------------------------------------------------
def _print_prediction(m: Material, palette: List[PaletteEntry], k: int = 5) -> None:
    branch, target_ok, picks = top_k(m, palette, k=k)
    L, a, b = target_ok
    print(f"=== {m.name} (Kd={m.kd}, Ke={m.ke}, d={m.d}) → branch={branch} ===")
    print(f"    target Oklab = (L={L:.4f}, a={a:.4f}, b={b:.4f})")
    for score, opord, lumord, name in picks:
        # Look up opacity+luminance from the palette for display.
        entry = next(e for e in palette if e.name == name)
        print(f"      score={score:.4f}  op={entry.opacity:2d}  "
              f"lum={entry.luminance:2d}  {name}")
    print()


def main() -> int:
    palette = load_palette()
    print(f"Loaded {len(palette)} full-cube palette entries.")
    n_emissive_pool    = sum(1 for e in palette if e.luminance > 0)
    n_transparent_pool = sum(1 for e in palette if e.opacity < 15)
    print(f"  emissive pool    (luminance > 0):  {n_emissive_pool}")
    print(f"  transparent pool (opacity < 15):   {n_transparent_pool}")
    print()
    for m in CANONICALS:
        _print_prediction(m, palette)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
