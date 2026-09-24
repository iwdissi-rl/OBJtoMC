#pragma once

#include "objtomc/mesh.hpp"

// ---------------------------------------------------------------------------
// Street-grid auto-alignment — v0.5 sub-item (b)
// (research/semantic_reconstruction_proposal.md §3, Session 69).
//
// Scan meshes are rarely axis-aligned, so large flat facades voxelise to
// diagonal stair-stepping. detect_grid_angle() finds the dominant
// horizontal facade direction (area-weighted histogram of horizontal
// face-normal angles folded modulo 90°, refined by a weighted mean around
// the peak bin) and returns the signed rotation in degrees, in
// (-45, 45], that rotate_mesh_y() should apply to make that direction
// axis-aligned. rotate_mesh_y() rotates positions about the Y axis
// through the mesh AABB centre (UVs, faces, materials untouched — the
// pipeline consumes positions only).
//
// Deterministic pure geometry; no effect unless invoked (--align).
// ---------------------------------------------------------------------------
namespace objtomc::alignment {

// Returns the correction angle in degrees, in (-45, 45]. Meshes with no
// meaningful horizontal facade signal (everything flat/vertical-normal-
// free) return 0.
float detect_grid_angle(const mesh::Mesh& m);

// Rotate positions by `degrees` about the +Y axis through the AABB centre
// (right-handed; positive = counter-clockwise looking down -Y).
void rotate_mesh_y(mesh::Mesh& m, float degrees);

}  // namespace objtomc::alignment
