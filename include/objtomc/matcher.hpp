#pragma once

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "objtomc/chunk.hpp"           // chunk::BlockState
#include "objtomc/gpu_preference.hpp"  // objtomc::GpuPreference (under OBJTOMC_WITH_CUDA)
#include "objtomc/mesh.hpp"            // mesh::Material
#include "objtomc/palette.hpp"         // palette::PaletteDB

namespace objtomc::matcher {

#ifdef OBJTOMC_WITH_CUDA
// Re-export the shared enum into objtomc::matcher so callers can spell it
// as matcher::GpuPreference::ON. Same type as objtomc::GpuPreference.
using ::objtomc::GpuPreference;
#endif

// ---------------------------------------------------------------------------
// Oklab — perceptually-uniform colour space (Björn Ottosson, 2020).
// L is lightness in [0, ~1]; a is red-green; b is yellow-blue. Both
// a and b can be negative.
// ---------------------------------------------------------------------------
struct Oklab {
    float L;
    float a;
    float b;
};

// ---------------------------------------------------------------------------
// MatcherOptions — v0.1 hardcoded defaults (default_options()), tunable by
// maintainer if matcher output quality requires it. The thresholds below are
// the empirical defaults from research/matcher_proposal.md §3.5, §3.6.
// ---------------------------------------------------------------------------
struct MatcherOptions {
    // Above this max(ke) the material is treated as emissive (§3.5).
    float emission_threshold;       // default 0.5
    // Below this d the material is treated as transparent (§3.6).
    float opacity_threshold;        // default 0.5
    // Bonus weights for luminance / opacity proximity in their branches.
    float luminance_proximity_weight; // default 0.05
    float opacity_proximity_weight;   // default 0.05
#ifdef OBJTOMC_WITH_CUDA
    // GPU dispatch preference. Default AUTO. See objtomc/gpu_preference.hpp.
    // AUTO: GPU when batch size >= GPU_BATCH_THRESHOLD (1000 materials);
    //       else CPU. ON: always GPU. OFF: always CPU. Field is guarded by
    //       OBJTOMC_WITH_CUDA so CPU-only builds stay ABI-identical to
    //       Session 37 (proposal §3.11 preservation contract).
    ::objtomc::GpuPreference prefer_gpu;
#endif
};

MatcherOptions default_options() noexcept;

// ---------------------------------------------------------------------------
// Errors. Programmer misuse only. Valid input never throws.
//   - empty palette
//   - non-finite kd / d / ke
//   - invalid options
//   - batch span size mismatch
// ---------------------------------------------------------------------------
class matcher_error : public std::runtime_error {
public:
    explicit matcher_error(const std::string& msg)
        : std::runtime_error(msg) {}
};

// ---------------------------------------------------------------------------
// Matcher — owns a non-owning view of the palette plus a pre-converted
// Oklab cache. Construction is O(n) over the palette; per-match is O(n).
//
// Lifetime: caller owns the PaletteDB; the Matcher's reference must outlive
// the Matcher. (Established pattern in the codebase — palette.hpp follows
// the same.)
//
// Thread-safety: const member functions are safe to call concurrently from
// multiple threads on a single Matcher instance. The cache is read-only
// after construction.
// ---------------------------------------------------------------------------
class Matcher {
public:
    Matcher(const palette::PaletteDB& db, MatcherOptions opts);

    chunk::BlockState match(const mesh::Material& material) const;

    void match_batch(std::span<const mesh::Material> in,
                     std::span<chunk::BlockState>    out) const;

    // The cached Oklab values, indexed by palette entry position. Exposed
    // for tests and the future GPU staging path; not for general use.
    std::span<const Oklab> cached_oklab() const noexcept;

private:
    const palette::PaletteDB*  db_;
    MatcherOptions             opts_;
    std::vector<Oklab>         cached_;
};

// ---------------------------------------------------------------------------
// One-shot convenience wrappers. Equivalent to:
//   Matcher m(db, opts); return m.match(material);
// Useful in tests and small scripts; not the hot path.
// ---------------------------------------------------------------------------
chunk::BlockState
match(const mesh::Material& material,
      const palette::PaletteDB& db,
      MatcherOptions opts = default_options());

void match_batch(std::span<const mesh::Material> in,
                 std::span<chunk::BlockState>    out,
                 const palette::PaletteDB& db,
                 MatcherOptions opts = default_options());

}  // namespace objtomc::matcher
