#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "objtomc/chunk.hpp"   // chunk::BlockState

// ---------------------------------------------------------------------------
// Semantic detection + replacement — v0.5 sub-items (c)/(d)
// (research/semantic_reconstruction_proposal.md §4; Sessions 70–71,
// direct-implement per the maintainer's 2026-09-24 "don't stop until stage
// D" directive, decisions recorded in DECISIONS.md).
//
// Operates on the assembled sparse voxel view (world positions + per-
// voxel kd colours) between cleanup and matching. Produces:
//   - drop[i]  — voxel i is removed from the world (its scan geometry is
//                being replaced), and
//   - stamps   — explicit (world pos, BlockState) blocks that BYPASS the
//                matcher (they are already Minecraft-native), applied
//                after matching and overriding any surviving voxel at
//                the same position.
//
// (c) trees: canopies are connected components (26-conn) of GREEN voxels
//     — colour separates canopy from trunk/ground even when the scan
//     geometry is fully attached, which is why component analysis works
//     here at all. Each accepted canopy (+ a thin support column beneath
//     it) is replaced by a procedural Minecraft tree (oak_log trunk +
//     persistent oak_leaves canopy) scaled to the detected size.
//
// (d) buildings: see detect_and_replace_buildings below.
//
// Deterministic (fixed iteration orders, integer arithmetic in stamping),
// but NOT part of the byte-equality-gated default pipeline: everything
// here is opt-in via --semantic.
// ---------------------------------------------------------------------------
namespace objtomc::semantic {

struct SemanticOptions {
    bool  trees     = false;
    bool  buildings = false;

    // ---- tree detection knobs ----
    // green test on sRGB kd: g > r + margin && g > b + margin
    float green_margin      = 0.04f;
    int   min_canopy_voxels = 30;        // reject moss/grass speckle
    int   max_canopy_voxels = 4000000;   // reject "everything is forest"

    // ---- building detection knobs ----
    int   min_building_height = 12;      // blocks above ground estimate
    int   min_footprint_cells = 25;      // reject poles/antennas
    int   window_row_period   = 4;       // every k-th row is glass
};

struct SemanticResult {
    std::vector<char> drop;   // parallel to the input voxel arrays
    std::vector<std::pair<std::array<std::int32_t, 3>, chunk::BlockState>>
        stamps;
    std::size_t detected = 0;   // trees or buildings found
};

// (c) trees. positions/kd are parallel arrays of the surviving voxels
// (world block coords; kd = sRGB [0,1]).
SemanticResult detect_and_replace_trees(
    std::span<const std::array<std::int32_t, 3>> positions,
    std::span<const std::array<float, 3>>        kd,
    const SemanticOptions&                       opts);

// (d) buildings. Same inputs; wall_block_of is a callback mapping a
// dominant sRGB kd colour to the Minecraft block used for that
// building's walls (the caller wires it to the real matcher so walls
// stay perceptually matched). Ground level is estimated as the modal
// column-top height; connected XZ regions whose column tops rise
// min_building_height above it are regenerated as clean structures:
// boundary-column walls with a window row every window_row_period
// storeys, hollow interior, flat roof at the component's median top.
SemanticResult detect_and_replace_buildings(
    std::span<const std::array<std::int32_t, 3>> positions,
    std::span<const std::array<float, 3>>        kd,
    const SemanticOptions&                       opts,
    const std::function<chunk::BlockState(const std::array<float, 3>&)>&
        wall_block_of);

}  // namespace objtomc::semantic
