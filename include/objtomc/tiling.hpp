#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

// ---------------------------------------------------------------------------
// Tiled voxelisation driver (v0.4 item #1, research/scale_v04_proposal.md
// §3.1–§3.3). Decomposes the full voxel-grid frame into XZ tile windows
// (full Y — MC's 384-block height cap makes scale a horizontal problem,
// §1.5) and voxelises each window independently with the EXISTING kernels,
// so the whole-world dense grid never has to exist.
//
// Byte-equality contract (§3.3): for every cell, the tiled result equals
// the monolithic voxelize_mesh result bit-for-bit. Guaranteed by
// construction on two fronts, and gated by ctest voxel_tiled_equality:
//   1. Geometry — the window fill iterates GLOBAL cell indices and uses
//      the GLOBAL grid origin in every float expression (a per-tile float
//      origin would differ by ulps and flip SAT knife-edge cells).
//   2. Binning — a face is assigned to a tile iff the kernel's own
//      triangle-AABB cell-range arithmetic (same floor/clamp expressions)
//      intersects the tile window. Exact, not conservative: the kernel
//      visits no cell outside that range, so a tile's face list contains
//      every face that could touch any of its cells. Face lists preserve
//      ascending face order; the strict-improvement update rule then picks
//      the same (distance, face_idx) lexicographic winner as the full loop
//      restricted to that cell.
//
// v0.4 sequencing: this session (proposal §6.2) is the CPU driver;
// orchestrate consumption (region-at-a-time write) is §6.3; GPU per-tile
// dispatch is §6.4 — voxelize_tile currently always fills on CPU.
// ---------------------------------------------------------------------------
namespace objtomc::tiling {

// One XZ window of the full grid frame. offset/dims are in CELLS of the
// full grid (Y is never windowed). Edge tiles are clipped to the grid.
struct Tile {
    std::array<int, 2> offset_xz;
    std::array<int, 2> dims_xz;
};

// The tile decomposition of a mesh's voxel-grid frame plus the per-tile
// face lists. grid_dims/grid_origin/voxel_size echo the frame that
// monolithic voxelize_mesh would compute for the same (mesh, opts) — a
// consumer stitching tiles back into world coordinates must use
// grid_origin + global-cell-index arithmetic, never a per-tile origin.
struct TilePlan {
    std::array<int,   3>                    grid_dims;
    std::array<float, 3>                    grid_origin;
    float                                   voxel_size;
    std::vector<Tile>                       tiles;       // tz-major, then tx
    std::vector<std::vector<std::int32_t>>  tile_faces;  // parallel to tiles; ascending
};

// Result of voxelising one tile. cells/tris are dense over
// dims_xz[0] × grid_dims[1] × dims_xz[1] in the same x-then-z-then-y
// storage order as VoxelGrid (local index = lx + wx * (lz + wz * y)).
// tris holds GLOBAL face indices (index into the source mesh's faces),
// EMPTY (-1) for unoccupied — so per-voxel UV recovery works unchanged.
struct TileResult {
    Tile                        tile;
    std::array<int, 3>          dims;    // {wx, grid_dims[1], wz}
    std::vector<std::int32_t>   cells;   // material ids, EMPTY for unoccupied
    std::vector<std::int32_t>   tris;    // winning GLOBAL face idx, EMPTY
};

// Compute the tile decomposition + exact per-tile face bins.
// tile_cells_x/z are the window size in cells (>= 1; edge tiles clip).
// Validates (mesh, opts) exactly like voxelize_mesh — same voxel_error
// contract — except that opts.max_grid_cells is applied PER TILE
// (proposal §3.5), which is the whole point of tiling.
TilePlan plan_tiles(const mesh::Mesh& mesh,
                    const voxel::VoxelizerOptions& opts,
                    int tile_cells_x,
                    int tile_cells_z);

// Region-aligned decomposition (Session 57, proposal §3.1): tile
// boundaries fall where the WORLD block coordinate (world_origin_xz +
// cell) crosses a multiple of block_span. With block_span = 512 (one
// Anvil region) every tile's chunks land in exactly one region file, so
// a consumer can write-and-free one region per tile; 512 is a multiple
// of 16, so tile boundaries are also chunk boundaries. Edge tiles clip
// to the grid (the first tile is smaller when world_origin is not a
// multiple of block_span). Same validation contract as plan_tiles.
TilePlan plan_tiles_aligned(const mesh::Mesh& mesh,
                            const voxel::VoxelizerOptions& opts,
                            const std::array<int, 2>& world_origin_xz,
                            int block_span);

// Voxelise one tile of the plan. faces must be the plan's face list for
// this tile (ascending global indices). CPU fill path (GPU dispatch is
// proposal §6.4).
TileResult voxelize_tile(const mesh::Mesh& mesh,
                         const voxel::VoxelizerOptions& opts,
                         const Tile& tile,
                         std::span<const std::int32_t> faces);

}  // namespace objtomc::tiling
