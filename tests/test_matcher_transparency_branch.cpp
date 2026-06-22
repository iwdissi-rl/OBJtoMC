// matcher_transparency_branch — proposal §3.13 (c).
//
// Cool-white-ish translucent Material (kd = {0.8, 0.9, 1.0} sRGB-gamma,
// d = 0.2) → matcher enters the transparent branch (d < 0.5 threshold) and
// selects from the opacity < 15 pool. Acceptable set TIGHTENED to the
// §1.5-harness empirical top-2 picks for this exact input per OPEN_QUESTIONS
// 2026-06-22 option (C): {beacon, glass}. Beacon is the harness top-1
// (score 0.2140); glass is the runner-up (0.2195) — the 0.0055 margin
// could flip on a future float-precision shift, so bounding the top-2
// keeps the test stable while remaining the strictest sensible set.
// Posture per §6: empirical harness output IS the spec; widening this set
// further would relax (forbidden), narrowing further would risk float drift.

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

    const std::set<std::string> acceptable = {
        "minecraft:beacon",   // empirical top-1 (score 0.2140)
        "minecraft:glass",    // runner-up      (score 0.2195)
    };
    if (acceptable.find(result.name) == acceptable.end()) {
        fail("cool-white translucent match picked '" + result.name +
             "' which is outside the harness-empirical acceptable set "
             "{beacon, glass}. Per proposal §6 (and OPEN_QUESTIONS "
             "2026-06-22 option C), the empirical top-2 picks ARE the spec; "
             "deviation here means a matcher-behavior change (algorithm, "
             "weights, or pool filter), not a test bug — re-run the harness "
             "before adjusting the set.");
    }

    std::printf("PASS: matcher_transparency_branch (picked '%s' for kd=(0.8,0.9,1.0), d=0.2)\n",
                result.name.c_str());
    return 0;
}
