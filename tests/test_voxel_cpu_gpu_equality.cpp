// ---------------------------------------------------------------------------
// voxel_cpu_gpu_equality — Session 40 correctness gate for the GPU voxelizer
// path. Runs the vendored Sponza mesh through voxelize_mesh() twice at each
// of two resolutions (64^3 fast + 256^3 production-scale), once with
// prefer_gpu=OFF (CPU) and once with prefer_gpu=ON (GPU), and asserts the
// resulting VoxelGrid::cells_ arrays are byte-identical.
//
// See research/gpu_port_proposal.md §3.6 for the test contract. Byte-
// equality is the primary correctness gate; any inequality is a real bug
// per §3.12 first-run mismatch triage protocol — do NOT modify the test
// bands to pass, invoke the triage flow instead.
//
// SKIP behaviour: when the CPU-only build (WITH_CUDA=OFF) is used,
// OBJTOMC_WITH_CUDA is undefined and this executable prints "SKIP" +
// returns 0. The CMake gate `if(WITH_CUDA_ENABLED)` at tests/CMakeLists.txt
// prevents this file from ever being built in the CPU-only path, so the
// skip stub is defensive only.
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

// Byte-equality accessor helper — VoxelGrid::at_unchecked() reads the
// same underlying cells_ entry that both CPU and GPU paths write. Walk
// dims order = same for both.
struct FirstDiff {
    bool             any;
    int              x, y, z;
    std::int32_t     cpu_val;
    std::int32_t     gpu_val;
};

FirstDiff compare_grids(const objtomc::voxel::VoxelGrid& cpu,
                        const objtomc::voxel::VoxelGrid& gpu) {
    const auto cd = cpu.dimensions();
    const auto gd = gpu.dimensions();
    if (cd != gd) {
        std::cerr << "FAIL: dimension mismatch: CPU=("
                  << cd[0] << "," << cd[1] << "," << cd[2] << ") GPU=("
                  << gd[0] << "," << gd[1] << "," << gd[2] << ")\n";
        std::exit(1);
    }
    for (int y = 0; y < cd[1]; ++y) {
        for (int z = 0; z < cd[2]; ++z) {
            for (int x = 0; x < cd[0]; ++x) {
                const std::int32_t cv = cpu.at_unchecked(x, y, z);
                const std::int32_t gv = gpu.at_unchecked(x, y, z);
                if (cv != gv) return {true, x, y, z, cv, gv};
            }
        }
    }
    return {false, 0, 0, 0, 0, 0};
}

int run_one_resolution(const objtomc::mesh::Mesh& mesh,
                       int dim, const char* label) {
    using clock = std::chrono::steady_clock;
    using ms    = std::chrono::milliseconds;

    // Compute a voxel_size that produces exactly a dim^3 grid over the
    // Sponza AABB (same setup as tests/test_orchestrate_sponza_scale). We
    // use dim on longest axis; other axes shrink to fit.
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
    const float voxel_size = longest / static_cast<float>(dim);

    auto opts = objtomc::voxel::default_options();
    opts.voxel_size = voxel_size;
    // Explicit origin so CPU + GPU compute the same dims from the same
    // origin (AUTO-derived AABB min is float; setting explicit removes
    // any risk of dim off-by-one between runs).
    opts.origin_world = std::array<float, 3>{min_x, min_y, min_z};

    // ---- CPU run ----
    opts.prefer_gpu = objtomc::voxel::GpuPreference::OFF;
    const auto t0_cpu = clock::now();
    const objtomc::voxel::VoxelGrid cpu_grid =
        objtomc::voxel::voxelize_mesh(mesh, opts);
    const auto t1_cpu = clock::now();
    const auto cpu_ms = std::chrono::duration_cast<ms>(t1_cpu - t0_cpu).count();

    // ---- GPU run ----
    opts.prefer_gpu = objtomc::voxel::GpuPreference::ON;
    const auto t0_gpu = clock::now();
    const objtomc::voxel::VoxelGrid gpu_grid =
        objtomc::voxel::voxelize_mesh(mesh, opts);
    const auto t1_gpu = clock::now();
    const auto gpu_ms = std::chrono::duration_cast<ms>(t1_gpu - t0_gpu).count();

    const auto dims = cpu_grid.dimensions();
    std::cout << "[" << label << "] dims=("
              << dims[0] << "," << dims[1] << "," << dims[2]
              << "), occupied CPU=" << cpu_grid.occupied_count()
              << " GPU="            << gpu_grid.occupied_count()
              << ", CPU=" << cpu_ms << " ms, GPU=" << gpu_ms << " ms\n";

    const FirstDiff d = compare_grids(cpu_grid, gpu_grid);
    if (d.any) {
        std::cerr << "FAIL: [" << label << "] byte-equality: first diff at ("
                  << d.x << "," << d.y << "," << d.z
                  << "): CPU=" << d.cpu_val << " GPU=" << d.gpu_val << "\n"
                  << "Per proposal §3.12: do NOT modify test to pass. Invoke "
                     "triage protocol (a) determinism, (b) stage isolation, "
                     "(c) precision, (d) STOP + ask maintainer.\n";
        return 1;
    }
    std::cout << "PASS: [" << label << "] CPU and GPU output byte-identical\n";
    return 0;
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
        std::cerr << "usage: test_voxel_cpu_gpu_equality <sponza.obj>\n";
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

    // Run at 64^3 first (fast — sub-minute) so a regression surfaces
    // quickly; then 256^3 (production-scale, matches Session 37 Sponza
    // Tier-4 CPU baseline).
    if (run_one_resolution(mesh, 64,  "64^3")  != 0) return 1;
    if (run_one_resolution(mesh, 256, "256^3") != 0) return 1;
    return 0;
#endif
}
