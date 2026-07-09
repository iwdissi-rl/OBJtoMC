#!/usr/bin/env python3
"""baseline.py — the frozen §2.1 metric evaluated against the three-tier
corpus. v0.2 item #4.

Evaluates the MatcherQualityScore (per matcher_quality_proposal §2.1) against:

  Tier I   — 8 maintainer-verified canonical fixture materials (Cornell Box 4
             + transparent scene 4). Ground truth per Sessions 31 + 34
             Tier-4 PASSes.
  Tier II  — 2128 synthetic materials, ported verbatim from
             tests/test_matcher_cpu_gpu_equality.cpp build_synthetic_batch
             (1728 opaque RGB cube + 200 emissive + 200 transparent).
  Tier III — 25 Sponza per-material average-colour materials from
             research/matcher_quality/sponza_texture_avg_colours.csv
             (all treated as opaque per §2.6 note; Sponza's MTL is
             uniformly opaque).

Emits Phase-3T + Phase-4T-preliminary scores per §2.4.2 maintainer edit. The
translucent-mismatch penalty term uses `research/matcher_quality/
renders_translucent_provisional.csv` (PROVISIONAL — superseded by sub-item
(c)'s Session 46 authoritative curation).

Weights are FROZEN per §2.4.1 maintainer edit at:
  w_opacity_mismatch      = 0.10
  w_luminance_mismatch    = 0.10
  w_translucent_mismatch  = 0.10

Retuning during item #4 execution requires explicit maintainer approval AND
re-running ALL prior sub-items' before/after comparisons under the new
weights. Do not touch these constants without a DECISIONS entry.

Emits penalty-term hit counts alongside scores — how many corpus entries
trigger each penalty. These are the interpretable face of the metric
("N solid materials currently pick see-through blocks").
"""
from __future__ import annotations

import csv
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

# Import the maintained matcher model.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from predict import (  # noqa: E402
    Material, PaletteEntry, load_palette, match, build_synthetic_batch,
    CANONICALS, oklab_distance, material_color_to_oklab, srgb_to_linear,
    linear_to_oklab, EMISSION_THRESHOLD, OPACITY_THRESHOLD,
    BRANCH_EMISSIVE, BRANCH_TRANSPARENT, BRANCH_OPAQUE, branch_for,
)

# ---------------------------------------------------------------------------
# Session 46 (c′) locked Phase-4T + corrected Phase-3T′ metric definition.
#
# Phase-3T (original filterLight ruler, Session 44) is retired. Session 44
# numbers stay in DECISIONS as historical record but no NEW metric line uses
# the filterLight ruler going forward (per translucency_pool_partitioning_
# proposal.md §5.3 re-baseline protocol + §5.4 metric definition change).
#
# Phase-4T-preliminary (used the provisional hand-audit CSV) is superseded
# by Phase-4T-locked (uses the authoritative renders_translucent column in
# data/block_palette_db.csv).
#
# Both Phase-3T′ (comparison-only, keeps luminance_mismatch retained) and
# Phase-4T (locked, adds translucent_mismatch on renders_translucent ruler)
# are emitted here.
# ---------------------------------------------------------------------------
PROJECT_ROOT = os.environ.get("OBJTOMC_ROOT", "/path/to/OBJtoMC")
SPONZA_CSV   = os.path.join(PROJECT_ROOT, "research", "matcher_quality",
                            "sponza_texture_avg_colours.csv")
TRANSLUCENT_CSV = os.path.join(PROJECT_ROOT, "research", "matcher_quality",
                               "renders_translucent_provisional.csv")


# ---------------------------------------------------------------------------
# FROZEN METRIC WEIGHTS (per matcher_quality_proposal §2.4.1 maintainer edit).
# DO NOT modify without maintainer approval AND full re-run of all prior
# sub-items' before/after comparisons under the new weights.
# ---------------------------------------------------------------------------
W_OPACITY     = 0.10
W_LUMINANCE   = 0.10
W_TRANSLUCENT = 0.10


