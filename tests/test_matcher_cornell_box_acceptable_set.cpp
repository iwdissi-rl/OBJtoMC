// matcher_cornell_box_acceptable_set — Cornell Box proposal §3.7.2.
//
// For each of the Cornell Box's 4 distinct Material variants (red leftWall,
// green rightWall, white surface = floor/ceiling/backWall/shortBox/tallBox,
// emissive light), constructs the mesh::Material with the exact Kd/Ke/d
// values from CornellBox-Original.mtl, runs matcher::Matcher::match, and
// asserts the picked block name is in the §1.7 empirical acceptable-set.
//
// CRITICAL: prints the actual matcher pick to stdout for each material so
// the test report records the empirical top-1 picks. This stdout becomes
// the empirical reference for the Session 31 Tier-4's F3 cross-checks AND
// for any future palette-drift recalibration (matcher_proposal §6 "tighten
// not relax" posture).
//
// Acceptable-sets per cornell_box_tier4_proposal §1.7 (derived by
// research/scratch/cornell_box/predict.py against the 414-entry
// data/block_palette_db.csv at Session 29):
//
//   leftWall  (Kd=0.63 / 0.065 / 0.05; red) → opaque branch
//     {red_wool, fire_coral_block, redstone_block,
//      red_concrete, red_stained_glass}
//   rightWall (Kd=0.14 / 0.45 / 0.091; green) → opaque branch
//     {jungle_leaves, dark_oak_leaves, green_wool,
//      acacia_leaves, grass_block}
//   white     (Kd=0.725 / 0.71 / 0.68; floor/ceiling/backWall/shortBox/tallBox)
//     → opaque branch
//     {diorite, white_terracotta, polished_diorite,
//      sea_lantern, mushroom_stem}
//   light     (Kd=0.78s, Ke=(17, 12, 4); max(Ke) > 0.5) → emissive branch
//     {ochre_froglight, pearlescent_froglight, verdant_froglight,
//      beacon, sea_lantern}
//
// STOP discipline on first-run FAIL (per proposal §3.10 + §8 sign-off E3):
// if any material's pick is OUTSIDE its acceptable-set, this is the palette-
// drift signal. The implementation MUST NOT silently update the acceptable-
// set; instead re-run research/scratch/cornell_box/predict.py against the
// live palette CSV, compare to §1.7, ask the maintainer to either (a) tighten
// to the new empirical top-K (matcher_proposal §6 posture), or (b)
// investigate as a matcher regression.

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

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
// acceptable-set; on failure, error message names the offending material +
// what was picked + what the acceptable-set was for §3.10 diagnostic.
void check_material(mat::Matcher& matcher,
                    const std::string& material_name,
                    const msh::Material& m,
                    const std::set<std::string>& acceptable) {
    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(material_name + ": matcher.match threw: " + e.what());
    }
    std::printf("  %-9s -> %s\n", material_name.c_str(), result.name.c_str());

    if (acceptable.find(result.name) == acceptable.end()) {
        std::string set_str = "{";
        for (auto it = acceptable.begin(); it != acceptable.end(); ++it) {
            if (it != acceptable.begin()) set_str += ", ";
            set_str += *it;
        }
        set_str += "}";
        fail(material_name + " picked '" + result.name +
             "' which is NOT in §1.7 acceptable-set " + set_str +
             ". This is the palette-drift signal per cornell_box_tier4_"
             "proposal §3.10. STOP — do NOT silently update the test's "
             "acceptable-set. Re-run research/scratch/cornell_box/predict.py "
             "against the live palette CSV and ask the maintainer.");
    }
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

    std::printf("matcher_cornell_box_acceptable_set — empirical top-1 picks:\n");

    // ---- leftWall (red opaque) ----
    {
        msh::Material m{};
        m.name = "leftWall";
        m.kd   = {0.63f, 0.065f, 0.05f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:red_wool",
            "minecraft:fire_coral_block",
            "minecraft:redstone_block",
            "minecraft:red_concrete",
            "minecraft:red_stained_glass",
        };
        check_material(matcher, "leftWall", m, acceptable);
    }

    // ---- rightWall (green opaque) ----
    {
        msh::Material m{};
        m.name = "rightWall";
        m.kd   = {0.14f, 0.45f, 0.091f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:jungle_leaves",
            "minecraft:dark_oak_leaves",
            "minecraft:green_wool",
            "minecraft:acacia_leaves",
            "minecraft:grass_block",
        };
        check_material(matcher, "rightWall", m, acceptable);
    }

    // ---- white surface (Kd identical across floor/ceiling/backWall/shortBox/tallBox) ----
    {
        msh::Material m{};
        m.name = "whiteSurface";
        m.kd   = {0.725f, 0.71f, 0.68f};
        m.ke   = {0.0f, 0.0f, 0.0f};
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:diorite",
            "minecraft:white_terracotta",
            "minecraft:polished_diorite",
            "minecraft:sea_lantern",
            "minecraft:mushroom_stem",
        };
        check_material(matcher, "white", m, acceptable);
    }

    // ---- light (emissive; Ke wildly out-of-gamut) ----
    {
        msh::Material m{};
        m.name = "light";
        m.kd   = {0.78f, 0.78f, 0.78f};
        m.ke   = {17.0f, 12.0f, 4.0f};   // physical radiance from photon mapping
        m.d    = 1.0f;
        const std::set<std::string> acceptable = {
            "minecraft:ochre_froglight",
            "minecraft:pearlescent_froglight",
            "minecraft:verdant_froglight",
            "minecraft:beacon",
            "minecraft:sea_lantern",
        };
        check_material(matcher, "light", m, acceptable);
    }

    std::printf("PASS: matcher_cornell_box_acceptable_set "
                "(4 materials in §1.7 acceptable-sets)\n");
    return 0;
}
