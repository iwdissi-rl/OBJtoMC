// match_input_equivalence — v0.4 §3.4 / §5.2 (research/scale_v04_proposal.md).
//
// The Material overload of match_batch MUST produce byte-identical
// BlockState output to the MatchInput overload for equal (kd, ke, d).
// Guards the Session 55 refactor that made MatchInput the core path
// (orchestrate step 4a now feeds MatchInput directly): if the Material
// overload's validate-convert-delegate ever diverges from matching the
// Materials themselves, every pre-refactor byte-oracle would still pass
// while CLI output silently changed.
//
// Corpus mirrors matcher_cpu_gpu_equality's synthetic recipe (12^3 opaque
// RGB sweep + 200 emissive + 200 transparent = 2128) so that under
// -DWITH_CUDA=ON at default (AUTO) options both overloads dispatch to the
// GPU (2128 >= GPU_BATCH_THRESHOLD) and the equivalence is exercised on
// the device path too; the 3-element and 1-element cases below stay CPU
// in every build. Also asserts match(m) == MatchInput-batch-of-1, closing
// the loop with matcher_batch_consistency.

#include "objtomc/chunk.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

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

std::vector<msh::Material> build_synthetic_batch() {
    std::vector<msh::Material> mats;

    // 12x12x12 = 1728 opaque materials sweeping the RGB cube.
    for (int r = 0; r < 12; ++r) {
        for (int g = 0; g < 12; ++g) {
            for (int b = 0; b < 12; ++b) {
                msh::Material m{};
                m.name = "opaque_" + std::to_string(r) + "_" +
                         std::to_string(g) + "_" + std::to_string(b);
                m.kd = {static_cast<float>(r) / 11.0f,
                        static_cast<float>(g) / 11.0f,
                        static_cast<float>(b) / 11.0f};
                m.ke = {0.0f, 0.0f, 0.0f};
                m.d  = 1.0f;
                mats.push_back(m);
            }
        }
    }

    // ~200 emissive materials — sweep ke intensity + hue.
    for (int i = 0; i < 200; ++i) {
        msh::Material m{};
        m.name = "emit_" + std::to_string(i);
        const float hue   = static_cast<float>(i) / 200.0f;
        const float boost = 0.6f + (static_cast<float>(i % 20)) * 0.02f;
        m.kd = {0.4f + 0.3f * hue, 0.5f, 0.4f + 0.3f * (1.0f - hue)};
        m.ke = {boost * (1.0f - hue), boost * 0.5f, boost * hue};
        m.d  = 1.0f;
        mats.push_back(m);
    }

    // ~200 transparent materials — d < 0.5, ke = 0.
    for (int i = 0; i < 200; ++i) {
        msh::Material m{};
        m.name = "trans_" + std::to_string(i);
        const float hue = static_cast<float>(i) / 200.0f;
        m.kd = {hue, 0.4f + 0.3f * (1.0f - hue), 1.0f - hue};
        m.ke = {0.0f, 0.0f, 0.0f};
        m.d  = 0.15f + 0.3f * hue;
        mats.push_back(m);
    }

    return mats;
}

mat::MatchInput to_input(const msh::Material& m) {
    return mat::MatchInput{m.kd, m.ke, m.d};
}

// Run both overloads on the same matcher and compare element-wise.
void check_equivalence(const mat::Matcher& matcher,
                       const std::vector<msh::Material>& mats,
                       const std::string& label) {
    std::vector<mat::MatchInput> inputs;
    inputs.reserve(mats.size());
    for (const auto& m : mats) inputs.push_back(to_input(m));

    std::vector<objtomc::chunk::BlockState> out_material(mats.size());
    std::vector<objtomc::chunk::BlockState> out_input(mats.size());
    matcher.match_batch(std::span<const msh::Material>(mats), out_material);
    matcher.match_batch(std::span<const mat::MatchInput>(inputs), out_input);

    for (std::size_t i = 0; i < mats.size(); ++i) {
        if (out_material[i].name != out_input[i].name ||
            out_material[i].properties != out_input[i].properties) {
            fail(label + ": overloads diverge at index " + std::to_string(i) +
                 " ('" + mats[i].name + "'): Material overload -> " +
                 out_material[i].name + ", MatchInput overload -> " +
                 out_input[i].name);
        }
    }
    std::cout << "PASS: " << label << " (" << mats.size()
              << " inputs, overloads byte-identical)\n";
}

}  // namespace

int main() {
    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }
    const mat::Matcher matcher(db, mat::default_options());

    // (1) The 2128-input synthetic corpus (GPU dispatch under CUDA+AUTO).
    check_equivalence(matcher, build_synthetic_batch(), "synthetic-2128");

    // (2) A 3-material all-branch batch, below any GPU threshold — always
    // exercises the CPU loop.
    std::vector<msh::Material> small;
    {
        msh::Material opaque{};
        opaque.name = "mid_gray";
        opaque.kd   = {0.5f, 0.5f, 0.5f};
        opaque.ke   = {0.0f, 0.0f, 0.0f};
        opaque.d    = 1.0f;
        msh::Material emissive{};
        emissive.name = "warm_white_emitter";
        emissive.kd   = {0.8f, 0.8f, 0.7f};
        emissive.ke   = {0.9f, 0.85f, 0.7f};
        emissive.d    = 1.0f;
        msh::Material transparent{};
        transparent.name = "cool_white_glass";
        transparent.kd   = {0.9f, 0.95f, 1.0f};
        transparent.ke   = {0.0f, 0.0f, 0.0f};
        transparent.d    = 0.2f;
        small = {opaque, emissive, transparent};
    }
    check_equivalence(matcher, small, "three-branch-small");

    // (3) Single-material closure: match(m) == MatchInput batch of 1.
    for (const auto& m : small) {
        const auto single = matcher.match(m);
        const mat::MatchInput mi = to_input(m);
        objtomc::chunk::BlockState batched;
        matcher.match_batch(std::span<const mat::MatchInput>(&mi, 1),
                            std::span<objtomc::chunk::BlockState>(&batched, 1));
        if (single.name != batched.name ||
            single.properties != batched.properties) {
            fail("single '" + m.name + "': match() -> " + single.name +
                 " but MatchInput batch-of-1 -> " + batched.name);
        }
    }
    std::cout << "PASS: single-vs-MatchInput-batch-of-1 (3 branches)\n";

    std::cout << "PASS: match_input_equivalence\n";
    return 0;
}
