// Semantic detection + replacement — v0.5 sub-items (c)/(d), Sessions 70–71.
// See include/objtomc/semantic.hpp for the contract.

#include "objtomc/semantic.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <unordered_map>
#include <vector>

namespace objtomc::semantic {

namespace {

// Pack a world coordinate triple into a 64-bit key (21 bits per axis,
// offset by 2^20 — comfortably covers the ±2M block range).
inline std::uint64_t pos_key(std::int32_t x, std::int32_t y, std::int32_t z) {
    const std::uint64_t bias = 1u << 20;
    return ((static_cast<std::uint64_t>(x) + bias) << 42) |
           ((static_cast<std::uint64_t>(y) + bias) << 21) |
           (static_cast<std::uint64_t>(z) + bias);
}
inline std::uint64_t xz_key(std::int32_t x, std::int32_t z) {
    const std::uint64_t bias = 1u << 20;
    return ((static_cast<std::uint64_t>(x) + bias) << 21) |
           (static_cast<std::uint64_t>(z) + bias);
}

inline bool is_green(const std::array<float, 3>& kd, float margin) {
    return kd[1] > kd[0] + margin && kd[1] > kd[2] + margin;
}

chunk::BlockState log_block() {
    return chunk::BlockState{"minecraft:oak_log", {}};
}
chunk::BlockState leaves_block() {
    // persistent=true so the canopy never random-tick-decays; unspecified
    // properties take vanilla defaults (same partial-property posture as
    // every matcher-emitted block since v0.1 — DECISIONS 2026-09-24).
    return chunk::BlockState{"minecraft:oak_leaves",
                             {{"persistent", "true"}}};
}

}  // namespace

// ===========================================================================
// (c) trees
// ===========================================================================
SemanticResult detect_and_replace_trees(
    std::span<const std::array<std::int32_t, 3>> positions,
    std::span<const std::array<float, 3>>        kd,
    const SemanticOptions&                       opts) {
    SemanticResult res;
    res.drop.assign(positions.size(), 0);

    // Index green voxels only — colour is what separates a canopy from
    // the trunk/ground it is welded to in the scan.
    std::unordered_map<std::uint64_t, std::size_t> green_at;
    green_at.reserve(positions.size() / 4 + 16);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (is_green(kd[i], opts.green_margin)) {
            green_at.emplace(
                pos_key(positions[i][0], positions[i][1], positions[i][2]), i);
        }
    }
    // Full-occupancy index for the trunk scan.
    std::unordered_map<std::uint64_t, std::size_t> any_at;
    any_at.reserve(positions.size() + 16);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        any_at.emplace(
            pos_key(positions[i][0], positions[i][1], positions[i][2]), i);
    }

    std::vector<char> visited(positions.size(), 0);
    std::vector<std::size_t> stack, comp;

    for (std::size_t seed = 0; seed < positions.size(); ++seed) {
        if (visited[seed] || !is_green(kd[seed], opts.green_margin)) continue;

        comp.clear();
        stack.assign(1, seed);
        visited[seed] = 1;
        std::array<std::int32_t, 3> lo = positions[seed];
        std::array<std::int32_t, 3> hi = positions[seed];
        while (!stack.empty()) {
            const std::size_t cur = stack.back();
            stack.pop_back();
            comp.push_back(cur);
            const auto& p = positions[cur];
            for (int k = 0; k < 3; ++k) {
                lo[static_cast<std::size_t>(k)] = std::min(
                    lo[static_cast<std::size_t>(k)],
                    p[static_cast<std::size_t>(k)]);
                hi[static_cast<std::size_t>(k)] = std::max(
                    hi[static_cast<std::size_t>(k)],
                    p[static_cast<std::size_t>(k)]);
            }
            for (int oy = -1; oy <= 1; ++oy)
                for (int oz = -1; oz <= 1; ++oz)
                    for (int ox = -1; ox <= 1; ++ox) {
                        if (!ox && !oy && !oz) continue;
                        const auto it = green_at.find(
                            pos_key(p[0] + ox, p[1] + oy, p[2] + oz));
                        if (it != green_at.end() && !visited[it->second]) {
                            visited[it->second] = 1;
                            stack.push_back(it->second);
                        }
                    }
        }

        if (comp.size() <
                static_cast<std::size_t>(opts.min_canopy_voxels) ||
            comp.size() >
                static_cast<std::size_t>(opts.max_canopy_voxels)) {
            continue;
        }

        // Accepted canopy: drop it, drop the 3x3 support column beneath
        // the centroid, then stamp a procedural tree.
        for (const std::size_t i : comp) res.drop[i] = 1;

        const std::int32_t cx = (lo[0] + hi[0]) / 2;
        const std::int32_t cz = (lo[2] + hi[2]) / 2;
        std::int32_t base_y = lo[1];
        for (std::int32_t y = lo[1] - 1; y > lo[1] - 48; --y) {
            bool layer_hit = false;
            for (int ox = -1; ox <= 1; ++ox)
                for (int oz = -1; oz <= 1; ++oz) {
                    const auto it = any_at.find(pos_key(cx + ox, y, cz + oz));
                    if (it == any_at.end()) continue;
                    if (is_green(kd[it->second], opts.green_margin)) continue;
                    res.drop[it->second] = 1;
                    layer_hit = true;
                }
            if (!layer_hit) break;
            base_y = y;
        }

        const int h  = std::clamp(hi[1] - base_y + 1, 5, 64);
        const int r  = std::clamp(h / 4, 2, 10);
        const int th = std::max(2, h - 2 * r);   // trunk height
        for (int y = 0; y < th; ++y) {
            res.stamps.push_back({{cx, base_y + y, cz}, log_block()});
        }
        const std::int32_t yc = base_y + th + r - 1;
        for (int dy = -r; dy <= r; ++dy)
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy + dz * dz > r * r) continue;
                    if (dx == 0 && dz == 0 && dy < 0) continue;  // trunk gap
                    res.stamps.push_back(
                        {{cx + dx, yc + dy, cz + dz}, leaves_block()});
                }
        ++res.detected;
    }
    return res;
}