# ---------------------------------------------------------------------------
# Corpus loaders
# ---------------------------------------------------------------------------
def load_sponza_csv(path: str = SPONZA_CSV) -> List[Material]:
    """Read the committed Sponza per-material avg-colour CSV → 25 Materials.

    Every row becomes an opaque non-emissive material with Kd = the CSV's
    average sRGB colour (uint8 → [0, 1] float). Sponza MTL is uniformly
    d=1.0, Ke=(0,0,0) — Tier III treats all 25 as opaque per §2.6.
    """
    mats: List[Material] = []
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            name, r, g, b = row[0], int(row[1]), int(row[2]), int(row[3])
            mats.append(Material(
                name=f"sponza:{name}",
                kd=(r / 255.0, g / 255.0, b / 255.0),
                ke=(0.0, 0.0, 0.0),
                d=1.0,
            ))
    return mats


def load_translucent_provisional(path: str = TRANSLUCENT_CSV
                                 ) -> dict:
    """Return {block_name: bool_renders_translucent} from the Session 44
    provisional hand-audit CSV. HISTORICAL — retained only for Session 44
    baseline reproduction. Session 46 (c′) makes the palette CSV's
    renders_translucent column authoritative; see palette_renders_
    translucent_map() below.

    Blocks not in the file are assumed renders_translucent = false
    (opacity=15 blocks OR palette entries the hand-audit didn't cover).
    """
    result: dict = {}
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            name = row[0]
            val  = row[1].strip().lower()
            result[name] = (val == "true")
    return result


def palette_renders_translucent_map(palette: List[PaletteEntry]) -> dict:
    """Return {block_name: bool_renders_translucent} from the authoritative
    palette CSV column (Session 46 c′ schema_version 2)."""
    return {e.name: e.renders_translucent for e in palette}


# ---------------------------------------------------------------------------
# §2.1 metric terms
# ---------------------------------------------------------------------------
def opacity_mismatch_penalty(m: Material, b: PaletteEntry) -> float:
    """§2.1: 1.0 iff (solid source picked see-through block) OR
    (transparent source picked solid block); else 0.0.
    """
    if m.d >= 0.5 and b.opacity < 15:
        return 1.0
    if m.d < 0.5 and b.opacity == 15:
        return 1.0
    return 0.0


def luminance_mismatch_penalty(m: Material, b: PaletteEntry) -> float:
    """§2.1: 1.0 for emissive-picks-non-luminous OR
    non-emissive-picks-high-luminance (>=10); 0.5 for
    non-emissive-picks-dim (luminance in [1,9]); else 0.0.
    """
    max_ke = max(m.ke)
    if max_ke > 0.5 and b.luminance == 0:
        return 1.0
    if max_ke <= 0.5 and b.luminance >= 10:
        return 1.0
    if max_ke <= 0.5 and 1 <= b.luminance <= 9:
        return 0.5
    return 0.0


def translucent_mismatch_penalty(m: Material, b: PaletteEntry,
                                 renders_translucent: dict) -> float:
    """Phase-4T (locked per Session 46 c′): 1.0 iff (m.d >= 0.5 AND
    b.renders_translucent) OR (m.d < 0.5 AND NOT b.renders_translucent);
    else 0.0. Two-directional per proposal §5.4.

    Under Phase-3T (original, retired) this term is IDENTICALLY ZERO
    (translucent_mismatch was not evaluated). Under Phase-4T-preliminary
    (retired) it was one-directional and used the provisional hand-audit.
    Under Phase-4T-locked it is two-directional and uses the authoritative
    palette CSV column.
    """
    is_translucent = renders_translucent.get(b.name, False)
    if m.d >= 0.5 and is_translucent:
        return 1.0
    if m.d < 0.5 and not is_translucent:
        return 1.0
    return 0.0


