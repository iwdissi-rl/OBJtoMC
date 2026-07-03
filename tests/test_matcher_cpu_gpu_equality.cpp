// ---------------------------------------------------------------------------
// matcher_cpu_gpu_equality — Session 41 correctness gate for the GPU matcher
// path. Runs a large synthetic batch (12^3 opaque RGB grid + 200 emissive +
// 200 transparent, total 2128 materials) through match_batch twice — once
// with prefer_gpu=OFF, once with prefer_gpu=ON — and asserts the resulting
// std::vector<chunk::BlockState> arrays are byte-identical (same name +
// same properties for every material).
//
// Additional guard: 8 canonical Tier-4-empirically-verified fixture
// materials (Cornell Box 4 + transparent-scene 4) must produce their known
// picks on the GPU path — pins the byte-equality guarantee to the specific
// records the maintainer signed off in the Sessions 31 / 34 Tier-4 PASSes.
//
// See research/gpu_port_proposal.md §3.6 for the test contract + §3.12 for
// the first-run mismatch triage protocol. Byte-equality is the primary
// gate; do NOT modify the test to pass, invoke §3.12(a)-(d) instead.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef OBJTOMC_WITH_CUDA
#include "objtomc/chunk.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

namespace {

std::vector<objtomc::mesh::Material> build_synthetic_batch() {
    std::vector<objtomc::mesh::Material> mats;

    // 12x12x12 = 1728 opaque materials sweeping the RGB cube.
    for (int r = 0; r < 12; ++r) {
        for (int g = 0; g < 12; ++g) {
            for (int b = 0; b < 12; ++b) {
                objtomc::mesh::Material m{};
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
        objtomc::mesh::Material m{};
        m.name = "emit_" + std::to_string(i);
        const float hue    = static_cast<float>(i) / 200.0f;
        const float boost  = 0.6f + (static_cast<float>(i % 20)) * 0.02f;  // >0.5 to enter emissive branch
        m.kd = {0.4f + 0.3f * hue, 0.5f, 0.4f + 0.3f * (1.0f - hue)};
        m.ke = {boost * (1.0f - hue), boost * 0.5f, boost * hue};
        m.d  = 1.0f;
        mats.push_back(m);
    }

    // ~200 transparent materials — d < 0.5, ke = 0.
    for (int i = 0; i < 200; ++i) {
        objtomc::mesh::Material m{};
        m.name = "trans_" + std::to_string(i);
        const float hue = static_cast<float>(i) / 200.0f;
        m.kd = {hue, 0.4f + 0.3f * (1.0f - hue), 1.0f - hue};
        m.ke = {0.0f, 0.0f, 0.0f};
        m.d  = 0.15f + 0.3f * hue;  // in [0.15, 0.45) — all below 0.5 threshold
        mats.push_back(m);
    }

    return mats;
}

// Byte-compare two BlockState arrays. Returns first-diff index or -1 on match.
long compare(const std::vector<objtomc::chunk::BlockState>& a,
             const std::vector<objtomc::chunk::BlockState>& b) {
    if (a.size() != b.size()) return 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].properties != b[i].properties) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

// 8 canonical fixtures pinned by Sessions 31 / 34 Tier-4 empirical PASSes.
// These are the actual Cornell Box + transparent-scene materials (from
// research/cornell_box_tier4_proposal.md §1.7 + research/
// transparent_tier4_proposal.md §1.7) with their maintainer-verified
// top-1 picks. Any change to the matcher path that would flip one of
// these picks is a regression regardless of what byte-equality says.
struct CanonicalPick {
    const char*             name;
    std::array<float, 3>    kd;
    std::array<float, 3>    ke;
    float                   d;
    const char*             expected_block;
};

const CanonicalPick kCanonicals[] = {
    // Cornell Box (Session 31 Tier-4 PASS).
    {"cornell_leftWall",  {0.63f, 0.065f, 0.05f}, {0, 0, 0}, 1.0f, "minecraft:red_wool"},
    {"cornell_rightWall", {0.14f, 0.45f,  0.091f},{0, 0, 0}, 1.0f, "minecraft:jungle_leaves"},
    {"cornell_white",     {0.725f,0.71f,  0.68f}, {0, 0, 0}, 1.0f, "minecraft:diorite"},
    // Cornell light: strong emitter; Session 30 empirical pick is a
    // froglight variant. Accept any froglight name.
    {"cornell_light",     {0.78f, 0.78f,  0.78f}, {0.95f, 0.85f, 0.55f}, 1.0f, "froglight"},
    // Transparent scene (Session 34 Tier-4 PASS). Material values taken
    // verbatim from tests/test_matcher_transparent_acceptable_set.cpp so
    // the expected picks below match the empirically-verified Session 34
    // top-1s (verified inline by the acceptable_set test's pick_* checks).
    {"clear_glass",   {0.90f, 0.90f, 0.90f}, {0, 0, 0}, 0.30f, "minecraft:white_stained_glass"},
    {"red_glass",     {0.60f, 0.20f, 0.20f}, {0, 0, 0}, 0.30f, "minecraft:red_stained_glass"},
    {"opaque_red",    {0.80f, 0.10f, 0.10f}, {0, 0, 0}, 1.0f,  "minecraft:red_mushroom_block"},
    {"opaque_white",  {0.90f, 0.90f, 0.90f}, {0, 0, 0}, 1.0f,  "minecraft:white_concrete_powder"},
};

int check_canonicals(const objtomc::palette::PaletteDB& db,
                     objtomc::matcher::MatcherOptions opts_gpu) {
    for (const auto& c : kCanonicals) {
        objtomc::mesh::Material m{};
        m.name = c.name;
        m.kd = c.kd; m.ke = c.ke; m.d = c.d;
        std::vector<objtomc::mesh::Material> in{m};
        std::vector<objtomc::chunk::BlockState> out(1);
        // Force GPU with prefer_gpu=ON (even though 1 material is below
        // AUTO threshold; the CPU-vs-GPU comparison for these lives
        // above, this is an additional canonical-record guard).
        objtomc::matcher::Matcher m_gpu(db, opts_gpu);
        m_gpu.match_batch(std::span<const objtomc::mesh::Material>(in.data(), 1),
                          std::span<objtomc::chunk::BlockState>(out.data(), 1));
        const std::string& got = out[0].name;
        std::string expected = c.expected_block;
        // "froglight" is a substring match for any variant (ochre / verdant /
        // pearlescent). Others are exact.
        bool ok;
        if (expected == "froglight") {
            ok = got.find("froglight") != std::string::npos;
        } else {
            ok = got == expected;
        }
        if (!ok) {
            std::cerr << "FAIL: canonical '" << c.name << "' picked '" << got
                      << "', expected '" << expected << "'\n";
            return 1;
        }
        std::cout << "canonical [" << c.name << "] → " << got << " OK\n";
    }
    return 0;
}

}  // namespace
#endif  // OBJTOMC_WITH_CUDA

int main(int argc, char** argv) {
#ifndef OBJTOMC_WITH_CUDA
    (void)argc; (void)argv;
    std::cout << "SKIP: WITH_CUDA not enabled\n";
    return 0;
#else
    (void)argc; (void)argv;

    // Load the real default 414-entry palette (maintainer-nominated at
    // Session 41 AskUserQuestion 2026-07-03).
    objtomc::palette::PaletteDB db;
    try {
        db = objtomc::palette::load_default_palette();
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: load_default_palette threw: " << ex.what() << "\n";
        return 1;
    }
    std::cout << "loaded " << db.size() << " palette entries\n";

    const std::vector<objtomc::mesh::Material> in = build_synthetic_batch();
    std::cout << "synthetic batch: " << in.size() << " materials "
              << "(1728 opaque + 200 emissive + 200 transparent)\n";

    auto opts_cpu = objtomc::matcher::default_options();
    opts_cpu.prefer_gpu = objtomc::matcher::GpuPreference::OFF;
    auto opts_gpu = objtomc::matcher::default_options();
    opts_gpu.prefer_gpu = objtomc::matcher::GpuPreference::ON;

    using clock = std::chrono::steady_clock;
    using ms    = std::chrono::milliseconds;

    // CPU run.
    std::vector<objtomc::chunk::BlockState> out_cpu(in.size());
    {
        objtomc::matcher::Matcher m(db, opts_cpu);
        const auto t0 = clock::now();
        m.match_batch(std::span<const objtomc::mesh::Material>(in.data(), in.size()),
                      std::span<objtomc::chunk::BlockState>(out_cpu.data(), out_cpu.size()));
        const auto t1 = clock::now();
        std::cout << "CPU match_batch: "
                  << std::chrono::duration_cast<ms>(t1 - t0).count() << " ms\n";
    }

    // GPU run.
    std::vector<objtomc::chunk::BlockState> out_gpu(in.size());
    {
        objtomc::matcher::Matcher m(db, opts_gpu);
        const auto t0 = clock::now();
        m.match_batch(std::span<const objtomc::mesh::Material>(in.data(), in.size()),
                      std::span<objtomc::chunk::BlockState>(out_gpu.data(), out_gpu.size()));
        const auto t1 = clock::now();
        std::cout << "GPU match_batch: "
                  << std::chrono::duration_cast<ms>(t1 - t0).count() << " ms\n";
    }

    // Byte-compare.
    const long diff = compare(out_cpu, out_gpu);
    if (diff >= 0) {
        std::cerr << "FAIL: byte-equality: first diff at material index "
                  << diff << " ('" << in[diff].name << "') — "
                  << "CPU='" << out_cpu[diff].name
                  << "' GPU='" << out_gpu[diff].name << "'\n"
                  << "Per proposal §3.12: do NOT modify test to pass. Invoke "
                     "triage protocol (a) determinism, (b) stage isolation "
                     "per branch, (c) precision (are CPU distances to top-1 "
                     "and top-2 within ~1e-6 for this material? → fmad or "
                     "pow/cbrt divergence). (d) STOP + ask maintainer if none "
                     "of (a)-(c) explains.\n";
        return 1;
    }
    std::cout << "PASS: CPU vs GPU byte-identical across "
              << in.size() << " materials\n";

    // Canonical-pick guard.
    if (check_canonicals(db, opts_gpu) != 0) return 1;

    return 0;
#endif
}
