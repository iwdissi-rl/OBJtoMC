// orchestrate_target_size — Session 50 v0.3 item #3 test.
//
// voxel_size_for_target contract: the derived voxel_size must make the
// voxel layer's auto-sized grid EXACTLY target_size cells along the mesh
// AABB's longest axis, with the other axes scaled proportionally.
//
//   1. In-code triangle mesh with AABB extents 10 x 2 x 4 (longest = X):
//      exact dims through the real voxel layer for several targets —
//      N=1 → {1,1,1}, N=5 → {5,1,2}, N=10 → {10,2,4}, N=200 →
//      {200,40,80} (headroom factor prevents ceil rounding to N+1).
//   2. Orchestrate plumbing: cube.obj with opts.target_size = 8 produces
//      an r.0.0.mca byte-identical to an explicit-voxel_size run using
//      voxel_size_for_target's own value (one shared implementation).
//   3. Errors: target_size < 1, voxel_size+target_size both set, and a
//      degenerate point mesh all throw orchestrate_error.

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"
#include "objtomc/voxel.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace vox  = objtomc::voxel;
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

// One triangle spanning AABB extents 10 (x) x 2 (y) x 4 (z).
msh::Mesh tri_mesh() {
    msh::Mesh m;
    m.positions = {{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}, {10.0f, 2.0f, 4.0f}};
    msh::Face f{};
    f.position_indices = {0, 1, 2};
    f.uv_indices       = {-1, -1, -1};
    f.normal_indices   = {-1, -1, -1};
    f.material_id      = -1;
    m.faces.push_back(f);
    return m;
}

void check_dims(const msh::Mesh& m, int target,
                std::array<int, 3> want) {
    const float vs = orch::voxel_size_for_target(m, target);
    vox::VoxelizerOptions vopts = vox::default_options();
    vopts.voxel_size = vs;
    const auto dims = vox::voxelize_mesh(m, vopts).dimensions();
    for (int k = 0; k < 3; ++k) {
        if (dims[static_cast<std::size_t>(k)] != want[static_cast<std::size_t>(k)]) {
            fail("target " + std::to_string(target) + ": dims axis " +
                 std::to_string(k) + " = " +
                 std::to_string(dims[static_cast<std::size_t>(k)]) + ", want " +
                 std::to_string(want[static_cast<std::size_t>(k)]));
        }
    }
}

void expect_orch_error(const char* label, void (*body)()) {
    bool threw = false;
    try {
        body();
    } catch (const orch::orchestrate_error&) {
        threw = true;
    }
    if (!threw) fail(std::string(label) + ": expected orchestrate_error");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-cube.obj>\n";
        return 2;
    }
    const fs::path cube_path = argv[1];

    // --- 1. exact grid dims via the real voxel layer -----------------------
    const msh::Mesh tri = tri_mesh();
    check_dims(tri, 1,    {1, 1, 1});
    check_dims(tri, 5,    {5, 1, 2});
    check_dims(tri, 10,   {10, 2, 4});
    check_dims(tri, 200,  {200, 40, 80});

    // --- 2. orchestrate plumbing: target_size == explicit-voxel_size run ---
    const auto base = fs::temp_directory_path() /
        ("objtomc_targetsize_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    orch::OrchestrateOptions opts_a = orch::default_options();
    opts_a.target_size = 8;
    opts_a.now_unix_s  = 0;

    const msh::Mesh cube = msh::load_obj(cube_path);
    orch::OrchestrateOptions opts_b = orch::default_options();
    opts_b.voxel_size = orch::voxel_size_for_target(cube, 8);
    opts_b.now_unix_s = 0;

    try {
        orch::orchestrate(cube_path, base / "world_a", opts_a);
        orch::orchestrate(cube_path, base / "world_b", opts_b);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }
    const auto mca_a = read_bytes(base / "world_a" / "region" / "r.0.0.mca");
    const auto mca_b = read_bytes(base / "world_b" / "region" / "r.0.0.mca");
    if (mca_a != mca_b) {
        fail("target_size run differs from explicit voxel_size_for_target run");
    }

    // --- 3. error paths -----------------------------------------------------
    expect_orch_error("target_size 0", [] {
        (void)orch::voxel_size_for_target(tri_mesh(), 0);
    });
    expect_orch_error("point mesh", [] {
        msh::Mesh m = tri_mesh();
        m.positions = {{1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}};
        (void)orch::voxel_size_for_target(m, 8);
    });
    {
        bool threw = false;
        orch::OrchestrateOptions bad = orch::default_options();
        bad.voxel_size  = 0.5f;
        bad.target_size = 8;
        try {
            orch::orchestrate(cube_path,
                              fs::temp_directory_path() / "objtomc_ts_unreached",
                              bad);
        } catch (const orch::orchestrate_error&) {
            threw = true;
        }
        if (!threw) fail("voxel_size + target_size both set: expected orchestrate_error");
    }

    fs::remove_all(base);
    std::cout << "PASS: orchestrate_target_size (exact dims x4 targets, "
                 "plumbing byte-equality, 3 error paths)\n";
    return 0;
}
