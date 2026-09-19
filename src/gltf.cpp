// ---------------------------------------------------------------------------
// glTF/GLB loading layer implementation (Session 49, v0.3 item #2).
//
// Parser: vendored third_party/cgltf/cgltf.h (v1.15, MIT — see
// third_party/cgltf/VERSION.txt for the SHA pin). This file is the
// project's single cgltf implementation TU (CGLTF_IMPLEMENTATION is the
// only upstream-blessed compile knob used, per the vendoring contract).
// ---------------------------------------------------------------------------

#include "objtomc/gltf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace objtomc::gltf {

namespace {

[[noreturn]] void throw_gltf(const std::string& what,
                             const std::filesystem::path& path) {
    throw mesh::mesh_error("load_gltf: " + what + ": " + path.string());
}

const char* result_name(cgltf_result r) {
    switch (r) {
        case cgltf_result_success:         return "success";
        case cgltf_result_data_too_short:  return "data too short";
        case cgltf_result_unknown_format:  return "unknown format";
        case cgltf_result_invalid_json:    return "invalid JSON";
        case cgltf_result_invalid_gltf:    return "invalid glTF";
        case cgltf_result_invalid_options: return "invalid options";
        case cgltf_result_file_not_found:  return "file not found";
        case cgltf_result_io_error:        return "I/O error";
        case cgltf_result_out_of_memory:   return "out of memory";
        case cgltf_result_legacy_gltf:     return "legacy (1.0) glTF";
        default:                           return "unknown error";
    }
}

struct CgltfGuard {
    cgltf_data* data = nullptr;
    ~CgltfGuard() { if (data != nullptr) cgltf_free(data); }
};

// Linear → sRGB gamma encoding (IEC 61966-2-1). glTF PBR factors are
// linear; mesh::Material carries the MTL sRGB-gamma convention, and the
// matcher applies sRGB→linear downstream — so factors are encoded here
// to keep one colour convention across both loaders.
float srgb_encode(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    if (c <= 0.0031308f) return 12.92f * c;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Column-major 4x4 (glTF convention, as emitted by
// cgltf_node_transform_world) applied to a point (w = 1).
std::array<float, 3> transform_point(const cgltf_float m[16],
                                     const std::array<float, 3>& p) {
    return {
        m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12],
        m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13],
        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
    };
}

void extract_material(const cgltf_material& src, std::size_t index,
                      mesh::Material& out) {
    out.name = (src.name != nullptr && src.name[0] != '\0')
        ? std::string(src.name)
        : ("gltf_material_" + std::to_string(index));
    out.kd    = {0.5f, 0.5f, 0.5f};
    out.ke    = {0.0f, 0.0f, 0.0f};
    out.ks    = {0.0f, 0.0f, 0.0f};
    out.d     = 1.0f;
    out.illum = 2;

    float base_alpha = 1.0f;
    if (src.has_pbr_metallic_roughness != 0) {
        const auto& pbr = src.pbr_metallic_roughness;
        out.kd = {srgb_encode(pbr.base_color_factor[0]),
                  srgb_encode(pbr.base_color_factor[1]),
                  srgb_encode(pbr.base_color_factor[2])};
        base_alpha = pbr.base_color_factor[3];

        const cgltf_texture* tex = pbr.base_color_texture.texture;
        if (tex != nullptr && tex->image != nullptr) {
            const cgltf_image& img = *tex->image;
            if (img.buffer_view != nullptr &&
                img.buffer_view->buffer != nullptr &&
                img.buffer_view->buffer->data != nullptr) {
                // GLB-embedded (or buffer-backed) image: copy the encoded
                // bytes out; TextureCache::load_from_memory decodes them.
                const auto* base = static_cast<const std::uint8_t*>(
                    img.buffer_view->buffer->data) + img.buffer_view->offset;
                out.map_kd_bytes.assign(base, base + img.buffer_view->size);
                out.map_kd = (img.name != nullptr && img.name[0] != '\0')
                    ? std::string(img.name)
                    : ("embedded_image_" + std::to_string(index));
            } else if (img.uri != nullptr && img.uri[0] != '\0') {
                if (std::strncmp(img.uri, "data:", 5) == 0) {
                    // Base64 data URI: decode the payload to bytes.
                    const char* comma = std::strchr(img.uri, ',');
                    if (comma != nullptr) {
                        const char* b64 = comma + 1;
                        // Decoded size: 3/4 of the base64 length, minus
                        // padding. cgltf's decoder wants the exact size.
                        const std::size_t b64_len = std::strlen(b64);
                        std::size_t decoded = (b64_len / 4) * 3;
                        if (b64_len >= 1 && b64[b64_len - 1] == '=') --decoded;
                        if (b64_len >= 2 && b64[b64_len - 2] == '=') --decoded;
                        cgltf_options topts{};
                        void* mem = nullptr;
                        if (cgltf_load_buffer_base64(&topts, decoded, b64,
                                                     &mem) ==
                                cgltf_result_success && mem != nullptr) {
                            const auto* bytes =
                                static_cast<const std::uint8_t*>(mem);
                            out.map_kd_bytes.assign(bytes, bytes + decoded);
                            std::free(mem);
                            out.map_kd =
                                "data_uri_image_" + std::to_string(index);
                        } else {
                            std::fprintf(stderr,
                                         "objtomc: warning: cannot decode "
                                         "base64 data URI for material '%s'; "
                                         "falling back to flat Kd\n",
                                         out.name.c_str());
                        }
                    }
                } else {
                    // External file: percent-decode the URI; path resolves
                    // against the asset's directory in orchestrate (same
                    // contract as MTL map_Kd).
                    std::string uri(img.uri);
                    const cgltf_size n = cgltf_decode_uri(uri.data());
                    uri.resize(n);
                    out.map_kd = std::move(uri);
                }
            }
        }
    }

    float strength = 1.0f;
    if (src.has_emissive_strength != 0) {
        strength = src.emissive_strength.emissive_strength;
    }
    out.ke = {srgb_encode(src.emissive_factor[0] * strength),
              srgb_encode(src.emissive_factor[1] * strength),
              srgb_encode(src.emissive_factor[2] * strength)};

    // BLEND → translucent with the base colour's alpha. MASK is a binary
    // cutoff (block exists or not) and OPAQUE is opaque: both d = 1.
    out.d = (src.alpha_mode == cgltf_alpha_mode_blend) ? base_alpha : 1.0f;
}

void append_primitive(const cgltf_data& data, const cgltf_primitive& prim,
                      const cgltf_float world[16], mesh::Mesh& out,
                      const std::filesystem::path& path) {
    if (prim.type != cgltf_primitive_type_triangles) {
        std::fprintf(stderr,
                     "objtomc: warning: skipping non-TRIANGLES glTF "
                     "primitive (mode %d) in '%s'\n",
                     static_cast<int>(prim.type), path.string().c_str());
        return;
    }

    const cgltf_accessor* pos_acc = nullptr;
    const cgltf_accessor* uv_acc  = nullptr;
    for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
        const cgltf_attribute& attr = prim.attributes[a];
        if (attr.type == cgltf_attribute_type_position) {
            pos_acc = attr.data;
        } else if (attr.type == cgltf_attribute_type_texcoord &&
                   attr.index == 0) {
            uv_acc = attr.data;
        }
    }
    if (pos_acc == nullptr) throw_gltf("primitive has no POSITION", path);
    if (uv_acc != nullptr && uv_acc->count != pos_acc->count) {
        throw_gltf("TEXCOORD_0 count differs from POSITION count", path);
    }

