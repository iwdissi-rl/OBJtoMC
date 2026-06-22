// matcher_emission_branch — proposal §3.13 (b).
//
// Warm-white emissive Material (ke = {1.0, 0.9, 0.5} sRGB-gamma) → matcher
// enters the emissive branch (max(ke) = 1.0 > 0.5 threshold) and selects
// from the luminance > 0 pool. Acceptable set per the §1.5 harness:
// {sea_lantern, glowstone, ochre_froglight}; the empirical pick is
// ochre_froglight (warm saturation match). Posture per §6: outside the set
// → TIGHTEN (do not relax).

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
    m.name = "warm_white_emitter";
    m.kd   = {1.0f, 0.9f, 0.5f};
    m.ke   = {1.0f, 0.9f, 0.5f};
    m.d    = 1.0f;

    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(std::string("matcher.match threw: ") + e.what());
    }

    const std::set<std::string> acceptable = {
        "minecraft:sea_lantern",
        "minecraft:glowstone",
        "minecraft:ochre_froglight",
    };
    if (acceptable.find(result.name) == acceptable.end()) {
        fail("warm-white emissive match picked '" + result.name +
             "' which is outside the harness-predicted acceptable set "
             "{sea_lantern, glowstone, ochre_froglight}. Per proposal §6, "
             "tighten the test (do not relax) — record the actual pick and "
             "update the acceptable set with the empirical harness output.");
    }

    std::printf("PASS: matcher_emission_branch (picked '%s' for ke=(1.0,0.9,0.5))\n",
                result.name.c_str());
    return 0;
}
