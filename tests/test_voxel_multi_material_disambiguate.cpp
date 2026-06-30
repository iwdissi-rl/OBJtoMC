// voxel_multi_material_disambiguate — proposal §3.12 (c).
//
// Synthetic 2-material unit cube: bottom + 4 side faces use material 1;
// the top 2 faces use material 0. Voxelised at 8³ with the default
// CLOSEST_TO_CENTRE disambiguation rule.
//
// Asserts (per proposal §3.5 + §3.12 (c)):
//   - Interior top voxel (4, 7, 4): material 0 (only the top-face triangles
//     overlap this voxel — the side faces are too far).
//   - Interior bottom voxel (4, 0, 4): material 1.
//   - Interior left-side voxel (0, 4, 4): material 1.
//   - Interior front-side voxel (4, 4, 0): material 1.
//   - Top corner voxel (0, 7, 0): tied between top (mat 0), front (mat 1),
//     and left (mat 1) at distance 0.0625. Per (d, triangle_index)
//     comparison key + face-emit order (bottom→front→back→left→right→top),
//     the front face's upper-left triangle wins first → material 1.
//     The proposal §3.12(c) explicitly predicts this outcome.
//
// If the corner-voxel result differs from the proposal's prediction
// (material 0 instead of material 1), STOP per the session prompt's
// algorithmic discipline — could be a tie-break bug OR the proposal's
// prediction was wrong; both worth investigating.

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

msh::Mesh build_two_material_cube() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},   // v0, v1
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},   // v2, v3
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},   // v4, v5
        {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},   // v6, v7
    };

    struct TriSpec {
        std::array<std::uint32_t, 3> indices;
        std::int32_t                 material_id;
    };

    // Face emit order matters for the corner tie-break: front comes BEFORE
    // top in the list, so when voxel (0, 7, 0) is tied between the front
    // face's upper-left triangle and the top face's triangle, the front
    // face wins first (lower triangle index, material 1).
    const std::array<TriSpec, 12> tris = {{
        // bottom (y=0) — material 1
        {{0, 4, 1}, 1}, {{1, 4, 5}, 1},
        // front (z=0) — material 1
        {{0, 1, 3}, 1}, {{0, 3, 2}, 1},
        // back (z=1) — material 1
        {{4, 6, 5}, 1}, {{5, 6, 7}, 1},
        // left (x=0) — material 1
        {{0, 2, 6}, 1}, {{0, 6, 4}, 1},
        // right (x=1) — material 1
        {{1, 5, 7}, 1}, {{1, 7, 3}, 1},
        // top (y=1) — material 0
        {{2, 3, 7}, 0}, {{2, 7, 6}, 0},
    }};

    for (const auto& t : tris) {
        msh::Face f{};
        f.position_indices = t.indices;
        f.normal_indices   = {-1, -1, -1};
        f.uv_indices       = {-1, -1, -1};
        f.material_id      = t.material_id;
        m.faces.push_back(f);
    }

    // Two materials.
    msh::Material mat0{};
    mat0.name = "TopMat";
    mat0.kd   = {1.0f, 0.8f, 0.2f};
    mat0.d    = 1.0f;
    m.materials.push_back(mat0);

    msh::Material mat1{};
    mat1.name = "SidesAndBottomMat";
    mat1.kd   = {0.2f, 0.4f, 0.9f};
    mat1.d    = 1.0f;
    m.materials.push_back(mat1);

    return m;
}

void expect_mid(const vox::VoxelGrid& g, int x, int y, int z,
                std::int32_t want, const char* label) {
    const std::int32_t got = g.at(x, y, z);
    if (got != want) {
        fail(std::string(label) + " at (" + std::to_string(x) + ", " +
             std::to_string(y) + ", " + std::to_string(z) +
             "): expected material_id " + std::to_string(want) +
             ", got " + std::to_string(got));
    }
}

}  // namespace

int main() {
    const msh::Mesh m = build_two_material_cube();

    vox::VoxelizerOptions opts = vox::default_options();
    opts.voxel_size = 0.125f;

    const vox::VoxelGrid grid = vox::voxelize_mesh(m, opts);

    const auto dims = grid.dimensions();
    if (dims[0] != 8 || dims[1] != 8 || dims[2] != 8) {
        fail("dimensions expected {8, 8, 8}, got {" +
             std::to_string(dims[0]) + ", " +
             std::to_string(dims[1]) + ", " +
             std::to_string(dims[2]) + "}");
    }

    // ---- interior top: only top face overlaps → material 0 ----
    expect_mid(grid, 4, 7, 4, 0, "interior top");

    // ---- interior bottom: only bottom face → material 1 ----
    expect_mid(grid, 4, 0, 4, 1, "interior bottom");

    // ---- interior side faces: only the relevant side face → material 1 ----
    expect_mid(grid, 0, 4, 4, 1, "interior left side");
    expect_mid(grid, 7, 4, 4, 1, "interior right side");
    expect_mid(grid, 4, 4, 0, 1, "interior front side");
    expect_mid(grid, 4, 4, 7, 1, "interior back side");

    // ---- corner voxel (0, 7, 0): proposal §3.12(c) prediction ----
    // Tied at distance 0.0625 between top (mat 0), front (mat 1), and
    // left (mat 1) face triangles. Front face triangles emit at indices
    // 2-3, before top's 10-11, so front wins by lower index → material 1.
    const std::int32_t corner_mid = grid.at(0, 7, 0);
    std::printf("INFO: corner (0,7,0) material_id = %d "
                "(proposal §3.12(c) predicts 1)\n", corner_mid);
    if (corner_mid != 1) {
        fail("top corner (0, 7, 0): expected material 1 per proposal "
             "§3.12(c) (front face's upper-left triangle wins on "
             "(distance, triangle_index) tie-break), got " +
             std::to_string(corner_mid) +
             ". STOP per session-prompt algorithmic discipline — could "
             "be a tie-break bug OR the proposal's prediction was wrong.");
    }

    std::printf("PASS: voxel_multi_material_disambiguate\n"
                "  interior top (4,7,4)=mat0, interior bot (4,0,4)=mat1\n"
                "  interior sides=mat1; corner (0,7,0)=mat1 "
                "(front face wins tie-break per §3.12(c))\n");
    return 0;
}
