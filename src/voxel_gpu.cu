// ---------------------------------------------------------------------------
// GPU voxelisation adapter + kernels (v0.2 work item #3, Session 40).
//
// Contract: byte-identical VoxelGrid::cells_ output vs src/voxel.cpp's CPU
// path for any input mesh + options. Verified by tests/
// test_voxel_cpu_gpu_equality.cpp on Sponza @ 64^3 + @ 256^3, and self-
// consistency by tests/test_voxel_gpu_determinism.cpp (10x same-input runs
// byte-identical).
//
// Design (per research/gpu_port_proposal.md §3.3-§3.8 + Session 40 maintainer
// choice — AskUserQuestion 2026-07-03 "Port Akenine-Möller SAT into
// disambiguation kernel"):
//
//   1. Host: fetch shared grid setup (dims, origin, voxel_size, axis) from
//      the CPU dispatcher, apply axis swap while flattening triangle data
//      to 9-floats-per-triangle SoA, build per-triangle material_id array.
//
//   2. Device buffer allocation: triangle_data + material_id + occupancy
//      bitmap + best_key + cells. Peak Sponza @ 256^3 ~ 65 MB (proposal
//      §3.3 memory-scaling table).
//
//   3. Kernel 1 (vendored): third_party/cuda_voxelizer/voxelize.cu writes
//      the occupancy bitmap via Schwarz-Seidel plane+projection overlap +
//      atomicOr. We retain the vendored kernel per the Session 40 maintainer
//      choice (fills bitmap; NOT used as an early-out in our disambiguation
//      kernel — see kernel 3 below). Kept as scaffolding for a future
//      byte-equality-proven bitmap early-out (proposal §3.10 open risk 1).
//
//   4. Kernel 2 (init_best_key): fills best_key with ULLONG_MAX so
//      atomicMin correctly picks the first written key on each voxel.
//
//   5. Kernel 3 (disambiguation_kernel): one thread per triangle, iterate
//      the triangle's grid-space AABB (same clamp as CPU), run the SAME
//      Akenine-Möller 13-axis SAT that src/voxel.cpp uses (ported verbatim),
//      compute Eberly 1999 closest-point-on-triangle (ported verbatim),
//      atomicMin the packed uint64 key = (float_as_uint(dist) << 32) |
//      triangle_index. For non-negative distances the float-to-uint
//      bit-cast is monotone in unsigned int comparison, so atomicMin picks
//      the (dist, tri_idx) lexicographic minimum — exact match for CPU's
//      tie-break-by-lower-triangle-index rule (proposal §3.8 packed-key
//      determinism strategy).
//
//   6. Kernel 4 (postprocess_kernel): one thread per voxel, extract
//      tri_idx from the packed key, look up material_id, write to cells;
//      unhit voxels (key still ULLONG_MAX) stay at EMPTY.
//
//   7. Host: cudaMemcpy cells back into the caller-provided std::vector.
//
// Any CUDA runtime error throws voxel_error carrying the cudaGetErrorString
// message + source location (proposal §3.10 (A)/(B)/(C) failure classes).
// RAII device-buffer wrappers guarantee cudaFree on the throw path.
// ---------------------------------------------------------------------------

#include "objtomc/mesh.hpp"
#include "objtomc/voxel.hpp"
#include "voxel_internal.hpp"

