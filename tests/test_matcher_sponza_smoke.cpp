// matcher_sponza_smoke — scale Tier-4 proposal §3.10.1
// + Session 46 (c′) update per research/translucency_pool_partitioning_
// proposal.md §6.2 + §5.2 Tier III counterfactual.
//
// All 25 Sponza materials have identical Kd = (1, 1, 1) (uniform pure white),
// Ke = (0, 0, 0), d = 1.0 per the §1.6 empirical MTL analysis. Under v0.1
// the matcher's opaque branch picked minecraft:white_stained_glass at Oklab
// distance 0.0000 (exact match) — the OQ 2026-07-04 "Near-white opaque-
// branch matcher-quality" pathology at its origin. Session 46 (c′) fixed
// this pathology STRUCTURALLY via two-pool partitioning: white_stained_glass
// is renders_translucent=true → excluded from the opaque-branch pool. The
// perceptually next-nearest opaque neighbour, minecraft:snow_block
// (opacity=15, Oklab distance 0.0086), wins.
//
// Assertions (post-(c′) discipline):
//   1. All 5 top-1 picks are exactly minecraft:snow_block. Any drift →
//      palette-drift signal per matcher_proposal §6 "tighten not relax".
//      STOP; do NOT silently update this test.
//   2. Discriminator (inverted from the pre-(c′) v0.1 assertion): the top-1
//      must be snow_block (opacity=15, opaque, renders_translucent=false)
//      and NOT white_stained_glass (opacity=0, renders_translucent=true;
//      structurally excluded from opaque-branch pool). The 0.0086 Oklab
//      distance to snow_block reflects the small perceptual cost of the
//      pool partitioning — accepted per proposal §5.6 as an anticipated
//      side-effect. If white_stained_glass IS ever picked here again,
//      that would signal a (c′) regression (curation drift or pool-filter
//      code change).
//
// Prints all 5 top-1 picks to stdout in the established format.

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace mat = objtomc::matcher;
namespace pal = objtomc::palette;
namespace msh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::string match_kd_white(mat::Matcher& matcher,
                           const std::string& material_name) {
    msh::Material m{};
    m.name = material_name;
    m.kd   = {1.0f, 1.0f, 1.0f};    // Sponza's uniform pure-white Kd
    m.ke   = {0.0f, 0.0f, 0.0f};    // no emission
    m.d    = 1.0f;                  // opaque

    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(material_name + ": matcher.match threw: " + e.what());
    }
    std::printf("  %-16s -> %s\n", material_name.c_str(), result.name.c_str());
    return result.name;
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

    // Confirm the palette catalogue does have both blocks the assertion
    // discriminates between — if a future palette regeneration removes
    // either, the near-white matcher-quality assertion below becomes
    // vacuous and this ctest should surface that too.
    if (!db.find("minecraft:white_stained_glass")) {
        fail("palette catalogue does not contain minecraft:white_stained_glass "
             "— cannot evaluate §1.8 top-1 assertion. Palette regenerated?");
    }
    if (!db.find("minecraft:snow_block")) {
        fail("palette catalogue does not contain minecraft:snow_block "
             "— cannot evaluate the near-white opaque-branch matcher-quality "
             "assertion. Palette regenerated?");
    }

    std::printf("matcher_sponza_smoke — empirical top-1 picks "
                "(5 Sponza materials, uniform Kd=(1,1,1)):\n");

    // 5 representative Sponza material names sampled from sponza.mtl.
    // All 25 have identical Kd=(1,1,1); these 5 (leaf, vase_round, arch,
    // background, roof) are large-surface-area architectural materials
    // spanning the atrium's floor / vertical / horizontal surfaces so
    // future readers can trace stdout back to real Sponza content.
    const std::array<std::string, 5> sponza_material_names = {
        "leaf",
        "vase_round",
        "arch",
        "background",
        "roof",
    };

    std::array<std::string, 5> picks{};
    for (std::size_t i = 0; i < sponza_material_names.size(); ++i) {
        picks[i] = match_kd_white(matcher, sponza_material_names[i]);
    }

    // ---- Assertion 1: all 5 top-1 picks are snow_block (post-(c′)) ----
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (picks[i] != "minecraft:snow_block") {
            fail(sponza_material_names[i] + " picked '" + picks[i] +
                 "' — expected minecraft:snow_block per Session 46 (c′) "
                 "counterfactual (translucency_pool_partitioning_proposal "
                 "§5.2 Tier III). This is the palette-drift signal per "
                 "matcher_proposal §6 'tighten not relax'. STOP — do NOT "
                 "silently update. Re-run research/matcher_quality/"
                 "predict.py against the live palette CSV and ask the "
                 "maintainer whether the new top-1 is a palette regeneration "
                 "(curation change) or a matcher regression.");
        }
    }
    std::printf("  all 5 picks:      minecraft:snow_block (post-(c′) top-1)\n");

    // ---- Assertion 2: (c′) discriminator — snow_block wins over white_stained_glass ----
    // Under Session 46 (c′), white_stained_glass is renders_translucent=true
    // → structurally excluded from the opaque-branch pool. snow_block
    // (opacity=15, renders_translucent=false, Oklab distance 0.0086) is the
    // perceptually next-nearest opaque neighbour and now wins. Inverse of
    // the pre-(c′) v0.1 assertion.
    msh::Material sponza_kd{};
    sponza_kd.name = "sponza_uniform";
    sponza_kd.kd   = {1.0f, 1.0f, 1.0f};
    sponza_kd.ke   = {0.0f, 0.0f, 0.0f};
    sponza_kd.d    = 1.0f;
    const auto discriminator_pick = mat::match(sponza_kd, db).name;

    if (discriminator_pick == "minecraft:white_stained_glass") {
        fail("(c′) regression FIRED: sponza_uniform Kd=(1,1,1) picked '" +
             discriminator_pick + "' — Session 46 (c′) two-pool partition "
             "should have excluded white_stained_glass (renders_translucent="
             "true) from the opaque-branch pool. Either the palette CSV's "
             "renders_translucent curation drifted or the matcher pool-"
             "filter code was reverted. Investigate BEFORE touching this "
             "ctest.");
    }
    if (discriminator_pick != "minecraft:snow_block") {
        fail("near-white discriminator picked '" + discriminator_pick +
             "' — expected minecraft:snow_block per (c′) §5.2 Tier III "
             "counterfactual. Neither the pre-(c′) v0.1 top-1 nor the "
             "expected post-(c′) top-1; investigate before any test change.");
    }
    std::printf("  discriminator:    snow_block wins over white_stained_glass "
                "(Session 46 (c′) two-pool partition resolves OQ 2026-07-04 "
                "'Near-white opaque-branch matcher-quality' structurally; "
                "opacity=15 opaque neighbour picked instead of opacity=0 "
                "renders_translucent=true block)\n");

    std::printf("PASS: matcher_sponza_smoke "
                "(5/5 samples pick snow_block post-(c′); discriminator holds)\n");
    return 0;
}
