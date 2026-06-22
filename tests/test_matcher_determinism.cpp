// matcher_determinism — proposal §3.13 (d).
//
// Match the same Material twice through the same Matcher; assert that the
// result is byte-identical. Defends against std::sort/std::min_element
// instability on tied candidates (the 414-entry palette has clusters of
// near-equivalent neighbours — proposal §3.12 cites cobblestone /
// infested_cobblestone tied at Oklab distance 0.0029 for mid-gray).
//
// We exercise three materials covering all three branches so that any
// branch-specific non-determinism (different pool, different comparator)
// would surface.

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
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

    msh::Material opaque{};
    opaque.name = "mid_gray";
    opaque.kd   = {0.5f, 0.5f, 0.5f};
    opaque.ke   = {0.0f, 0.0f, 0.0f};
    opaque.d    = 1.0f;

    msh::Material emissive{};
    emissive.name = "warm_white_emitter";
    emissive.kd   = {1.0f, 0.9f, 0.5f};
    emissive.ke   = {1.0f, 0.9f, 0.5f};
    emissive.d    = 1.0f;

    msh::Material transparent{};
    transparent.name = "cool_white_translucent";
    transparent.kd   = {0.8f, 0.9f, 1.0f};
    transparent.ke   = {0.0f, 0.0f, 0.0f};
    transparent.d    = 0.2f;

    const std::array<const msh::Material*, 3> samples{&opaque, &emissive, &transparent};
    for (const auto* sample : samples) {
        const auto first  = matcher.match(*sample);
        const auto second = matcher.match(*sample);
        if (first != second) {
            fail("non-deterministic pick for material '" + sample->name +
                 "': first='" + first.name + "', second='" + second.name + "'");
        }
    }

    std::printf("PASS: matcher_determinism (3 materials × 2 calls each)\n");
    return 0;
}
