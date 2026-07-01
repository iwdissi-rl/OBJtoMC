// matcher_sponza_smoke — scale Tier-4 proposal §3.10.1.
//
// All 25 Sponza materials have identical Kd = (1, 1, 1) (uniform pure white),
// Ke = (0, 0, 0), d = 1.0 per the §1.6 empirical MTL analysis. The matcher's
// opaque branch on this input picks minecraft:white_stained_glass at Oklab
// distance 0.0000 (exact match — palette CSV row for white_stained_glass has
// r,g,b = 255,255,255 which maps to Oklab (1.0, 0, 0)).
//
// Construction: since all 25 materials feed the matcher identical input, this
// ctest samples 5 representative Sponza material names (the top 5 by usage
// area in the mesh; picked from sponza.mtl for readability) and runs the
// matcher on each with the same Kd/Ke/d values. This is sufficient per
// §1.8 — 25 identical inputs produce 25 identical outputs; testing 5 is
// enough to prove the matcher's opaque-branch behaviour on this Kd, and the
// stdout serves as the reference record for the Session 37 F3 cross-checks.
//
// Assertions (per user's Session 36 prompt task 2):
//   1. All 5 picks are exactly minecraft:white_stained_glass (§1.8 top-1).
//      Any drift → palette-drift signal per matcher_proposal §6 "tighten
//      not relax". STOP; do NOT silently update this test.
//   2. Session-36 new-discipline: the top-1 must be white_stained_glass
//      (opacity=0, TRANSPARENT) and NOT snow_block (opacity=15, opaque)
//      even though snow_block is only 0.0086 Oklab units behind. This
//      reflects the CURRENT opaque-branch behaviour documented in
//      OPEN_QUESTIONS.md 2026-07-04 "Near-white opaque-branch matcher-
//      quality". If a future v0.2 item #4 change resolves that entry, this
//      ctest is the first to surface the flip (e.g. by tightening opacity
//      into the opaque-branch score), and the flip should be intentional
//      + reviewed rather than silent.
//
// Prints all 5 top-1 picks to stdout in the established format. The picks
// are the empirical record for the Session 37 Tier-4's F3 cross-checks
// (proposal §3.7 conditions vii, viii, xi).

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

    // ---- Assertion 1: all 5 top-1 picks are white_stained_glass ----
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (picks[i] != "minecraft:white_stained_glass") {
            fail(sponza_material_names[i] + " picked '" + picks[i] +
                 "' — expected minecraft:white_stained_glass per §1.8 "
                 "empirical preview against the 414-entry palette CSV. "
                 "This is the palette-drift signal per matcher_proposal §6 "
                 "'tighten not relax'. STOP — do NOT silently update this "
                 "ctest. Re-run research/scratch/scale/predict.py against "
                 "the live palette CSV and ask the maintainer whether the "
                 "new top-1 is a palette regeneration (tighten this ctest) "
                 "or a matcher regression (investigate).");
        }
    }
    std::printf("  all 5 picks:      minecraft:white_stained_glass (per §1.8 top-1)\n");

    // ---- Assertion 2: near-white opaque-branch matcher-quality gate ----
    // For Kd=(1,1,1), the OPEN_QUESTIONS.md 2026-07-04 "Near-white opaque-
    // branch matcher-quality" entry documents that white_stained_glass
    // (opacity=0) wins by 0.0086 Oklab units over snow_block (opacity=15).
    // If a v0.2 item #4 change gives opaque-branch tie-break preference
    // to fully-opaque blocks (Option A/B/C in that entry), snow_block will
    // become the top-1 and this ctest will surface the flip.
    //
    // Assert that white_stained_glass wins and snow_block does NOT — using
    // the same input via free-function match() for a second independent
    // call path (proves it's not a stateful Matcher instance effect).
    msh::Material sponza_kd{};
    sponza_kd.name = "sponza_uniform";
    sponza_kd.kd   = {1.0f, 1.0f, 1.0f};
    sponza_kd.ke   = {0.0f, 0.0f, 0.0f};
    sponza_kd.d    = 1.0f;
    const auto discriminator_pick = mat::match(sponza_kd, db).name;

    if (discriminator_pick == "minecraft:snow_block") {
        fail("near-white opaque-branch matcher-quality assertion FIRED: "
             "sponza_uniform Kd=(1,1,1) picked '" + discriminator_pick +
             "' instead of the historical minecraft:white_stained_glass. "
             "This is expected IF a v0.2 item #4 change resolved the "
             "OPEN_QUESTIONS.md 2026-07-04 'Near-white opaque-branch "
             "matcher-quality' entry (opacity-preferring tie-break for "
             "opaque branch). If so, update this ctest + the OPEN_QUESTIONS "
             "entry status. If not, investigate as a matcher regression.");
    }
    if (discriminator_pick != "minecraft:white_stained_glass") {
        fail("near-white discriminator picked '" + discriminator_pick +
             "' — expected minecraft:white_stained_glass per §1.8. "
             "Neither the historical top-1 nor the OPEN_QUESTIONS-flagged "
             "snow_block runner-up; this is an unexpected pick that "
             "warrants investigation before any test-code change.");
    }
    std::printf("  discriminator:    white_stained_glass wins over snow_block "
                "(OPEN_QUESTIONS 2026-07-04 'Near-white opaque-branch "
                "matcher-quality' currently unresolved; opacity=0 wins over "
                "opacity=15 by 0.0086 Oklab units)\n");

    std::printf("PASS: matcher_sponza_smoke "
                "(5/5 samples pick white_stained_glass; discriminator holds)\n");
    return 0;
}
