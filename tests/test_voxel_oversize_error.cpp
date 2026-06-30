// voxel_oversize_error — proposal §3.12 (f).
//
// Three sub-cases:
//   (1) Auto-dims overflow — a tiny mesh AABB (1.0 OBJ-unit) with a
//       too-small voxel_size (1e-4) produces dims ~= {10000, 10000, 10000}
//       = 10^12 cells, far above MAX_GRID_CELLS_DEFAULT = 1024³. Throws
//       voxel_error with the word "exceeds" / "maximum".
//   (2) Explicit dims overflow — explicit dims {2048, 2048, 2048} = ~8.6e9
//       cells, above the default cap. Throws.
//   (3) max_grid_cells = 0 → validate_options throws "must be positive".

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

void expect_message_contains(const std::string& what,
                             const std::string& needle,
                             const std::string& case_name) {
    if (what.find(needle) == std::string::npos) {
        fail(case_name + ": voxel_error message '" + what +
             "' does not contain expected substring '" + needle + "'");
    }
}

// A single-triangle mesh that spans all three axes with extent 1.0 so the
// AABB is the full unit cube — needed so dim_z > 1 in the auto-dims
// overflow sub-case (a planar triangle would give dim_z = 1 and total
// cells would stay below the 1024³ cap at voxel_size = 1e-4).
msh::Mesh build_unit_extent_triangle() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 1.0f},
    };
    msh::Face f{};
    f.position_indices = {0, 1, 2};
    f.normal_indices   = {-1, -1, -1};
    f.uv_indices       = {-1, -1, -1};
    f.material_id      = 0;
    m.faces.push_back(f);
    return m;
}

}  // namespace

int main() {
    const msh::Mesh m = build_unit_extent_triangle();

    // (1) Auto-dims overflow: voxel_size 1e-4 over 1.0 AABB → 10000³ cells.
    {
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = 1.0e-4f;
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            // Either "exceeds" or "maximum" is fine; we check for "exceeds".
            expect_message_contains(e.what(), "exceeds",
                                    "auto-dims-overflow");
        }
        if (!threw) {
            fail("auto-dims overflow did not throw voxel_error");
        }
    }

    // (2) Explicit dims overflow: {2048, 2048, 2048} ≈ 8.6e9 cells.
    {
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = 1.0f;
        opts.dimensions = std::array<int, 3>{2048, 2048, 2048};
        opts.origin_world = std::array<float, 3>{0.0f, 0.0f, 0.0f};
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "exceeds",
                                    "explicit-dims-overflow");
        }
        if (!threw) {
            fail("explicit-dims overflow did not throw voxel_error");
        }
    }

    // (3) max_grid_cells = 0 → "must be positive".
    {
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size      = 1.0f;
        opts.max_grid_cells  = 0;
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "max_grid_cells",
                                    "zero-max-grid-cells");
            expect_message_contains(e.what(), "positive",
                                    "zero-max-grid-cells");
        }
        if (!threw) {
            fail("max_grid_cells = 0 did not throw voxel_error");
        }
    }

    // (3b) max_grid_cells = -1 → same code path.
    {
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size      = 1.0f;
        opts.max_grid_cells  = -1;
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "max_grid_cells",
                                    "negative-max-grid-cells");
        }
        if (!threw) {
            fail("max_grid_cells = -1 did not throw voxel_error");
        }
    }

    std::printf("PASS: voxel_oversize_error (4 sub-cases)\n");
    return 0;
}
