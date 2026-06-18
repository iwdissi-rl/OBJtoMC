#include "objtomc/mesh.hpp"

// Compile-time configuration for tinyobjloader. Per the vendoring contract
// (third_party/README.md), upstream-blessed #defines go here in the wrapper
// translation unit — not in the vendored header. This is the ONLY .cpp in
// the project that includes tiny_obj_loader.h.
//
// We do NOT define TINYOBJLOADER_USE_DOUBLE — float32 gives ~1 part in 10^6
// precision (research/obj_loading_proposal.md §5.7).
// We do NOT define TINYOBJLOADER_USE_MAPBOX_EARCUT — fan triangulation is
// acceptable for v0.1 voxelisation downstream (proposal §5.6).
//
// We DO define TINYOBJLOADER_DISABLE_FAST_FLOAT. The vendored
// release-branch SHA 62ff207968f3dc14a64a1e2378dce67b760e7c4a embeds
// `fast_float` v8.0.2 with the upstream `std::distance` renamed to
// `tinyobj_ff::distance`. The rename loses the constexpr qualifier from
// the iterator-`distance` overload that fast_float's
// `loop_parse_if_eight_digits` (declared `FASTFLOAT_CONSTEXPR20` =
// `constexpr` under our toolchain) calls — GCC 11.4 rejects the
// constexpr-calls-non-constexpr violation as a hard error. Disabling
// the fast_float SIMD path falls back to `strtod` for float parsing,
// which is correct (slower on multi-MB OBJs by a few percent) and is
// the upstream-blessed escape hatch documented at the gate of the
// embedded-fast_float block in tiny_obj_loader.h. This is a config
// knob, not a patch — vendored source remains unchanged (vendoring
// contract: third_party/README.md §"Modification policy").
#define TINYOBJLOADER_DISABLE_FAST_FLOAT
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

