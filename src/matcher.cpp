// Color matching layer — Oklab + Euclidean distance, three-branch algorithm.
// Contract: research/matcher_proposal.md (Sessions 21 / 21.1 / 21.2).
//
// Pipeline: mesh::Material (sRGB-gamma kd/ke per MTL convention; the mesh
// layer passes raw tinyobjloader values through unchanged) → sRGB-linear
// decode → Oklab → Euclidean argmin against a pre-converted palette of
// 414 full-cube MC 1.21.5 blocks.

#include "objtomc/matcher.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>

namespace objtomc::matcher {

namespace {

// ---------------------------------------------------------------------------
// sRGB transfer function (IEC 61966-2-1). Decodes sRGB-gamma in [0, 1] to
// linear sRGB in [0, 1]. The piecewise definition (linear segment below the
// 0.04045 break + gamma-2.4 segment above) is the canonical specification;
// the constants 0.04045 / 12.92 / 0.055 / 1.055 / 2.4 are part of the
// IEC 61966-2-1 standard formula and are reproduced verbatim here.
// ---------------------------------------------------------------------------
inline float srgb_to_linear_channel(float c) noexcept {
    return (c <= 0.04045f)
        ? (c / 12.92f)
        : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline std::array<float, 3> srgb_to_linear(std::array<float, 3> rgb_gamma) noexcept {
    return {srgb_to_linear_channel(rgb_gamma[0]),
            srgb_to_linear_channel(rgb_gamma[1]),
            srgb_to_linear_channel(rgb_gamma[2])};
}

// ---------------------------------------------------------------------------
// linear-sRGB → Oklab. Source: Björn Ottosson, "A perceptual color space
// for image processing" (2020), https://bottosson.github.io/posts/oklab/.
// License: dual public domain / MIT
// (https://bottosson.github.io/misc/License.txt). Date fetched: 2026-06-20
// (per DECISIONS.md 2026-06-20 "Color Matching scope (v0.1)").
//
// The 18 floating-point coefficients below are reproduced verbatim from the
// canonical reference C++ snippet in the post. Do NOT modify or "optimize"
// the coefficients — they are the published constants for the LMS matrix
// (M1) and the LMS' → Lab matrix (M2). Any deviation would silently shift
// the perceptual axes and break the empirical 7/10 agreement with CIEDE2000
// recorded in research/matcher_proposal.md §1.5(b).
// ---------------------------------------------------------------------------
inline Oklab linear_srgb_to_oklab(std::array<float, 3> rgb_lin) noexcept {
    const float r = rgb_lin[0];
    const float g = rgb_lin[1];
    const float b = rgb_lin[2];

    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    const float l_ = std::cbrt(l);
    const float m_ = std::cbrt(m);
    const float s_ = std::cbrt(s);

    return Oklab{
        0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_,
        1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_,
        0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_,
    };
}

// Combined helper for Material fields, which are sRGB-gamma per the MTL
// convention (see include/objtomc/mesh.hpp and OPEN_QUESTIONS.md
// 2026-06-21 "Material color-space convention").
inline Oklab material_color_to_oklab(std::array<float, 3> srgb_gamma) noexcept {
    return linear_srgb_to_oklab(srgb_to_linear(srgb_gamma));
}

// Note: palette::BlockPaletteEntry::avg_color_linear is ALREADY linear sRGB
// (palette.cpp applies srgb_to_linear at load time per
// palette_db_proposal.md §3.4), so the matcher feeds it directly to
// linear_srgb_to_oklab — applying srgb_to_linear a second time would be a
// double-decode bug.

inline float oklab_distance(Oklab a, Oklab b) noexcept {
    const float dL = a.L - b.L;
    const float da = a.a - b.a;
    const float db = a.b - b.b;
    return std::sqrt(dL * dL + da * da + db * db);
}

inline bool is_finite_array(const std::array<float, 3>& v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

void validate_options(const MatcherOptions& opts) {
    if (!std::isfinite(opts.emission_threshold) ||
        opts.emission_threshold < 0.0f || opts.emission_threshold > 1.0f) {
        throw matcher_error(
            "invalid emission_threshold (out of [0, 1]): " +
            std::to_string(opts.emission_threshold));
    }
    if (!std::isfinite(opts.opacity_threshold) ||
        opts.opacity_threshold < 0.0f || opts.opacity_threshold > 1.0f) {
        throw matcher_error(
            "invalid opacity_threshold (out of [0, 1]): " +
            std::to_string(opts.opacity_threshold));
    }
    if (!std::isfinite(opts.luminance_proximity_weight) ||
        opts.luminance_proximity_weight < 0.0f) {
        throw matcher_error(
            "invalid luminance_proximity_weight (must be finite and >= 0): " +
            std::to_string(opts.luminance_proximity_weight));
    }
    if (!std::isfinite(opts.opacity_proximity_weight) ||
        opts.opacity_proximity_weight < 0.0f) {
        throw matcher_error(
            "invalid opacity_proximity_weight (must be finite and >= 0): " +
            std::to_string(opts.opacity_proximity_weight));
    }
}

// ---------------------------------------------------------------------------
// Tie-break key (§3.12). The min_element comparator turns each candidate
// (entry, score) into this tuple and lexicographic-compares; smaller wins.
//
//   score                   primary: Oklab-distance + branch proximity bonus
//   opacity_order           secondary: signed under transparent-targeted
//                           branch (lower opacity wins → use +opacity)
//                           OR inverted for opaque-targeted (we prefer
//                           opacity=15 → use -opacity so it sorts smaller)
//   luminance_order         tertiary: signed under non-emissive branch
//                           (lower luminance wins → use +luminance)
//                           OR inverted for emissive branch (higher
//                           luminance wins → use -luminance)
//   name                    final: alphabetical
// ---------------------------------------------------------------------------
struct TieBreakKey {
    float       score;
    int         opacity_order;
    int         luminance_order;
    const std::string* name;  // non-owning; entry outlives the comparator

    bool operator<(const TieBreakKey& other) const noexcept {
        return std::tie(score, opacity_order, luminance_order, *name) <
               std::tie(other.score, other.opacity_order, other.luminance_order, *other.name);
    }
};

enum class Branch { Emissive, Transparent, Opaque };

}  // namespace

MatcherOptions default_options() noexcept {
    MatcherOptions o;
    o.emission_threshold          = 0.5f;
    o.opacity_threshold           = 0.5f;
    o.luminance_proximity_weight  = 0.05f;
    o.opacity_proximity_weight    = 0.05f;
    return o;
}

Matcher::Matcher(const palette::PaletteDB& db, MatcherOptions opts)
    : db_(&db), opts_(opts), cached_() {
    validate_options(opts_);
    if (db.size() == 0) {
        throw matcher_error("empty palette");
    }
    cached_.reserve(db.size());
    for (const auto& entry : db) {
        // entry.avg_color_linear is already linear sRGB (gamma-decoded at
        // load time by palette.cpp — see palette_db_proposal.md §3.4).
        // Feeding it through srgb_to_linear again would be a double-decode
        // bug; we go straight to Oklab.
        cached_.push_back(linear_srgb_to_oklab(entry.avg_color_linear));
    }
}

std::span<const Oklab> Matcher::cached_oklab() const noexcept {
    return std::span<const Oklab>(cached_);
}

chunk::BlockState Matcher::match(const mesh::Material& material) const {
    chunk::BlockState out;
    match_batch(std::span<const mesh::Material>(&material, 1),
                std::span<chunk::BlockState>(&out, 1));
    return out;
}

void Matcher::match_batch(std::span<const mesh::Material> in,
                          std::span<chunk::BlockState>    out) const {
    if (in.size() != out.size()) {
        throw matcher_error("batch size mismatch: in.size()=" +
                            std::to_string(in.size()) +
                            ", out.size()=" + std::to_string(out.size()));
    }

    for (std::size_t i = 0; i < in.size(); ++i) {
        const mesh::Material& material = in[i];

        if (!is_finite_array(material.kd) ||
            !is_finite_array(material.ke) ||
            !std::isfinite(material.d)) {
            throw matcher_error(
                "non-finite kd/d/ke in material '" + material.name + "'");
        }

        const float max_ke = std::max({material.ke[0], material.ke[1], material.ke[2]});

        // Branch selection per §3.4 / §3.5 / §3.6 / §3.7.
        Branch branch;
        Oklab target_oklab;
        if (max_ke > opts_.emission_threshold) {
            branch = Branch::Emissive;
            target_oklab = material_color_to_oklab(material.ke);
        } else if (material.d < opts_.opacity_threshold) {
            branch = Branch::Transparent;
            target_oklab = material_color_to_oklab(material.kd);
        } else {
            branch = Branch::Opaque;
            target_oklab = material_color_to_oklab(material.kd);
        }

        // Scan candidates with branch-specific pool filter, scoring, and
        // tie-break orientation.
        const palette::PaletteDB& db = *db_;
        const auto entries_begin = db.begin();
        const palette::BlockPaletteEntry* best_entry = nullptr;
        TieBreakKey best_key{};
        bool seen = false;

        std::size_t idx = 0;
        for (auto it = entries_begin; it != db.end(); ++it, ++idx) {
            const palette::BlockPaletteEntry& entry = *it;

            // Pool filter (§3.5 / §3.6 / §3.7).
            if (branch == Branch::Emissive && entry.luminance == 0) continue;
            if (branch == Branch::Transparent && entry.opacity >= 15) continue;

            const float perceptual = oklab_distance(target_oklab, cached_[idx]);
            float score = perceptual;
            if (branch == Branch::Emissive) {
                score += opts_.luminance_proximity_weight *
                         std::abs(max_ke * 15.0f -
                                  static_cast<float>(entry.luminance));
            } else if (branch == Branch::Transparent) {
                score += opts_.opacity_proximity_weight *
                         std::abs(material.d * 15.0f -
                                  static_cast<float>(entry.opacity));
            }
            // Opaque branch: no proximity bonus (§3.7).

            // Tie-break orientation per §3.12:
            //   - transparent-targeted → lower opacity wins
            //     (so opacity_order = +entry.opacity)
            //   - opaque-targeted      → higher opacity wins
            //     (so opacity_order = -entry.opacity)
            //   - non-emissive         → lower luminance wins
            //     (so luminance_order = +entry.luminance)
            //   - emissive             → higher luminance wins
            //     (so luminance_order = -entry.luminance)
            const int opacity_order =
                (branch == Branch::Transparent)
                    ? static_cast<int>(entry.opacity)
                    : -static_cast<int>(entry.opacity);
            const int luminance_order =
                (branch == Branch::Emissive)
                    ? -static_cast<int>(entry.luminance)
                    : static_cast<int>(entry.luminance);
            const TieBreakKey key{score, opacity_order, luminance_order,
                                  &entry.name};

            if (!seen || key < best_key) {
                seen = true;
                best_key = key;
                best_entry = &entry;
            }
        }

        // A pool can only be empty if the palette catalogue has no entries
        // matching the branch filter. The default catalogue has 12 emissive
        // and 45 transparent entries per the empirical harness; an empty
        // pool means a user-supplied fixture is mis-shaped. Report it.
        if (!seen) {
            throw matcher_error(
                "empty candidate pool for branch (palette has no entries "
                "matching the branch's pool filter)");
        }

        out[i].name       = best_entry->name;
        out[i].properties = best_entry->properties;
    }
}

chunk::BlockState match(const mesh::Material& material,
                        const palette::PaletteDB& db,
                        MatcherOptions opts) {
    Matcher m(db, opts);
    return m.match(material);
}

void match_batch(std::span<const mesh::Material> in,
                 std::span<chunk::BlockState>    out,
                 const palette::PaletteDB& db,
                 MatcherOptions opts) {
    Matcher m(db, opts);
    m.match_batch(in, out);
}

}  // namespace objtomc::matcher
