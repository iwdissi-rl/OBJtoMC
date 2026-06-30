#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "objtomc/mesh.hpp"   // mesh::Mesh, mesh::Material

namespace objtomc::voxel {

// ---------------------------------------------------------------------------
// Voxel grid sentinels and v0.1 size cap.
// ---------------------------------------------------------------------------
static constexpr std::int32_t  EMPTY                   = -1;
static constexpr std::int64_t  MAX_GRID_CELLS_DEFAULT  = 1024LL * 1024LL * 1024LL;

// ---------------------------------------------------------------------------
// AxisConvention — the source OBJ-space axis layout. The voxel grid output is
// ALWAYS Y-up (to match Minecraft chunk Y conventions); this enum selects how
// the input is interpreted.
// ---------------------------------------------------------------------------
enum class AxisConvention {
    Y_UP,   // OBJ Y == voxel Y (default; OBJ convention)
    Z_UP,   // OBJ Z == voxel Y; OBJ Y == voxel Z (Y/Z swap applied at voxelise start)
};

// ---------------------------------------------------------------------------
// DisambiguationRule — how a voxel that is overlapped by multiple triangles
// is assigned a single material_id. v0.1 has a single value; the type is
// exposed for forward-compatibility so v0.2 can add FIRST_TOUCHED without
// breaking ABI. See proposal §3.5 and §7 for the v0.1 determinism rationale.
// ---------------------------------------------------------------------------
enum class DisambiguationRule {
    CLOSEST_TO_CENTRE,   // closest interior-clamped-distance from voxel centre to triangle (the v0.1 default and only value)
    // FIRST_TOUCHED      // deferred to v0.2 — ABI-compatible addition
};

// ---------------------------------------------------------------------------
// VoxelizerOptions — v0.1 hardcoded defaults (default_options()), tunable by
// the caller. See proposal §3.4 for the trade-offs.
// ---------------------------------------------------------------------------
struct VoxelizerOptions {
    // OBJ-space units per voxel. Mandatory; must be > 0 and finite.
    float                                    voxel_size;
    // Input mesh's axis convention. Default Y_UP.
    AxisConvention                           axis_convention;
    // Explicit grid dimensions. If empty, computed from mesh AABB / voxel_size.
    std::optional<std::array<int,   3>>      dimensions;
    // Explicit world-space origin (the position of voxel (0,0,0) in OBJ-space,
    // post-axis-swap). If empty, computed as the mesh AABB minimum corner.
    std::optional<std::array<float, 3>>      origin_world;
    // Per-voxel material disambiguation. Default CLOSEST_TO_CENTRE.
    DisambiguationRule                       disambiguation;
    // Maximum grid size in cells (dim_x * dim_y * dim_z). Default 1024^3.
    std::int64_t                             max_grid_cells;
};

VoxelizerOptions default_options() noexcept;

// ---------------------------------------------------------------------------
// Errors. Programmer-misuse only. Valid input never throws.
//   - empty mesh / no faces
//   - non-finite mesh coordinates
//   - voxel_size <= 0 or non-finite
//   - explicit dimensions with any axis <= 0
//   - explicit dimensions insufficient to cover mesh AABB
//   - computed grid size > max_grid_cells
//   - max_grid_cells <= 0
// ---------------------------------------------------------------------------
class voxel_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// VoxelGrid — the dense voxelisation output. Each cell holds a material_id
// indexing into materials(); EMPTY (= -1) indicates an unoccupied cell.
//
// Storage order: x-major-then-z-major-then-y. Linear index =
//   x + dim_x * (z + dim_z * y).
// This order matches chunk::ChunkBuilder::build()'s outer iteration so the
// chunk-routing layer can stream through occupied voxels in chunk-emit order.
//
// Lifetime: standalone; not aliased to the source mesh's data. The
// materials() span is a copy of the source mesh's materials at voxelisation
// time. (Subsequent mutation of the source mesh does NOT affect the grid.)
//
// Thread-safety: const member functions are safe to call concurrently from
// multiple threads on a single VoxelGrid instance.
// ---------------------------------------------------------------------------
class VoxelGrid {
public:
    // Grid metadata — set at construction time, immutable afterward.
    std::array<int,   3>   dimensions()      const noexcept;
    std::array<float, 3>   origin_world()    const noexcept;
    float                  voxel_size()      const noexcept;
    AxisConvention         axis_convention() const noexcept;

