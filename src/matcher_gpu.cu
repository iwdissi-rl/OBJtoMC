// ---------------------------------------------------------------------------
// GPU matcher: per-thread argmin over Oklab palette (v0.2 work item #3,
// Session 41). One thread per material, no atomics, no cross-thread state.
//
// Contract: byte-identical std::vector<chunk::BlockState> output vs
// src/matcher.cpp's CPU loop for the same input. Verified by
// tests/test_matcher_cpu_gpu_equality.cpp on a ~2K synthetic batch
// spanning all three branches + the 8 Tier-4-canonical fixture materials.
//
// Byte-equality strategy (proposal §3.5 + §3.6, refined by Session 40's
// FMA finding documented in DECISIONS.md Session 40 SHA-pin):
//
//   - sRGB → linear → Oklab arithmetic ported verbatim from
//     src/matcher.cpp (same coefficient order, same intermediate variable
//     order, same std::pow / std::cbrt semantics via device powf / cbrtf).
//   - Three-branch dispatch (emissive / transparent / opaque) uses the
//     exact same predicates as the CPU loop.
//   - Score = Oklab distance + branch-specific proximity bonus, computed
//     in the same operand order as CPU.
//   - Tie-break lex key = (score, opacity_order, luminance_order,
//     alphabetical_rank). CPU's string-alphabetical tie-break is
//     replaced by a precomputed int32 rank in the sorted-by-name palette
//     order — semantically identical; string compare on GPU would be
//     pointer-arithmetic-heavy without adding correctness.
//   - Every score, distance, and Oklab arithmetic op is compiled with
//     --fmad=false (CMakeLists.txt target_compile_options on `matcher`)
//     to force IEEE 754 mult-then-add semantics matching GCC on host.
//
// Palette metadata (Oklab + opacity + luminance + alphabetical rank) is
// uploaded to __constant__ memory once at first kernel launch and reused
// across calls in the same process. Constant-memory footprint = 414 × 24
// bytes ≈ 10 KB, comfortably below the 64 KB device limit.
// ---------------------------------------------------------------------------

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/chunk.hpp"
#include "objtomc/palette.hpp"
#include "matcher_internal.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace objtomc::matcher::detail {

