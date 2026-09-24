// Deterministic voxel cleanup — v0.5 sub-item (a), Session 67.
// See include/objtomc/cleanup.hpp for the contract.

#include "objtomc/cleanup.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace objtomc::cleanup {

namespace {

constexpr std::int32_t EMPTY = -1;

inline std::size_t idx_of(int x, int y, int z, const std::array<int, 3>& d) {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(d[0]) *
               (static_cast<std::size_t>(z) +
                static_cast<std::size_t>(d[2]) * static_cast<std::size_t>(y));
}

// ---------------------------------------------------------------------------
// Pinhole fill. Reads the pre-pass state via a snapshot of `cells`;
// collects fills, then applies — no intra-pass cascading.
// ---------------------------------------------------------------------------
std::size_t pinhole_fill(std::vector<std::int32_t>& cells,
                         std::vector<std::int32_t>& tris,
                         const std::array<int, 3>& d,
                         int min_neighbors) {
    const std::vector<std::int32_t> before = cells;   // snapshot

    struct Fill { std::size_t at; std::int32_t mid; std::int32_t tri; };
    std::vector<Fill> fills;

    static constexpr int OFF[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
                                      {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};
    for (int y = 0; y < d[1]; ++y) {
        for (int z = 0; z < d[2]; ++z) {
            for (int x = 0; x < d[0]; ++x) {
                const std::size_t i = idx_of(x, y, z, d);
                if (before[i] != EMPTY) continue;

                // Collect occupied face-neighbours (pre-pass state).
                std::pair<std::int32_t, std::int32_t> nb[6];
                int n = 0;
                for (const auto& o : OFF) {
                    const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
                    if (nx < 0 || ny < 0 || nz < 0 ||
                        nx >= d[0] || ny >= d[1] || nz >= d[2]) continue;
                    const std::size_t ni = idx_of(nx, ny, nz, d);
                    if (before[ni] != EMPTY) {
                        nb[n++] = {before[ni], tris[ni]};
                    }
                }
                if (n < min_neighbors) continue;

                // Most-common (material, tri) pair; ties -> lexicographically
                // smallest pair. n <= 6, so the quadratic count is trivial.
                std::sort(nb, nb + n);
                std::pair<std::int32_t, std::int32_t> best = nb[0];
                int best_count = 0;
                for (int a = 0; a < n;) {
                    int b = a;
                    while (b < n && nb[b] == nb[a]) ++b;
                    if (b - a > best_count) {   // strict: first (smallest) wins ties
                        best_count = b - a;
                        best = nb[a];
                    }
                    a = b;
                }
                fills.push_back({i, best.first, best.second});
            }
        }
    }

    for (const Fill& f : fills) {
        cells[f.at] = f.mid;
        tris[f.at]  = f.tri;
    }
    return fills.size();
}

// ---------------------------------------------------------------------------
// Crumb removal. Iterative flood fill over 26-connectivity; components of
// <= max_voxels occupied cells are cleared. `visited` doubles as the
// membership set for the component being grown.
// ---------------------------------------------------------------------------
std::size_t remove_crumbs(std::vector<std::int32_t>& cells,
                          std::vector<std::int32_t>& tris,
                          const std::array<int, 3>& d,
                          int max_voxels) {
    std::vector<std::uint8_t> visited(cells.size(), 0);
    std::vector<std::size_t>  stack;
    std::vector<std::size_t>  component;
    std::size_t removed = 0;

    for (int y = 0; y < d[1]; ++y) {
        for (int z = 0; z < d[2]; ++z) {
            for (int x = 0; x < d[0]; ++x) {
                const std::size_t seed = idx_of(x, y, z, d);
                if (cells[seed] == EMPTY || visited[seed]) continue;

                component.clear();
                stack.assign(1, seed);
                visited[seed] = 1;
                bool small_enough = true;
                while (!stack.empty()) {
                    const std::size_t cur = stack.back();
                    stack.pop_back();
                    component.push_back(cur);
                    if (small_enough &&
                        component.size() >
                            static_cast<std::size_t>(max_voxels)) {
                        small_enough = false;
                        // Keep flooding to mark the whole component visited,
                        // but we already know it survives.
                    }
                    const int cy = static_cast<int>(
                        cur / (static_cast<std::size_t>(d[0]) *
                               static_cast<std::size_t>(d[2])));
                    const std::size_t rem = cur - static_cast<std::size_t>(cy) *
                        static_cast<std::size_t>(d[0]) *
                        static_cast<std::size_t>(d[2]);
                    const int cz = static_cast<int>(
                        rem / static_cast<std::size_t>(d[0]));
                    const int cx = static_cast<int>(
                        rem % static_cast<std::size_t>(d[0]));
                    for (int oy = -1; oy <= 1; ++oy) {
                        for (int oz = -1; oz <= 1; ++oz) {
                            for (int ox = -1; ox <= 1; ++ox) {
                                if (ox == 0 && oy == 0 && oz == 0) continue;
                                const int nx = cx + ox, ny = cy + oy,
                                          nz = cz + oz;
                                if (nx < 0 || ny < 0 || nz < 0 ||
                                    nx >= d[0] || ny >= d[1] ||
                                    nz >= d[2]) continue;
                                const std::size_t ni = idx_of(nx, ny, nz, d);
                                if (cells[ni] != EMPTY && !visited[ni]) {
                                    visited[ni] = 1;
                                    stack.push_back(ni);
                                }
                            }
                        }
                    }
                }
                if (small_enough) {
                    for (const std::size_t i : component) {
                        cells[i] = EMPTY;
                        tris[i]  = EMPTY;
                    }
                    removed += component.size();
                }
            }
        }
    }
    return removed;
}

}  // namespace

CleanupStats apply_cleanup(std::vector<std::int32_t>& cells,
                           std::vector<std::int32_t>& tris,
                           const std::array<int, 3>& dims,
                           const CleanupOptions& opts) {
    if (opts.fill_min_neighbors < 1 || opts.fill_min_neighbors > 6) {
        throw std::invalid_argument(
            "cleanup: fill_min_neighbors must be in [1, 6], got " +
            std::to_string(opts.fill_min_neighbors));
    }
    if (opts.crumb_max_voxels < 0) {
        throw std::invalid_argument(
            "cleanup: crumb_max_voxels must be >= 0, got " +
            std::to_string(opts.crumb_max_voxels));
    }
    const std::size_t n = static_cast<std::size_t>(dims[0]) *
                          static_cast<std::size_t>(dims[1]) *
                          static_cast<std::size_t>(dims[2]);
    if (cells.size() != n || tris.size() != n) {
        throw std::invalid_argument("cleanup: cells/tris size mismatch");
    }

    CleanupStats stats;
    if (!opts.enabled) return stats;
    stats.filled = pinhole_fill(cells, tris, dims, opts.fill_min_neighbors);
    if (opts.crumb_max_voxels > 0) {
        stats.removed = remove_crumbs(cells, tris, dims,
                                      opts.crumb_max_voxels);
    }
    return stats;
}

}  // namespace objtomc::cleanup
