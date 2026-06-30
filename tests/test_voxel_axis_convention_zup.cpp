// voxel_axis_convention_zup — proposal §3.12 (d).
//
// Builds a synthetic mesh with a single triangle on the Z = 0.875 plane
// (XY-axis-aligned, at "Z near top"). Voxelises twice:
//
//   (1) With AxisConvention::Z_UP: the OBJ Y/Z are swapped at voxelisation
//       start, so the triangle lands at Y ≈ 0.875 in voxel-space (top
//       layer) and Z = 0 — assert the top layer (y == 3) is occupied at
//       the triangle's footprint and the bottom layer (y == 0) is empty.
//
//   (2) With AxisConvention::Y_UP (sanity mirror): the same triangle stays
//       at Z ≈ 0.875 in OBJ-space and the top-z slab (z == 3) is occupied;
//       y == 0 is empty.
//
// Grid: dimensions = {4, 4, 4}, voxel_size = 0.25, origin_world = (0,0,0).

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

#include <array>
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

// Triangle filling the XY square at z = 0.875: two triangles to cover the
// whole z-slab face (so SAT picks them up reliably across the slab).
msh::Mesh build_z_high_plane() {
    msh::Mesh m;
    constexpr float zh = 0.875f;
    m.positions = {
        {0.0f, 0.0f, zh}, {1.0f, 0.0f, zh},
        {0.0f, 1.0f, zh}, {1.0f, 1.0f, zh},
    };
    msh::Face f0{};
    f0.position_indices = {0, 1, 3};
    f0.normal_indices   = {-1, -1, -1};
    f0.uv_indices       = {-1, -1, -1};
    f0.material_id      = 0;
    m.faces.push_back(f0);

    msh::Face f1{};
    f1.position_indices = {0, 3, 2};
    f1.normal_indices   = {-1, -1, -1};
    f1.uv_indices       = {-1, -1, -1};
    f1.material_id      = 0;
    m.faces.push_back(f1);

    msh::Material mat{};
    mat.name = "PlaneMat";
    mat.kd   = {0.5f, 0.5f, 0.5f};
    mat.d    = 1.0f;
    m.materials.push_back(mat);
    return m;
}

}  // namespace

int main() {
    const msh::Mesh m = build_z_high_plane();

    vox::VoxelizerOptions opts = vox::default_options();
    opts.voxel_size  = 0.25f;
    opts.dimensions  = std::array<int, 3>{4, 4, 4};
    opts.origin_world = std::array<float, 3>{0.0f, 0.0f, 0.0f};

    // ---- (1) Z_UP: OBJ Z becomes voxel Y; triangle at z=0.875 → voxel-y top
    {
        opts.axis_convention = vox::AxisConvention::Z_UP;
        const vox::VoxelGrid g = vox::voxelize_mesh(m, opts);

        std::size_t top_occ = 0;
        std::size_t bot_occ = 0;
        for (int x = 0; x < 4; ++x) {
            for (int z = 0; z < 4; ++z) {
                if (g.at(x, 3, z) != vox::EMPTY) ++top_occ;
                if (g.at(x, 0, z) != vox::EMPTY) ++bot_occ;
            }
        }
        if (top_occ != 16) {
            fail("Z_UP: top layer (y=3) expected 16 occupied, got " +
                 std::to_string(top_occ));
        }
        if (bot_occ != 0) {
            fail("Z_UP: bottom layer (y=0) expected 0 occupied, got " +
                 std::to_string(bot_occ));
        }
        std::printf("Z_UP: top y=3 occ=%zu, bot y=0 occ=%zu (expected 16, 0)\n",
                    top_occ, bot_occ);
    }

    // ---- (2) Y_UP mirror: triangle stays at z=0.875 → voxel-z top
    {
        opts.axis_convention = vox::AxisConvention::Y_UP;
        const vox::VoxelGrid g = vox::voxelize_mesh(m, opts);

        std::size_t top_occ = 0;
        std::size_t bot_occ = 0;
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                if (g.at(x, y, 3) != vox::EMPTY) ++top_occ;
                if (g.at(x, y, 0) != vox::EMPTY) ++bot_occ;
            }
        }
        if (top_occ != 16) {
            fail("Y_UP: top-z layer (z=3) expected 16 occupied, got " +
                 std::to_string(top_occ));
        }
        if (bot_occ != 0) {
            fail("Y_UP: bottom-z layer (z=0) expected 0 occupied, got " +
                 std::to_string(bot_occ));
        }
        std::printf("Y_UP: top z=3 occ=%zu, bot z=0 occ=%zu (expected 16, 0)\n",
                    top_occ, bot_occ);
    }

    std::printf("PASS: voxel_axis_convention_zup (Z_UP swap + Y_UP mirror)\n");
    return 0;
}