// ===========================================================================
// (d) buildings
// ===========================================================================
SemanticResult detect_and_replace_buildings(
    std::span<const std::array<std::int32_t, 3>> positions,
    std::span<const std::array<float, 3>>        kd,
    const SemanticOptions&                       opts,
    const std::function<chunk::BlockState(const std::array<float, 3>&)>&
        wall_block_of) {
    SemanticResult res;
    res.drop.assign(positions.size(), 0);

    // Column tops. std::map keeps iteration deterministic without a
    // separate sort.
    std::map<std::uint64_t, std::int32_t> top_of;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        auto [it, fresh] = top_of.emplace(
            xz_key(positions[i][0], positions[i][2]), positions[i][1]);
        if (!fresh) it->second = std::max(it->second, positions[i][1]);
    }
    if (top_of.empty()) return res;

    // Ground estimate: modal top height (streets/terrain dominate area).
    std::map<std::int32_t, std::size_t> top_hist;
    for (const auto& [k, t] : top_of) ++top_hist[t];
    std::int32_t ground = top_of.begin()->second;
    std::size_t  ground_n = 0;
    for (const auto& [t, n] : top_hist) {
        if (n > ground_n) { ground_n = n; ground = t; }
    }

    // Building mask + 4-connected components over XZ.
    const std::int32_t min_top = ground + opts.min_building_height;
    std::unordered_map<std::uint64_t, char> visited;
    std::vector<std::pair<std::int32_t, std::int32_t>> stack, comp;

    // Per-voxel column membership index for drops/colour, built once.
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> col_voxels;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        col_voxels[xz_key(positions[i][0], positions[i][2])].push_back(i);
    }

    for (const auto& [key0, top0] : top_of) {
        if (top0 < min_top || visited.count(key0)) continue;
        // Unpack the seed key.
        const std::uint64_t bias = 1u << 20;
        const std::int32_t sx = static_cast<std::int32_t>((key0 >> 21) - bias);
        const std::int32_t sz =
            static_cast<std::int32_t>((key0 & ((1u << 21) - 1)) - bias);

        comp.clear();
        stack.assign(1, {sx, sz});
        visited[key0] = 1;
        while (!stack.empty()) {
            const auto [x, z] = stack.back();
            stack.pop_back();
            comp.push_back({x, z});
            static constexpr int O[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& o : O) {
                const std::uint64_t nk = xz_key(x + o[0], z + o[1]);
                if (visited.count(nk)) continue;
                const auto it = top_of.find(nk);
                if (it == top_of.end() || it->second < min_top) continue;
                visited[nk] = 1;
                stack.push_back({x + o[0], z + o[1]});
            }
        }
        if (comp.size() <
            static_cast<std::size_t>(opts.min_footprint_cells)) continue;

        // Deterministic order for all subsequent passes.
        std::sort(comp.begin(), comp.end());

        // Median top -> regenerated height; average kd -> wall block.
        std::vector<std::int32_t> tops;
        tops.reserve(comp.size());
        double kr = 0, kg = 0, kb = 0;
        std::size_t kn = 0;
        for (const auto& [x, z] : comp) {
            tops.push_back(top_of.at(xz_key(x, z)));
            for (const std::size_t i : col_voxels[xz_key(x, z)]) {
                if (positions[i][1] > ground) {
                    kr += kd[i][0]; kg += kd[i][1]; kb += kd[i][2]; ++kn;
                    res.drop[i] = 1;   // original above-ground scan removed
                }
            }
        }
        std::sort(tops.begin(), tops.end());
        const std::int32_t H = tops[tops.size() / 2];
        const double dkn = static_cast<double>(kn);
        const std::array<float, 3> avg_kd{
            static_cast<float>(kn ? kr / dkn : 0.5),
            static_cast<float>(kn ? kg / dkn : 0.5),
            static_cast<float>(kn ? kb / dkn : 0.5)};
        const chunk::BlockState wall  = wall_block_of(avg_kd);
        const chunk::BlockState glass{"minecraft:glass", {}};

        // Membership set for boundary tests.
        std::unordered_map<std::uint64_t, char> in_comp;
        for (const auto& [x, z] : comp) in_comp[xz_key(x, z)] = 1;

        for (const auto& [x, z] : comp) {
            bool boundary = false;
            static constexpr int O[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            for (const auto& o : O) {
                if (!in_comp.count(xz_key(x + o[0], z + o[1]))) {
                    boundary = true;
                    break;
                }
            }
            if (boundary) {
                for (std::int32_t y = ground + 1; y <= H; ++y) {
                    const bool window =
                        y > ground + 1 && y < H &&
                        ((y - ground) % opts.window_row_period) == 0;
                    res.stamps.push_back({{x, y, z}, window ? glass : wall});
                }
            } else {
                res.stamps.push_back({{x, H, z}, wall});   // roof
            }
        }
        ++res.detected;
    }
    return res;
}

}  // namespace objtomc::semantic
