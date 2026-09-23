// ---------------------------------------------------------------------------
// Project-internal header — NOT part of the public include tree.
//
// Shared between src/voxel.cpp (CPU dispatcher + CPU kernel) and, when
// -DWITH_CUDA=ON, src/voxel_gpu.cu (GPU kernel).
//
// The purpose of the split is byte-equality: the CPU and GPU paths must
// produce identical VoxelGrid::cells_ output for the same input, and the
// only way to guarantee that is to compute grid dims / origin / voxel_size /
// axis once (in detail::compute_grid_setup) and then have both paths
// consume that shared GridSetup. Without this, the two paths could disagree
// on whether an edge-touching triangle rounds into voxel k or k+1 due to
// independent AABB / floor-clamp math paths.
//
// See research/gpu_port_proposal.md §3.3 "Critical: this is where CPU-vs-GPU
// byte-equality risk lives" — this header centralises the setup arithmetic
// so byte-equality is provable by construction on the dims/origin frame.
// The overlap test (Akenine-Möller SAT) and closest-point (Eberly 1999) are
// still separately ported per §3.3, but they operate on the SAME grid frame
// on both paths.
// ---------------------------------------------------------------------------
#pragma once

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace objtomc::voxel::detail {

// Grid frame computed once from validated mesh + options. Used by both CPU
// and GPU cell-filling paths; both paths must consume the same instance
// unchanged for byte-equality.
struct GridSetup {
    std::array<int,   3>  dims;
    std::array<float, 3>  origin;
    float                 voxel_size;
    AxisConvention        axis;
};

// Validate options + mesh, compute post-axis-swap AABB, resolve
// dims + origin, size-check against opts.max_grid_cells. Throws
// voxel_error on any misuse. On success returns the GridSetup that both
// downstream fill paths (CPU + GPU) share.
GridSetup compute_grid_setup(const ::objtomc::mesh::Mesh& mesh_in,
                             const VoxelizerOptions& opts);

// CPU cell-filling: iterate triangles, run Akenine-Möller SAT overlap test,
// Eberly 1999 closest-point-on-triangle disambiguation, tie-break by lower
// triangle index. Writes into cells (which must be pre-sized to
// dims[0]*dims[1]*dims[2] and pre-filled with EMPTY = -1).
//
// tris receives the WINNING triangle index (index into mesh_in.faces) for
// each occupied cell, EMPTY for unoccupied cells. Must be pre-sized and
// pre-filled like cells. The winner is exactly the triangle whose
// material_id lands in cells — i.e. the (distance, triangle_index)
// lexicographic minimum under the disambiguation rule. Added Session 48
// for per-voxel texture sampling (v0.3 item #1): the UV of a voxel is
// recovered post-hoc from its winning triangle.
void fill_cells_cpu(const ::objtomc::mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells,
                    std::vector<std::int32_t>& tris);

// Windowed CPU cell-filling (Session 56, v0.4 tiling — proposal §3.2).
// Fills only the XZ window [x0, x0+wx) × full Y × [z0, z0+wz) of the
// setup's GLOBAL grid frame, writing into window-local dense buffers
// (pre-sized to wx * dims[1] * wz, pre-filled EMPTY; local index =
// lx + wx * (lz + wz * y)).
//
// BYTE-EQUALITY-CRITICAL: every float expression (triangle AABB → index
// ranges, box centres) uses the GLOBAL origin and GLOBAL cell indices —
// identical arithmetic to the full-grid path, so a window cell's result
// is bit-identical to the same cell in a monolithic fill. The full-grid
// fill_cells_cpu above is this function with the full window.
//
// faces: ascending GLOBAL face indices to iterate (a tile's exact bin per
// tiling::plan_tiles); an EMPTY span means "all faces". tris receives
// GLOBAL face indices either way.
void fill_cells_cpu_window(const ::objtomc::mesh::Mesh& mesh_in,
                           const GridSetup& setup,
                           int window_x0, int window_z0,
                           int window_wx, int window_wz,
                           std::span<const std::int32_t> faces,
                           std::vector<std::int32_t>& cells,
                           std::vector<std::int32_t>& tris);

#ifdef OBJTOMC_WITH_CUDA
// GPU cell-filling: same semantics as fill_cells_cpu — byte-identical
// cells AND tris output on the same input. (The GPU path's packed
// atomicMin key already carries the winning triangle index in its low
// 32 bits, so tris is extracted from the same key that decides cells —
// equality is by construction.) Defined in src/voxel_gpu.cu.
//
// Throws voxel_error on any CUDA runtime failure (kernel launch, malloc,
// memcpy). No CUDA state leaks on the throw path (RAII device-buffer
// wrappers per proposal §3.3 "Device-buffer lifetime discipline").
void fill_cells_gpu(const ::objtomc::mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells,
                    std::vector<std::int32_t>& tris);
#endif  // OBJTOMC_WITH_CUDA

// ---------------------------------------------------------------------------
// Helpers exported for the orchestrate layer's per-voxel UV recovery
// (Session 48). Both are the SAME arithmetic the CPU fill path uses — a
// caller that recomputes the closest point for a voxel's winning triangle
// gets bit-identical coordinates to what the disambiguation saw.
// ---------------------------------------------------------------------------

// Axis swap: Y_UP is identity; Z_UP swaps Y and Z. Identical math to the
// fill paths' per-vertex swap.
std::array<float, 3> swap_axes(const std::array<float, 3>& p,
                               AxisConvention axis) noexcept;

// Eberly 1999 closest point on triangle, additionally reporting the
// barycentric parameters (s, t) of the closest point under
// T(s, t) = v0 + s*(v1 - v0) + t*(v2 - v0), with s >= 0, t >= 0,
// s + t <= 1. Use (1 - s - t, s, t) as the weights on (v0, v1, v2)
// attributes — e.g. UV interpolation.
std::array<float, 3> closest_point_on_triangle_bary(
    const std::array<float, 3>& p,
    const std::array<float, 3>& v0,
    const std::array<float, 3>& v1,
    const std::array<float, 3>& v2,
    float& s_out,
    float& t_out) noexcept;

}  // namespace objtomc::voxel::detail
