// ---------------------------------------------------------------------------
// Voxelisation layer implementation.
//
// Algorithm references (cited verbatim per established project convention,
// see DECISIONS.md 2026-06-29 "Voxelisation scope (v0.1)"):
//
//   - Surface voxelisation: Schwarz & Seidel 2010, "Fast Parallel Surface
//     and Solid Voxelization on GPUs", ACM ToG 29(6), SIGGRAPH Asia 2010,
//     Article 179. §3 (conservative surface).
//     URL: https://www.michael-schwarz.com/research/publ/files/vox-siga10.pdf
//     License: paper, not code — algorithm reproduced from published text.
//
//   - Triangle-AABB overlap: Akenine-Möller 2001, "Fast 3D Triangle-Box
//     Overlap Testing", Journal of Graphics Tools 6(1).
//     Algorithm: 13-axis separating axis theorem test (1 triangle normal +
//     3 box face normals + 9 edge-edge cross products).
//
//   - Closest point on triangle: Eberly 1999, "Distance Between a Point
//     and a Triangle in 3D" (also in Schneider & Eberly "Geometric Tools
//     for Computer Graphics" 2003). 7-region barycentric classification.
//     URL: https://www.geometrictools.com/Documentation/DistancePoint3Triangle3.pdf
//
//   - Empirical-reference baseline (used at test time, NOT vendored):
//     Forceflow/cuda_voxelizer (MIT, Jeroen Baert),
//     https://github.com/Forceflow/cuda_voxelizer
//     src/cpu_voxelizer.cpp implements the same Schwarz-Seidel §3 algorithm;
//     for single-material meshes our occupancy bitmap should agree with
//     cuda_voxelizer's output.
// ---------------------------------------------------------------------------

#include "objtomc/voxel.hpp"
#include "voxel_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace objtomc::voxel {