def score_pair(m: Material, b: PaletteEntry,
               renders_translucent: Optional[dict],
               phase: str) -> Tuple[float, dict]:
    """Return (score, per-term-hit-flags) for the (m, b) pair under the
    given phase.

    Supported phases:
      "phase3t"          — original Session 44 (filterLight ruler; opacity
                           + luminance terms; translucent term = 0).
                           Retained for historical replay of Session 44
                           numbers against unchanged code paths.
      "phase3t_prime"    — corrected Session 46+ (drops opacity term; keeps
                           luminance term; translucent term = 0). Used only
                           for direct pre/post-(c′) comparison alongside
                           phase4t_locked; new baseline numbers use
                           phase4t_locked.
      "phase4t_locked"   — Session 46 (c′) locked (perceptual + luminance
                           + translucent, two-directional, using authoritative
                           palette CSV column via renders_translucent map).
                           GOING-FORWARD standard ruler.
      "phase4t_prelim"   — RETIRED (Session 44 provisional hand-audit,
                           one-directional). Kept only for reproducing
                           Session 44's DECISIONS record.
    """
    branch = branch_for(m)
    target = material_color_to_oklab(m.ke if branch == BRANCH_EMISSIVE else m.kd)
    perceptual = oklab_distance(target, b.oklab)

    lum_pen = luminance_mismatch_penalty(m, b)

    # opacity term fires only under phase3t (retired, historical replay).
    op_pen = 0.0
    if phase == "phase3t":
        op_pen = opacity_mismatch_penalty(m, b)

    # translucent term fires under phase4t_prelim (historical) + phase4t_locked
    # (going-forward). phase3t + phase3t_prime keep it at zero.
    trans_pen = 0.0
    if phase in ("phase4t_prelim", "phase4t_locked") and renders_translucent is not None:
        trans_pen = translucent_mismatch_penalty(m, b, renders_translucent)

    score = (perceptual
             + W_OPACITY     * op_pen
             + W_LUMINANCE   * lum_pen
             + W_TRANSLUCENT * trans_pen)

    return score, {
        "opacity_hit":      op_pen > 0,
        "luminance_hit":    lum_pen > 0,
        "luminance_half":   (lum_pen == 0.5),
        "translucent_hit":  trans_pen > 0,
        "perceptual":       perceptual,
    }


# ---------------------------------------------------------------------------
# Tier evaluation
# ---------------------------------------------------------------------------
@dataclass
class TierResult:
    tier_name: str
    n: int
    mean_score: float
    perceptual_mean: float
    opacity_hits: int
    luminance_hits: int
    luminance_half_hits: int
    translucent_hits: int
    top1: List[Tuple[str, str]]  # (material_name, pick_name) — for display


def evaluate_tier(name: str, materials: List[Material],
                  palette: List[PaletteEntry],
                  renders_translucent: Optional[dict],
                  phase: str,
                  record_top1: bool) -> TierResult:
    scores: List[float] = []
    perceptual_sum = 0.0
    op_hits = lum_hits = lum_half = trans_hits = 0
    top1: List[Tuple[str, str]] = []
    for m in materials:
        pick = match(m, palette)
        score, hits = score_pair(m, pick, renders_translucent, phase)
        scores.append(score)
        perceptual_sum += hits["perceptual"]
        if hits["opacity_hit"]:      op_hits    += 1
        if hits["luminance_hit"]:    lum_hits   += 1
        if hits["luminance_half"]:   lum_half   += 1
        if hits["translucent_hit"]:  trans_hits += 1
        if record_top1:
            top1.append((m.name, pick.name))
    n = len(materials)
    return TierResult(
        tier_name=name, n=n,
        mean_score=sum(scores) / n if n else 0.0,
        perceptual_mean=perceptual_sum / n if n else 0.0,
        opacity_hits=op_hits,
        luminance_hits=lum_hits,
        luminance_half_hits=lum_half,
        translucent_hits=trans_hits,
        top1=top1,
    )


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
def format_tier(t: TierResult, phase: str) -> str:
    lines = [f"{t.tier_name} ({phase}): n={t.n}"]
    lines.append(f"  mean MatcherQualityScore      : {t.mean_score:.4f}")
    lines.append(f"  mean perceptual (Oklab)       : {t.perceptual_mean:.4f}")
    if phase == "phase3t":
        lines.append(f"  opacity_mismatch hits         : {t.opacity_hits}"
                     f"  ({100.0 * t.opacity_hits / t.n:.1f}% of corpus)")
    lines.append(f"  luminance_mismatch full hits  : {t.luminance_hits}"
                 f"  ({100.0 * t.luminance_hits / t.n:.1f}% of corpus)")
    lines.append(f"  luminance_mismatch half hits  : {t.luminance_half_hits}"
                 f"  ({100.0 * t.luminance_half_hits / t.n:.1f}% of corpus)")
    if phase in ("phase4t_prelim", "phase4t_locked"):
        lines.append(f"  translucent_mismatch hits     : {t.translucent_hits}"
                     f"  ({100.0 * t.translucent_hits / t.n:.1f}% of corpus)")
    return "\n".join(lines)


