// matcher_default_smoke — proposal §3.13 (a).
//
// Constructs a Matcher with the real 414-entry default palette
// (load_default_palette()) and matches a mid-gray opaque non-emissive
// Material. Asserts the picked block is one of the harness-predicted
// near-neighbours of mid-gray in the catalogue. Posture per proposal §6:
// if the pick falls OUTSIDE the acceptable set, the response is to TIGHTEN
// the set (the test is a CI assertion of the empirical-harness spec), not
// to relax it.

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
    m.name = "mid_gray";
    m.kd   = {0.5f, 0.5f, 0.5f};
    m.ke   = {0.0f, 0.0f, 0.0f};
    m.d    = 1.0f;

    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(std::string("matcher.match threw: ") + e.what());
    }

    const std::set<std::string> acceptable = {
        "minecraft:cobblestone",
        "minecraft:infested_cobblestone",
        "minecraft:gravel",
        "minecraft:stone",
        "minecraft:infested_stone",
    };
    if (acceptable.find(result.name) == acceptable.end()) {
        fail("mid-gray opaque match picked '" + result.name +
             "' which is outside the harness-predicted acceptable set "
             "{cobblestone, infested_cobblestone, gravel, stone, infested_stone}. "
             "Per proposal §6, tighten the test (do not relax) — record the "
             "actual pick and update the acceptable set with the empirical "
             "harness output.");
    }

    std::printf("PASS: matcher_default_smoke (picked '%s' for mid-gray)\n",
                result.name.c_str());
    return 0;
}
