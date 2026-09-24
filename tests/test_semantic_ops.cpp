// semantic_ops — v0.5 sub-items (c)/(d) unit gates (Sessions 70–71).
//
// Trees:  a 5x5x5 green canopy welded onto a brown trunk + ground plane
//         is detected exactly once; the canopy (125), trunk (5) and the
//         3x3 ground patch under it (9) are dropped (139 total); the
//         stamped replacement is an oak_log column from the detected
//         ground base plus a persistent oak_leaves canopy. A red blob of
//         the same shape detects nothing.
// Buildings: a 6x6 footprint of 20-tall gray columns on a 30x30 ground
//         plane regenerates as perimeter walls with window rows every
//         4th storey, hollow interior, flat roof at the median height;
//         all 720 above-ground scan voxels drop.
// Both:   deterministic double-run.

#include "objtomc/semantic.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <string>
#include <vector>

namespace sem = objtomc::semantic;
using Pos = std::array<std::int32_t, 3>;
using Kd  = std::array<float, 3>;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

struct Scene {
    std::vector<Pos> pos;
    std::vector<Kd>  kd;
    void add(std::int32_t x, std::int32_t y, std::int32_t z, Kd c) {
        pos.push_back({x, y, z});
        kd.push_back(c);
    }
};

constexpr Kd kGreen{0.20f, 0.60f, 0.20f};
constexpr Kd kBrown{0.40f, 0.25f, 0.10f};
constexpr Kd kRed{0.60f, 0.20f, 0.20f};
constexpr Kd kGray{0.50f, 0.50f, 0.50f};

Scene tree_scene(Kd canopy_colour) {
    Scene s;
    for (int x = 0; x < 20; ++x)                      // ground plane y=0
        for (int z = 0; z < 20; ++z) s.add(x, 0, z, kBrown);
    for (int y = 1; y <= 5; ++y) s.add(10, y, 10, kBrown);   // trunk
    for (int x = 8; x <= 12; ++x)                     // canopy 5x5x5
        for (int y = 6; y <= 10; ++y)
            for (int z = 8; z <= 12; ++z) s.add(x, y, z, canopy_colour);
    return s;
}

std::size_t count_drops(const sem::SemanticResult& r) {
    std::size_t n = 0;
    for (const char c : r.drop) n += (c != 0);
    return n;
}

}  // namespace

int main() {
    sem::SemanticOptions o;
    o.trees = true;

    // --- trees: green canopy detected, replaced ------------------------------
    {
        const Scene s = tree_scene(kGreen);
        const auto r = sem::detect_and_replace_trees(s.pos, s.kd, o);
        if (r.detected != 1) {
            fail("trees: detected " + std::to_string(r.detected) + ", want 1");
        }
        if (count_drops(r) != 125 + 5 + 9) {
            fail("trees: dropped " + std::to_string(count_drops(r)) +
                 ", want 139 (canopy+trunk+ground patch)");
        }
        bool base_log = false, persistent_leaves = false;
        for (const auto& [p, b] : r.stamps) {
            if (b.name == "minecraft:oak_log" && p == Pos{10, 0, 10}) {
                base_log = true;
            }
            if (b.name == "minecraft:oak_leaves") {
                const auto it = b.properties.find("persistent");
                if (it != b.properties.end() && it->second == "true") {
                    persistent_leaves = true;
                }
            }
        }
        if (!base_log) fail("trees: no oak_log at the detected ground base");
        if (!persistent_leaves) fail("trees: no persistent oak_leaves");
        std::cout << "PASS: tree detected/replaced (139 dropped, "
                  << r.stamps.size() << " stamped)\n";
    }

    // --- trees: red blob ignored ---------------------------------------------
    {
        const Scene s = tree_scene(kRed);
        const auto r = sem::detect_and_replace_trees(s.pos, s.kd, o);
        if (r.detected != 0 || count_drops(r) != 0 || !r.stamps.empty()) {
            fail("trees: red canopy wrongly detected");
        }
        std::cout << "PASS: non-green blob ignored\n";
    }

    // --- buildings ------------------------------------------------------------
    {
        Scene s;
        for (int x = 0; x < 30; ++x)
            for (int z = 0; z < 30; ++z) s.add(x, 0, z, kBrown);
        for (int x = 6; x <= 11; ++x)
            for (int z = 6; z <= 11; ++z)
                for (int y = 1; y <= 20; ++y) s.add(x, y, z, kGray);

        sem::SemanticOptions ob;
        ob.buildings = true;
        const objtomc::chunk::BlockState stone{"minecraft:stone", {}};
        const std::function<objtomc::chunk::BlockState(const Kd&)>
            wall_of = [&](const Kd&) { return stone; };
        const auto r = sem::detect_and_replace_buildings(
            s.pos, s.kd, ob, wall_of);

        if (r.detected != 1) {
            fail("buildings: detected " + std::to_string(r.detected));
        }
        if (count_drops(r) != 36u * 20u) {
            fail("buildings: dropped " + std::to_string(count_drops(r)) +
                 ", want 720");
        }
        // 20 boundary cells x 20 storeys + 16 interior roofs = 416.
        if (r.stamps.size() != 416) {
            fail("buildings: stamped " + std::to_string(r.stamps.size()) +
                 ", want 416");
        }
        bool glass_row = false, roof = false, wall = false;
        for (const auto& [p, b] : r.stamps) {
            if (b.name == "minecraft:glass" && p[1] == 4) glass_row = true;
            if (b.name == "minecraft:stone" && p == Pos{8, 20, 8}) roof = true;
            if (b.name == "minecraft:stone" && p == Pos{6, 2, 6}) wall = true;
        }
        if (!glass_row) fail("buildings: no window row at y=4");
        if (!roof)      fail("buildings: no interior roof cell at (8,20,8)");
        if (!wall)      fail("buildings: no wall at corner (6,2,6)");
        std::cout << "PASS: building regenerated (720 dropped, 416 stamped, "
                     "windows+roof+walls verified)\n";
    }

    // --- determinism -----------------------------------------------------------
    {
        const Scene s = tree_scene(kGreen);
        const auto a = sem::detect_and_replace_trees(s.pos, s.kd, o);
        const auto b = sem::detect_and_replace_trees(s.pos, s.kd, o);
        if (a.drop != b.drop || a.stamps.size() != b.stamps.size()) {
            fail("non-deterministic");
        }
        for (std::size_t i = 0; i < a.stamps.size(); ++i) {
            if (a.stamps[i].first != b.stamps[i].first ||
                !(a.stamps[i].second == b.stamps[i].second)) {
                fail("non-deterministic stamp order");
            }
        }
        std::cout << "PASS: determinism\n";
    }

    std::cout << "PASS: semantic_ops\n";
    return 0;
}
