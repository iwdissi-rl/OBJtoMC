// matcher_transparent_acceptable_set — transparent Tier-4 proposal §3.7.2.
//
// For each of the 4 hand-written fixture materials (clear_glass, red_glass,
// opaque_red, opaque_white per research/transparent_tier4_proposal.md §1.6
// Kd/d values), constructs the mesh::Material, runs matcher::Matcher::match,
// and asserts the picked block name is in the §1.7 empirical acceptable-set.
//
// CRITICAL: prints the actual matcher pick to stdout for each material so
// the test report records the empirical top-1 picks. This stdout is the
// empirical reference for the Session 34 Tier-4's F3 cross-checks AND for
// any future palette-drift recalibration (matcher_proposal §6 "tighten not
// relax" posture).
//
// EXTRA vs Cornell Box (session-33 new discipline):
//   - Asserts the 4 top-1 picks are pairwise-distinct (mirrors proposal
//     §1.7's cross-material distinctness verification).
//   - Asserts branch-firing: transparent-material picks
//     (clear_glass + red_glass) MUST have opacity < 15 in the palette CSV
//     — proves the transparent branch actually fired. Opaque-material picks
//     (opaque_red + opaque_white) MUST have opacity == 15 — proves the
//     opaque branch fired (didn't accidentally route to transparent).
//     Catches a specific class of matcher-branch-dispatch bug that
//     Cornell Box's tests couldn't surface.
//
// Acceptable-sets per transparent_tier4_proposal §1.7 (derived by
// research/scratch/transparent/predict.py against the 414-entry
// data/block_palette_db.csv at Session 32):
//
//   clear_glass  (Kd=0.90/0.90/0.90; d=0.30) -> transparent branch
//     {white_stained_glass, beacon, glass, cherry_leaves, ice}
//   red_glass    (Kd=0.60/0.20/0.20; d=0.30) -> transparent branch
//     {red_stained_glass, brown_stained_glass, flowering_azalea_leaves,
//      pale_oak_leaves, birch_leaves}
//   opaque_red   (Kd=0.80/0.10/0.10; d=1.0)  -> opaque branch
//     {red_mushroom_block, red_glazed_terracotta, redstone_block,
//      red_concrete_powder, fire_coral_block}
//   opaque_white (Kd=0.90/0.90/0.90; d=1.0)  -> opaque branch
//     {white_concrete_powder, quartz_bricks, quartz_block, smooth_quartz,
//      quartz_pillar}
//
// STOP discipline on first-run FAIL (per proposal §3.10 + established
// discipline): if any material's pick is OUTSIDE its acceptable-set, this
// is the palette-drift signal. The implementation MUST NOT silently update
// the acceptable-set; instead re-run research/scratch/transparent/predict.py
// against the live palette CSV, compare to §1.7, and ask the maintainer.

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace mat = objtomc::matcher;
namespace pal = objtomc::palette;
namespace msh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Run one material check; print actual pick to stdout; assert it's in the
// acceptable-set; return the picked block name (for downstream assertions).
std::string check_material(mat::Matcher& matcher,
                           const std::string& material_name,
                           const msh::Material& m,
                           const std::set<std::string>& acceptable) {
    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(material_name + ": matcher.match threw: " + e.what());
    }
    std::printf("  %-12s -> %s\n", material_name.c_str(), result.name.c_str());

    if (acceptable.find(result.name) == acceptable.end()) {
        std::string set_str = "{";
        for (auto it = acceptable.begin(); it != acceptable.end(); ++it) {
            if (it != acceptable.begin()) set_str += ", ";
            set_str += *it;
        }
        set_str += "}";
        fail(material_name + " picked '" + result.name +
             "' which is NOT in §1.7 acceptable-set " + set_str +
             ". This is the palette-drift signal per transparent_tier4_"
             "proposal §3.10. STOP — do NOT silently update the test's "
             "acceptable-set. Re-run research/scratch/transparent/predict.py "
             "against the live palette CSV and ask the maintainer.");
    }
    return result.name;
}

// Look up the picked block in the palette and return its opacity.
// Fails if the pick is not in the palette (should never happen — the
// matcher only picks from the loaded palette).
std::uint8_t opacity_of(const pal::PaletteDB& db, const std::string& name) {
    const auto* entry = db.find(name);
    if (!entry) {
        fail("palette lookup failed for '" + name +
             "': matcher picked a block not in the loaded catalog "
             "(impossible — matcher bug or palette state corruption).");
    }
    return entry->opacity;
}

}  // namespace

