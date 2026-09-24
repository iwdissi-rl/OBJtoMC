#pragma once

#include <array>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Deterministic voxel cleanup — v0.5 sub-item (a)
// (research/semantic_reconstruction_proposal.md §2, Session 67).
//
// Operates on the (cells, tris, dims) triple produced by the voxel layer
// (storage order x + dx*(z + dz*y); EMPTY = -1), BEFORE matching:
//
//   1. Pinhole fill — an EMPTY cell with >= fill_min_neighbors occupied
//      6-neighbours becomes occupied, copying (material, tri) from its
//      most-common occupied neighbour (tie-break: lexicographically
//      smallest (material, tri) pair — deterministic). Decisions are made
//      against the PRE-pass state (double-buffered), so fills cannot
//      cascade within the pass.
//   2. Crumb removal — 26-connected components of <= crumb_max_voxels
//      occupied voxels are deleted (floating scan debris).
//
// Fill runs before crumb removal (a fill can legitimately rescue a
// borderline component by bridging it to the main body — order is part of
// the deterministic contract). Both ops have bounded influence radius,
// which is what makes the Session 68 tiled-apron equality argument work
// (proposal §2).
//
// Fully deterministic; the default-OFF pipeline is byte-identical to
// pre-v0.5 output.
// ---------------------------------------------------------------------------
namespace objtomc::cleanup {

struct CleanupOptions {
    bool enabled            = false;
    int  fill_min_neighbors = 5;    // in [1, 6]; a cell has 6 face-neighbours
    int  crumb_max_voxels   = 16;   // >= 0; 0 disables crumb removal
};

struct CleanupStats {
    std::size_t filled  = 0;   // cells added by pinhole fill
    std::size_t removed = 0;   // cells deleted by crumb removal
};

// Mutates cells/tris in place. dims = {dx, dy, dz}. Throws std::invalid_
// argument on option misuse (fill_min_neighbors outside [1,6], negative
// crumb_max_voxels, size mismatch).
CleanupStats apply_cleanup(std::vector<std::int32_t>& cells,
                           std::vector<std::int32_t>& tris,
                           const std::array<int, 3>& dims,
                           const CleanupOptions& opts);

}  // namespace objtomc::cleanup
