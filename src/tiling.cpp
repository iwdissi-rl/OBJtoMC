// ---------------------------------------------------------------------------
// Tiled voxelisation driver — Sessions 56–57 (v0.4 item #1, proposal
// §3.1–§3.3). See include/objtomc/tiling.hpp for the byte-equality contract.
// ---------------------------------------------------------------------------
#include "objtomc/tiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "voxel_internal.hpp"

namespace objtomc::tiling {

namespace {

// The kernel's own triangle-AABB → cell-index-range arithmetic, verbatim
// (src/voxel.cpp fill_cells_cpu_window idx_clamp). Binning MUST use the
// identical expression so a tile's face list contains exactly the faces
// whose kernel loop range intersects the tile — exact binning, no apron.
inline int idx_clamp(float v, float origin, float vs, int axis_dim) noexcept {
    return std::clamp(static_cast<int>(std::floor((v - origin) / vs)),
                      0, axis_dim - 1);
}

// Floor division (round toward negative infinity) for region-aligned
// boundaries at negative world coordinates.
inline int floor_div(int a, int b) noexcept {
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}

voxel::detail::GridSetup resolve_frame(const mesh::Mesh& mesh,
                                       const voxel::VoxelizerOptions& opts) {
    if (opts.max_grid_cells <= 0) {
        throw voxel::voxel_error(
            "voxelize_mesh: max_grid_cells must be positive, got " +
            std::to_string(opts.max_grid_cells));
    }
    // Resolve the FULL grid frame with the size cap lifted — the whole
    // point of tiling is that the monolithic grid may exceed
    // max_grid_cells; the cap is enforced PER TILE (proposal §3.5).
    // All other validation (mesh shape, finiteness, voxel_size,
    // explicit-dimension coverage) is voxelize_mesh's, unchanged.
    voxel::VoxelizerOptions frame_opts = opts;
    frame_opts.max_grid_cells = std::numeric_limits<std::int64_t>::max();
    return voxel::detail::compute_grid_setup(mesh, frame_opts);
}

// Shared plan body: per-tile cap check + exact face binning. tix/tiz map
// a GLOBAL cell index to its tile column/row; both decompositions in this
// file give contiguous tile ranges for contiguous cell ranges.
template <typename TixFn, typename TizFn>
TilePlan build_plan(const mesh::Mesh& mesh,
                    const voxel::detail::GridSetup& setup,
                    std::int64_t max_grid_cells,
                    std::vector<Tile> tiles,
                    int ntx,
                    TixFn tix, TizFn tiz) {
    TilePlan plan;
    plan.grid_dims   = setup.dims;
    plan.grid_origin = setup.origin;
    plan.voxel_size  = setup.voxel_size;
    plan.tiles       = std::move(tiles);

    for (const Tile& t : plan.tiles) {
        const std::int64_t tile_cells =
            static_cast<std::int64_t>(t.dims_xz[0]) *
            static_cast<std::int64_t>(setup.dims[1]) *
            static_cast<std::int64_t>(t.dims_xz[1]);
        if (tile_cells > max_grid_cells) {
            throw voxel::voxel_error(
                "plan_tiles: tile size " + std::to_string(tile_cells) +
                " cells exceeds max_grid_cells " +
                std::to_string(max_grid_cells) +
                " (shrink the tile decomposition)");
        }
    }
    plan.tile_faces.resize(plan.tiles.size());

    // Exact binning: replicate the kernel's cell-range math per face and
    // append the face to every tile its X/Z range intersects. Iterating
    // faces in ascending order keeps every per-tile list ascending, which
    // the strict-improvement disambiguation update relies on (§3.3).
    for (std::size_t face_idx = 0; face_idx < mesh.faces.size(); ++face_idx) {
        const auto& face = mesh.faces[face_idx];
        const auto v0 = voxel::detail::swap_axes(
            mesh.positions[static_cast<std::size_t>(face.position_indices[0])],
            setup.axis);
        const auto v1 = voxel::detail::swap_axes(
            mesh.positions[static_cast<std::size_t>(face.position_indices[1])],
            setup.axis);
        const auto v2 = voxel::detail::swap_axes(
            mesh.positions[static_cast<std::size_t>(face.position_indices[2])],
            setup.axis);

        const float xmin = std::min({v0[0], v1[0], v2[0]});
        const float xmax = std::max({v0[0], v1[0], v2[0]});
        const float zmin = std::min({v0[2], v1[2], v2[2]});
        const float zmax = std::max({v0[2], v1[2], v2[2]});

        const int x_lo = idx_clamp(xmin, setup.origin[0], setup.voxel_size,
                                   setup.dims[0]);
        const int x_hi = idx_clamp(xmax, setup.origin[0], setup.voxel_size,
                                   setup.dims[0]);
        const int z_lo = idx_clamp(zmin, setup.origin[2], setup.voxel_size,
                                   setup.dims[2]);
        const int z_hi = idx_clamp(zmax, setup.origin[2], setup.voxel_size,
                                   setup.dims[2]);

        for (int tz = tiz(z_lo); tz <= tiz(z_hi); ++tz) {
            for (int tx = tix(x_lo); tx <= tix(x_hi); ++tx) {
                plan.tile_faces[static_cast<std::size_t>(tz) *
                                    static_cast<std::size_t>(ntx) +
                                static_cast<std::size_t>(tx)]
                    .push_back(static_cast<std::int32_t>(face_idx));
            }
        }
    }

    return plan;
}

}  // namespace

TilePlan plan_tiles(const mesh::Mesh& mesh,
                    const voxel::VoxelizerOptions& opts,
                    const int tile_cells_x,
                    const int tile_cells_z) {
    if (tile_cells_x < 1 || tile_cells_z < 1) {
        throw voxel::voxel_error(
            "plan_tiles: tile_cells must be >= 1, got " +
            std::to_string(tile_cells_x) + " x " + std::to_string(tile_cells_z));
    }
    const voxel::detail::GridSetup setup = resolve_frame(mesh, opts);

    const int ntx = (setup.dims[0] + tile_cells_x - 1) / tile_cells_x;
    const int ntz = (setup.dims[2] + tile_cells_z - 1) / tile_cells_z;
    std::vector<Tile> tiles;
    tiles.reserve(static_cast<std::size_t>(ntx) *
                  static_cast<std::size_t>(ntz));
    for (int tz = 0; tz < ntz; ++tz) {
        for (int tx = 0; tx < ntx; ++tx) {
            Tile t;
            t.offset_xz = {tx * tile_cells_x, tz * tile_cells_z};
            t.dims_xz   = {std::min(tile_cells_x,
                                    setup.dims[0] - t.offset_xz[0]),
                           std::min(tile_cells_z,
                                    setup.dims[2] - t.offset_xz[1])};
            tiles.push_back(t);
        }
    }
    return build_plan(mesh, setup, opts.max_grid_cells, std::move(tiles),
                      ntx,
                      [&](int cell) { return cell / tile_cells_x; },
                      [&](int cell) { return cell / tile_cells_z; });
}

TilePlan plan_tiles_aligned(const mesh::Mesh& mesh,
                            const voxel::VoxelizerOptions& opts,
                            const std::array<int, 2>& world_origin_xz,
                            const int block_span) {
    if (block_span < 1) {
        throw voxel::voxel_error(
            "plan_tiles_aligned: block_span must be >= 1, got " +
            std::to_string(block_span));
    }
    const voxel::detail::GridSetup setup = resolve_frame(mesh, opts);

    // Cell c maps to world block world_origin + c; tile boundaries fall
    // where the world coordinate crosses a multiple of block_span, so a
    // tile's output lands entirely in one span (one Anvil region for
    // block_span = 512; 512 is a multiple of 16, so tile boundaries are
    // also chunk boundaries — no chunk ever spans two tiles). First/last
    // tiles clip to the grid.
    const int wx0 = world_origin_xz[0];
    const int wz0 = world_origin_xz[1];
    const int rx_min = floor_div(wx0, block_span);
    const int rx_max = floor_div(wx0 + setup.dims[0] - 1, block_span);
    const int rz_min = floor_div(wz0, block_span);
    const int rz_max = floor_div(wz0 + setup.dims[2] - 1, block_span);
    const int ntx = rx_max - rx_min + 1;
    const int ntz = rz_max - rz_min + 1;

    std::vector<Tile> tiles;
    tiles.reserve(static_cast<std::size_t>(ntx) *
                  static_cast<std::size_t>(ntz));
    for (int rz = rz_min; rz <= rz_max; ++rz) {
        for (int rx = rx_min; rx <= rx_max; ++rx) {
            Tile t;
            const int cx_lo = std::max(0, rx * block_span - wx0);
            const int cx_hi = std::min(setup.dims[0],
                                       (rx + 1) * block_span - wx0);
            const int cz_lo = std::max(0, rz * block_span - wz0);
            const int cz_hi = std::min(setup.dims[2],
                                       (rz + 1) * block_span - wz0);
            t.offset_xz = {cx_lo, cz_lo};
            t.dims_xz   = {cx_hi - cx_lo, cz_hi - cz_lo};
            tiles.push_back(t);
        }
    }
    return build_plan(
        mesh, setup, opts.max_grid_cells, std::move(tiles), ntx,
        [&](int cell) { return floor_div(wx0 + cell, block_span) - rx_min; },
        [&](int cell) { return floor_div(wz0 + cell, block_span) - rz_min; });
}

TileResult voxelize_tile(const mesh::Mesh& mesh,
                         const voxel::VoxelizerOptions& opts,
                         const Tile& tile,
                         std::span<const std::int32_t> faces) {
    const voxel::detail::GridSetup setup = resolve_frame(mesh, opts);

    if (tile.offset_xz[0] < 0 || tile.offset_xz[1] < 0 ||
        tile.dims_xz[0] < 1 || tile.dims_xz[1] < 1 ||
        tile.offset_xz[0] + tile.dims_xz[0] > setup.dims[0] ||
        tile.offset_xz[1] + tile.dims_xz[1] > setup.dims[2]) {
        throw voxel::voxel_error(
            "voxelize_tile: tile window [" +
            std::to_string(tile.offset_xz[0]) + ", " +
            std::to_string(tile.offset_xz[1]) + ") + (" +
            std::to_string(tile.dims_xz[0]) + " x " +
            std::to_string(tile.dims_xz[1]) +
            ") is outside the grid frame");
    }

    TileResult res;
    res.tile = tile;
    res.dims = {tile.dims_xz[0], setup.dims[1], tile.dims_xz[1]};
    const std::size_t n =
        static_cast<std::size_t>(res.dims[0]) *
        static_cast<std::size_t>(res.dims[1]) *
        static_cast<std::size_t>(res.dims[2]);
    res.cells.assign(n, voxel::EMPTY);
    res.tris.assign(n, voxel::EMPTY);

    // An empty face list means the tile is genuinely empty — do NOT pass
    // the empty span to the kernel, where it is the "all faces" sentinel.
    if (!faces.empty()) {
        // CPU/GPU dispatch (Session 58) — same rules as voxelize_mesh:
        // ON always GPU; AUTO GPU when the TILE's cell count reaches the
        // shared threshold; OFF (and CPU-only builds) CPU. Both paths are
        // byte-identical (voxel_tiled_equality GPU section + the packed-
        // key/local-index argument in voxel_internal.hpp).
        bool use_gpu = false;
#ifdef OBJTOMC_WITH_CUDA
        const std::int64_t tile_cells =
            static_cast<std::int64_t>(res.dims[0]) *
            static_cast<std::int64_t>(res.dims[1]) *
            static_cast<std::int64_t>(res.dims[2]);
        if (opts.prefer_gpu == GpuPreference::ON) {
            use_gpu = true;
        } else if (opts.prefer_gpu == GpuPreference::AUTO &&
                   tile_cells >= voxel::detail::GPU_GRID_THRESHOLD_CELLS) {
            use_gpu = true;
        }
        if (use_gpu) {
            voxel::detail::fill_cells_gpu_window(
                mesh, setup, tile.offset_xz[0], tile.offset_xz[1],
                tile.dims_xz[0], tile.dims_xz[1], faces, res.cells, res.tris);
        }
#endif
        if (!use_gpu) {
            voxel::detail::fill_cells_cpu_window(
                mesh, setup, tile.offset_xz[0], tile.offset_xz[1],
                tile.dims_xz[0], tile.dims_xz[1], faces, res.cells, res.tris);
        }
    }
    return res;
}

}  // namespace objtomc::tiling