namespace objtomc::mesh {

namespace {

// IO precheck: tinyobjloader's ParseFromFile returns false with an opaque
// "Cannot open file" string for missing/unreadable inputs. We surface the
// system errno reason explicitly so the caller's error message is useful.
//
// Empty-file check is also here because tinyobjloader treats an empty file
// as a successful parse with zero vertices — we want it surfaced as
// mesh_error per the proposal §5.4 throwable cases.
void precheck_readable_nonempty(const std::filesystem::path& obj_path) {
    std::error_code ec;
    if (!std::filesystem::exists(obj_path, ec)) {
        throw mesh_error("load_obj: file not found: " + obj_path.string());
    }
    if (ec) {
        throw mesh_error("load_obj: filesystem error on " +
                         obj_path.string() + ": " + ec.message());
    }
    std::ifstream in(obj_path, std::ios::binary);
    if (!in) {
        throw mesh_error("load_obj: cannot open file: " + obj_path.string() +
                         ": " + std::strerror(errno));
    }
    in.seekg(0, std::ios::end);
    const std::streampos size = in.tellg();
    if (size <= 0) {
        throw mesh_error("load_obj: file is empty: " + obj_path.string());
    }
}

// Translate tinyobj::material_t → our Material. Only the fields the
// proposal §5.3 calls out for v0.1; PBR / specular highlight maps / bump
// maps / etc. are intentionally dropped.
Material translate_material(const tinyobj::material_t& src) {
    Material out;
    out.name   = src.name;
    out.kd     = {src.diffuse[0],  src.diffuse[1],  src.diffuse[2]};
    out.ke     = {src.emission[0], src.emission[1], src.emission[2]};
    out.ks     = {src.specular[0], src.specular[1], src.specular[2]};
    out.d      = src.dissolve;
    out.illum  = src.illum;
    out.map_kd = src.diffuse_texname;
    return out;
}

// Pull a position triple out of attrib_t::vertices (flat 3*N float array)
// and validate finiteness — non-finite coords would break voxelisation
// downstream, so they surface as mesh_error per proposal §5.4.
std::array<float, 3> read_position(const tinyobj::attrib_t& attrib,
                                   std::size_t vertex_index) {
    const std::size_t base = vertex_index * 3;
    const float x = attrib.vertices[base + 0];
    const float y = attrib.vertices[base + 1];
    const float z = attrib.vertices[base + 2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        throw mesh_error("load_obj: non-finite vertex coordinate at index " +
                         std::to_string(vertex_index));
    }
    return {x, y, z};
}

std::array<float, 3> read_normal(const tinyobj::attrib_t& attrib,
                                 std::size_t normal_index) {
    const std::size_t base = normal_index * 3;
    return {attrib.normals[base + 0],
            attrib.normals[base + 1],
            attrib.normals[base + 2]};
}

std::array<float, 2> read_uv(const tinyobj::attrib_t& attrib,
                             std::size_t uv_index) {
    const std::size_t base = uv_index * 2;
    return {attrib.texcoords[base + 0], attrib.texcoords[base + 1]};
}

}  // namespace

Mesh load_obj(const std::filesystem::path& obj_path) {
    precheck_readable_nonempty(obj_path);

    tinyobj::ObjReaderConfig config;
    config.triangulate  = true;       // proposal §5.6
    config.vertex_color = false;      // §9: vertex colours deferred
    // mtl_search_path empty → tinyobj resolves .mtl next to the .obj.

    tinyobj::ObjReader reader;
    const bool ok = reader.ParseFromFile(obj_path.string(), config);

    // Warnings (e.g. "MTL not found") are non-fatal per proposal §5.4 —
    // log to stderr; do not throw.
    if (!reader.Warning().empty()) {
        std::cerr << "load_obj warning [" << obj_path.string() << "]: "
                  << reader.Warning();
        if (reader.Warning().back() != '\n') std::cerr << '\n';
    }
    if (!ok || !reader.Valid()) {
        throw mesh_error("load_obj: parse failed for " + obj_path.string() +
                         ": " + reader.Error());
    }

    const tinyobj::attrib_t&             attrib    = reader.GetAttrib();
    const std::vector<tinyobj::shape_t>& shapes    = reader.GetShapes();
    const std::vector<tinyobj::material_t>& materials = reader.GetMaterials();

    // Zero-vertex result is a usability error — voxelisation downstream
    // cannot do anything with it (proposal §5.4).
    if (attrib.vertices.empty() || (attrib.vertices.size() % 3) != 0) {
        throw mesh_error("load_obj: parsed mesh has zero vertices: " +
                         obj_path.string());
    }

    Mesh out;

    // Positions. Always populated. Triggers the finite-coord check.
    const std::size_t n_pos = attrib.vertices.size() / 3;
    out.positions.reserve(n_pos);
    for (std::size_t i = 0; i < n_pos; ++i) {
        out.positions.push_back(read_position(attrib, i));
    }

    // Normals + UVs are optional — empty when absent (proposal §5.1 SoA).
    if (!attrib.normals.empty() && (attrib.normals.size() % 3) == 0) {
        const std::size_t n_norm = attrib.normals.size() / 3;
        out.normals.reserve(n_norm);
        for (std::size_t i = 0; i < n_norm; ++i) {
            out.normals.push_back(read_normal(attrib, i));
        }
    }
    if (!attrib.texcoords.empty() && (attrib.texcoords.size() % 2) == 0) {
        const std::size_t n_uv = attrib.texcoords.size() / 2;
        out.uvs.reserve(n_uv);
        for (std::size_t i = 0; i < n_uv; ++i) {
            out.uvs.push_back(read_uv(attrib, i));
        }
    }

    // Materials. Translated one-to-one with index preservation so
    // shape_t::mesh_t::material_ids remain valid indices into out.materials.
    out.materials.reserve(materials.size());
    for (const auto& src : materials) {
        out.materials.push_back(translate_material(src));
    }

    // Faces. tinyobjloader has triangulated by config — every face has
    // exactly 3 vertices and num_face_vertices[i] == 3.
    std::size_t total_faces = 0;
    for (const auto& s : shapes) total_faces += s.mesh.num_face_vertices.size();
    out.faces.reserve(total_faces);

    for (const auto& shape : shapes) {
        const auto& mesh_in = shape.mesh;
        std::size_t corner_offset = 0;
        for (std::size_t fi = 0; fi < mesh_in.num_face_vertices.size(); ++fi) {
            const unsigned nv = mesh_in.num_face_vertices[fi];
            if (nv != 3) {
                throw mesh_error(
                    "load_obj: post-triangulation face has " +
                    std::to_string(nv) + " vertices (expected 3) at " +
                    obj_path.string());
            }

            Face face{};
            for (unsigned c = 0; c < 3; ++c) {
                const tinyobj::index_t& idx =
                    mesh_in.indices[corner_offset + c];
                if (idx.vertex_index < 0 ||
                    static_cast<std::size_t>(idx.vertex_index) >= n_pos) {
                    throw mesh_error(
                        "load_obj: face references invalid vertex_index " +
                        std::to_string(idx.vertex_index) + " in " +
                        obj_path.string());
                }
                face.position_indices[c] =
                    static_cast<std::uint32_t>(idx.vertex_index);
                face.normal_indices[c]   = idx.normal_index;
                face.uv_indices[c]       = idx.texcoord_index;
            }
            face.material_id =
                fi < mesh_in.material_ids.size() ? mesh_in.material_ids[fi] : -1;
            out.faces.push_back(face);
            corner_offset += nv;
        }
    }

    return out;
}

}  // namespace objtomc::mesh