// Vendored + shim include chain (see src/voxel_gpu_shim/{TriMesh.h,
// util_cuda.h, libs/cuda/helper_math.h} for the empirically-derived shim
// surface per proposal §3.10 open risk 6).
#include "libs/cuda/helper_math.h"   // our shim: float3/int3 operators, dot, cross...
#include "util.h"                    // vendored: AABox<T>, voxinfo
// NOTE: We deliberately do NOT #include "voxelize.cuh" from voxel_gpu.cu.
// voxelize.cuh contains three __constant__ uint32_t morton256_{x,y,z}[256]
// definitions; including it in more than one TU produces multiple-definition
// link errors. Instead we forward-declare the one host function we call
// (extern "C++" implicit).
extern void voxelize(const voxinfo& v,
                     float* triangle_data,
                     unsigned int* vtable,
                     bool morton_code);

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace objtomc::voxel::detail {

namespace {

// ---------------------------------------------------------------------------
// CUDA error check — throws voxel_error with source location + CUDA
// message on any non-success return. Same idiom as util_cuda.h shim's
// checkCudaErrors but scoped to our namespace so the exception type is
// voxel_error (not the shim's std::runtime_error).
// ---------------------------------------------------------------------------
#define OBJTOMC_CUDA_CHECK(call)                                               \
    do {                                                                       \
        const cudaError_t _e = (call);                                         \
        if (_e != cudaSuccess) {                                               \
            throw ::objtomc::voxel::voxel_error(                               \
                std::string("voxelize_mesh_gpu: CUDA error at ")               \
                + __FILE__ + ":" + std::to_string(__LINE__) + ": "             \
                + cudaGetErrorString(_e));                                     \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// RAII device-buffer wrapper. Ensures cudaFree runs on all exit paths
// (normal return + exception unwind). Move-only; not copyable.
// ---------------------------------------------------------------------------
template <typename T>
class DevBuf {
public:
    DevBuf() = default;
    explicit DevBuf(std::size_t n) { alloc(n); }
    ~DevBuf() { if (ptr_) cudaFree(ptr_); }
    DevBuf(const DevBuf&)            = delete;
    DevBuf& operator=(const DevBuf&) = delete;
    DevBuf(DevBuf&& other) noexcept : ptr_(other.ptr_), n_(other.n_) {
        other.ptr_ = nullptr; other.n_ = 0;
    }
    DevBuf& operator=(DevBuf&& other) noexcept {
        if (this != &other) {
            if (ptr_) cudaFree(ptr_);
            ptr_ = other.ptr_; n_ = other.n_;
            other.ptr_ = nullptr; other.n_ = 0;
        }
        return *this;
    }
    void alloc(std::size_t n) {
        if (ptr_) { cudaFree(ptr_); ptr_ = nullptr; }
        n_ = n;
        if (n_ == 0) { ptr_ = nullptr; return; }
        OBJTOMC_CUDA_CHECK(cudaMalloc(&ptr_, n_ * sizeof(T)));
    }
    T*          data()       noexcept { return ptr_; }
    const T*    data() const noexcept { return ptr_; }
    std::size_t size() const noexcept { return n_; }

private:
    T*          ptr_ = nullptr;
    std::size_t n_   = 0;
};

// ---------------------------------------------------------------------------
// axis_swap_host — same math as apply_axis_swap in src/voxel.cpp; kept
// separate so voxel_gpu.cu has no dependency on that translation unit's
// anonymous namespace.
// ---------------------------------------------------------------------------
inline void axis_swap_into(float& out_x, float& out_y, float& out_z,
                           float in_x, float in_y, float in_z,
                           AxisConvention axis) noexcept {
    if (axis == AxisConvention::Z_UP) {
        out_x = in_x; out_y = in_z; out_z = in_y;
    } else {
        out_x = in_x; out_y = in_y; out_z = in_z;
    }
}

// ---------------------------------------------------------------------------
// Device helpers: 3-arg min/max on floats. These match std::min<float>()
// exactly (IEEE 754 with NaN passthrough is not required here since our
// closest-point + SAT inputs are finite triangle coordinates).
// ---------------------------------------------------------------------------
__device__ __forceinline__ float d_min3(float a, float b, float c) noexcept {
    const float m = a < b ? a : b; return m < c ? m : c;
}
__device__ __forceinline__ float d_max3(float a, float b, float c) noexcept {
    const float m = a > b ? a : b; return m > c ? m : c;
}

// ---------------------------------------------------------------------------
// axis_test_sat — the Akenine-Möller 2001 axis test lambda from
// src/voxel.cpp, ported verbatim as a __device__ function. Same operand
// order, same std::min/std::max composition (via d_min3/d_max3), same
// short-circuit return. Byte-identical result to CPU on identical inputs.
// ---------------------------------------------------------------------------
__device__ __forceinline__ bool axis_test_sat(
    float a, float b, float fa, float fb,
    float p0_a, float p0_b,
    float p1_a, float p1_b,
    float p2_a, float p2_b,
    float bh_a, float bh_b) noexcept
{
    const float p0 = a * p0_a + b * p0_b;
    const float p1 = a * p1_a + b * p1_b;
    const float p2 = a * p2_a + b * p2_b;
    const float min_p = d_min3(p0, p1, p2);
    const float max_p = d_max3(p0, p1, p2);
    const float r = fa * bh_a + fb * bh_b;
    return !(min_p > r || max_p < -r);
}

// ---------------------------------------------------------------------------
// triangle_aabb_overlap_gpu — Akenine-Möller 13-axis SAT ported from
// src/voxel.cpp line 129 verbatim. Same axis order, same operand order.
// See src/voxel.cpp comment header for the algorithm reference.
// ---------------------------------------------------------------------------
__device__ bool triangle_aabb_overlap_gpu(
    float3 v0w, float3 v1w, float3 v2w,
    float3 box_centre, float3 box_half) noexcept
{
    // Translate so box is centred at origin.
    const float3 v0 = v0w - box_centre;
    const float3 v1 = v1w - box_centre;
    const float3 v2 = v2w - box_centre;

    // Triangle edges.
    const float3 e0 = v1 - v0;
    const float3 e1 = v2 - v1;
    const float3 e2 = v0 - v2;

    // --- 9 axes: edge × box-face-normal ---
    // e0 × X = (0, -e0.z, e0.y)
    {
        const float fey = fabsf(e0.y);
        const float fez = fabsf(e0.z);
        if (!axis_test_sat(-e0.z, e0.y, fez, fey,
                           v0.y, v0.z, v1.y, v1.z, v2.y, v2.z,
                           box_half.y, box_half.z)) return false;
    }
    // e0 × Y = (e0.z, 0, -e0.x)
    {
        const float fex = fabsf(e0.x);
        const float fez = fabsf(e0.z);
        if (!axis_test_sat(e0.z, -e0.x, fez, fex,
                           v0.x, v0.z, v1.x, v1.z, v2.x, v2.z,
                           box_half.x, box_half.z)) return false;
    }
    // e0 × Z = (-e0.y, e0.x, 0)
    {
        const float fex = fabsf(e0.x);
        const float fey = fabsf(e0.y);
        if (!axis_test_sat(-e0.y, e0.x, fey, fex,
                           v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
                           box_half.x, box_half.y)) return false;
    }
    // e1 × X
    {
        const float fey = fabsf(e1.y);
        const float fez = fabsf(e1.z);
        if (!axis_test_sat(-e1.z, e1.y, fez, fey,
                           v0.y, v0.z, v1.y, v1.z, v2.y, v2.z,
                           box_half.y, box_half.z)) return false;
    }
    // e1 × Y
    {
        const float fex = fabsf(e1.x);
        const float fez = fabsf(e1.z);
        if (!axis_test_sat(e1.z, -e1.x, fez, fex,
                           v0.x, v0.z, v1.x, v1.z, v2.x, v2.z,
                           box_half.x, box_half.z)) return false;
    }
    // e1 × Z
    {
        const float fex = fabsf(e1.x);
        const float fey = fabsf(e1.y);
        if (!axis_test_sat(-e1.y, e1.x, fey, fex,
                           v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
                           box_half.x, box_half.y)) return false;
    }
    // e2 × X
    {
        const float fey = fabsf(e2.y);
        const float fez = fabsf(e2.z);
        if (!axis_test_sat(-e2.z, e2.y, fez, fey,
                           v0.y, v0.z, v1.y, v1.z, v2.y, v2.z,
                           box_half.y, box_half.z)) return false;
    }
    // e2 × Y
    {
        const float fex = fabsf(e2.x);
        const float fez = fabsf(e2.z);
        if (!axis_test_sat(e2.z, -e2.x, fez, fex,
                           v0.x, v0.z, v1.x, v1.z, v2.x, v2.z,
                           box_half.x, box_half.z)) return false;
    }
    // e2 × Z
    {
        const float fex = fabsf(e2.x);
        const float fey = fabsf(e2.y);
        if (!axis_test_sat(-e2.y, e2.x, fey, fex,
                           v0.x, v0.y, v1.x, v1.y, v2.x, v2.y,
                           box_half.x, box_half.y)) return false;
    }

    // --- 3 axes: box face normals (X, Y, Z) ---
    {
        const float min_v = d_min3(v0.x, v1.x, v2.x);
        const float max_v = d_max3(v0.x, v1.x, v2.x);
        if (min_v > box_half.x || max_v < -box_half.x) return false;
    }
    {
        const float min_v = d_min3(v0.y, v1.y, v2.y);
        const float max_v = d_max3(v0.y, v1.y, v2.y);
        if (min_v > box_half.y || max_v < -box_half.y) return false;
    }
    {
        const float min_v = d_min3(v0.z, v1.z, v2.z);
        const float max_v = d_max3(v0.z, v1.z, v2.z);
        if (min_v > box_half.z || max_v < -box_half.z) return false;
    }

    // --- 1 axis: triangle plane normal ---
    const float3 n = cross(e0, e1);
    const float d = dot(n, v0);
    const float r = fabsf(n.x) * box_half.x
                  + fabsf(n.y) * box_half.y
                  + fabsf(n.z) * box_half.z;
    if (fabsf(d) > r) return false;

    return true;
}

// ---------------------------------------------------------------------------
// closest_point_on_triangle_gpu — Eberly 1999 7-region case analysis
// ported from src/voxel.cpp line 290 verbatim. Same branch order, same
// operand order, same clamp arithmetic. Byte-identical on identical inputs.
// ---------------------------------------------------------------------------
__device__ float3 closest_point_on_triangle_gpu(
    float3 p, float3 v0, float3 v1, float3 v2) noexcept
{
    const float3 E0 = v1 - v0;
    const float3 E1 = v2 - v0;
    const float3 D  = v0 - p;

    const float a = dot(E0, E0);
    const float b = dot(E0, E1);
    const float c = dot(E1, E1);
    const float d = dot(E0, D);
    const float e = dot(E1, D);

    const float det = a * c - b * b;
    float s = b * e - c * d;
    float t = b * d - a * e;

    if (s + t <= det) {
        if (s < 0.0f) {
            if (t < 0.0f) {
                // Region 4
                if (d < 0.0f) {
                    t = 0.0f;
                    s = (-d >= a) ? 1.0f : (-d / a);
                } else {
                    s = 0.0f;
                    t = (e >= 0.0f) ? 0.0f : ((-e >= c) ? 1.0f : (-e / c));
                }
            } else {
                // Region 3
                s = 0.0f;
                t = (e >= 0.0f) ? 0.0f : ((-e >= c) ? 1.0f : (-e / c));
            }
        } else if (t < 0.0f) {
            // Region 5
            t = 0.0f;
            s = (d >= 0.0f) ? 0.0f : ((-d >= a) ? 1.0f : (-d / a));
        } else {
            // Region 0 (interior)
            const float inv_det = 1.0f / det;
            s *= inv_det;
            t *= inv_det;
        }
    } else {
        if (s < 0.0f) {
            // Region 2
            const float tmp0 = b + d;
            const float tmp1 = c + e;
            if (tmp1 > tmp0) {
                const float numer = tmp1 - tmp0;
                const float denom = a - 2.0f * b + c;
                s = (numer >= denom) ? 1.0f : (numer / denom);
                t = 1.0f - s;
            } else {
                s = 0.0f;
                t = (tmp1 <= 0.0f) ? 1.0f : ((e >= 0.0f) ? 0.0f : (-e / c));
            }
        } else if (t < 0.0f) {
            // Region 6
            const float tmp0 = b + e;
            const float tmp1 = a + d;
            if (tmp1 > tmp0) {
                const float numer = tmp1 - tmp0;
                const float denom = a - 2.0f * b + c;
                t = (numer >= denom) ? 1.0f : (numer / denom);
                s = 1.0f - t;
            } else {
                t = 0.0f;
                s = (tmp1 <= 0.0f) ? 1.0f : ((d >= 0.0f) ? 0.0f : (-d / a));
            }
        } else {
            // Region 1
            const float numer = (c + e) - (b + d);
            if (numer <= 0.0f) {
                s = 0.0f;
            } else {
                const float denom = a - 2.0f * b + c;
                s = (numer >= denom) ? 1.0f : (numer / denom);
            }
            t = 1.0f - s;
        }
    }

    return make_float3(v0.x + s * E0.x + t * E1.x,
                       v0.y + s * E0.y + t * E1.y,
                       v0.z + s * E0.z + t * E1.z);
}

// ---------------------------------------------------------------------------
// init_best_key — fills a uint64 buffer with the sentinel ULLONG_MAX so
// atomicMin picks the first written value on each voxel.
// ---------------------------------------------------------------------------
__global__ void init_best_key(unsigned long long* best_key, std::size_t n) {
    const std::size_t stride =
        static_cast<std::size_t>(blockDim.x) *
        static_cast<std::size_t>(gridDim.x);
    std::size_t i = static_cast<std::size_t>(blockIdx.x) *
                    static_cast<std::size_t>(blockDim.x) +
                    static_cast<std::size_t>(threadIdx.x);
    constexpr unsigned long long kEmptyKey = ~0ULL;
    while (i < n) { best_key[i] = kEmptyKey; i += stride; }
}

// ---------------------------------------------------------------------------
// disambiguation_kernel — one thread per triangle. Iterates the triangle's
// voxel-space AABB (same clamp as CPU), runs Akenine-Möller SAT overlap
// test (device port), Eberly 1999 closest-point disambiguation (device
// port), then atomicMin the packed uint64 key against best_key[voxel_idx].
//
// Packed key: high 32 bits = __float_as_uint(distance) — monotone in
// unsigned int comparison for non-negative float values; low 32 bits =
// triangle index. Comparing packed keys: distance dominates, tie-break by
// lower triangle_index. Exact match for CPU's (dist, face_idx) tie-break
// via strict-less-on-pairs rule.
//
// See proposal §3.8 for the determinism argument. atomicMin(uint64) is
// per-voxel-slot atomic; scheduling order does not affect the final min.
// ---------------------------------------------------------------------------
__global__ void disambiguation_kernel(
    const float*        triangle_data,    // 9 floats per triangle
    std::uint32_t       n_triangles,
    int                 dim_x,
    int                 dim_y,
    int                 dim_z,
    // XZ window of the GLOBAL grid frame (Session 58, v0.4 tiling).
    // Full-grid callers pass (0, 0, dim_x, dim_z). Storage is
    // window-local; all geometric arithmetic stays GLOBAL.
    int                 win_x0,
    int                 win_z0,
    int                 win_wx,
    int                 win_wz,
    float               origin_x,
    float               origin_y,
    float               origin_z,
    float               vs,
    unsigned long long* best_key)         // size win_wx*dim_y*win_wz
{
    const std::uint32_t stride = blockDim.x * gridDim.x;
    std::uint32_t tri_idx = blockIdx.x * blockDim.x + threadIdx.x;

    const float half = 0.5f * vs;
    const float3 box_half = make_float3(half, half, half);

    while (tri_idx < n_triangles) {
        const std::uint32_t t = tri_idx * 9u;
        const float3 v0 = make_float3(triangle_data[t + 0],
                                      triangle_data[t + 1],
                                      triangle_data[t + 2]);
        const float3 v1 = make_float3(triangle_data[t + 3],
                                      triangle_data[t + 4],
                                      triangle_data[t + 5]);
        const float3 v2 = make_float3(triangle_data[t + 6],
                                      triangle_data[t + 7],
                                      triangle_data[t + 8]);

        // Triangle AABB in world-space.
        const float tmin_x = fminf(fminf(v0.x, v1.x), v2.x);
        const float tmin_y = fminf(fminf(v0.y, v1.y), v2.y);
        const float tmin_z = fminf(fminf(v0.z, v1.z), v2.z);
        const float tmax_x = fmaxf(fmaxf(v0.x, v1.x), v2.x);
        const float tmax_y = fmaxf(fmaxf(v0.y, v1.y), v2.y);
        const float tmax_z = fmaxf(fmaxf(v0.z, v1.z), v2.z);

        // Same idx_clamp as CPU: floor((coord - origin) / vs), then clamp
        // to [0, dim-1]. Keeps triangles just-touching the upper grid
        // boundary from being skipped.
        auto idx_clamp = [] (float v, float o, float vs2, int dim) -> int {
            int i = static_cast<int>(floorf((v - o) / vs2));
            if (i < 0)          i = 0;
            if (i > dim - 1)    i = dim - 1;
            return i;
        };
        const int x_lo = idx_clamp(tmin_x, origin_x, vs, dim_x);
        const int y_lo = idx_clamp(tmin_y, origin_y, vs, dim_y);
        const int z_lo = idx_clamp(tmin_z, origin_z, vs, dim_z);
        const int x_hi = idx_clamp(tmax_x, origin_x, vs, dim_x);
        const int y_hi = idx_clamp(tmax_y, origin_y, vs, dim_y);
        const int z_hi = idx_clamp(tmax_z, origin_z, vs, dim_z);

        // Iterate the triangle's voxel-space AABB, restricted to the
        // window in X/Z (identical intersection to the CPU window core;
        // window cells see the exact same floats as the full-grid loop).
        const int x_first = max(x_lo, win_x0);
        const int x_last  = min(x_hi, win_x0 + win_wx - 1);
        const int z_first = max(z_lo, win_z0);
        const int z_last  = min(z_hi, win_z0 + win_wz - 1);
        for (int y = y_lo; y <= y_hi; ++y) {
            for (int z = z_first; z <= z_last; ++z) {
                for (int x = x_first; x <= x_last; ++x) {
                    const float3 box_centre = make_float3(
                        origin_x + (static_cast<float>(x) + 0.5f) * vs,
                        origin_y + (static_cast<float>(y) + 0.5f) * vs,
                        origin_z + (static_cast<float>(z) + 0.5f) * vs);

                    if (!triangle_aabb_overlap_gpu(v0, v1, v2,
                                                   box_centre, box_half)) {
                        continue;
                    }

                    const float3 cp = closest_point_on_triangle_gpu(
                        box_centre, v0, v1, v2);
                    const float3 dv = box_centre - cp;
                    // CPU uses std::sqrt on dot(dv,dv); device uses sqrtf.
                    // Both are IEEE 754 correctly-rounded for well-defined
                    // inputs (proposal §3.8 open risk 1 on sqrtf/sqrt
                    // divergence is caught by the byte-equality test).
                    const float dist = sqrtf(dot(dv, dv));

                    // Bit-cast: non-negative fp32 is monotone in u32
                    // comparison. dist >= 0 by construction (closest-point
                    // distance).
                    const std::uint32_t dist_bits = __float_as_uint(dist);
                    const unsigned long long key =
                        (static_cast<unsigned long long>(dist_bits) << 32)
                        | static_cast<unsigned long long>(tri_idx);

                    // Storage order: window-local linear =
                    // (x - win_x0) + win_wx * ((z - win_z0) + win_wz * y),
                    // matching the CPU window core in src/voxel.cpp. For
                    // the full window this reduces to the pre-58
                    // x + dim_x * (z + dim_z * y) exactly.
                    const std::size_t voxel_idx =
                        static_cast<std::size_t>(x - win_x0) +
                        static_cast<std::size_t>(win_wx) *
                          (static_cast<std::size_t>(z - win_z0) +
                           static_cast<std::size_t>(win_wz) *
                             static_cast<std::size_t>(y));

                    atomicMin(&best_key[voxel_idx], key);
                }
            }
        }
        tri_idx += stride;
    }
}

// ---------------------------------------------------------------------------
// postprocess_kernel — one thread per voxel. Extract triangle_index from
// low 32 bits of the winning packed key, look up material_id from the
// per-triangle table, write to cells; also write the triangle index itself
// to tris (Session 48, per-voxel texture sampling — the tris output is
// derived from the SAME winning key that decides cells, so CPU/GPU
// byte-equality of tris follows from the packed-key determinism argument
// in the disambiguation_kernel comment). Unhit voxels (key == ULLONG_MAX)
// stay EMPTY = -1 in both outputs.
// ---------------------------------------------------------------------------
__global__ void postprocess_kernel(
    const unsigned long long* best_key,
    std::size_t               n_voxels,
    const std::int32_t*       tri_material,
    const std::int32_t*       tri_global,   // local subset idx -> global face idx (Session 58; identity for the full path)
    std::int32_t*             cells,
    std::int32_t*             tris)
{
    const std::size_t stride =
        static_cast<std::size_t>(blockDim.x) *
        static_cast<std::size_t>(gridDim.x);
    std::size_t i = static_cast<std::size_t>(blockIdx.x) *
                    static_cast<std::size_t>(blockDim.x) +
                    static_cast<std::size_t>(threadIdx.x);
    constexpr unsigned long long kEmptyKey = ~0ULL;
    constexpr std::int32_t       kEmpty    = ::objtomc::voxel::EMPTY;

    while (i < n_voxels) {
        const unsigned long long k = best_key[i];
        if (k == kEmptyKey) {
            cells[i] = kEmpty;
            tris[i]  = kEmpty;
        } else {
            const std::uint32_t tri_idx =
                static_cast<std::uint32_t>(k & 0xFFFFFFFFULL);
            cells[i] = tri_material[tri_idx];
            tris[i]  = tri_global[tri_idx];
        }
        i += stride;
    }
}

}  // anonymous namespace

// ===========================================================================
// fill_cells_gpu — full-grid host adapter. Byte-identical semantics to
// fill_cells_cpu per src/voxel.cpp for the same input; see the file header
// for the design. Since Session 58 this delegates to the windowed core
// with the full window + all faces (local tri index == global face index,
// so the packed keys — and therefore every output byte — are unchanged).
// ===========================================================================
void fill_cells_gpu(const ::objtomc::mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells,
                    std::vector<std::int32_t>& tris)
{
    fill_cells_gpu_window(mesh_in, setup,
                          /*window_x0=*/0, /*window_z0=*/0,
                          /*window_wx=*/setup.dims[0],
                          /*window_wz=*/setup.dims[2],
                          std::span<const std::int32_t>(), cells, tris);
}

// ===========================================================================
// fill_cells_gpu_window — windowed host adapter (Session 58, v0.4 §6.4).
// See voxel_internal.hpp for the contract and the local-vs-global tri
// index byte-equality argument.
// ===========================================================================
void fill_cells_gpu_window(const ::objtomc::mesh::Mesh& mesh_in,
                           const GridSetup& setup,
                           const int window_x0, const int window_z0,
                           const int window_wx, const int window_wz,
                           std::span<const std::int32_t> faces,
                           std::vector<std::int32_t>& cells,
                           std::vector<std::int32_t>& tris)
{
    // 0. Verify at least one CUDA device is visible. Prompt-required
    // (opts.prefer_gpu == ON must throw when no device is present per
    // proposal §3.4).
    {
        int dev_count = 0;
        OBJTOMC_CUDA_CHECK(cudaGetDeviceCount(&dev_count));
        if (dev_count == 0) {
            throw ::objtomc::voxel::voxel_error(
                "voxelize_mesh_gpu: no CUDA-capable device found");
        }
    }

    const bool all_faces = faces.empty();
    const std::size_t n_tris =
        all_faces ? mesh_in.faces.size() : faces.size();
    const std::size_t n_voxels =
        static_cast<std::size_t>(window_wx) *
        static_cast<std::size_t>(setup.dims[1]) *
        static_cast<std::size_t>(window_wz);

    // 1. Flatten the (subset of) mesh triangles into a 9-floats-per-
    // triangle SoA array, applying the axis swap once at flatten time so
    // the disambiguation kernel does not need per-vertex swap logic
    // (which would risk operand-order divergence vs CPU's
    // apply_axis_swap). Subset flattening preserves ascending global
    // face order, so the packed key's LOCAL index tie-break picks the
    // same winner as global indices (monotone reindexing); h_tri_global
    // maps the winner back for the tris output.
    std::vector<float> h_tris(n_tris * 9);
    std::vector<std::int32_t> h_material(n_tris);
    std::vector<std::int32_t> h_tri_global(n_tris);
    for (std::size_t i = 0; i < n_tris; ++i) {
        const std::size_t gi =
            all_faces ? i : static_cast<std::size_t>(faces[i]);
        const auto& face = mesh_in.faces[gi];
        const auto& p0 = mesh_in.positions[face.position_indices[0]];
        const auto& p1 = mesh_in.positions[face.position_indices[1]];
        const auto& p2 = mesh_in.positions[face.position_indices[2]];
        axis_swap_into(h_tris[i * 9 + 0], h_tris[i * 9 + 1], h_tris[i * 9 + 2],
                       p0[0], p0[1], p0[2], setup.axis);
        axis_swap_into(h_tris[i * 9 + 3], h_tris[i * 9 + 4], h_tris[i * 9 + 5],
                       p1[0], p1[1], p1[2], setup.axis);
        axis_swap_into(h_tris[i * 9 + 6], h_tris[i * 9 + 7], h_tris[i * 9 + 8],
                       p2[0], p2[1], p2[2], setup.axis);
        // Same material_id coercion as CPU: -1 → 0 so faces without an
        // MTL still produce occupied voxels.
        h_material[i] = (face.material_id < 0)
            ? std::int32_t{0}
            : face.material_id;
        h_tri_global[i] = static_cast<std::int32_t>(gi);
    }

    // 2. Allocate device buffers (RAII; freed on any exception).
    DevBuf<float>              d_tris(n_tris * 9);
    DevBuf<std::int32_t>       d_material(n_tris);
    DevBuf<std::int32_t>       d_tri_global(n_tris);
    DevBuf<unsigned long long> d_best_key(n_voxels);
    DevBuf<std::int32_t>       d_cells(n_voxels);
    DevBuf<std::int32_t>       d_win_tri(n_voxels);

    // 3. Host → device copies.
    OBJTOMC_CUDA_CHECK(cudaMemcpy(d_tris.data(), h_tris.data(),
                                  n_tris * 9 * sizeof(float),
                                  cudaMemcpyHostToDevice));
    OBJTOMC_CUDA_CHECK(cudaMemcpy(d_material.data(), h_material.data(),
                                  n_tris * sizeof(std::int32_t),
                                  cudaMemcpyHostToDevice));
    OBJTOMC_CUDA_CHECK(cudaMemcpy(d_tri_global.data(), h_tri_global.data(),
                                  n_tris * sizeof(std::int32_t),
                                  cudaMemcpyHostToDevice));

    // 4. Vendored voxelize kernel (fills an occupancy bitmap that is NOT
    // consulted — retained on the FULL-grid path only, per the Session 40
    // maintainer choice as "cheap early-out at most" scaffolding). Windowed
    // tile calls skip it: its voxinfo is full-frame/all-triangles shaped,
    // and its output has never influenced cells/tris.
    if (all_faces) {
        const std::size_t bitmap_words = (n_voxels + 31u) / 32u;
        DevBuf<unsigned int> d_bitmap(bitmap_words);
        OBJTOMC_CUDA_CHECK(cudaMemset(d_bitmap.data(), 0,
                                      bitmap_words * sizeof(unsigned int)));
        voxinfo vinfo(
            AABox<float3>(
                make_float3(setup.origin[0], setup.origin[1], setup.origin[2]),
                make_float3(
                    setup.origin[0] + setup.voxel_size *
                        static_cast<float>(setup.dims[0]),
                    setup.origin[1] + setup.voxel_size *
                        static_cast<float>(setup.dims[1]),
                    setup.origin[2] + setup.voxel_size *
                        static_cast<float>(setup.dims[2]))),
            make_uint3(static_cast<unsigned int>(setup.dims[0]),
                       static_cast<unsigned int>(setup.dims[1]),
                       static_cast<unsigned int>(setup.dims[2])),
            n_tris);
        // morton_code=false so the vendored kernel writes a plain linear
        // bitmap (Morton would need separate LUT setup + inverse mapping
        // in our disambiguation kernel; not worth the complexity given we
        // do not read the bitmap).
        voxelize(vinfo, d_tris.data(), d_bitmap.data(), false);
        // voxelize() calls cudaDeviceSynchronize internally on its return
        // path (vendored — see voxelize.cu line 218). Still check for any
        // deferred error from the kernel launch.
        OBJTOMC_CUDA_CHECK(cudaGetLastError());
    }

    // 5. Init best_key with ULLONG_MAX.
    {
        constexpr int kBlock = 256;
        const std::size_t n_blocks_full =
            (n_voxels + static_cast<std::size_t>(kBlock) - 1u) /
            static_cast<std::size_t>(kBlock);
        const int kGrid = static_cast<int>(std::min<std::size_t>(
            n_blocks_full, static_cast<std::size_t>(65535)));
        init_best_key<<<kGrid, kBlock>>>(d_best_key.data(), n_voxels);
        OBJTOMC_CUDA_CHECK(cudaGetLastError());
    }

    // 6. Disambiguation kernel — one thread per triangle (grid-stride).
    {
        constexpr int kBlock = 256;
        const std::size_t n_blocks_full =
            (n_tris + static_cast<std::size_t>(kBlock) - 1u) /
            static_cast<std::size_t>(kBlock);
        const int kGrid = static_cast<int>(std::min<std::size_t>(
            n_blocks_full, static_cast<std::size_t>(65535)));
        disambiguation_kernel<<<kGrid, kBlock>>>(
            d_tris.data(),
            static_cast<std::uint32_t>(n_tris),
            setup.dims[0], setup.dims[1], setup.dims[2],
            window_x0, window_z0, window_wx, window_wz,
            setup.origin[0], setup.origin[1], setup.origin[2],
            setup.voxel_size,
            d_best_key.data());
        OBJTOMC_CUDA_CHECK(cudaGetLastError());
    }

    // 7. Postprocess kernel — one thread per voxel (grid-stride).
    {
        constexpr int kBlock = 256;
        const std::size_t n_blocks_full =
            (n_voxels + static_cast<std::size_t>(kBlock) - 1u) /
            static_cast<std::size_t>(kBlock);
        const int kGrid = static_cast<int>(std::min<std::size_t>(
            n_blocks_full, static_cast<std::size_t>(65535)));
        postprocess_kernel<<<kGrid, kBlock>>>(
            d_best_key.data(), n_voxels,
            d_material.data(), d_tri_global.data(),
            d_cells.data(), d_win_tri.data());
        OBJTOMC_CUDA_CHECK(cudaGetLastError());
    }

    // 8. Sync + copyback into the caller-provided std::vectors.
    OBJTOMC_CUDA_CHECK(cudaDeviceSynchronize());
    if (cells.size() != n_voxels) cells.assign(n_voxels, EMPTY);
    if (tris.size()  != n_voxels) tris.assign(n_voxels, EMPTY);
    OBJTOMC_CUDA_CHECK(cudaMemcpy(cells.data(), d_cells.data(),
                                  n_voxels * sizeof(std::int32_t),
                                  cudaMemcpyDeviceToHost));
    OBJTOMC_CUDA_CHECK(cudaMemcpy(tris.data(), d_win_tri.data(),
                                  n_voxels * sizeof(std::int32_t),
                                  cudaMemcpyDeviceToHost));

    // 9. Device buffers freed automatically by DevBuf destructors.
}

}  // namespace objtomc::voxel::detail
