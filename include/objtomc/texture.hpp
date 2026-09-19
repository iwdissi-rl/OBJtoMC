#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace objtomc::texture {

// ---------------------------------------------------------------------------
// Texture layer (Session 48, v0.3 item #1 — per-voxel texture sampling).
//
// Decodes MTL map_Kd diffuse textures (PNG/JPG/BMP/TGA via vendored
// third_party/stb/stb_image.h) so the orchestrate layer can sample a
// per-voxel diffuse colour at each voxel's winning-triangle UV instead of
// using the material's single flat Kd.
//
// Error posture: a missing or undecodable texture file is EXPECTED input
// variance (OBJ exports routinely reference textures that were not shipped
// alongside — e.g. the repo's Sponza fixture). It is NOT an error: the
// loader warns once per path on stderr and the caller falls back to the
// material's flat Kd. This mirrors the mesh layer's "MTL not found →
// stderr warning, no throw" precedent (mesh.hpp error doc block).
//
// Colour convention: decoded texel values are sRGB-gamma encoded (PNG/JPG
// store sRGB), returned as floats in [0, 1] — the SAME encoding as
// mesh::Material::kd, so a sampled texel drops into Material.kd without
// conversion. The matcher's sRGB→linear→Oklab transform (src/matcher.cpp)
// applies downstream, exactly as it does for flat Kd values.
// ---------------------------------------------------------------------------

// Decoded image, always RGB8 (alpha and grey channels expanded/dropped by
// the decoder at load time). Rows are stored top-first (row 0 = image top),
// as decoded — the V flip for OBJ's bottom-left UV origin happens in
// sample_nearest, not at decode time.
struct Image {
    int                        width  = 0;
    int                        height = 0;
    std::vector<std::uint8_t>  rgb;         // size = width * height * 3, row-major, top row first
};

// ---------------------------------------------------------------------------
// sample_nearest — nearest-neighbour sample at UV (u, v).
//
//   - Wrap mode: repeat on both axes (u - floor(u)), the OBJ/GL default.
//     Handles the common Sponza-style UVs outside [0, 1].
//   - V flip: OBJ UV origin is bottom-left (v = 0 → image bottom row);
//     Image stores rows top-first, so v is flipped here.
//   - Nearest-neighbour (no bilinear): a voxel is a hard-edged cube, and
//     the matcher quantises to a single block anyway; bilinear would blur
//     texel boundaries without observable output benefit. Deterministic
//     and byte-stable across platforms (pure float mul + floor + clamp).
//
// Returns the texel as sRGB-gamma floats in [0, 1] (Material.kd encoding).
// Precondition: img.width > 0 && img.height > 0 (guaranteed for any Image
// returned by TextureCache::load).
// ---------------------------------------------------------------------------
std::array<float, 3> sample_nearest(const Image& img, float u, float v) noexcept;

// ---------------------------------------------------------------------------
// TextureCache — per-path decode cache.
//
// load() decodes the file on first call and caches the result (including
// failures, so a broken path warns on stderr exactly once). Returns
// nullptr when the file is missing or undecodable — the caller falls back
// to the material's flat Kd. Returned pointers remain valid for the
// lifetime of the cache.
//
// Thread-safety: NOT thread-safe; the orchestrate layer drives it from a
// single thread.
// ---------------------------------------------------------------------------
class TextureCache {
public:
    const Image* load(const std::filesystem::path& path);

    // Decode encoded image bytes held in memory (GLB buffer views / glTF
    // data URIs — Session 49). `key` is the caller-chosen cache key; it
    // must be distinct from any on-disk path the same cache may load
    // (orchestrate prefixes "embedded:"). Same contract as load():
    // failures warn once on stderr, are cached, and return nullptr.
    const Image* load_from_memory(const std::string& key,
                                  const std::uint8_t* bytes,
                                  std::size_t size);

private:
    std::unordered_map<std::string, std::optional<Image>> cache_;
};

}  // namespace objtomc::texture
