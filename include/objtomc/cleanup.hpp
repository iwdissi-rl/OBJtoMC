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

// Which window edges are CLIPS of a larger grid (Session 68 tiled-apron
// scheme) rather than true grid boundaries. On a clipped side the window
// cannot see past its edge, so two conservative rules keep the result
// byte-identical to cleaning the whole grid (proposal §2 argument):
//   - fill: cells in the outermost layer of a clipped side are skipped
//     (their 6-neighbourhood extends past the window); every other fill
//     decision sees its full neighbourhood and matches the whole-grid run.
//   - crumbs: a component containing any cell in a clipped side's
//     outermost layer is kept regardless of size. With apron A =
//     crumb_max_voxels + 1, a <= N-voxel component intersecting the
//     cropped tile can never reach the clipped edge (26-connected extent
//     of N voxels is <= N - 1 < A), so this rule only fires for
//     components the whole-grid run would keep anyway.
// Default: nothing clipped (whole-grid behaviour, byte-identical pre-68).
struct ClippedEdges {
    bool x_lo = false;
    bool x_hi = false;
    bool z_lo = false;
    bool z_hi = false;
};

// Mutates cells/tris in place. dims = {dx, dy, dz}. Throws std::invalid_
// argument on option misuse (fill_min_neighbors outside [1,6], negative
// crumb_max_voxels, size mismatch).
CleanupStats apply_cleanup(std::vector<std::int32_t>& cells,
                           std::vector<std::int32_t>& tris,
                           const std::array<int, 3>& dims,
                           const CleanupOptions& opts,
                           const ClippedEdges& clipped = ClippedEdges{});

}  // namespace objtomc::cleanup
