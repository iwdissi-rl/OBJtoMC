// matcher_batch_consistency — proposal §3.13 (f).
//
// match(m) MUST equal match_batch(span(&m, 1))[0] for any Material. If they
// diverge the future GPU port (which will replace match_batch's serial loop
// with a kernel) will silently produce different output from the CPU
// baseline. Exercises all three branches via three sample materials and
// asserts both single + batched results coincide.

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

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
    opaque.d    = 1.0f;

    msh::Material emissive{};
    emissive.name = "warm_white_emitter";
    emissive.kd   = {1.0f, 0.9f, 0.5f};
    emissive.ke   = {1.0f, 0.9f, 0.5f};
    emissive.d    = 1.0f;

    msh::Material transparent{};
    transparent.name = "cool_white_translucent";
    transparent.kd   = {0.8f, 0.9f, 1.0f};
    transparent.d    = 0.2f;

    const std::array<msh::Material, 3> materials{opaque, emissive, transparent};

    // Verify single-call wrapper matches batched path for each sample.
    for (const auto& sample : materials) {
        const auto single = matcher.match(sample);

        objtomc::chunk::BlockState batched;
        matcher.match_batch(std::span<const msh::Material>(&sample, 1),
                            std::span<objtomc::chunk::BlockState>(&batched, 1));

        if (single != batched) {
            fail("single vs batch (size=1) divergence for '" + sample.name +
                 "': single='" + single.name + "', batched='" + batched.name + "'");
        }
    }

    // Same check at batch size N (the GPU staging buffer size in v0.2).
    std::vector<objtomc::chunk::BlockState> batched_n(materials.size());
    matcher.match_batch(std::span<const msh::Material>(materials.data(),
                                                       materials.size()),
                        std::span<objtomc::chunk::BlockState>(batched_n));
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const auto single = matcher.match(materials[i]);
        if (single != batched_n[i]) {
            fail("single vs batch (size=N) divergence at index " +
                 std::to_string(i) + " for '" + materials[i].name +
                 "': single='" + single.name +
                 "', batched='" + batched_n[i].name + "'");
        }
    }

    std::printf("PASS: matcher_batch_consistency (3 materials × {size=1, size=N})\n");
    return 0;
}
