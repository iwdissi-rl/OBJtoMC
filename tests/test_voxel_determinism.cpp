// voxel_determinism — proposal §3.12 (e).
//
// Voxelises the same synthetic 12-triangle unit cube twice with identical
// opts; asserts the two grids are byte-identical via an at_unchecked walk.
// Catches any reliance on iteration-order non-determinism, uninitialised
// memory, system-time / rand() / unordered-container iteration order.

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

msh::Mesh build_unit_cube() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
    };
    const std::array<std::array<std::uint32_t, 3>, 12> tris = {{
        {0, 4, 1}, {1, 4, 5},
        {0, 1, 3}, {0, 3, 2},
        {4, 6, 5}, {5, 6, 7},
        {0, 2, 6}, {0, 6, 4},
        {1, 5, 7}, {1, 7, 3},
        {2, 3, 7}, {2, 7, 6},
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
    mat.name = "M";
    mat.kd   = {0.5f, 0.5f, 0.5f};
    mat.d    = 1.0f;
    m.materials.push_back(mat);
    return m;
}

}  // namespace

int main() {
    const msh::Mesh m = build_unit_cube();

    vox::VoxelizerOptions opts = vox::default_options();
    opts.voxel_size = 0.125f;

    const vox::VoxelGrid g1 = vox::voxelize_mesh(m, opts);
    const vox::VoxelGrid g2 = vox::voxelize_mesh(m, opts);

    if (g1.dimensions() != g2.dimensions()) {
        fail("dimensions differ between runs");
    }
    if (g1.voxel_size() != g2.voxel_size()) {
        fail("voxel_size differs between runs");
    }
    if (g1.origin_world() != g2.origin_world()) {
        fail("origin_world differs between runs");
    }

    const auto dims = g1.dimensions();
    std::size_t mismatches = 0;
    for (int y = 0; y < dims[1]; ++y) {
        for (int z = 0; z < dims[2]; ++z) {
            for (int x = 0; x < dims[0]; ++x) {
                if (g1.at_unchecked(x, y, z) != g2.at_unchecked(x, y, z)) {
                    ++mismatches;
                }
            }
        }
    }
    if (mismatches != 0) {
        fail(std::to_string(mismatches) +
             " cell mismatch(es) between two identical voxelisations");
    }

    if (g1.occupied_count() != g2.occupied_count()) {
        fail("occupied_count differs: " +
             std::to_string(g1.occupied_count()) + " vs " +
             std::to_string(g2.occupied_count()));
    }

    std::printf("PASS: voxel_determinism (2 voxelisations, %zu cells, %zu occupied, byte-identical)\n",
                static_cast<std::size_t>(dims[0]) *
                static_cast<std::size_t>(dims[1]) *
                static_cast<std::size_t>(dims[2]),
                g1.occupied_count());
    return 0;
}
