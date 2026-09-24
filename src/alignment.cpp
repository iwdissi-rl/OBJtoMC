// Street-grid auto-alignment — v0.5 sub-item (b), Session 69.
// See include/objtomc/alignment.hpp for the contract.

#include "objtomc/alignment.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace objtomc::alignment {

namespace {

struct V3 { float x, y, z; };

inline V3 sub(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline V3 cross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

float detect_grid_angle(const mesh::Mesh& m) {
    // 0.5-degree bins over the folded [0, 90) range.
    constexpr int kBins = 180;
    std::vector<double> hist(kBins, 0.0);
    std::vector<double> hist_wa(kBins, 0.0);   // w * angle, for sub-bin refinement

    for (const auto& face : m.faces) {
        const auto& p0 = m.positions[face.position_indices[0]];
        const auto& p1 = m.positions[face.position_indices[1]];
        const auto& p2 = m.positions[face.position_indices[2]];
        const V3 n = cross(sub(p1, p0), sub(p2, p0));   // |n| = 2 * area

        // Horizontal component of the (unnormalised) normal. Vertical
        // faces (facades) have large horizontal normals; roofs/roads have
        // nearly none and contribute ~nothing.
        const float hx = n.x, hz = n.z;
        const float hmag2 = hx * hx + hz * hz;
        if (hmag2 <= 0.0f) continue;
        const float nmag2 = hmag2 + n.y * n.y;
        // Weight = facade-ness x face area: |n_horiz|^2 / |n| combines
        // both without extra square roots beyond one.
        const double w = static_cast<double>(hmag2) /
                         std::sqrt(static_cast<double>(nmag2));

        // Angle of the horizontal normal, folded modulo 90 degrees (a
        // facade normal at t and the perpendicular street at t+90 vote
        // for the same grid).
        float deg = std::atan2(hz, hx) * (180.0f / kPi);
        deg = std::fmod(deg, 90.0f);
        if (deg < 0.0f) deg += 90.0f;
        const int bin = std::min(kBins - 1,
                                 static_cast<int>(deg * (kBins / 90.0f)));
        hist[static_cast<std::size_t>(bin)]    += w;
        hist_wa[static_cast<std::size_t>(bin)] += w * static_cast<double>(deg);
    }

    // Peak bin.
    int best = 0;
    double best_w = 0.0;
    for (int b = 0; b < kBins; ++b) {
        if (hist[static_cast<std::size_t>(b)] > best_w) {
            best_w = hist[static_cast<std::size_t>(b)];
            best = b;
        }
    }
    if (best_w <= 0.0) return 0.0f;

    // Refine: weighted mean of the ACTUAL angles over the peak +/- 4 bins
    // (sub-bin precision — bin-centre refinement leaves up to 0.25 deg of
    // quantisation, enough to re-introduce one stair line per ~200-cell
    // facade). Wrapped bins unwrap by +/-90 relative to the peak so the
    // mean does not straddle the fold.
    double sum_w = 0.0, sum_wd = 0.0;
    for (int off = -4; off <= 4; ++off) {
        const int raw = best + off;
        const int b = ((raw % kBins) + kBins) % kBins;
        const double w = hist[static_cast<std::size_t>(b)];
        double wa = hist_wa[static_cast<std::size_t>(b)];
        if (raw < 0)       wa -= 90.0 * w;
        if (raw >= kBins)  wa += 90.0 * w;
        sum_w  += w;
        sum_wd += wa;
    }
    double theta = sum_wd / sum_w;         // dominant direction in [0, 90)
    theta = std::fmod(theta + 90.0, 90.0);

    // Correction: rotate_mesh_y(c) maps a direction at angle a to a - c,
    // so axis-alignment needs c ≡ a (mod 90) — i.e. the folded dominant
    // angle itself, re-folded to the minimal rotation in (-45, 45].
    double corr = theta;
    if (corr > 45.0) corr -= 90.0;
    return static_cast<float>(corr);
}

void rotate_mesh_y(mesh::Mesh& m, float degrees) {
    if (degrees == 0.0f || m.positions.empty()) return;

    std::array<float, 3> lo{std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::max()};
    std::array<float, 3> hi{std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest(),
                            std::numeric_limits<float>::lowest()};
    for (const auto& p : m.positions) {
        for (int k = 0; k < 3; ++k) {
            lo[static_cast<std::size_t>(k)] =
                std::min(lo[static_cast<std::size_t>(k)],
                         p[static_cast<std::size_t>(k)]);
            hi[static_cast<std::size_t>(k)] =
                std::max(hi[static_cast<std::size_t>(k)],
                         p[static_cast<std::size_t>(k)]);
        }
    }
    const float cx = 0.5f * (lo[0] + hi[0]);
    const float cz = 0.5f * (lo[2] + hi[2]);

    const float rad = degrees * (kPi / 180.0f);
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    for (auto& p : m.positions) {
        const float dx = p[0] - cx;
        const float dz = p[2] - cz;
        // Rotation about +Y (right-handed, positive CCW looking down -Y):
        // x' =  c*dx + s*dz ; z' = -s*dx + c*dz
        p[0] = cx + c * dx + s * dz;
        p[2] = cz - s * dx + c * dz;
    }
}

}  // namespace objtomc::alignment
