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
void fill_cells_cpu(const ::objtomc::mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells);

#ifdef OBJTOMC_WITH_CUDA
// GPU cell-filling: same semantics as fill_cells_cpu — byte-identical
// output on the same input. Defined in src/voxel_gpu.cu.
//
// Throws voxel_error on any CUDA runtime failure (kernel launch, malloc,
// memcpy). No CUDA state leaks on the throw path (RAII device-buffer
// wrappers per proposal §3.3 "Device-buffer lifetime discipline").
void fill_cells_gpu(const ::objtomc::mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells);
#endif  // OBJTOMC_WITH_CUDA

}  // namespace objtomc::voxel::detail
