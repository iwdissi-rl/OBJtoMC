#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace objtomc::mesh {

// ---------------------------------------------------------------------------
// Mesh data structures
//
// Layout: struct-of-arrays at the attribute level, AoS at the face level.
// Positions are always populated. Normals and uvs may be empty if the OBJ
// did not provide them — check .empty() before indexing.
//
// Coordinate convention: values are preserved verbatim from the input OBJ
// file. OBJ uses a right-handed coordinate system, Y is up by convention,
// units are whatever the author exported in. Any axis remapping or unit
// conversion is the voxeliser's responsibility.
// ---------------------------------------------------------------------------
struct Material {
    std::string            name;     // from `newmtl <name>` in the .mtl
    std::array<float, 3>   kd;       // diffuse Kd, sRGB-gamma encoding (per MTL convention), default {0.5, 0.5, 0.5}
    std::array<float, 3>   ke;       // emission Ke, sRGB-gamma encoding (per MTL convention), default {0, 0, 0}
    std::array<float, 3>   ks;       // specular Ks,  default {0, 0, 0}
    float                  d;        // alpha   d/Tr, default 1 (opaque)
    int                    illum;    // illumination model, default 2
    std::string            map_kd;   // diffuse texture filename, "" if absent
};

struct Face {
    // Per-corner indices into the parent Mesh's attribute arrays. Each
    // corner of a triangle may pull position, normal, uv from independent
    // indices (this matches OBJ's v/vt/vn corner syntax).
    //
    // position_indices entries are always valid indices into mesh.positions.
    // normal_indices / uv_indices entries are -1 when that attribute is
    // absent for this corner (or the entire mesh, when the attribute array
    // is empty).
    std::array<std::uint32_t, 3> position_indices;
    std::array<std::int32_t,  3> normal_indices;
    std::array<std::int32_t,  3> uv_indices;

    // -1 means "no material assigned" — downstream consumers should
    // substitute their default. The valid range is [0, materials.size()).
    std::int32_t material_id;
};

struct Mesh {
    std::vector<std::array<float, 3>>  positions;   // always populated
    std::vector<std::array<float, 3>>  normals;     // empty if no normals
    std::vector<std::array<float, 2>>  uvs;         // empty if no uvs
    std::vector<Face>                  faces;       // always populated
    std::vector<Material>              materials;   // may be empty
};

// ---------------------------------------------------------------------------
// Errors. Programmer misuse, IO failures, and parse failures all surface
// as a single exception type; the caller's recovery path is the same in
// all cases ("the file is unusable"). Valid input never throws. Parser
// warnings (e.g. MTL not found) are logged to stderr but do NOT throw.
// ---------------------------------------------------------------------------
class mesh_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// load_obj — load a Wavefront OBJ file from disk into an in-memory Mesh.
//
// Throws mesh_error on: file not found, file unreadable, empty file, parse
// error reported by tinyobjloader, missing referenced .mtl, zero-vertex
// result, non-finite coordinate values.
//
// N-gons in the source OBJ are triangulated by tinyobjloader's built-in
// fan triangulator (default config). Convex n-gons triangulate correctly;
// pathological concave n-gons may produce overlapping triangles, which
// voxelisation downstream tolerates. If a future input breaks this
// assumption, mapbox/earcut can be vendored via a single compile flag
// (see research/obj_loading_proposal.md §5.6).
// ---------------------------------------------------------------------------
Mesh load_obj(const std::filesystem::path& obj_path);

}  // namespace objtomc::mesh