def main() -> int:
    print("=" * 70)
    print("BASELINE (Session 46 (c′) locked) — matcher_quality_proposal §2.1")
    print("+ translucency_pool_partitioning_proposal §5.4 metric refinement")
    print("=" * 70)
    print(f"FROZEN weights (§2.4.1): "
          f"w_opacity={W_OPACITY}  w_luminance={W_LUMINANCE}  "
          f"w_translucent={W_TRANSLUCENT}")
    print()

    palette = load_palette()
    print(f"Loaded {len(palette)} full-cube palette entries.")
    rt_map_authoritative = palette_renders_translucent_map(palette)
    n_rt_true = sum(1 for v in rt_map_authoritative.values() if v)
    print(f"  renders_translucent=true (authoritative, Session 46 c′): {n_rt_true}")
    print(f"  renders_translucent=false: {len(palette) - n_rt_true}")

    tier1_mats = list(CANONICALS)
    tier2_mats = build_synthetic_batch()
    tier3_mats = load_sponza_csv()
    print(f"Tier I  : {len(tier1_mats)} canonicals")
    print(f"Tier II : {len(tier2_mats)} synthetic (1728 opaque + 200 emit + 200 trans)")
    print(f"Tier III: {len(tier3_mats)} Sponza per-texture avg colours")
    print()

    all_tiers = []

    print("-" * 70)
    print("Phase-3T′ (corrected, Session 46 c′): perceptual + luminance only.")
    print("Dropped opacity_mismatch (was filterLight ruler, retired per §5.4).")
    print("Reference-only for pre/post-(c′) comparison; going-forward is")
    print("phase4t_locked.")
    print("-" * 70)
    for phase in ["phase3t_prime"]:
        for name, mats, record in (("Tier I", tier1_mats, True),
                                   ("Tier II", tier2_mats, False),
                                   ("Tier III", tier3_mats, True)):
            t = evaluate_tier(name, mats, palette,
                              rt_map_authoritative, phase, record)
            print(format_tier(t, phase))
            print()
            all_tiers.append((phase, t))

    print("-" * 70)
    print("Phase-4T-locked (Session 46 c′ LOCKED): perceptual + luminance +")
    print("translucent_mismatch (two-directional, using authoritative palette")
    print("CSV column). GOING-FORWARD standard ruler.")
    print("-" * 70)
    for phase in ["phase4t_locked"]:
        for name, mats, record in (("Tier I", tier1_mats, True),
                                   ("Tier II", tier2_mats, False),
                                   ("Tier III", tier3_mats, True)):
            t = evaluate_tier(name, mats, palette,
                              rt_map_authoritative, phase, record)
            print(format_tier(t, phase))
            print()
            all_tiers.append((phase, t))

    # Combined mean per phase (unweighted mean of per-tier means).
    print("-" * 70)
    print("Combined summary (mean of per-tier means, unweighted)")
    print("-" * 70)
    for phase in ["phase3t_prime", "phase4t_locked"]:
        tiers = [t for p, t in all_tiers if p == phase]
        combined = sum(t.mean_score for t in tiers) / len(tiers)
        print(f"  {phase:16s} combined mean MatcherQualityScore: {combined:.4f}")
    print()

    # Canonical picks under phase4t_locked — for evolution table post-(c′)
    # column fill (proposal §3.6 discipline).
    print("-" * 70)
    print("Canonical pick evolution table — post-(c′) [merged a+c] column")
    print("-" * 70)
    for phase, t in all_tiers:
        if t.tier_name != "Tier I" or phase != "phase4t_locked":
            continue
        for mat_name, pick_name in t.top1:
            print(f"  {mat_name:20s} -> {pick_name}")
    print()

    # Tier III picks — help interpret Sponza Tier III result.
    for phase, t in all_tiers:
        if t.tier_name != "Tier III" or phase != "phase4t_locked":
            continue
        print("Tier III Sponza picks (post-(c′) matcher):")
        for mat_name, pick_name in t.top1:
            print(f"  {mat_name:35s} -> {pick_name}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