    // Cell access. at() is bounds-checked; throws voxel_error on OOB.
    // at_unchecked() is bounds-unchecked; UB on OOB. Use at() in tests;
    // at_unchecked() in performance-critical loops.
    std::int32_t           at(int x, int y, int z) const;
    std::int32_t           at_unchecked(int x, int y, int z) const noexcept;

    // Occupancy queries.
    bool                   is_occupied(int x, int y, int z) const;
    std::size_t            occupied_count() const;   // linear scan; not cached

    // Iterate occupied voxels in storage order (x fastest, then z, then y).
    // Visitor is called as: visitor(int x, int y, int z, std::int32_t material_id).
    // visitor is NOT called for EMPTY cells.
    template <typename Visitor>
    void for_each_voxel(Visitor&& visitor) const;

    // Span view of the materials echoed from the source mesh. material_id
    // values returned by at() / for_each_voxel index into this span.
    std::span<const mesh::Material> materials() const noexcept;

private:
    // Constructor is private; only voxelize_mesh(...) constructs a VoxelGrid.
    // (Friend declaration so src/voxel.cpp can call it; not part of the
    // public surface.)
    friend VoxelGrid voxelize_mesh(const mesh::Mesh&, const VoxelizerOptions&);

    VoxelGrid();

    std::array<int,   3>           dimensions_;
    std::array<float, 3>           origin_world_;
    float                          voxel_size_;
    AxisConvention                 axis_convention_;
    std::vector<std::int32_t>      cells_;       // size = dim_x*dim_y*dim_z; default-filled to EMPTY
    std::vector<mesh::Material>    materials_;   // copy of source mesh.materials at voxelise time
};

// ---------------------------------------------------------------------------
// voxelize_mesh — the primary entry point. Reads the mesh, applies the axis
// swap (per opts.axis_convention), computes the voxel grid dimensions and
// origin (per opts.dimensions / opts.origin_world or auto-derived from mesh
// AABB), iterates each triangle, marks overlapping voxels (Akenine-Möller
// SAT), and assigns the triangle's material_id under the
// opts.disambiguation rule.
//
// Throws voxel_error on programmer misuse (see voxel_error doc above).
// Valid input never throws.
//
// Implementation reference: Schwarz & Seidel 2010 §3 "Fast Parallel Surface
// and Solid Voxelization on GPUs" (https://www.michael-schwarz.com/research/
// publ/files/vox-siga10.pdf); Akenine-Möller 2001 "Fast 3D Triangle-Box
// Overlap Testing" (JGT 6(1)). cuda_voxelizer's CPU surface kernel (MIT,
// Jeroen Baert; src/cpu_voxelizer.cpp) serves as the empirical-reference
// baseline at implementation + test time.
// ---------------------------------------------------------------------------
VoxelGrid voxelize_mesh(const mesh::Mesh& mesh,
                        const VoxelizerOptions& opts);

}  // namespace objtomc::voxel

// ---------------------------------------------------------------------------
// Template definitions for callers (must live in the header since the visitor
// type is generic).
// ---------------------------------------------------------------------------
namespace objtomc::voxel {

template <typename Visitor>
void VoxelGrid::for_each_voxel(Visitor&& visitor) const {
    const int dim_x = dimensions_[0];
    const int dim_y = dimensions_[1];
    const int dim_z = dimensions_[2];
    std::size_t i = 0;
    for (int y = 0; y < dim_y; ++y) {
        for (int z = 0; z < dim_z; ++z) {
            for (int x = 0; x < dim_x; ++x, ++i) {
                const std::int32_t mid = cells_[i];
                if (mid != EMPTY) {
                    visitor(x, y, z, mid);
                }
            }
        }
    }
}

}  // namespace objtomc::voxel
