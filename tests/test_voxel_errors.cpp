// voxel_errors — proposal §3.12 (g).
//
// 7 sub-cases — each expected to throw voxel_error with a substring naming
// the offending condition:
//
//   (1) empty mesh (no faces)          → "no faces"
//   (2) empty mesh (no positions)      → "no positions"
//   (3) voxel_size = 0                 → "voxel_size"
//   (4) voxel_size = -1                → "voxel_size"
//   (5) voxel_size = +Inf              → "finite"
//   (6) dimensions = {0, 0, 0}         → "dimensions"
//   (7) dimensions insufficient        → "cannot cover"
//   (8) non-finite mesh position (NaN) → "non-finite"

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
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

msh::Mesh build_unit_triangle() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
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
    // (1) Empty mesh: no faces.
    {
        msh::Mesh m;
        m.positions = {{0.0f, 0.0f, 0.0f}};   // non-empty positions
        vox::VoxelizerOptions opts = vox::default_options();
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "no faces", "empty-faces");
        }
        if (!threw) fail("empty faces did not throw voxel_error");
    }

    // (2) Empty mesh: no positions.
    {
        msh::Mesh m;   // both empty
        vox::VoxelizerOptions opts = vox::default_options();
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "no positions",
                                    "empty-positions");
        }
        if (!threw) fail("empty positions did not throw voxel_error");
    }

    // (3) voxel_size = 0.
    {
        const msh::Mesh m = build_unit_triangle();
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = 0.0f;
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "voxel_size", "voxel-size-zero");
        }
        if (!threw) fail("voxel_size = 0 did not throw voxel_error");
    }

    // (4) voxel_size = -1.
    {
        const msh::Mesh m = build_unit_triangle();
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = -1.0f;
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "voxel_size", "voxel-size-neg");
        }
        if (!threw) fail("voxel_size = -1 did not throw voxel_error");
    }

    // (5) voxel_size = +Inf.
    {
        const msh::Mesh m = build_unit_triangle();
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = std::numeric_limits<float>::infinity();
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "finite", "voxel-size-inf");
        }
        if (!threw) fail("voxel_size = +Inf did not throw voxel_error");
    }

    // (6) dimensions = {0, 0, 0}.
    {
        const msh::Mesh m = build_unit_triangle();
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size = 0.1f;
        opts.dimensions = std::array<int, 3>{0, 0, 0};
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "dimensions", "zero-dimensions");
        }
        if (!threw) fail("dimensions {0,0,0} did not throw voxel_error");
    }

    // (7) dimensions = {5, 5, 5} with voxel_size = 0.1 → 0.5 OBJ-units of
    // coverage per axis, but the unit-triangle mesh AABB spans 1.0 → throws.
    {
        const msh::Mesh m = build_unit_triangle();
        vox::VoxelizerOptions opts = vox::default_options();
        opts.voxel_size   = 0.1f;
        opts.dimensions   = std::array<int, 3>{5, 5, 5};
        opts.origin_world = std::array<float, 3>{0.0f, 0.0f, 0.0f};
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "cannot cover",
                                    "insufficient-dimensions");
        }
        if (!threw) {
            fail("insufficient dimensions did not throw voxel_error");
        }
    }

    // (8) Non-finite mesh position (NaN).
    {
        msh::Mesh m = build_unit_triangle();
        m.positions[1][1] = std::nanf("");
        vox::VoxelizerOptions opts = vox::default_options();
        bool threw = false;
        try {
            (void)vox::voxelize_mesh(m, opts);
        } catch (const vox::voxel_error& e) {
            threw = true;
            expect_message_contains(e.what(), "non-finite", "nan-position");
        }
        if (!threw) fail("NaN position did not throw voxel_error");
    }

    std::printf("PASS: voxel_errors (8 sub-cases)\n");
    return 0;
}