    const auto base_pos = static_cast<std::uint32_t>(out.positions.size());
    const bool has_uv   = (uv_acc != nullptr);
    const auto base_uv  = static_cast<std::int32_t>(out.uvs.size());

    for (cgltf_size i = 0; i < pos_acc->count; ++i) {
        std::array<float, 3> p{};
        if (cgltf_accessor_read_float(pos_acc, i, p.data(), 3) == 0) {
            throw_gltf("cannot read POSITION accessor", path);
        }
        const auto wp = transform_point(world, p);
        if (!std::isfinite(wp[0]) || !std::isfinite(wp[1]) ||
            !std::isfinite(wp[2])) {
            throw_gltf("non-finite vertex position", path);
        }
        out.positions.push_back(wp);
    }
    if (has_uv) {
        for (cgltf_size i = 0; i < uv_acc->count; ++i) {
            std::array<float, 2> uv{};
            if (cgltf_accessor_read_float(uv_acc, i, uv.data(), 2) == 0) {
                throw_gltf("cannot read TEXCOORD_0 accessor", path);
            }
            // glTF UV origin is top-left; mesh::Mesh carries OBJ's
            // bottom-left convention (the sampler flips V again).
            out.uvs.push_back({uv[0], 1.0f - uv[1]});
        }
    }

