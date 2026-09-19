#pragma once

#include <filesystem>

#include "objtomc/mesh.hpp"

namespace objtomc::gltf {

// ---------------------------------------------------------------------------
// glTF/GLB loading layer (Session 49, v0.3 item #2 — Sketchfab import).
//
// Loads a glTF 2.0 asset (.gltf JSON with external/embedded buffers, or
// .glb binary) into the SAME mesh::Mesh model that load_obj produces, so
// every downstream layer (voxel/texture/matcher/chunk/anvil/orchestrate)
// is untouched. Parser: vendored third_party/cgltf/cgltf.h (v1.15, MIT).
//
// Geometry:
//   - The default scene (asset "scene", else scene 0; else all root
//     nodes) is traversed; node hierarchy transforms are flattened into
//     world space (cgltf_node_transform_world), so multi-node Sketchfab
//     exports land with correct relative placement.
//   - TRIANGLES primitives only; other modes are skipped with a stderr
//     warning. Indexed and non-indexed primitives both supported.
//   - POSITION is required per primitive; TEXCOORD_0 is optional.
//
// Material mapping (glTF PBR → the MTL-shaped mesh::Material):
//   - baseColorFactor.rgb (LINEAR per glTF spec) → Kd, sRGB-ENCODED at
//     load so Material.kd keeps its documented sRGB-gamma convention
//     (the matcher's sRGB→linear transform applies downstream, exactly
//     as for MTL Kd values).
//   - emissiveFactor × KHR_materials_emissive_strength (linear), clamped
//     to [0,1] → Ke, sRGB-encoded likewise. Strength > 1 saturates: MC
//     luminance maxes out at 15 regardless.
//   - alphaMode BLEND → d = baseColorFactor.a; OPAQUE and MASK → d = 1
//     (MASK is cutoff-based — a block either exists or not; the
//     voxelizer has no per-texel alpha, so treat as opaque).
//   - baseColorTexture → map_kd (external URI, percent-decoded, relative
//     to the asset's directory) or map_kd_bytes (GLB buffer view /
//     base64 data URI — encoded bytes copied out; the texture layer
//     decodes them via TextureCache::load_from_memory).
//
// UV convention: glTF's texcoord origin is TOP-left; mesh::Mesh carries
// OBJ's BOTTOM-left convention (texture::sample_nearest flips V for
// top-first image rows). Loaded V values are therefore flipped here
// (v_mesh = 1 - v_gltf) so identical geometry produces identical samples
// through the shared downstream path.
//
// Errors: throws mesh::mesh_error (same type as load_obj — callers'
// recovery path is identical: "the file is unusable") on: file not
// found, parse failure, missing/unloadable buffers, validation failure,
// zero triangles, non-finite coordinates.
// ---------------------------------------------------------------------------
mesh::Mesh load_gltf(const std::filesystem::path& gltf_path);

}  // namespace objtomc::gltf
