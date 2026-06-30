// voxel_unit_cube_smoke — proposal §3.12 (a).
//
// Builds a synthetic 12-triangle axis-aligned unit cube spanning [0, 1]³
// with a single material; voxelises at voxel_size = 0.125 (→ dimensions
// {8, 8, 8}); asserts the surface-shell occupancy is exactly 296 voxels
// (= 8³ − 6³, the outer shell of an axis-aligned cube at exact alignment),
// plus per-cell spot checks (corner occupied, interior empty, face-centre
// occupied), and the dimensions/voxel_size readback.
//
// The 296 count is exact for an axis-aligned cube at exact alignment;
// deviation indicates an SAT bug at exact-alignment edges (per the
// session prompt's STOP condition).

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

namespace msh = objtomc::mesh;
namespace vox = objtomc::voxel;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Hand-build a unit-cube mesh: 8 corners, 12 triangles, 1 material.
msh::Mesh build_unit_cube() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},   // v0, v1
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},   // v2, v3
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},   // v4, v5
        {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},   // v6, v7
    };

    // Face ordering: bottom, front, back, left, right, top.
    // Triangle indices (CCW winding viewed from outside):
    const std::array<std::array<std::uint32_t, 3>, 12> tris = {{
        {0, 4, 1}, {1, 4, 5},   // bottom (y=0)
        {0, 1, 3}, {0, 3, 2},   // front  (z=0)
        {4, 6, 5}, {5, 6, 7},   // back   (z=1)
        {0, 2, 6}, {0, 6, 4},   // left   (x=0)
        {1, 5, 7}, {1, 7, 3},   // right  (x=1)
        {2, 3, 7}, {2, 7, 6},   // top    (y=1)
    }};

    for (const auto& t : tris) {
        msh::Face f{};
        f.position_indices = t;
        f.normal_indices   = {-1, -1, -1};
        f.uv_indices       = {-1, -1, -1};
        f.material_id      = 0;
        m.faces.push_back(f);
    }

    msh::Material mat{};
    mat.name = "CubeMat";
    mat.kd   = {0.5f, 0.5f, 0.5f};
    mat.ke   = {0.0f, 0.0f, 0.0f};
    mat.d    = 1.0f;
    mat.illum = 2;
    m.materials.push_back(mat);
    return m;
}

}  // namespace

int main() {
    const msh::Mesh m = build_unit_cube();

    vox::VoxelizerOptions opts = vox::default_options();
    opts.voxel_size = 0.125f;

    const vox::VoxelGrid grid = vox::voxelize_mesh(m, opts);

    // Dimensions: {8, 8, 8}.
    const auto dims = grid.dimensions();
    if (dims[0] != 8 || dims[1] != 8 || dims[2] != 8) {
        fail("dimensions expected {8, 8, 8}, got {" +
             std::to_string(dims[0]) + ", " +
             std::to_string(dims[1]) + ", " +
             std::to_string(dims[2]) + "}");
    }
    if (grid.voxel_size() != 0.125f) {
        fail("voxel_size expected 0.125, got " +
             std::to_string(grid.voxel_size()));
    }

    // Occupancy count: 8³ − 6³ = 296 (outer shell).
    const std::size_t occ = grid.occupied_count();
    if (occ != 296) {
        fail("occupied_count expected 296 (= 8³−6³), got " +
             std::to_string(occ));
    }

    // Spot checks per proposal §3.12 (a):
    //   - corner (0, 0, 0) occupied
    //   - interior (3, 3, 3) empty (surface-only voxelisation)
    //   - face-centre (0, 4, 4) on left face occupied
    if (grid.at(0, 0, 0) == vox::EMPTY) fail("corner (0,0,0) should be occupied");
    if (grid.at(3, 3, 3) != vox::EMPTY) {
        fail("interior (3,3,3) should be EMPTY (surface-only), got " +
             std::to_string(grid.at(3, 3, 3)));
    }
    if (grid.at(0, 4, 4) == vox::EMPTY) fail("face centre (0,4,4) should be occupied");

    // All occupied cells must hold material_id 0 (the only material).
    grid.for_each_voxel([&](int, int, int, std::int32_t mid) {
        if (mid != 0) {
            fail("expected material_id 0 everywhere, got " +
                 std::to_string(mid));
        }
    });

    std::printf("PASS: voxel_unit_cube_smoke\n"
                "  dims=%dx%dx%d voxel_size=%.3f occupied=%zu (= 8^3 - 6^3 = 296)\n",
                dims[0], dims[1], dims[2],
                static_cast<double>(grid.voxel_size()), occ);
    return 0;
}
