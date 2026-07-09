// matcher_transparency_branch — proposal §3.13 (c).
//
// Cool-white-ish translucent Material (kd = {0.8, 0.9, 1.0} sRGB-gamma,
// d = 0.2) → matcher enters the transparent branch (d < 0.5 threshold) and
// selects from the transparent pool.
//
// Session 46 (c′) update: pool = {renders_translucent == true} per
// research/translucency_pool_partitioning_proposal.md §4.1. Beacon is
// renders_translucent=false → EXITS the pool structurally; the OQ
// 2026-06-22 beacon-vs-glass ordering issue is closed at the palette layer.
// Acceptable set narrows to {white_stained_glass, glass} — the empirical
// top-2 picks under (c′)'s partitioned pool for this Kd. Predicted by
// research/matcher_quality/predict.py at Session 45 counterfactual.

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

}  // namespace

int main() {
    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }

    mat::Matcher matcher(db, mat::default_options());

    msh::Material m{};
    m.name = "cool_white_translucent";
    m.kd   = {0.8f, 0.9f, 1.0f};
    m.ke   = {0.0f, 0.0f, 0.0f};
    m.d    = 0.2f;

    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(std::string("matcher.match threw: ") + e.what());
    }

    // Session 46 (c′): transparent-branch pool = {renders_translucent==true}.
    // Beacon is renders_translucent=false → excluded; glass + stained-glass
    // family are the only remaining perceptual near-neighbours for cool-white
    // Kd. Empirical top-2 under (c′): {white_stained_glass, glass}.
    const std::set<std::string> acceptable = {
        "minecraft:white_stained_glass",
        "minecraft:glass",
    };
    if (acceptable.find(result.name) == acceptable.end()) {
        fail("cool-white translucent match picked '" + result.name +
             "' which is outside the (c′)-partitioned acceptable set "
             "{white_stained_glass, glass}. Per proposal §6, deviation "
             "here means a matcher-behavior change (algorithm, weights, "
             "pool filter, or renders_translucent curation) — re-run "
             "predict.py before adjusting the set.");
    }

    std::printf("PASS: matcher_transparency_branch (picked '%s' for kd=(0.8,0.9,1.0), d=0.2)\n",
                result.name.c_str());
    return 0;
}
