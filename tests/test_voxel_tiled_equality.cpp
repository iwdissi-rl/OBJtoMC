// voxel_tiled_equality — v0.4 §5.1 voxel-layer slice of the tiled
// byte-equality gate (research/scale_v04_proposal.md §3.3; Session 56).
//
// For each input, voxelises monolithically (CPU path) and through the
// tile driver at deliberately awkward tile sizes (small, non-divisor of
// the grid dims, and 1-cell-wide extremes), then asserts EVERY cell —
// material id AND winning global triangle index — is bit-identical
// between the tiled windows and the corresponding monolithic sub-window.
// Any mismatch is a real bug per the §3.12-style first-run triage
// discipline, NOT a signal to loosen the comparison.
//
// Inputs: the committed cube.obj (multi-material, explicit mid-cell
// frame) and bunny.obj (69 451 faces, auto frame — real disambiguation
// contention) fixtures.
//
// CPU-only semantics: under -DWITH_CUDA=ON the monolithic reference is
// forced to the CPU path (prefer_gpu = OFF) — the tile driver is CPU
// this session; GPU per-tile dispatch is proposal §6.4.

#include "objtomc/mesh.hpp"
#include "objtomc/tiling.hpp"
#include "objtomc/voxel.hpp"

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace msh = objtomc::mesh;
namespace vox = objtomc::voxel;
namespace til = objtomc::tiling;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

vox::VoxelizerOptions cpu_options(float voxel_size) {
    vox::VoxelizerOptions o = vox::default_options();
    o.voxel_size = voxel_size;
#ifdef OBJTOMC_WITH_CUDA
    o.prefer_gpu = vox::GpuPreference::OFF;
#endif
    return o;
}

// Voxelize monolithically and via tiles at (tile_x, tile_z); compare every
// cell of every tile window against the monolithic grid.
void check_tiled_equality(const msh::Mesh& mesh,
                          const vox::VoxelizerOptions& opts,
                          int tile_x, int tile_z,
                          const std::string& label) {
    const vox::VoxelGrid mono = vox::voxelize_mesh(mesh, opts);
    const auto dims = mono.dimensions();

    const til::TilePlan plan = til::plan_tiles(mesh, opts, tile_x, tile_z);
    if (plan.grid_dims != dims) {
        fail(label + ": plan grid_dims disagree with voxelize_mesh");
    }

    std::size_t tiled_occupied = 0;
    for (std::size_t ti = 0; ti < plan.tiles.size(); ++ti) {
        const til::TileResult res = til::voxelize_tile(
            mesh, opts, plan.tiles[ti],
            std::span<const std::int32_t>(plan.tile_faces[ti]));
        const int x0 = res.tile.offset_xz[0];
        const int z0 = res.tile.offset_xz[1];
        const int wx = res.dims[0];
        const int wz = res.dims[2];
        for (int y = 0; y < res.dims[1]; ++y) {
            for (int lz = 0; lz < wz; ++lz) {
                for (int lx = 0; lx < wx; ++lx) {
                    const std::size_t li = static_cast<std::size_t>(
                        lx + wx * (lz + wz * y));
                    const std::int32_t cell = res.cells[li];
                    const std::int32_t tri  = res.tris[li];
                    const std::int32_t mono_cell =
                        mono.at_unchecked(x0 + lx, y, z0 + lz);
                    const std::int32_t mono_tri =
                        mono.triangle_at_unchecked(x0 + lx, y, z0 + lz);
                    if (cell != mono_cell || tri != mono_tri) {
                        fail(label + " tile " + std::to_string(ti) +
                             ": mismatch at global (" +
                             std::to_string(x0 + lx) + ", " +
                             std::to_string(y) + ", " +
                             std::to_string(z0 + lz) + "): tiled (mat " +
                             std::to_string(cell) + ", tri " +
                             std::to_string(tri) + ") vs monolithic (mat " +
                             std::to_string(mono_cell) + ", tri " +
                             std::to_string(mono_tri) + ")");
                    }
                    if (cell != vox::EMPTY) ++tiled_occupied;
                }
            }
        }
    }
    if (tiled_occupied != mono.occupied_count()) {
        fail(label + ": tiled occupied " + std::to_string(tiled_occupied) +
             " != monolithic " + std::to_string(mono.occupied_count()));
    }
    std::cout << "PASS: " << label << " (" << plan.tiles.size()
              << " tiles, dims " << dims[0] << "x" << dims[1] << "x"
              << dims[2] << ", occupied " << tiled_occupied
              << ", byte-identical)\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <path-to-cube.obj> <path-to-bunny.obj>\n";
        return 2;
    }

    // Cube: multi-material fixture, mid-cell explicit-frame-free voxelise
    // at a size giving a small grid; tile sizes chosen to be non-divisors
    // and the 1-cell extreme.
    const msh::Mesh cube = msh::load_obj(argv[1]);
    check_tiled_equality(cube, cpu_options(0.25f), 3, 2, "cube t3x2");
    check_tiled_equality(cube, cpu_options(0.25f), 1, 1, "cube t1x1");

    // Bunny: 69 451 faces at ~64 cells across the longest extent —
    // real disambiguation contention across tile boundaries.
    const msh::Mesh bunny = msh::load_obj(argv[2]);
    check_tiled_equality(bunny, cpu_options(0.024437f), 24, 16,
                         "bunny t24x16");
    check_tiled_equality(bunny, cpu_options(0.024437f), 17, 23,
                         "bunny t17x23");

    std::cout << "PASS: voxel_tiled_equality\n";
    return 0;
}