// ===========================================================================
// Anonymous namespace — private helpers
// ===========================================================================
namespace {

using Vec3 = std::array<float, 3>;

// ---------------------------------------------------------------------------
// Tiny vector math.
// ---------------------------------------------------------------------------
inline Vec3 v_sub(const Vec3& a, const Vec3& b) noexcept {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Vec3 v_cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}
inline float v_dot(const Vec3& a, const Vec3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline float v_len(const Vec3& a) noexcept {
    return std::sqrt(v_dot(a, a));
}

// ---------------------------------------------------------------------------
// Axis swap helper. Y_UP is identity; Z_UP swaps Y and Z so the output grid
// is always Y-up to match MC chunk conventions (chunk::WORLD_BOTTOM_Y = -64).
// See proposal §3.3.
// ---------------------------------------------------------------------------
inline Vec3 apply_axis_swap(const Vec3& p, AxisConvention axis) noexcept {
    if (axis == AxisConvention::Z_UP) {
        return {p[0], p[2], p[1]};
    }
    return p;
}

// ---------------------------------------------------------------------------
// compute_mesh_aabb — single-pass AABB over the (axis-swapped) mesh positions.
// Returns (min, max). Throws via the caller's voxel_error if a non-finite
// position is encountered (callers pre-validate, but we double-check here).
// ---------------------------------------------------------------------------
struct Aabb {
    Vec3 min;
    Vec3 max;
};

inline Aabb compute_mesh_aabb(const mesh::Mesh& m, AxisConvention axis) {
    constexpr float kInf = std::numeric_limits<float>::infinity();
    Aabb out{{ kInf,  kInf,  kInf},
             {-kInf, -kInf, -kInf}};
    for (const auto& pos : m.positions) {
        const Vec3 p = apply_axis_swap(pos, axis);
        for (int k = 0; k < 3; ++k) {
            if (p[k] < out.min[k]) out.min[k] = p[k];
            if (p[k] > out.max[k]) out.max[k] = p[k];
        }
    }
    return out;
}

// ===========================================================================
// Akenine-Möller 2001 — triangle-AABB overlap test.
//
// Reference: Tomas Akenine-Möller, "Fast 3D Triangle-Box Overlap Testing",
// Journal of Graphics Tools 6(1), 2001. The 13-axis separating-axis theorem
// (SAT) test for overlap between a triangle and an axis-aligned box:
//
//     axis 1     : triangle plane normal
//     axes 2-4   : box face normals (X, Y, Z unit vectors)
//     axes 5-13  : 9 cross products of triangle edges with box face normals
//
// Returns true iff the triangle and box overlap.
//
// Inputs:
//   v0, v1, v2  — triangle vertices (in voxel-grid space)
//   box_centre  — box centre (= voxel centre, voxel-grid space)
//   box_half    — box half-extents (= 0.5 along each axis, since voxels are
//                 unit cubes in voxel-grid space)
//
// The implementation reproduces Möller's published axis order verbatim
// (axes a00, a01, a02 from e0; a10, a11, a12 from e1; a20, a21, a22 from e2;
// then box face normals X/Y/Z; then the triangle plane). Do NOT reorder for
// "optimization" — the published algorithm IS the spec (per session prompt's
// algorithmic discipline).
// ===========================================================================
inline bool triangle_aabb_overlap(const Vec3& v0_world,
                                  const Vec3& v1_world,
                                  const Vec3& v2_world,
                                  const Vec3& box_centre,
                                  const Vec3& box_half) noexcept {
    // Translate everything so the box is centred at the origin.
    const Vec3 v0 = v_sub(v0_world, box_centre);
    const Vec3 v1 = v_sub(v1_world, box_centre);
    const Vec3 v2 = v_sub(v2_world, box_centre);

    // Triangle edges.
    const Vec3 e0 = v_sub(v1, v0);
    const Vec3 e1 = v_sub(v2, v1);
    const Vec3 e2 = v_sub(v0, v2);

    // ---- 9 axes: edge × box-face-normal (the cross products) -------------
    // The pattern below mirrors Möller's macros AXISTEST_X01, X2, Y02, Y1,
    // Z12, Z0 — each test projects the triangle vertices onto an axis and
    // compares against the box's projected radius.

    auto axis_test = [&](float a, float b, float fa, float fb,
                         float p0_a, float p0_b,
                         float p1_a, float p1_b,
                         float p2_a, float p2_b,
                         float bh_a, float bh_b) noexcept -> bool {
        // p_i = a * p_i_a + b * p_i_b (the projection of v_i onto the axis,
        // where the cross product's i-th component vanishes).
        const float p0 = a * p0_a + b * p0_b;
        const float p1 = a * p1_a + b * p1_b;
        const float p2 = a * p2_a + b * p2_b;
        const float min_p = std::min({p0, p1, p2});
        const float max_p = std::max({p0, p1, p2});
        const float r = fa * bh_a + fb * bh_b;
        return !(min_p > r || max_p < -r);
    };

    // e0 × X = (0, -e0.z, e0.y)
    {
        const float fey = std::fabs(e0[1]);
        const float fez = std::fabs(e0[2]);
        if (!axis_test(-e0[2],  e0[1], fez, fey,
                       v0[1], v0[2],  v1[1], v1[2],  v2[1], v2[2],
                       box_half[1], box_half[2])) return false;
    }
    // e0 × Y = (e0.z, 0, -e0.x)
    {
        const float fex = std::fabs(e0[0]);
        const float fez = std::fabs(e0[2]);
        if (!axis_test( e0[2], -e0[0], fez, fex,
                       v0[0], v0[2],  v1[0], v1[2],  v2[0], v2[2],
                       box_half[0], box_half[2])) return false;
    }
    // e0 × Z = (-e0.y, e0.x, 0)
    {
        const float fex = std::fabs(e0[0]);
        const float fey = std::fabs(e0[1]);
        if (!axis_test(-e0[1],  e0[0], fey, fex,
                       v0[0], v0[1],  v1[0], v1[1],  v2[0], v2[1],
                       box_half[0], box_half[1])) return false;
    }

    // e1 × X
    {
        const float fey = std::fabs(e1[1]);
        const float fez = std::fabs(e1[2]);
        if (!axis_test(-e1[2],  e1[1], fez, fey,
                       v0[1], v0[2],  v1[1], v1[2],  v2[1], v2[2],
                       box_half[1], box_half[2])) return false;
    }
    // e1 × Y
    {
        const float fex = std::fabs(e1[0]);
        const float fez = std::fabs(e1[2]);
        if (!axis_test( e1[2], -e1[0], fez, fex,
                       v0[0], v0[2],  v1[0], v1[2],  v2[0], v2[2],
                       box_half[0], box_half[2])) return false;
    }
    // e1 × Z
    {
        const float fex = std::fabs(e1[0]);
        const float fey = std::fabs(e1[1]);
        if (!axis_test(-e1[1],  e1[0], fey, fex,
                       v0[0], v0[1],  v1[0], v1[1],  v2[0], v2[1],
                       box_half[0], box_half[1])) return false;
    }

    // e2 × X
    {
        const float fey = std::fabs(e2[1]);
        const float fez = std::fabs(e2[2]);
        if (!axis_test(-e2[2],  e2[1], fez, fey,
                       v0[1], v0[2],  v1[1], v1[2],  v2[1], v2[2],
                       box_half[1], box_half[2])) return false;
    }
    // e2 × Y
    {
        const float fex = std::fabs(e2[0]);
        const float fez = std::fabs(e2[2]);
        if (!axis_test( e2[2], -e2[0], fez, fex,
                       v0[0], v0[2],  v1[0], v1[2],  v2[0], v2[2],
                       box_half[0], box_half[2])) return false;
    }
    // e2 × Z
    {
        const float fex = std::fabs(e2[0]);
        const float fey = std::fabs(e2[1]);
        if (!axis_test(-e2[1],  e2[0], fey, fex,
                       v0[0], v0[1],  v1[0], v1[1],  v2[0], v2[1],
                       box_half[0], box_half[1])) return false;
    }

    // ---- 3 axes: box face normals (X, Y, Z) ------------------------------
    // Project the triangle onto each axis and check overlap with [-h, +h].
    for (int k = 0; k < 3; ++k) {
        const float min_v = std::min({v0[k], v1[k], v2[k]});
        const float max_v = std::max({v0[k], v1[k], v2[k]});
        if (min_v >  box_half[k] || max_v < -box_half[k]) return false;
    }

    // ---- 1 axis: triangle plane normal -----------------------------------
    // Project the box onto the triangle's normal and check it contains the
    // plane (i.e. distance from box centre to plane ≤ box's projected radius).
    const Vec3  n = v_cross(e0, e1);
    const float d = v_dot(n, v0);                  // plane: n·x = d, with x = v0
    const float r = std::fabs(n[0]) * box_half[0]
                  + std::fabs(n[1]) * box_half[1]
                  + std::fabs(n[2]) * box_half[2];
    // Box centre is at origin in this translated frame, so signed distance
    // from centre to plane = -d (since n·0 - d = -d). The box overlaps the
    // plane iff |d| ≤ r.
    if (std::fabs(d) > r) return false;

    return true;
}

// ===========================================================================
// Eberly 1999 — closest point on triangle to a 3D point.
//
// Reference: David Eberly, "Distance Between a Point and a Triangle in 3D",
// Geometric Tools, 1999.
// URL: https://www.geometrictools.com/Documentation/DistancePoint3Triangle3.pdf
//
// Parametrise the triangle as T(s, t) = B + s*E0 + t*E1, with B = v0,
// E0 = v1 - v0, E1 = v2 - v0. The signed squared distance from a point P
// to T(s, t) is a quadratic Q(s, t); minimising Q(s, t) over the valid
// barycentric region (s ≥ 0, t ≥ 0, s + t ≤ 1) gives the closest point.
//
// The (s, t) plane is divided into 7 regions (named per Eberly):
//
//   region 0 — interior (s >= 0, t >= 0, s + t <= 1)
//   region 1 — edge t (s + t > 1, s >= 0, t >= 0): closest point on the
//              hypotenuse v1-v2
//   region 2 — vertex v2 corner area (s < 0, s + t > 1)
//   region 3 — edge s=0 (s < 0, t in [0, 1]): closest point on edge v0-v2
//   region 4 — vertex v0 corner area (s < 0, t < 0)
//   region 5 — edge t=0 (t < 0, s in [0, 1]): closest point on edge v0-v1
//   region 6 — vertex v1 corner area (s > 1, t < 0)
//
// The code dispatches on the sign of s, t, and (s + t - 1) and on the
// gradient of Q at the candidate edge, picking the right clamping branch.
// ===========================================================================
inline Vec3 closest_point_on_triangle(const Vec3& p,
                                      const Vec3& v0,
                                      const Vec3& v1,
                                      const Vec3& v2) noexcept {
    const Vec3 E0 = v_sub(v1, v0);
    const Vec3 E1 = v_sub(v2, v0);
    const Vec3 D  = v_sub(v0, p);

    const float a = v_dot(E0, E0);
    const float b = v_dot(E0, E1);
    const float c = v_dot(E1, E1);
    const float d = v_dot(E0, D);
    const float e = v_dot(E1, D);
    // const float f = v_dot(D, D);  // constant term — not needed for argmin

    const float det = a * c - b * b;
    float s = b * e - c * d;
    float t = b * d - a * e;

    if (s + t <= det) {
        if (s < 0.0f) {
            if (t < 0.0f) {
                // ---------- region 4 ----------
                if (d < 0.0f) {
                    t = 0.0f;
                    s = (-d >= a) ? 1.0f : (-d / a);
                } else {
                    s = 0.0f;
                    t = (e >= 0.0f) ? 0.0f : ((-e >= c) ? 1.0f : (-e / c));
                }
            } else {
                // ---------- region 3 ----------
                s = 0.0f;
                t = (e >= 0.0f) ? 0.0f : ((-e >= c) ? 1.0f : (-e / c));
            }
        } else if (t < 0.0f) {
            // ---------- region 5 ----------
            t = 0.0f;
            s = (d >= 0.0f) ? 0.0f : ((-d >= a) ? 1.0f : (-d / a));
        } else {
            // ---------- region 0 (interior) ----------
            const float inv_det = 1.0f / det;
            s *= inv_det;
            t *= inv_det;
        }
    } else {
        if (s < 0.0f) {
            // ---------- region 2 ----------
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
            // ---------- region 6 ----------
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
            // ---------- region 1 ----------
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

    return {v0[0] + s * E0[0] + t * E1[0],
            v0[1] + s * E0[1] + t * E1[1],
            v0[2] + s * E0[2] + t * E1[2]};
}

// ---------------------------------------------------------------------------
// Storage-order helper. Linear index = x + dim_x * (z + dim_z * y) per
// proposal §3.6. The body of for_each_voxel in the header walks this order
// directly; this helper is for the at() / at_unchecked() accessors.
// ---------------------------------------------------------------------------
inline std::size_t cell_index(int x, int y, int z,
                              const std::array<int, 3>& dims) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(dims[0]) *
               (static_cast<std::size_t>(z) +
                static_cast<std::size_t>(dims[2]) *
                    static_cast<std::size_t>(y));
}

// ---------------------------------------------------------------------------
// Validation: options + mesh. Programmer-misuse only; throws voxel_error.
// ---------------------------------------------------------------------------
void validate_options(const VoxelizerOptions& opts) {
    if (!std::isfinite(opts.voxel_size) || opts.voxel_size <= 0.0f) {
        throw voxel_error(
            "voxelize_mesh: voxel_size must be positive and finite, got " +
            std::to_string(opts.voxel_size));
    }
    if (opts.max_grid_cells <= 0) {
        throw voxel_error(
            "voxelize_mesh: max_grid_cells must be positive, got " +
            std::to_string(opts.max_grid_cells));
    }
    if (opts.dimensions.has_value()) {
        const auto& d = *opts.dimensions;
        for (int k = 0; k < 3; ++k) {
            if (d[k] <= 0) {
                throw voxel_error(
                    "voxelize_mesh: dimensions axis " + std::to_string(k) +
                    " is not positive, got " + std::to_string(d[k]));
            }
        }
    }
}

void validate_mesh(const mesh::Mesh& m) {
    if (m.positions.empty()) {
        throw voxel_error("voxelize_mesh: input mesh has no positions");
    }
    if (m.faces.empty()) {
        throw voxel_error("voxelize_mesh: input mesh has no faces");
    }
    for (std::size_t i = 0; i < m.positions.size(); ++i) {
        const auto& p = m.positions[i];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
            !std::isfinite(p[2])) {
            throw voxel_error(
                "voxelize_mesh: non-finite position coordinate at index " +
                std::to_string(i));
        }
    }
}

}  // anonymous namespace

// ===========================================================================
// Public API: default_options, VoxelGrid accessors, voxelize_mesh.
// ===========================================================================

VoxelizerOptions default_options() noexcept {
    VoxelizerOptions opts{};
    opts.voxel_size      = 1.0f;
    opts.axis_convention = AxisConvention::Y_UP;
    opts.dimensions      = std::nullopt;
    opts.origin_world    = std::nullopt;
    opts.disambiguation  = DisambiguationRule::CLOSEST_TO_CENTRE;
    opts.max_grid_cells  = MAX_GRID_CELLS_DEFAULT;
#ifdef OBJTOMC_WITH_CUDA
    // GPU dispatch preference — default AUTO. See voxel.hpp GpuPreference
    // + proposal §3.4. On CPU-only builds this field does not exist.
    opts.prefer_gpu      = GpuPreference::AUTO;
#endif
    return opts;
}

VoxelGrid::VoxelGrid()
    : dimensions_{0, 0, 0},
      origin_world_{0.0f, 0.0f, 0.0f},
      voxel_size_(0.0f),
      axis_convention_(AxisConvention::Y_UP) {}

std::array<int, 3>   VoxelGrid::dimensions()      const noexcept { return dimensions_; }
std::array<float, 3> VoxelGrid::origin_world()    const noexcept { return origin_world_; }
float                VoxelGrid::voxel_size()      const noexcept { return voxel_size_; }
AxisConvention       VoxelGrid::axis_convention() const noexcept { return axis_convention_; }

std::int32_t VoxelGrid::at(int x, int y, int z) const {
    if (x < 0 || x >= dimensions_[0] ||
        y < 0 || y >= dimensions_[1] ||
        z < 0 || z >= dimensions_[2]) {
        throw voxel_error(
            "VoxelGrid::at: index (" + std::to_string(x) + ", " +
            std::to_string(y) + ", " + std::to_string(z) +
            ") out of bounds for dimensions (" +
            std::to_string(dimensions_[0]) + ", " +
            std::to_string(dimensions_[1]) + ", " +
            std::to_string(dimensions_[2]) + ")");
    }
    return cells_[cell_index(x, y, z, dimensions_)];
}

std::int32_t VoxelGrid::at_unchecked(int x, int y, int z) const noexcept {
    return cells_[cell_index(x, y, z, dimensions_)];
}

bool VoxelGrid::is_occupied(int x, int y, int z) const {
    return at(x, y, z) != EMPTY;
}

std::size_t VoxelGrid::occupied_count() const {
    std::size_t n = 0;
    for (const std::int32_t v : cells_) {
        if (v != EMPTY) ++n;
    }
    return n;
}

std::span<const mesh::Material> VoxelGrid::materials() const noexcept {
    return std::span<const mesh::Material>(materials_.data(), materials_.size());
}

// ---------------------------------------------------------------------------
// detail::compute_grid_setup — validation + AABB + dims + origin + size cap.
//
// Extracted from the pre-Session-40 monolithic voxelize_mesh so both CPU
// (fill_cells_cpu) and GPU (src/voxel_gpu.cu fill_cells_gpu) paths consume
// the SAME grid frame, giving byte-equality of dims/origin by construction.
// See src/voxel_internal.hpp for the byte-equality rationale.
// ---------------------------------------------------------------------------
namespace detail {

GridSetup compute_grid_setup(const mesh::Mesh& mesh_in,
                             const VoxelizerOptions& opts) {
    // ---- 1. validation -------------------------------------------------
    validate_options(opts);
    validate_mesh(mesh_in);

    // ---- 2. AABB in voxel-space (post-axis-swap) ----------------------
    const Aabb aabb = compute_mesh_aabb(mesh_in, opts.axis_convention);

    // ---- 3. resolve grid dimensions + origin --------------------------
    Vec3 origin;
    std::array<int, 3> dims;

    if (opts.origin_world.has_value()) {
        origin = *opts.origin_world;
    } else {
        origin = aabb.min;
    }

    auto auto_dim_for = [&](int axis) -> int {
        const float extent = aabb.max[axis] - origin[axis];
        if (extent <= 0.0f) return 1;
        // ceil((extent) / voxel_size)
        const float n = std::ceil(extent / opts.voxel_size);
        const int   ni = (n < 1.0f) ? 1 : static_cast<int>(n);
        return ni;
    };

    if (opts.dimensions.has_value()) {
        dims = *opts.dimensions;
        // Validate that the supplied dimensions actually cover the mesh AABB
        // (proposal §3.4: explicit dims insufficient to cover AABB → throw).
        for (int k = 0; k < 3; ++k) {
            const float needed = aabb.max[k] - origin[k];
            const float supplied = static_cast<float>(dims[k]) * opts.voxel_size;
            // Allow a tiny float-precision epsilon (the voxelizer's AABB walk
            // clips to [0, dim-1] anyway; the throw is for clear under-sizing).
            const float eps = opts.voxel_size * 1e-4f;
            if (supplied + eps < needed) {
                throw voxel_error(
                    "voxelize_mesh: dimensions axis " + std::to_string(k) +
                    " (" + std::to_string(dims[k]) +
                    ") cannot cover mesh AABB extent (" +
                    std::to_string(needed) +
                    " OBJ-units; needs at least " +
                    std::to_string(static_cast<int>(std::ceil(needed / opts.voxel_size))) +
                    " voxels)");
            }
        }
    } else {
        dims = {auto_dim_for(0), auto_dim_for(1), auto_dim_for(2)};
    }

    // ---- 4. size cap (int64 accumulator for overflow safety) -----------
    const std::int64_t cells_total =
        static_cast<std::int64_t>(dims[0]) *
        static_cast<std::int64_t>(dims[1]) *
        static_cast<std::int64_t>(dims[2]);
    if (cells_total > opts.max_grid_cells) {
        throw voxel_error(
            "voxelize_mesh: grid size " + std::to_string(cells_total) +
            " cells exceeds configured maximum " +
            std::to_string(opts.max_grid_cells) + " cells");
    }

    return GridSetup{dims, origin, opts.voxel_size, opts.axis_convention};
}

// ---------------------------------------------------------------------------
// detail::fill_cells_cpu — the CPU voxelisation kernel body verbatim from
// pre-Session-40 voxelize_mesh. Runs the Akenine-Möller SAT + Eberly 1999
// closest-point disambiguation over every triangle, writing into cells
// (pre-sized to dims[0]*dims[1]*dims[2] and pre-filled with EMPTY).
//
// Note on material_id storage: mesh::Face::material_id is -1 when no
// material is assigned (consistent with mesh.hpp's documented convention).
// We store `max(face.material_id, 0)` in the voxel grid so that a
// no-material face still produces an occupied voxel (the alternative would
// alias with EMPTY = -1 and produce silently-empty output on meshes like
// the bunny fixture that have no MTL).
// ---------------------------------------------------------------------------
void fill_cells_cpu(const mesh::Mesh& mesh_in,
                    const GridSetup& setup,
                    std::vector<std::int32_t>& cells) {
    const auto& dims = setup.dims;
    const auto& origin = setup.origin;
    const float vs   = setup.voxel_size;
    const float half = 0.5f * vs;
    const Vec3 box_half{half, half, half};

    // Side-buffer of best (closest) distances per cell. Allocated for the
    // duration of the kernel only; freed on return. See proposal §3.8.
    std::vector<float> best_dist(cells.size(),
                                 std::numeric_limits<float>::infinity());

    for (std::size_t face_idx = 0; face_idx < mesh_in.faces.size(); ++face_idx) {
        const auto& face = mesh_in.faces[face_idx];
        const Vec3 v0 = apply_axis_swap(
            mesh_in.positions[face.position_indices[0]], setup.axis);
        const Vec3 v1 = apply_axis_swap(
            mesh_in.positions[face.position_indices[1]], setup.axis);
        const Vec3 v2 = apply_axis_swap(
            mesh_in.positions[face.position_indices[2]], setup.axis);

        // Triangle AABB in OBJ-units.
        Vec3 tmin = {std::min({v0[0], v1[0], v2[0]}),
                     std::min({v0[1], v1[1], v2[1]}),
                     std::min({v0[2], v1[2], v2[2]})};
        Vec3 tmax = {std::max({v0[0], v1[0], v2[0]}),
                     std::max({v0[1], v1[1], v2[1]}),
                     std::max({v0[2], v1[2], v2[2]})};

        // Map to voxel-index range. Voxel index k covers the half-open
        // OBJ-space range [origin + k*vs, origin + (k+1)*vs); we use floor
        // on both bounds since for any coord in voxel k, floor((coord-
        // origin)/vs) == k. The edge case: a coord that lands exactly on
        // the upper grid boundary (= origin + dim*vs) yields dim, which is
        // out of [0, dim-1]; std::clamp pulls it down to dim-1 so the
        // triangle's touch-overlap with voxel dim-1's upper face is still
        // tested (the SAT itself rejects non-overlap). Same symmetric
        // clamp on the lower end keeps the loop correct for triangles
        // that just touch the lower grid boundary.
        auto idx_clamp = [&](float v, int axis_dim, int axis) noexcept -> int {
            return std::clamp(
                static_cast<int>(std::floor((v - origin[axis]) / vs)),
                0, axis_dim - 1);
        };
        const int x_lo = idx_clamp(tmin[0], dims[0], 0);
        const int y_lo = idx_clamp(tmin[1], dims[1], 1);
        const int z_lo = idx_clamp(tmin[2], dims[2], 2);
        const int x_hi = idx_clamp(tmax[0], dims[0], 0);
        const int y_hi = idx_clamp(tmax[1], dims[1], 1);
        const int z_hi = idx_clamp(tmax[2], dims[2], 2);

        // The material_id stored in the grid. mesh::Face::material_id is
        // -1 when no material is assigned; clamp to 0 so the voxel
        // becomes occupied (see the "Note on material_id storage" above).
        const std::int32_t mid =
            (face.material_id < 0) ? std::int32_t{0} : face.material_id;

        // Iterate the triangle's voxel-space AABB.
        for (int y = y_lo; y <= y_hi; ++y) {
            for (int z = z_lo; z <= z_hi; ++z) {
                for (int x = x_lo; x <= x_hi; ++x) {
                    const Vec3 box_centre{
                        origin[0] + (static_cast<float>(x) + 0.5f) * vs,
                        origin[1] + (static_cast<float>(y) + 0.5f) * vs,
                        origin[2] + (static_cast<float>(z) + 0.5f) * vs};

                    if (!triangle_aabb_overlap(v0, v1, v2,
                                               box_centre, box_half)) {
                        continue;
                    }

                    // Disambiguation: closest-to-voxel-centre, tie-break
                    // by lower triangle index. The (d, triangle_index)
                    // comparison key is iteration-order-independent.
                    const Vec3 cp = closest_point_on_triangle(
                        box_centre, v0, v1, v2);
                    const float dist = v_len(v_sub(box_centre, cp));

                    const std::size_t idx = cell_index(x, y, z, dims);
                    const std::int32_t current = cells[idx];
                    if (current == EMPTY) {
                        cells[idx] = mid;
                        best_dist[idx] = dist;
                    } else if (dist < best_dist[idx]) {
                        cells[idx] = mid;
                        best_dist[idx] = dist;
                    }
                    // (dist == best_dist[idx]): equal-distance case.
                    // Tie-break by lower triangle index: since we iterate
                    // faces in ascending face_idx order and only overwrite
                    // on STRICT improvement, the lower-index triangle wins.
                }
            }
        }
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// voxelize_mesh — public API. Small dispatcher: shared setup → CPU or GPU
// cells-fill → VoxelGrid construction (via friend access to private fields).
//
// Dispatch rules (see research/gpu_port_proposal.md §3.4):
//   - CPU-only build (OBJTOMC_WITH_CUDA undefined): CPU always. This is
//     the ABI-identical path preserved for pre-Session-40 callers.
//   - GPU build + opts.prefer_gpu == OFF: CPU.
//   - GPU build + opts.prefer_gpu == ON:  GPU (throws if no device).
//   - GPU build + opts.prefer_gpu == AUTO (default):
//       GPU if total grid cells >= GPU_GRID_THRESHOLD (64^3 = 262 144);
//       CPU otherwise (kernel-launch overhead not amortised on small
//       grids).
// ---------------------------------------------------------------------------
#ifdef OBJTOMC_WITH_CUDA
// Grid-cell threshold above which AUTO dispatches to GPU. 64^3 = 262 144
// is calibrated against cuda_voxelizer's launch overhead (~1-5 ms per
// launch) vs the CPU cost of a 64^3 mesh AABB scan (~50 ms for a
// typical fixture). See proposal §3.4.
static constexpr std::int64_t GPU_GRID_THRESHOLD = 64LL * 64LL * 64LL;
#endif

VoxelGrid voxelize_mesh(const mesh::Mesh& mesh_in,
                        const VoxelizerOptions& opts) {
    const detail::GridSetup setup =
        detail::compute_grid_setup(mesh_in, opts);
    const std::int64_t cells_total =
        static_cast<std::int64_t>(setup.dims[0]) *
        static_cast<std::int64_t>(setup.dims[1]) *
        static_cast<std::int64_t>(setup.dims[2]);

    std::vector<std::int32_t> cells(
        static_cast<std::size_t>(cells_total), EMPTY);

    bool use_gpu = false;
#ifdef OBJTOMC_WITH_CUDA
    if (opts.prefer_gpu == GpuPreference::ON) {
        use_gpu = true;
    } else if (opts.prefer_gpu == GpuPreference::AUTO &&
               cells_total >= GPU_GRID_THRESHOLD) {
        use_gpu = true;
    }
    if (use_gpu) {
        detail::fill_cells_gpu(mesh_in, setup, cells);
    } else {
        detail::fill_cells_cpu(mesh_in, setup, cells);
    }
#else
    (void)use_gpu;
    detail::fill_cells_cpu(mesh_in, setup, cells);
#endif

    // Friend-access construction of the immutable VoxelGrid. voxelize_mesh
    // is the only function declared as friend of VoxelGrid; both CPU and
    // GPU cell-fill paths return via this single wrap point.
    VoxelGrid grid;
    grid.dimensions_      = setup.dims;
    grid.origin_world_    = setup.origin;
    grid.voxel_size_      = setup.voxel_size;
    grid.axis_convention_ = setup.axis;
    grid.cells_           = std::move(cells);
    grid.materials_       = mesh_in.materials;   // copy
    return grid;
}

}  // namespace objtomc::voxel
