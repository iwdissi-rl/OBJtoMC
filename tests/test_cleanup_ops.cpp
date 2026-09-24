// cleanup_ops — v0.5 sub-item (a) unit gates (Session 67, proposal §2).
//
// Synthetic grids with hand-computed expected outputs:
//   1. crumb threshold is exact: a 16-voxel component dies at N=16, a
//      17-voxel component survives.
//   2. pinhole fill fires at K occupied face-neighbours, not K-1, and
//      copies the most-common neighbour's (material, tri).
//   3. tie-break: equal counts -> lexicographically smallest (mat, tri).
//   4. no intra-pass cascade: two adjacent pinholes that would only reach
//      K neighbours AFTER each other's fill stay empty.
//   5. determinism: same input twice -> byte-identical output.
//   6. disabled options are a no-op; invalid options throw.

#include "objtomc/cleanup.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cln = objtomc::cleanup;

namespace {

constexpr std::int32_t EMPTY = -1;
constexpr std::array<int, 3> D{8, 8, 8};

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::size_t at(int x, int y, int z) {
    return static_cast<std::size_t>(x) +
           8u * (static_cast<std::size_t>(z) + 8u * static_cast<std::size_t>(y));
}

struct Grid {
    std::vector<std::int32_t> cells = std::vector<std::int32_t>(512, EMPTY);
    std::vector<std::int32_t> tris  = std::vector<std::int32_t>(512, EMPTY);
    void set(int x, int y, int z, std::int32_t mid, std::int32_t tri) {
        cells[at(x, y, z)] = mid;
        tris[at(x, y, z)]  = tri;
    }
};

std::size_t count_occupied(const Grid& g) {
    std::size_t n = 0;
    for (const auto c : g.cells) n += (c != EMPTY);
    return n;
}

}  // namespace