int main() {
    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }
    mat::Matcher matcher(db, mat::default_options());

    std::printf("matcher_transparent_acceptable_set — empirical top-1 picks:\n");

    // ---- clear_glass (near-white transparent) ----
    std::string pick_clear;
    {
        msh::Material m{};
        m.name = "clear_glass";
        m.kd   = {0.90f, 0.90f, 0.90f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 0.30f;
        const std::set<std::string> acceptable = {
            "minecraft:white_stained_glass",
            "minecraft:beacon",
            "minecraft:glass",
            "minecraft:cherry_leaves",
            "minecraft:ice",
        };
        pick_clear = check_material(matcher, "clear_glass", m, acceptable);
    }

    // ---- red_glass (red-tinted transparent) ----
    std::string pick_red_glass;
    {
        msh::Material m{};
        m.name = "red_glass";
        m.kd   = {0.60f, 0.20f, 0.20f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 0.30f;
        const std::set<std::string> acceptable = {
            "minecraft:red_stained_glass",
            "minecraft:brown_stained_glass",
            "minecraft:flowering_azalea_leaves",
            "minecraft:pale_oak_leaves",
            "minecraft:birch_leaves",
        };
        pick_red_glass = check_material(matcher, "red_glass", m, acceptable);
    }

    // ---- opaque_red (saturated red opaque, contrast/backing) ----
    std::string pick_opaque_red;
    {
        msh::Material m{};
        m.name = "opaque_red";
        m.kd   = {0.80f, 0.10f, 0.10f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:red_mushroom_block",
            "minecraft:red_glazed_terracotta",
            "minecraft:redstone_block",
            "minecraft:red_concrete_powder",
            "minecraft:fire_coral_block",
        };
        pick_opaque_red = check_material(matcher, "opaque_red", m, acceptable);
    }

    // ---- opaque_white (near-white opaque, floor/back-right) ----
    std::string pick_opaque_white;
    {
        msh::Material m{};
        m.name = "opaque_white";
        m.kd   = {0.90f, 0.90f, 0.90f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:white_concrete_powder",
            "minecraft:quartz_bricks",
            "minecraft:quartz_block",
            "minecraft:smooth_quartz",
            "minecraft:quartz_pillar",
        };
        pick_opaque_white = check_material(matcher, "opaque_white", m, acceptable);
    }

    // ---- Pairwise-distinctness assertion (proposal §1.7 cross-material) ----
    // The 4 acceptable-sets are pairwise disjoint per §1.7, so an
    // in-acceptable-set pick automatically satisfies this — but assert
    // explicitly for parity with Cornell Box's condition (iii) discipline
    // and to catch any post-Session-32 palette shift that would create
    // an overlap.
    const std::array<std::pair<std::string, std::string>, 4> picks{{
        {"clear_glass",  pick_clear},
        {"red_glass",    pick_red_glass},
        {"opaque_red",   pick_opaque_red},
        {"opaque_white", pick_opaque_white},
    }};
    for (std::size_t i = 0; i < picks.size(); ++i) {
        for (std::size_t j = i + 1; j < picks.size(); ++j) {
            if (picks[i].second == picks[j].second) {
                fail("Pairwise-distinct FAIL: '" + picks[i].first + "' and '" +
                     picks[j].first + "' both picked '" + picks[i].second +
                     "'. Matcher failed to differentiate materials. This is a "
                     "matcher-discrimination bug, NOT a palette-drift signal.");
            }
        }
    }
    std::printf("  pairwise-distinct: OK (4 materials -> 4 distinct blocks)\n");

    // ---- Branch-firing assertions (Session 33 new discipline) ----
    // Transparent-material picks MUST have opacity < 15 (transparent-pool
    // membership per matcher_proposal §3.6). Opaque-material picks MUST
    // have opacity == 15 (opaque-branch full-palette pool, but empirically
    // opaque-branch top-1s for these Kd land on opacity=15 blocks).
    const std::uint8_t op_clear        = opacity_of(db, pick_clear);
    const std::uint8_t op_red_glass    = opacity_of(db, pick_red_glass);
    const std::uint8_t op_opaque_red   = opacity_of(db, pick_opaque_red);
    const std::uint8_t op_opaque_white = opacity_of(db, pick_opaque_white);

    if (op_clear >= 15) {
        fail("Branch-firing FAIL: clear_glass (d=0.3) picked '" + pick_clear +
             "' with opacity=" + std::to_string(op_clear) +
             " (>= 15). Transparent branch did NOT fire — matcher's `d < 0.5` "
             "dispatch is broken OR fixture's d value was misread by "
             "tinyobjloader.");
    }
    if (op_red_glass >= 15) {
        fail("Branch-firing FAIL: red_glass (d=0.3) picked '" + pick_red_glass +
             "' with opacity=" + std::to_string(op_red_glass) +
             " (>= 15). Transparent branch did NOT fire — see above.");
    }
    if (op_opaque_red < 15) {
        fail("Branch-firing FAIL: opaque_red (d=1.0) picked '" + pick_opaque_red +
             "' with opacity=" + std::to_string(op_opaque_red) +
             " (< 15). Opaque branch accidentally routed to transparent — "
             "matcher's branch dispatch is broken OR fixture's d value was "
             "misread.");
    }
    if (op_opaque_white < 15) {
        fail("Branch-firing FAIL: opaque_white (d=1.0) picked '" +
             pick_opaque_white + "' with opacity=" +
             std::to_string(op_opaque_white) + " (< 15). See above.");
    }
    std::printf("  branch-firing:     OK "
                "(transparent picks opacity<15; opaque picks opacity==15)\n");

    std::printf("PASS: matcher_transparent_acceptable_set "
                "(4 materials in §1.7 acceptable-sets; "
                "pairwise-distinct; branch-firing verified)\n");
    return 0;
}
