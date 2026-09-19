// orchestrate_emissive_quad — Session 51 per-voxel emissive texture
// sampling integration test + oracle helper.
//
// Fixture: tests/fixtures/texture/emissive.glb — the 4×4 quad with the
// embedded checker base-colour texture PLUS emissiveFactor [1,1,1] and an
// embedded 2×1 emissiveTexture (left texel white, right black): the
// Sketchfab-standard emissive-mask pattern that Session 51 exists to fix
// (factor-only mapping sent the whole DamagedHelmet to the emissive
// branch). With u = x/4 and the pinned quad options, voxels vx<2 sample
// the white mask texel (per-voxel ke → {1,1,1}, matcher emissive branch)
// and vx≥2 the black texel (ke → {0,0,0}, opaque branch on the checker
// quadrant colour).
//
// CTest assertions (shape level — block-exact checks are the oracle's job):
//   0. Loader mapping: load_gltf(emissive.glb) exposes non-empty
//      map_ke_bytes and flat ke {1,1,1} on the single material.
//   1. Determinism: two emissive runs produce byte-identical r.0.0.mca.
//   2. Activation: quad.glb (same geometry + base colour, no emissive
//      texture) produces a DIFFERENT r.0.0.mca — proof the ke path
//      changes output rather than silently no-oping.
//
// Oracle protocol: prints "EXPECT <vx> <vz> <block_name>" for all 16
// voxels, computed independently via matcher::match on Materials whose kd
// is the exact checker quadrant colour and whose ke is {1,1,1} (vx<2) or
// {0,0,0} (vx≥2) — the exact linear-space product for a [1,1,1] factor
// against a pure white/black mask. If argv[3] is given the first emissive
// world is written there and kept for the oracle to parse.

#include "objtomc/gltf.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::vector<char> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) fail("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

orch::OrchestrateOptions quad_options() {
    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size               = 1.0f;
    opts.voxel_grid_dimensions    = {{4, 1, 4}};
    opts.voxel_grid_origin_world  = {{0.0f, 0.0f, 0.0f}};
    opts.world_origin             = {0, -60, 0};
    opts.spawn_pos                = {2, -58, 2};
    opts.level_name               = "objtomc-emissive-quad";
    opts.now_unix_s               = 0;  // byte-determinism
    return opts;
}

std::array<float, 3> quadrant_colour(int vx, int vz) {
    const bool right = vx >= 2;
    const bool far_z = vz >= 2;
    if (!right && !far_z) return {1.0f, 0.0f, 0.0f};  // red
    if (right && !far_z)  return {0.0f, 0.0f, 1.0f};  // blue
    if (!right && far_z)  return {0.0f, 1.0f, 0.0f};  // green
    return {1.0f, 1.0f, 1.0f};                        // white
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: " << argv[0]
                  << " <path-to-emissive.glb> <path-to-quad.glb>"
                     " [keep-world-dir]\n";
        return 2;
    }
    const fs::path emissive_glb = argv[1];
    const fs::path plain_glb    = argv[2];
    const auto opts = quad_options();

    // ---- 0. loader mapping ----
    {
        const msh::Mesh m = objtomc::gltf::load_gltf(emissive_glb);
        if (m.materials.size() != 1) {
            fail("emissive.glb: expected 1 material, got " +
                 std::to_string(m.materials.size()));
        }
        const msh::Material& mat = m.materials[0];
        if (mat.map_ke_bytes.empty()) {
            fail("emissive.glb: map_ke_bytes is empty — emissiveTexture "
                 "was not extracted");
        }
        for (std::size_t c = 0; c < 3; ++c) {
            if (mat.ke[c] < 0.999f || mat.ke[c] > 1.001f) {
                fail("emissive.glb: flat ke[" + std::to_string(c) + "] = " +
                     std::to_string(mat.ke[c]) + ", expected ~1");
            }
        }
    }

    const auto base = fs::temp_directory_path() /
        ("objtomc_emiquad_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    // ---- 1. determinism ----
    const fs::path world_a = (argc == 4) ? fs::path(argv[3]) : base / "world_a";
    const fs::path world_b = base / "world_b";
    try {
        orch::orchestrate(emissive_glb, world_a, opts);
        orch::orchestrate(emissive_glb, world_b, opts);
    } catch (const std::exception& e) {
        fail(std::string("emissive orchestrate threw: ") + e.what());
    }
    const auto mca_a = read_bytes(world_a / "region" / "r.0.0.mca");
    const auto mca_b = read_bytes(world_b / "region" / "r.0.0.mca");
    if (mca_a != mca_b) fail("emissive runs are not byte-identical");

    // ---- 2. activation vs the emissive-less quad.glb ----
    const fs::path world_c = base / "world_c";
    try {
        orch::orchestrate(plain_glb, world_c, opts);
    } catch (const std::exception& e) {
        fail(std::string("plain-quad orchestrate threw: ") + e.what());
    }
    const auto mca_c = read_bytes(world_c / "region" / "r.0.0.mca");
    if (mca_a == mca_c) {
        fail("emissive and emissive-less runs are byte-identical — "
             "per-voxel emissive sampling did not activate");
    }

    // ---- oracle EXPECT lines ----
    const auto db = objtomc::palette::load_default_palette();
    const objtomc::matcher::Matcher m(db, opts.matcher_opts);
    for (int vz = 0; vz < 4; ++vz) {
        for (int vx = 0; vx < 4; ++vx) {
            msh::Material mat;
            mat.name   = "quadmat";
            mat.kd     = quadrant_colour(vx, vz);
            mat.ke     = (vx < 2) ? std::array<float, 3>{1.0f, 1.0f, 1.0f}
                                  : std::array<float, 3>{0.0f, 0.0f, 0.0f};
            mat.ks     = {0.0f, 0.0f, 0.0f};
            mat.d      = 1.0f;
            mat.illum  = 2;
            const auto bs = m.match(mat);
            std::printf("EXPECT %d %d %s\n", vx, vz, bs.name.c_str());
        }
    }

    fs::remove_all(base);
    std::printf("PASS: orchestrate_emissive_quad (loader mapping + "
                "determinism + activation; 16 EXPECT lines emitted)\n");
    return 0;
}
