// ---------------------------------------------------------------------------
// Tiled voxelisation driver — Session 56 (v0.4 item #1, proposal §3.1–§3.3).
// See include/objtomc/tiling.hpp for the byte-equality contract.
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
    if (opts.max_grid_cells <= 0) {
        throw voxel::voxel_error(
            "voxelize_mesh: max_grid_cells must be positive, got " +
            std::to_string(opts.max_grid_cells));
    }

    // Resolve the FULL grid frame with the size cap lifted — the whole
    // point of tiling is that the monolithic grid may exceed
    // max_grid_cells; the cap is enforced PER TILE below (proposal §3.5).
    // All other validation (mesh shape, finiteness, voxel_size,
    // explicit-dimension coverage) is voxelize_mesh's, unchanged.
    voxel::VoxelizerOptions frame_opts = opts;
    frame_opts.max_grid_cells = std::numeric_limits<std::int64_t>::max();
    const voxel::detail::GridSetup setup =
        voxel::detail::compute_grid_setup(mesh, frame_opts);

    TilePlan plan;
    plan.grid_dims   = setup.dims;
    plan.grid_origin = setup.origin;
    plan.voxel_size  = setup.voxel_size;

    const int ntx = (setup.dims[0] + tile_cells_x - 1) / tile_cells_x;
    const int ntz = (setup.dims[2] + tile_cells_z - 1) / tile_cells_z;
    plan.tiles.reserve(static_cast<std::size_t>(ntx) *
                       static_cast<std::size_t>(ntz));
    for (int tz = 0; tz < ntz; ++tz) {
        for (int tx = 0; tx < ntx; ++tx) {
            Tile t;
            t.offset_xz = {tx * tile_cells_x, tz * tile_cells_z};
            t.dims_xz   = {std::min(tile_cells_x,
                                    setup.dims[0] - t.offset_xz[0]),
                           std::min(tile_cells_z,
                                    setup.dims[2] - t.offset_xz[1])};
            const std::int64_t tile_cells =
                static_cast<std::int64_t>(t.dims_xz[0]) *
                static_cast<std::int64_t>(setup.dims[1]) *
                static_cast<std::int64_t>(t.dims_xz[1]);
            if (tile_cells > opts.max_grid_cells) {
                throw voxel::voxel_error(
                    "plan_tiles: tile size " + std::to_string(tile_cells) +
                    " cells exceeds max_grid_cells " +
                    std::to_string(opts.max_grid_cells) +
                    " (shrink tile_cells_x/z)");
            }
            plan.tiles.push_back(t);
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

        for (int tz = z_lo / tile_cells_z; tz <= z_hi / tile_cells_z; ++tz) {
            for (int tx = x_lo / tile_cells_x; tx <= x_hi / tile_cells_x;
                 ++tx) {
                plan.tile_faces[static_cast<std::size_t>(tz) *
                                    static_cast<std::size_t>(ntx) +
                                static_cast<std::size_t>(tx)]
                    .push_back(static_cast<std::int32_t>(face_idx));
            }
        }
    }

    return plan;
}

TileResult voxelize_tile(const mesh::Mesh& mesh,
                         const voxel::VoxelizerOptions& opts,
                         const Tile& tile,
                         std::span<const std::int32_t> faces) {
    voxel::VoxelizerOptions frame_opts = opts;
    frame_opts.max_grid_cells = std::numeric_limits<std::int64_t>::max();
    const voxel::detail::GridSetup setup =
        voxel::detail::compute_grid_setup(mesh, frame_opts);

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
        voxel::detail::fill_cells_cpu_window(
            mesh, setup, tile.offset_xz[0], tile.offset_xz[1],
            tile.dims_xz[0], tile.dims_xz[1], faces, res.cells, res.tris);
    }
    return res;
}

}  // namespace objtomc::tiling