    std::int32_t material_id = -1;
    if (prim.material != nullptr) {
        material_id =
            static_cast<std::int32_t>(prim.material - data.materials);
    }

    const cgltf_size index_count =
        (prim.indices != nullptr) ? prim.indices->count : pos_acc->count;
    if (index_count % 3 != 0) {
        throw_gltf("TRIANGLES index count not divisible by 3", path);
    }
    for (cgltf_size i = 0; i + 2 < index_count; i += 3) {
        mesh::Face face{};
        for (int c = 0; c < 3; ++c) {
            const cgltf_size raw = (prim.indices != nullptr)
                ? cgltf_accessor_read_index(prim.indices,
                                            i + static_cast<cgltf_size>(c))
                : i + static_cast<cgltf_size>(c);
            if (raw >= pos_acc->count) {
                throw_gltf("vertex index out of range", path);
            }
            const auto sc = static_cast<std::size_t>(c);
            face.position_indices[sc] =
                base_pos + static_cast<std::uint32_t>(raw);
            face.normal_indices[sc] = -1;
            face.uv_indices[sc] = has_uv
                ? base_uv + static_cast<std::int32_t>(raw)
                : -1;
        }
        face.material_id = material_id;
        out.faces.push_back(face);
    }
}

void traverse_node(const cgltf_data& data, const cgltf_node& node,
                   mesh::Mesh& out, const std::filesystem::path& path) {
    if (node.mesh != nullptr) {
        cgltf_float world[16];
        cgltf_node_transform_world(&node, world);
        for (cgltf_size p = 0; p < node.mesh->primitives_count; ++p) {
            append_primitive(data, node.mesh->primitives[p], world, out,
                             path);
        }
    }
    for (cgltf_size c = 0; c < node.children_count; ++c) {
        traverse_node(data, *node.children[c], out, path);
    }
}

}  // namespace

mesh::Mesh load_gltf(const std::filesystem::path& gltf_path) {
    const std::string path_str = gltf_path.string();

    cgltf_options options{};
    CgltfGuard guard;
    cgltf_result r = cgltf_parse_file(&options, path_str.c_str(),
                                      &guard.data);
    if (r != cgltf_result_success) {
        throw_gltf(std::string("parse failed (") + result_name(r) + ")",
                   gltf_path);
    }
    r = cgltf_load_buffers(&options, guard.data, path_str.c_str());
    if (r != cgltf_result_success) {
        throw_gltf(std::string("buffer load failed (") + result_name(r) +
                   ")", gltf_path);
    }
    r = cgltf_validate(guard.data);
    if (r != cgltf_result_success) {
        throw_gltf(std::string("validation failed (") + result_name(r) +
                   ")", gltf_path);
    }
    const cgltf_data& data = *guard.data;

    mesh::Mesh out;
    out.materials.resize(data.materials_count);
    for (cgltf_size i = 0; i < data.materials_count; ++i) {
        extract_material(data.materials[i], i, out.materials[i]);
    }

    const cgltf_scene* scene = data.scene;
    if (scene == nullptr && data.scenes_count > 0) scene = &data.scenes[0];
    if (scene != nullptr) {
        for (cgltf_size n = 0; n < scene->nodes_count; ++n) {
            traverse_node(data, *scene->nodes[n], out, gltf_path);
        }
    } else {
        // No scene declared: traverse all root nodes.
        for (cgltf_size n = 0; n < data.nodes_count; ++n) {
            if (data.nodes[n].parent == nullptr) {
                traverse_node(data, data.nodes[n], out, gltf_path);
            }
        }
    }

    if (out.positions.empty() || out.faces.empty()) {
        throw_gltf("no triangle geometry found", gltf_path);
    }
    return out;
}

}  // namespace objtomc::gltf
