// alignment_ops — v0.5 sub-item (b) unit gates (Session 69, proposal §3).
//
//   1. An axis-aligned box detects ~0 correction.
//   2. A box rotated by +17 deg (rotate_mesh_y(17)) detects ~-17 — and
//      applying the detected correction round-trips detection to ~0 AND
//      restores the axis-aligned voxelisation (same grid dims + occupied
//      count as the never-rotated box through the real voxel layer, which
//      is the whole point: stair-stepping eliminated).
//   3. Sign coverage: rotate_mesh_y(-30) detects ~+30.
//   4. A horizontal-only mesh (no facade signal) detects exactly 0.
//   5. Determinism: two runs on identical input give identical results.

#include "objtomc/alignment.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace aln = objtomc::alignment;
namespace msh = objtomc::mesh;
namespace vox = objtomc::voxel;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

msh::Mesh make_box(float sx, float sy, float sz) {
    // Non-integer SPANS keep the far faces mid-cell relative to the
    // auto origin (= AABB min): with integer spans the far faces sit
    // exactly ON cell boundaries (the knife-edge frame from
    // OPEN_QUESTIONS 2026-09-14) and the ~1e-6 round-trip origin shift
    // flips whole planes, which is scan-irrelevant test pathology. The
    // +0.3 offset is cosmetic once spans are non-integer.
    constexpr float o = 0.3f;
    msh::Mesh m;
    m.positions = {{o, o, o},           {sx + o, o, o},
                   {o, sy + o, o},      {sx + o, sy + o, o},
                   {o, o, sz + o},      {sx + o, o, sz + o},
                   {o, sy + o, sz + o}, {sx + o, sy + o, sz + o}};
    static const std::array<std::array<std::uint32_t, 3>, 12> tris = {{
        {0, 4, 1}, {1, 4, 5}, {0, 1, 3}, {0, 3, 2}, {4, 6, 5}, {5, 6, 7},
        {0, 2, 6}, {0, 6, 4}, {1, 5, 7}, {1, 7, 3}, {2, 3, 7}, {2, 7, 6},
    }};
    for (const auto& t : tris) {
        msh::Face f{};
        f.position_indices = t;
        f.normal_indices   = {-1, -1, -1};
        f.uv_indices       = {-1, -1, -1};
        f.material_id      = 0;
        m.faces.push_back(f);
    }
    return m;
}

void expect_near(float got, float want, float tol, const std::string& what) {
    if (std::fabs(got - want) > tol) {
        fail(what + ": got " + std::to_string(got) + ", want " +
             std::to_string(want) + " +/- " + std::to_string(tol));
    }
}

vox::VoxelGrid voxelize(const msh::Mesh& m) {
    vox::VoxelizerOptions o = vox::default_options();
    o.voxel_size = 1.0f;
#ifdef OBJTOMC_WITH_CUDA
    o.prefer_gpu = vox::GpuPreference::OFF;
#endif
    return vox::voxelize_mesh(m, o);
}

}  // namespace

int main() {
    // --- 1. axis-aligned box -> ~0 -----------------------------------------
    {
        const msh::Mesh box = make_box(20.4f, 10.4f, 12.4f);
        expect_near(aln::detect_grid_angle(box), 0.0f, 0.5f, "aligned box");
        std::cout << "PASS: aligned box detects ~0\n";
    }

    // --- 2. +17 deg rotation -> ~-17, round-trip restores voxelisation ------
    {
        const msh::Mesh box = make_box(20.4f, 10.4f, 12.4f);
        const vox::VoxelGrid ref = voxelize(box);
        const auto ref_dims = ref.dimensions();
        const std::size_t ref_occ = ref.occupied_count();

        msh::Mesh rot = box;
        aln::rotate_mesh_y(rot, 17.0f);
        const float corr = aln::detect_grid_angle(rot);
        expect_near(corr, -17.0f, 0.5f, "rotated(+17) correction");

        aln::rotate_mesh_y(rot, corr);
        expect_near(aln::detect_grid_angle(rot), 0.0f, 0.5f,
                    "round-trip residual");

        const vox::VoxelGrid back = voxelize(rot);
        const auto bd = back.dimensions();
        // Float residue from rotate+unrotate can inflate the AABB by ulps,
        // and auto-dims ceil() then adds one (mostly empty) cell — allow
        // ref..ref+1 per axis. The stair-step blow-up this guards against
        // would inflate dims by several cells AND occupancy by hundreds.
        for (int k = 0; k < 3; ++k) {
            const int r = ref_dims[static_cast<std::size_t>(k)];
            const int g = bd[static_cast<std::size_t>(k)];
            if (g < r || g > r + 1) {
                fail("re-aligned box dim[" + std::to_string(k) + "] = " +
                     std::to_string(g) + " outside [" + std::to_string(r) +
                     ", " + std::to_string(r + 1) + "]");
            }
        }
        const std::size_t bo = back.occupied_count();
        // Float round-trip can flip a handful of knife-edge cells; the
        // stair-step blow-up this guards against would be hundreds.
        const std::size_t hi = ref_occ + ref_occ / 50 + 2;
        const std::size_t lo = ref_occ - std::min(ref_occ, ref_occ / 50 + 2);
        if (bo < lo || bo > hi) {
            fail("re-aligned occupancy " + std::to_string(bo) +
                 " outside [" + std::to_string(lo) + ", " +
                 std::to_string(hi) + "] around reference " +
                 std::to_string(ref_occ));
        }
        std::cout << "PASS: +17 deg detect/round-trip, voxelisation restored ("
                  << ref_occ << " -> " << bo << ")\n";
    }

    // --- 3. sign coverage ----------------------------------------------------
    {
        msh::Mesh rot = make_box(20.4f, 10.4f, 12.4f);
        aln::rotate_mesh_y(rot, -30.0f);
        expect_near(aln::detect_grid_angle(rot), 30.0f, 0.5f,
                    "rotated(-30) correction");
        std::cout << "PASS: -30 deg detects +30\n";
    }

    // --- 4. no facade signal -> exactly 0 ------------------------------------
    {
        msh::Mesh flat;
        flat.positions = {{0, 0.5f, 0}, {4, 0.5f, 0}, {0, 0.5f, 4}, {4, 0.5f, 4}};
        for (const auto t : {std::array<std::uint32_t, 3>{0, 2, 1},
                             std::array<std::uint32_t, 3>{1, 2, 3}}) {
            msh::Face f{};
            f.position_indices = t;
            f.normal_indices   = {-1, -1, -1};
            f.uv_indices       = {-1, -1, -1};
            f.material_id      = 0;
            flat.faces.push_back(f);
        }
        if (aln::detect_grid_angle(flat) != 0.0f) {
            fail("horizontal-only mesh returned nonzero correction");
        }
        std::cout << "PASS: horizontal-only mesh -> 0\n";
    }

    // --- 5. determinism -------------------------------------------------------
    {
        msh::Mesh a = make_box(20.4f, 10.4f, 12.4f);
        msh::Mesh b = make_box(20.4f, 10.4f, 12.4f);
        aln::rotate_mesh_y(a, 23.0f);
        aln::rotate_mesh_y(b, 23.0f);
        if (aln::detect_grid_angle(a) != aln::detect_grid_angle(b) ||
            a.positions != b.positions) {
            fail("non-deterministic");
        }
        std::cout << "PASS: determinism\n";
    }

    std::cout << "PASS: alignment_ops\n";
    return 0;
}