namespace {

// ---- Error macro ----------------------------------------------------------
#define OBJTOMC_MATCHER_CUDA_CHECK(call)                                       \
    do {                                                                       \
        const cudaError_t _e = (call);                                         \
        if (_e != cudaSuccess) {                                               \
            throw ::objtomc::matcher::matcher_error(                           \
                std::string("match_batch_gpu: CUDA error at ")                 \
                + __FILE__ + ":" + std::to_string(__LINE__) + ": "             \
                + cudaGetErrorString(_e));                                     \
        }                                                                      \
    } while (0)

// ---- RAII device buffer (mirrors src/voxel_gpu.cu's DevBuf) --------------
template <typename T>
class DevBuf {
public:
    DevBuf() = default;
    explicit DevBuf(std::size_t n) { alloc(n); }
    ~DevBuf() { if (ptr_) cudaFree(ptr_); }
    DevBuf(const DevBuf&)            = delete;
    DevBuf& operator=(const DevBuf&) = delete;
    DevBuf(DevBuf&& o) noexcept : ptr_(o.ptr_), n_(o.n_) {
        o.ptr_ = nullptr; o.n_ = 0;
    }
    DevBuf& operator=(DevBuf&& o) noexcept {
        if (this != &o) {
            if (ptr_) cudaFree(ptr_);
            ptr_ = o.ptr_; n_ = o.n_;
            o.ptr_ = nullptr; o.n_ = 0;
        }
        return *this;
    }
    void alloc(std::size_t n) {
        if (ptr_) { cudaFree(ptr_); ptr_ = nullptr; }
        n_ = n;
        if (n_ == 0) return;
        OBJTOMC_MATCHER_CUDA_CHECK(cudaMalloc(&ptr_, n_ * sizeof(T)));
    }
    T*          data()       noexcept { return ptr_; }
    const T*    data() const noexcept { return ptr_; }
    std::size_t size() const noexcept { return n_; }
private:
    T*          ptr_ = nullptr;
    std::size_t n_   = 0;
};

// ---- Palette in __constant__ memory --------------------------------------
// Session 41 chose the 414-entry catalog as the byte-equality reference
// palette (see AskUserQuestion 2026-07-03 "load_default_palette() — real
// 414-entry catalog"). Constant-memory capacity is 64 KB per Volta+; we
// use ~10 KB. Overprovision slot count to 1024 so the same __constant__
// layout survives palette growth up to ~2.5× without recompiling.
static constexpr int kMaxPaletteEntries = 1024;

__constant__ float c_palette_L[kMaxPaletteEntries];
__constant__ float c_palette_a[kMaxPaletteEntries];
__constant__ float c_palette_b[kMaxPaletteEntries];
__constant__ int   c_palette_opacity[kMaxPaletteEntries];
__constant__ int   c_palette_luminance[kMaxPaletteEntries];
__constant__ int   c_palette_alpha_rank[kMaxPaletteEntries];
// Session 46 (c′): renders_translucent flag per palette entry — the ruler for
// the two-pool partition on transparent + opaque branches (translucency_pool_
// partitioning_proposal.md §4). Uploaded once per PaletteDB address alongside
// the existing metadata arrays; ~1.6 KB additional constant-memory usage
// (~12 KB total of the 64 KB budget). See ensure_palette_uploaded below.
__constant__ int   c_palette_renders_translucent[kMaxPaletteEntries];
__constant__ int   c_palette_size;

// ---- Device sRGB → linear → Oklab port -----------------------------------
// Ported verbatim from src/matcher.cpp lines 29-73. Same coefficient
// values, same operand order, same intermediate temporaries. sRGB decode
// via device powf (matches host std::pow up to well-known 1-ULP libm
// error — see proposal §3.10 open risk; empirically byte-equal at this
// scale, --fmad=false is what closes the divergence gap).

__device__ __forceinline__ float d_srgb_to_linear_channel(float c) noexcept {
    return (c <= 0.04045f)
        ? (c / 12.92f)
        : powf((c + 0.055f) / 1.055f, 2.4f);
}

__device__ __forceinline__ void d_srgb_to_linear(float r, float g, float b,
                                                 float& lr, float& lg, float& lb) noexcept {
    lr = d_srgb_to_linear_channel(r);
    lg = d_srgb_to_linear_channel(g);
    lb = d_srgb_to_linear_channel(b);
}

__device__ __forceinline__ void d_linear_srgb_to_oklab(
    float r, float g, float b,
    float& oL, float& oa, float& ob) noexcept
{
    const float l = 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b;
    const float m = 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b;
    const float s = 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b;

    const float l_ = cbrtf(l);
    const float m_ = cbrtf(m);
    const float s_ = cbrtf(s);

    oL = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    oa = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    ob = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

__device__ __forceinline__ void d_material_color_to_oklab(
    float sr, float sg, float sb,
    float& oL, float& oa, float& ob) noexcept
{
    float lr, lg, lb;
    d_srgb_to_linear(sr, sg, sb, lr, lg, lb);
    d_linear_srgb_to_oklab(lr, lg, lb, oL, oa, ob);
}

// ---- Branch enum ---------------------------------------------------------
enum : int { BR_EMISSIVE = 0, BR_TRANSPARENT = 1, BR_OPAQUE = 2 };

// ---- Kernel --------------------------------------------------------------
// One thread per material. Reads (kd, ke, d) from the input arrays,
// converts to Oklab, dispatches to one of three branches, scans the
// __constant__ palette, and writes the winning palette index to
// out_palette_idx[i]. Best is tracked as five register-local values
// updated on lex-less improvement — no atomics, no cross-thread state.
__global__ void match_batch_kernel(
    const float3*  d_kd,
    const float3*  d_ke,
    const float*   d_d,
    std::uint32_t  n_materials,
    float          emission_threshold,
    float          opacity_threshold,
    float          luminance_proximity_weight,
    float          opacity_proximity_weight,
    std::int32_t*  d_out_palette_idx)
{
    const std::uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_materials) return;

    const float3 kd = d_kd[i];
    const float3 ke = d_ke[i];
    const float  d  = d_d[i];

    // Branch dispatch — same predicate order as src/matcher.cpp line 216.
    const float max_ke = fmaxf(fmaxf(ke.x, ke.y), ke.z);
    int branch;
    float t_kd_r, t_kd_g, t_kd_b;
    if (max_ke > emission_threshold) {
        branch = BR_EMISSIVE;
        t_kd_r = ke.x; t_kd_g = ke.y; t_kd_b = ke.z;
    } else if (d < opacity_threshold) {
        branch = BR_TRANSPARENT;
        t_kd_r = kd.x; t_kd_g = kd.y; t_kd_b = kd.z;
    } else {
        branch = BR_OPAQUE;
        t_kd_r = kd.x; t_kd_g = kd.y; t_kd_b = kd.z;
    }

    float target_L, target_a, target_b;
    d_material_color_to_oklab(t_kd_r, t_kd_g, t_kd_b,
                              target_L, target_a, target_b);

    // Lex-min tracker. Sentinel: score = +INF, others irrelevant while
    // seen == false.
    float best_score        = INFINITY;
    int   best_opacity_ord  = 0;
    int   best_luminance_ord = 0;
    int   best_alpha_rank   = 0;
    int   best_palette_idx  = -1;
    bool  seen              = false;

    const int n_pal = c_palette_size;
    for (int p = 0; p < n_pal; ++p) {
        const int  entry_opac = c_palette_opacity[p];
        const int  entry_lum  = c_palette_luminance[p];
        const int  entry_rt   = c_palette_renders_translucent[p];

        // Pool filter (§3.5 emissive unchanged; §3.6 transparent + §3.7 opaque
        // replaced by Session 46 (c′) two-pool partition per
        // research/translucency_pool_partitioning_proposal.md §4).
        if (branch == BR_EMISSIVE    && entry_lum == 0) continue;
        if (branch == BR_TRANSPARENT && !entry_rt)      continue;
        if (branch == BR_OPAQUE      &&  entry_rt)      continue;

        const float pal_L = c_palette_L[p];
        const float pal_a = c_palette_a[p];
        const float pal_b = c_palette_b[p];

        const float dL = target_L - pal_L;
        const float da = target_a - pal_a;
        const float db = target_b - pal_b;
        const float perceptual = sqrtf(dL * dL + da * da + db * db);

        float score = perceptual;
        if (branch == BR_EMISSIVE) {
            score += luminance_proximity_weight *
                     fabsf(max_ke * 15.0f - static_cast<float>(entry_lum));
        } else if (branch == BR_TRANSPARENT) {
            score += opacity_proximity_weight *
                     fabsf(d * 15.0f - static_cast<float>(entry_opac));
        }
        // Opaque: no bonus (§3.7).

        // Tie-break orientation per src/matcher.cpp lines 265-272.
        const int opacity_order =
            (branch == BR_TRANSPARENT) ? entry_opac : -entry_opac;
        const int luminance_order =
            (branch == BR_EMISSIVE)    ? -entry_lum : entry_lum;
        const int alpha_rank = c_palette_alpha_rank[p];

        // Lex less: (score, opacity_order, luminance_order, alpha_rank).
        // Mirrors CPU's TieBreakKey::operator< (std::tie chain).
        bool less;
        if (score != best_score) {
            less = score < best_score;
        } else if (opacity_order != best_opacity_ord) {
            less = opacity_order < best_opacity_ord;
        } else if (luminance_order != best_luminance_ord) {
            less = luminance_order < best_luminance_ord;
        } else {
            less = alpha_rank < best_alpha_rank;
        }

        if (!seen || less) {
            seen              = true;
            best_score        = score;
            best_opacity_ord  = opacity_order;
            best_luminance_ord = luminance_order;
            best_alpha_rank   = alpha_rank;
            best_palette_idx  = p;
        }
    }

    // best_palette_idx is -1 iff the pool was empty for this material's
    // branch. CPU throws in that case; on GPU we cannot throw, so we
    // encode "-1" as an out-of-band sentinel that the host converts to
    // matcher_error("empty candidate pool for branch"). This mirrors the
    // CPU behaviour exactly.
    d_out_palette_idx[i] = best_palette_idx;
}

// ---- Palette upload — once per process, guarded by pointer identity ------
// Session 41 chose the default 414-entry catalog. A test that constructs a
// different PaletteDB (e.g. the tie-break-fixture 2-row palette) is not
// currently exercised by matcher_cpu_gpu_equality, but the host wrapper
// re-uploads the palette whenever the pointer changes; the guard keeps
// re-upload cost zero for the common case of one PaletteDB per process.
static const ::objtomc::palette::PaletteDB* g_last_uploaded_db = nullptr;
static int g_last_uploaded_size = 0;

void ensure_palette_uploaded(
    const ::objtomc::palette::PaletteDB& db,
    std::span<const Oklab>               palette_oklab)
{
    if (g_last_uploaded_db == &db &&
        g_last_uploaded_size == static_cast<int>(db.size())) {
        return;
    }
    const int n = static_cast<int>(db.size());
    if (n > kMaxPaletteEntries) {
        throw ::objtomc::matcher::matcher_error(
            "match_batch_gpu: palette has " + std::to_string(n) +
            " entries, exceeds kMaxPaletteEntries=" +
            std::to_string(kMaxPaletteEntries) +
            " (rebuild GPU matcher with larger constant-memory reservation)");
    }

    // Compute alphabetical rank per palette entry — the int32 proxy for
    // CPU's string-alphabetical tie-break (see file header comment).
    std::vector<int> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
        [&](int a_i, int b_i) {
            return db.begin()[a_i].name < db.begin()[b_i].name;
        });
    std::vector<int> alpha_rank(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        alpha_rank[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])] = k;
    }

    // Flatten Oklab + metadata into 7 host arrays for cudaMemcpyToSymbol.
    // Session 46 (c′) adds the renders_translucent array.
    std::vector<float> h_L(static_cast<std::size_t>(n));
    std::vector<float> h_a(static_cast<std::size_t>(n));
    std::vector<float> h_b(static_cast<std::size_t>(n));
    std::vector<int>   h_opacity(static_cast<std::size_t>(n));
    std::vector<int>   h_luminance(static_cast<std::size_t>(n));
    std::vector<int>   h_renders_translucent(static_cast<std::size_t>(n));
    for (int p = 0; p < n; ++p) {
        h_L[static_cast<std::size_t>(p)] = palette_oklab[static_cast<std::size_t>(p)].L;
        h_a[static_cast<std::size_t>(p)] = palette_oklab[static_cast<std::size_t>(p)].a;
        h_b[static_cast<std::size_t>(p)] = palette_oklab[static_cast<std::size_t>(p)].b;
        const auto& entry = db.begin()[p];
        h_opacity[static_cast<std::size_t>(p)]             = static_cast<int>(entry.opacity);
        h_luminance[static_cast<std::size_t>(p)]           = static_cast<int>(entry.luminance);
        h_renders_translucent[static_cast<std::size_t>(p)] = entry.renders_translucent ? 1 : 0;
    }

    const std::size_t nbytes_f = static_cast<std::size_t>(n) * sizeof(float);
    const std::size_t nbytes_i = static_cast<std::size_t>(n) * sizeof(int);

    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_L, h_L.data(), nbytes_f, 0, cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_a, h_a.data(), nbytes_f, 0, cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_b, h_b.data(), nbytes_f, 0, cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_opacity, h_opacity.data(), nbytes_i, 0, cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_luminance, h_luminance.data(), nbytes_i, 0,
        cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_alpha_rank, alpha_rank.data(), nbytes_i, 0,
        cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_renders_translucent, h_renders_translucent.data(), nbytes_i, 0,
        cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpyToSymbol(
        c_palette_size, &n, sizeof(int), 0, cudaMemcpyHostToDevice));

    g_last_uploaded_db   = &db;
    g_last_uploaded_size = n;
}

}  // anonymous namespace