int main() {
    // --- 1. crumb threshold exactness --------------------------------------
    {
        cln::CleanupOptions o;
        o.enabled = true;
        o.fill_min_neighbors = 6;   // effectively disable fill (needs all 6)
        o.crumb_max_voxels = 16;

        Grid g16;   // 4x2x2 = 16 voxels
        for (int x = 0; x < 4; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) g16.set(x, y, z, 0, 1);
        const auto s16 = cln::apply_cleanup(g16.cells, g16.tris, D, o);
        if (s16.removed != 16 || count_occupied(g16) != 0) {
            fail("16-voxel crumb not fully removed at N=16");
        }

        Grid g17 = Grid{};   // same box + one extra 26-connected voxel
        for (int x = 0; x < 4; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) g17.set(x, y, z, 0, 1);
        g17.set(4, 2, 2, 0, 1);   // diagonal touch of (3,1,1) -> 26-connected
        const auto s17 = cln::apply_cleanup(g17.cells, g17.tris, D, o);
        if (s17.removed != 0 || count_occupied(g17) != 17) {
            fail("17-voxel component wrongly removed (26-connectivity or "
                 "threshold broken)");
        }
        std::cout << "PASS: crumb threshold exact at N=16 (16 dies, 17 lives)\n";
    }

    // --- 2 + 3. pinhole fill count + majority + tie-break -------------------
    {
        cln::CleanupOptions o;
        o.enabled = true;
        o.fill_min_neighbors = 5;
        o.crumb_max_voxels = 0;     // isolate the fill op

        // Centre (4,4,4) empty; 5 neighbours: 2x(1,3), 2x(2,7), 1x(0,9).
        Grid g;
        g.set(3, 4, 4, 1, 3);
        g.set(5, 4, 4, 1, 3);
        g.set(4, 3, 4, 2, 7);
        g.set(4, 5, 4, 2, 7);
        g.set(4, 4, 3, 0, 9);
        const auto s = cln::apply_cleanup(g.cells, g.tris, D, o);
        if (s.filled != 1) fail("5-neighbour pinhole not filled at K=5");
        if (g.cells[at(4, 4, 4)] != 1 || g.tris[at(4, 4, 4)] != 3) {
            fail("tie-break wrong: got (" +
                 std::to_string(g.cells[at(4, 4, 4)]) + ", " +
                 std::to_string(g.tris[at(4, 4, 4)]) +
                 "), want smallest tied pair (1, 3)");
        }

        // Same but only 4 neighbours -> no fill.
        Grid g4;
        g4.set(3, 4, 4, 1, 3);
        g4.set(5, 4, 4, 1, 3);
        g4.set(4, 3, 4, 2, 7);
        g4.set(4, 5, 4, 2, 7);
        const auto s4 = cln::apply_cleanup(g4.cells, g4.tris, D, o);
        if (s4.filled != 0) fail("4-neighbour cell filled at K=5");
        std::cout << "PASS: pinhole fill K-exact + smallest-pair tie-break\n";
    }

    // --- 4. no intra-pass cascade -------------------------------------------
    {
        cln::CleanupOptions o;
        o.enabled = true;
        o.fill_min_neighbors = 5;
        o.crumb_max_voxels = 0;

        // Two adjacent empties at (4,4,4) and (5,4,4), each with exactly 4
        // occupied neighbours; if fills cascaded, each would see 5.
        Grid g;
        for (const int cx : {4, 5}) {
            g.set(cx, 3, 4, 3, 2);
            g.set(cx, 5, 4, 3, 2);
            g.set(cx, 4, 3, 3, 2);
            g.set(cx, 4, 5, 3, 2);
        }
        const auto s = cln::apply_cleanup(g.cells, g.tris, D, o);
        if (s.filled != 0) {
            fail("intra-pass cascade: adjacent 4-neighbour pinholes filled "
                 "each other");
        }
        std::cout << "PASS: fills judged against pre-pass state (no cascade)\n";
    }

    // --- 5. determinism ------------------------------------------------------
    {
        cln::CleanupOptions o;
        o.enabled = true;

        Grid a, b;
        for (int i = 0; i < 8; ++i) {
            a.set(i, i % 3, (i * 5) % 8, i % 4, i);
            b.set(i, i % 3, (i * 5) % 8, i % 4, i);
        }
        cln::apply_cleanup(a.cells, a.tris, D, o);
        cln::apply_cleanup(b.cells, b.tris, D, o);
        if (a.cells != b.cells || a.tris != b.tris) fail("non-deterministic");
        std::cout << "PASS: determinism double-run\n";
    }

    // --- 6. disabled no-op + option validation -------------------------------
    {
        Grid g;
        g.set(0, 0, 0, 0, 0);
        const Grid before = g;
        cln::CleanupOptions off;   // enabled = false
        const auto s = cln::apply_cleanup(g.cells, g.tris, D, off);
        if (s.filled != 0 || s.removed != 0 || g.cells != before.cells) {
            fail("disabled cleanup changed the grid");
        }
        const auto expect_throw = [&](cln::CleanupOptions bad,
                                      const std::string& what) {
            bad.enabled = true;
            Grid t;
            try {
                cln::apply_cleanup(t.cells, t.tris, D, bad);
            } catch (const std::invalid_argument&) { return; }
            fail(what + " not rejected");
        };
        cln::CleanupOptions bad_k;  bad_k.fill_min_neighbors = 0;
        cln::CleanupOptions bad_k7; bad_k7.fill_min_neighbors = 7;
        cln::CleanupOptions bad_n;  bad_n.crumb_max_voxels = -1;
        expect_throw(bad_k,  "fill_min_neighbors 0");
        expect_throw(bad_k7, "fill_min_neighbors 7");
        expect_throw(bad_n,  "crumb_max_voxels -1");
        std::cout << "PASS: disabled no-op + option validation\n";
    }

    std::cout << "PASS: cleanup_ops\n";
    return 0;
}
