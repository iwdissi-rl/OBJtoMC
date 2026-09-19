// ---------------------------------------------------------------------------
// Texture layer implementation (Session 48, v0.3 item #1).
//
// Decoder: vendored third_party/stb/stb_image.h (v2.30, dual public
// domain / MIT — see third_party/stb/VERSION.txt for the SHA pin).
// Upstream-blessed compile-time configuration only, per the vendoring
// contract (third_party/README.md "Modification policy"):
//
//   - STB_IMAGE_IMPLEMENTATION — single-TU implementation expansion
//     (this file is the project's one stb_image implementation TU).
//   - STBI_ONLY_PNG / JPEG / BMP / TGA — restrict the decoder to the
//     formats MTL map_Kd references in practice; drops GIF/PSD/PIC/PNM
//     parsing code from the build entirely.
//   - STBI_NO_LINEAR / STBI_NO_HDR — we never decode to float or load
//     Radiance HDR; texel values stay sRGB-gamma uint8 end to end (the
//     matcher owns the sRGB→linear transform downstream).
// ---------------------------------------------------------------------------

#include "objtomc/texture.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include <stb_image.h>

namespace objtomc::texture {

namespace {

// Nearest-texel byte offset shared by sample_nearest and sample_alpha so
// both channels always come from the SAME texel.
std::size_t texel_offset(const Image& img, float u, float v) noexcept {
    // Repeat wrap: floor-subtract maps any finite float into [0, 1).
    const float uw = u - std::floor(u);
    const float vw = v - std::floor(v);

    // Nearest texel. The V flip ((1 - vw) row lookup) converts OBJ's
    // bottom-left UV origin to the Image's top-first row storage. The
    // (1 - vw) product can reach exactly 1.0 (vw == 0), so both axes
    // clamp to the last valid index.
    int x = static_cast<int>(uw * static_cast<float>(img.width));
    int y = static_cast<int>((1.0f - vw) * static_cast<float>(img.height));
    if (x > img.width  - 1) x = img.width  - 1;
    if (y > img.height - 1) y = img.height - 1;

    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(img.width) +
            static_cast<std::size_t>(x)) * 4u;
}

}  // namespace

std::array<float, 3> sample_nearest(const Image& img, float u, float v) noexcept {
    const std::size_t i = texel_offset(img, u, v);
    return {static_cast<float>(img.rgba[i + 0]) / 255.0f,
            static_cast<float>(img.rgba[i + 1]) / 255.0f,
            static_cast<float>(img.rgba[i + 2]) / 255.0f};
}

float sample_alpha(const Image& img, float u, float v) noexcept {
    return static_cast<float>(img.rgba[texel_offset(img, u, v) + 3]) / 255.0f;
}

const Image* TextureCache::load_from_memory(const std::string& key,
                                            const std::uint8_t* bytes,
                                            std::size_t size) {
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second.has_value() ? &*it->second : nullptr;
    }

    int w = 0;
    int h = 0;
    int channels_in_file = 0;
    stbi_uc* data = stbi_load_from_memory(
        bytes, static_cast<int>(size), &w, &h, &channels_in_file, 4);
    if (data == nullptr || w <= 0 || h <= 0) {
        std::fprintf(stderr,
                     "objtomc: warning: cannot decode embedded texture '%s' "
                     "(%s); falling back to material Kd\n",
                     key.c_str(), stbi_failure_reason());
        if (data != nullptr) stbi_image_free(data);
        cache_.emplace(key, std::nullopt);
        return nullptr;
    }

    Image img;
    img.width  = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) *
                    static_cast<std::size_t>(h) * 4u);
    std::memcpy(img.rgba.data(), data, img.rgba.size());
    stbi_image_free(data);

    const auto [pos, inserted] = cache_.emplace(key, std::move(img));
    (void)inserted;
    return &*pos->second;
}

const Image* TextureCache::load(const std::filesystem::path& path) {
    const std::string key = path.string();
    const auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second.has_value() ? &*it->second : nullptr;
    }

    // Force 4-channel RGBA8 output regardless of the file's native channel
    // count (grey → replicated, alpha-less → alpha = 255). Session 52:
    // the alpha channel feeds Material.d for glTF BLEND materials whose
    // transparency lives in baseColorTexture.a (sample_alpha); RGB still
    // feeds kd/ke via sample_nearest.
    int w = 0;
    int h = 0;
    int channels_in_file = 0;
    stbi_uc* data = stbi_load(key.c_str(), &w, &h, &channels_in_file, 4);
    if (data == nullptr || w <= 0 || h <= 0) {
        std::fprintf(stderr,
                     "objtomc: warning: cannot load texture '%s' (%s); "
                     "falling back to material Kd\n",
                     key.c_str(), stbi_failure_reason());
        if (data != nullptr) stbi_image_free(data);
        cache_.emplace(key, std::nullopt);
        return nullptr;
    }

    Image img;
    img.width  = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) *
                    static_cast<std::size_t>(h) * 4u);
    std::memcpy(img.rgba.data(), data, img.rgba.size());
    stbi_image_free(data);

    const auto [pos, inserted] = cache_.emplace(key, std::move(img));
    (void)inserted;
    return &*pos->second;
}

}  // namespace objtomc::texture