// ===========================================================================
// fill_matches_gpu — host adapter. Mirrors the src/matcher.cpp
// Matcher::match_batch loop 1:1 in semantics. Byte-identical
// std::vector<chunk::BlockState> output on the same input.
// ===========================================================================
void fill_matches_gpu(std::span<const MatchInput>              in,
                      std::span<::objtomc::chunk::BlockState>  out,
                      const ::objtomc::palette::PaletteDB&      db,
                      std::span<const Oklab>                     palette_oklab,
                      const MatcherOptions&                      opts)
{
    if (in.size() != out.size()) {
        throw ::objtomc::matcher::matcher_error(
            "match_batch_gpu: in.size()=" + std::to_string(in.size()) +
            ", out.size()=" + std::to_string(out.size()));
    }
    if (in.empty()) return;
    if (db.size() != palette_oklab.size()) {
        throw ::objtomc::matcher::matcher_error(
            "match_batch_gpu: palette / cached-oklab size mismatch");
    }

    // Verify a CUDA device is visible (Matcher's dispatcher checks
    // prefer_gpu == ON earlier; this guard reports the same
    // matcher_error the CPU path never has to).
    {
        int dev_count = 0;
        OBJTOMC_MATCHER_CUDA_CHECK(cudaGetDeviceCount(&dev_count));
        if (dev_count == 0) {
            throw ::objtomc::matcher::matcher_error(
                "match_batch_gpu: no CUDA-capable device found");
        }
    }

    // CPU throws on non-finite input fields. Mirror that check on host
    // (the kernel would produce garbage NaNs otherwise; better to fail
    // with the same message as the CPU MatchInput path). The Material
    // overload validates before conversion, with material names.
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto& m = in[i];
        auto finite3 = [](const std::array<float, 3>& v) {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        };
        if (!finite3(m.kd) || !finite3(m.ke) || !std::isfinite(m.d)) {
            throw ::objtomc::matcher::matcher_error(
                "non-finite kd/d/ke in match input at index " +
                std::to_string(i));
        }
    }

    ensure_palette_uploaded(db, palette_oklab);

    const std::size_t n_mat = in.size();

    // Flatten input materials to float3/float SoA for the kernel.
    std::vector<float3> h_kd(n_mat);
    std::vector<float3> h_ke(n_mat);
    std::vector<float>  h_d(n_mat);
    for (std::size_t i = 0; i < n_mat; ++i) {
        h_kd[i] = make_float3(in[i].kd[0], in[i].kd[1], in[i].kd[2]);
        h_ke[i] = make_float3(in[i].ke[0], in[i].ke[1], in[i].ke[2]);
        h_d[i]  = in[i].d;
    }

    // Device buffers.
    DevBuf<float3>       d_kd(n_mat);
    DevBuf<float3>       d_ke(n_mat);
    DevBuf<float>        d_d(n_mat);
    DevBuf<std::int32_t> d_out(n_mat);

    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpy(d_kd.data(), h_kd.data(),
        n_mat * sizeof(float3), cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpy(d_ke.data(), h_ke.data(),
        n_mat * sizeof(float3), cudaMemcpyHostToDevice));
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpy(d_d.data(), h_d.data(),
        n_mat * sizeof(float), cudaMemcpyHostToDevice));

    // Launch — one thread per material, grid-stride not needed because
    // n_materials is typically <= few million; 256-thread blocks give
    // enough occupancy on Ampere / Ada. If n_mat > 65535 * 256 = ~16.7 M,
    // we would need grid-stride; check-then-clamp for defensive safety.
    constexpr int kBlock = 256;
    const std::size_t n_blocks_full =
        (n_mat + static_cast<std::size_t>(kBlock) - 1u) /
        static_cast<std::size_t>(kBlock);
    if (n_blocks_full > 65535u) {
        throw ::objtomc::matcher::matcher_error(
            "match_batch_gpu: batch size " + std::to_string(n_mat) +
            " exceeds one-launch grid capacity (kBlock*65535). "
            "Use grid-stride or split the batch.");
    }
    const int kGrid = static_cast<int>(n_blocks_full);

    match_batch_kernel<<<kGrid, kBlock>>>(
        d_kd.data(), d_ke.data(), d_d.data(),
        static_cast<std::uint32_t>(n_mat),
        opts.emission_threshold,
        opts.opacity_threshold,
        opts.luminance_proximity_weight,
        opts.opacity_proximity_weight,
        d_out.data());
    OBJTOMC_MATCHER_CUDA_CHECK(cudaGetLastError());
    OBJTOMC_MATCHER_CUDA_CHECK(cudaDeviceSynchronize());

    // Copyback + materialise BlockState by palette index.
    std::vector<std::int32_t> h_out(n_mat);
    OBJTOMC_MATCHER_CUDA_CHECK(cudaMemcpy(h_out.data(), d_out.data(),
        n_mat * sizeof(std::int32_t), cudaMemcpyDeviceToHost));

    for (std::size_t i = 0; i < n_mat; ++i) {
        const std::int32_t pal_idx = h_out[i];
        if (pal_idx < 0) {
            throw ::objtomc::matcher::matcher_error(
                "empty candidate pool for branch (palette has no entries "
                "matching the branch's pool filter)");
        }
        const auto& entry = db.begin()[pal_idx];
        out[i].name       = entry.name;
        out[i].properties = entry.properties;
    }
}

}  // namespace objtomc::matcher::detail
