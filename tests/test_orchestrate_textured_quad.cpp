// orchestrate_textured_quad — Session 48 v0.3 item #1 integration test +
// oracle helper.
//
// Fixture: tests/fixtures/texture/quad.obj — a 4×4 quad in the XZ plane at
// y = 0.5, UV-mapped u = x/4, v = z/4, material quadmat (flat Kd mid-grey,
// map_Kd checker.png). checker.png is 2×2: top row green|white, bottom row
// red|blue. With voxel_size = 1, explicit grid dims {4,1,4} and grid origin
// (0,0,0), every voxel centre (vx+0.5, 0.5, vz+0.5) lies ON the quad, so
// all 16 voxels fill, and the sampled quadrant colours are:
//     vx<2,vz<2 → red   vx≥2,vz<2 → blue
//     vx<2,vz≥2 → green vx≥2,vz≥2 → white
// (v < 0.5 samples the image BOTTOM row via the OBJ V flip.)
//
// CTest assertions (shape level — block-exact checks are the oracle's job):
//   1. Determinism: two textured runs produce byte-identical r.0.0.mca.
//   2. Activation: a run against a copy of obj+mtl WITHOUT checker.png
//      (flat-Kd fallback) produces a DIFFERENT r.0.0.mca — proof the
//      texture path changes output rather than silently no-oping.
//
// Oracle protocol: prints "EXPECT <vx> <vz> <block_name>" for all 16
// voxels, computed independently via matcher::match on Materials whose kd
// is the exact quadrant colour (same palette, same matcher options). With
// world_origin {0,-60,0} voxel (vx,0,vz) lands at block (vx,-60,vz) —
// chunk (0,0), section Y=-4, ly=4, lx=vx, lz=vz. If argv[2] is given the
// first textured world is written there and kept for the oracle to parse.

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
    opts.level_name               = "objtomc-textured-quad";
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
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <path-to-quad.obj> [keep-world-dir]\n";
        return 2;
    }
    const fs::path obj_path = argv[1];
    const auto opts = quad_options();

    const auto base = fs::temp_directory_path() /
        ("objtomc_texquad_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    // Run A (kept for the oracle when argv[2] is given) + run B: determinism.
    const fs::path world_a = (argc == 3) ? fs::path(argv[2]) : base / "world_a";
    const fs::path world_b = base / "world_b";
    try {
        orch::orchestrate(obj_path, world_a, opts);
        orch::orchestrate(obj_path, world_b, opts);
    } catch (const std::exception& e) {
        fail(std::string("textured orchestrate threw: ") + e.what());
    }

    const auto mca_a = read_bytes(world_a / "region" / "r.0.0.mca");
    const auto mca_b = read_bytes(world_b / "region" / "r.0.0.mca");
    if (mca_a != mca_b) fail("textured runs are not byte-identical");

    // Run C: same obj+mtl copied WITHOUT checker.png → flat-Kd fallback.
    // Output must differ from the textured run (texture path activation).
    const fs::path no_png_dir = base / "no_png";
    fs::create_directories(no_png_dir);
    fs::copy_file(obj_path, no_png_dir / obj_path.filename());
    const fs::path mtl_src = obj_path.parent_path() / "quad.mtl";
    fs::copy_file(mtl_src, no_png_dir / "quad.mtl");

    const fs::path world_c = base / "world_c";
    std::cerr << "(expected texture-fallback warning follows)\n";
    try {
        orch::orchestrate(no_png_dir / obj_path.filename(), world_c, opts);
    } catch (const std::exception& e) {
        fail(std::string("fallback orchestrate threw: ") + e.what());
    }
    const auto mca_c = read_bytes(world_c / "region" / "r.0.0.mca");
    if (mca_a == mca_c) {
        fail("textured and texture-less runs are byte-identical — "
             "texture sampling did not activate");
    }

    // Independent per-voxel expectations for the oracle.
    const auto db = objtomc::palette::load_default_palette();
    const objtomc::matcher::Matcher m(db, opts.matcher_opts);
    for (int vz = 0; vz < 4; ++vz) {
        for (int vx = 0; vx < 4; ++vx) {
            msh::Material mat;
            mat.name   = "quadmat";
            mat.kd     = quadrant_colour(vx, vz);
            mat.ke     = {0.0f, 0.0f, 0.0f};
            mat.ks     = {0.0f, 0.0f, 0.0f};
            mat.d      = 1.0f;
            mat.illum  = 2;
            mat.map_kd = "checker.png";
            const auto bs = m.match(mat);
            std::printf("EXPECT %d %d %s\n", vx, vz, bs.name.c_str());
        }
    }

    fs::remove_all(base);
    std::printf("PASS: orchestrate_textured_quad (determinism + activation; "
                "16 EXPECT lines emitted)\n");
    return 0;
}
