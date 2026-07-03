// ---------------------------------------------------------------------------
// voxel_gpu_determinism — Session 40 experimental verification of the
// atomicMin-with-packed-uint64 determinism strategy per
// research/gpu_port_proposal.md §3.8. Runs the GPU voxelizer 10x on the
// vendored Sponza @ 64^3 and asserts all 10 runs produce byte-identical
// VoxelGrid::cells_ output.
//
// If any two runs differ, do NOT loop-widen or accept variance — invoke
// §3.8 contingency plan (two-pass disambiguation or single-threaded scan)
// and report to maintainer per §3.12(a).
//
// Grid resolution 64^3 keeps the total wall-clock under a minute (10x
// runs @ ~1s each on RTX 4090 D) while still exercising the same kernel
// launch parameters + atomic-min path as the production 256^3 grid.
//
// SKIP behaviour: same as test_voxel_cpu_gpu_equality — CMake gate guards
// against builds under WITH_CUDA=OFF; the stub is defensive.
// ---------------------------------------------------------------------------

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef OBJTOMC_WITH_CUDA
#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

namespace {

constexpr int kRuns = 10;
constexpr int kDim  = 64;

// Byte-compare two grids of identical dimensions. Returns first-diff
// coordinate or {any=false} on match.
struct FirstDiff {
    bool          any;
    int           x, y, z;
    std::int32_t  ref_val;
    std::int32_t  new_val;
};

FirstDiff compare_grids(const objtomc::voxel::VoxelGrid& a,
                        const objtomc::voxel::VoxelGrid& b) {
    const auto d = a.dimensions();
    for (int y = 0; y < d[1]; ++y) {
        for (int z = 0; z < d[2]; ++z) {
            for (int x = 0; x < d[0]; ++x) {
                const std::int32_t av = a.at_unchecked(x, y, z);
                const std::int32_t bv = b.at_unchecked(x, y, z);
                if (av != bv) return {true, x, y, z, av, bv};
            }
        }
    }
    return {false, 0, 0, 0, 0, 0};
}

}  // namespace
#endif  // OBJTOMC_WITH_CUDA

int main(int argc, char** argv) {
#ifndef OBJTOMC_WITH_CUDA
    (void)argc; (void)argv;
    std::cout << "SKIP: WITH_CUDA not enabled\n";
    return 0;
#else
    if (argc != 2) {
        std::cerr << "usage: test_voxel_gpu_determinism <sponza.obj>\n";
        return 2;
    }
    objtomc::mesh::Mesh mesh;
    try {
        mesh = objtomc::mesh::load_obj(argv[1]);
    } catch (const std::exception& ex) {
        std::cerr << "FAIL: failed to load fixture " << argv[1] << ": "
                  << ex.what() << "\n";
        return 1;
    }
    std::cout << "loaded " << mesh.positions.size() << " positions, "
              << mesh.faces.size() << " faces from " << argv[1] << "\n";

    float min_x =  1e30f, min_y =  1e30f, min_z =  1e30f;
    float max_x = -1e30f, max_y = -1e30f, max_z = -1e30f;
    for (const auto& p : mesh.positions) {
        if (p[0] < min_x) min_x = p[0];
        if (p[1] < min_y) min_y = p[1];
        if (p[2] < min_z) min_z = p[2];
        if (p[0] > max_x) max_x = p[0];
        if (p[1] > max_y) max_y = p[1];
        if (p[2] > max_z) max_z = p[2];
    }
    const float ex = max_x - min_x;
    const float ey = max_y - min_y;
    const float ez = max_z - min_z;
    const float longest = ex > ey ? (ex > ez ? ex : ez) : (ey > ez ? ey : ez);
    const float voxel_size = longest / static_cast<float>(kDim);

    auto opts = objtomc::voxel::default_options();
    opts.voxel_size    = voxel_size;
    opts.origin_world  = std::array<float, 3>{min_x, min_y, min_z};
    opts.prefer_gpu    = objtomc::voxel::GpuPreference::ON;

    // First run: reference.
    using clock = std::chrono::steady_clock;
    using ms    = std::chrono::milliseconds;
    const auto t0 = clock::now();
    const objtomc::voxel::VoxelGrid ref =
        objtomc::voxel::voxelize_mesh(mesh, opts);
    const auto t1 = clock::now();
    std::cout << "run 1 (reference): " << ref.occupied_count()
              << " occupied, " << std::chrono::duration_cast<ms>(t1 - t0).count()
              << " ms\n";

    // Runs 2..N: byte-compare vs reference.
    for (int i = 2; i <= kRuns; ++i) {
        const auto rt0 = clock::now();
        const objtomc::voxel::VoxelGrid g =
            objtomc::voxel::voxelize_mesh(mesh, opts);
        const auto rt1 = clock::now();
        const auto d = compare_grids(ref, g);
        if (d.any) {
            std::cerr << "FAIL: run " << i << " differs from run 1 at ("
                      << d.x << "," << d.y << "," << d.z << "): "
                      << "ref=" << d.ref_val << " new=" << d.new_val << "\n"
                      << "Per proposal §3.8 contingency: do NOT loop-widen. "
                         "Invoke §3.12(a) triage — nondeterministic GPU "
                         "output; atomicMin packed-key strategy failed. "
                         "Fallback: two-pass or single-threaded scan. Report "
                         "to maintainer.\n";
            return 1;
        }
        std::cout << "run " << i << ": byte-identical to run 1 ("
                  << std::chrono::duration_cast<ms>(rt1 - rt0).count()
                  << " ms)\n";
    }
    std::cout << "PASS: all " << kRuns
              << " GPU runs byte-identical (occupied=" << ref.occupied_count()
              << ")\n";
    return 0;
#endif
}
