// orchestrate_sponza_scale — scale Tier-4 proposal §3.10.2.
//
// Full-pipeline sanity: run orchestrate() on the vendored Crytek Sponza
// fixture (270 788 triangles, 25 materials, uniform Kd=(1,1,1)) with the
// §3.3 parameters and verify:
//
//   - world_dir/level.dat exists and is at least 200 bytes.
//   - world_dir/region/ contains EXACTLY the 4 expected .mca files:
//       r.-1.-1.mca, r.-1.0.mca, r.0.-1.mca, r.0.0.mca
//     (no extras, no missing — the §3.9 arithmetic prediction, verbatim).
//   - Each .mca is 4096-aligned and ≥ 8192 bytes (empty-region floor).
//   - Each of the 4 regions has ≥ 1 non-zero location-table slot (proves
//     the mesh populates every region, not just some subset).
//   - Total non-zero location-table slots across the 4 regions ≥ 100
//     (weak lower bound per proposal §3.10.2; the mesh should populate
//     significantly more than 100 out of the 160-chunk expected total).
//
// Runtime: this test loads Sponza and runs the full mesh→voxel→matcher→
// chunk→anvil pipeline. Expected wall-clock 15-90 s depending on host per
// §1.7; NO ctest-level timeout is imposed (host-dependent). The end-to-end
// wall-clock is printed to stdout so future runs can spot regressions.
//
// The direct occupied-voxel count assertion (§3.7 condition v, band
// [100 000, 400 000]) is NOT computed here to avoid doubling the wall-clock
// with a separate voxelize() call — the direct count is verified by the
// Tier-4 helper's verbose stderr (proposal §3.11) which the maintainer reads
// before launching MC. This ctest's populated-chunk-slot lower bound is
// the CI proxy for that check.
//
// NO MC client check — that's the Session 37 maintainer-run Tier-4 manual
// gate. NO level.dat field assertions — that's orchestrate_level_dat_
// oracle's job.

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Count the number of non-zero (populated) slots in an .mca file's
// 1024-entry, 4-byte-per-slot location table (the first 4096 bytes of the
// region file). Same pattern as test_orchestrate_cornell_box.
std::size_t count_populated_slots(const fs::path& mca_path) {
    std::ifstream in(mca_path, std::ios::binary);
    if (!in) fail("could not open region file: " + mca_path.string());
    std::vector<unsigned char> location_table(4096);
    in.read(reinterpret_cast<char*>(location_table.data()), 4096);
    if (!in) fail("could not read 4096-byte location table from: " +
                  mca_path.string());
    std::size_t count = 0;
    for (std::size_t i = 0; i < 1024; ++i) {
        const auto* p = location_table.data() + 4 * i;
        const std::uint32_t loc =
            (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8)  |
            (static_cast<std::uint32_t>(p[3]));
        if (loc != 0) ++count;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-sponza.obj>\n";
        return 2;
    }
    const fs::path obj_path = argv[1];

    const auto base = fs::temp_directory_path() /
        ("objtomc_sponza_" + std::to_string(std::rand()));
    fs::remove_all(base);
    const auto world_dir = base / "world";

    // Proposal §3.3 + §3.4 orchestration parameters. Session 36 prompt
    // specifies level_name="objtomc-sponza-scale" (differs from proposal
    // §3.3's "objtomc-sponza-tier4" — prompt wins per session guidance).
    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size   = 14.531f;                   // 3720.854 (X extent) / 256 voxels
    opts.world_origin = {-128, -60, -80};          // centered shift → 4 regions
    opts.spawn_pos    = {0, -30, 0};               // interior atrium, chunk (0, 0)
    opts.level_name   = "objtomc-sponza-scale";
    opts.now_unix_s   = 0;                         // pin for CI determinism

    const auto t_start = std::chrono::steady_clock::now();
    try {
        orch::orchestrate(obj_path, world_dir, opts);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }
    const auto t_end = std::chrono::steady_clock::now();
    const auto wall_clock_s =
        std::chrono::duration<double>(t_end - t_start).count();

    // ---- level.dat structural sanity ----
    const auto level_dat_path = world_dir / "level.dat";
    if (!fs::exists(level_dat_path)) fail("level.dat missing");
    const auto level_dat_size = fs::file_size(level_dat_path);
    if (level_dat_size < 200) {
        fail("level.dat suspiciously small: " + std::to_string(level_dat_size));
    }

    // ---- Region-directory structural sanity ----
    const auto region_dir = world_dir / "region";
    if (!fs::is_directory(region_dir)) fail("region/ subdir missing");

    // Collect every .mca filename actually present.
    std::set<std::string> present;
    for (const auto& entry : fs::directory_iterator(region_dir)) {
        if (entry.path().extension() == ".mca") {
            present.insert(entry.path().filename().string());
        }
    }

    // Expected set per §3.9 arithmetic prediction: (rx, rz) ∈ {-1, 0}² → 4.
    const std::set<std::string> expected = {
        "r.-1.-1.mca",
        "r.-1.0.mca",
        "r.0.-1.mca",
        "r.0.0.mca",
    };

    // Exact set equality (no extras, no missing).
    if (present != expected) {
        std::string got = "{";
        for (auto it = present.begin(); it != present.end(); ++it) {
            if (it != present.begin()) got += ", ";
            got += *it;
        }
        got += "}";
        std::string want = "{";
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (it != expected.begin()) want += ", ";
            want += *it;
        }
        want += "}";
        fail("region file set mismatch. got=" + got + " want=" + want +
             " — either the coord transform diverged from §3.9 methodology "
             "OR the mesh AABB / voxel_size differ from proposal §1.7. "
             "STOP; investigate. Per §3.12 (C): count>4 → wrong voxel_size, "
             "count<4 → Y-range bug or empty-region-dropped, wrong filenames "
             "→ negative-coord arithmetic bug in region-namer.");
    }

    // ---- Per-region: alignment + minimum size + populated-slot count ----
    struct RegionStats { std::size_t size; std::size_t slots; };
    std::array<std::pair<const char*, RegionStats>, 4> per_region{{
        {"r.-1.-1.mca", {}},
        {"r.-1.0.mca",  {}},
        {"r.0.-1.mca",  {}},
        {"r.0.0.mca",   {}},
    }};

    std::size_t total_slots = 0;
    for (auto& [name, stats] : per_region) {
        const auto mca_path = region_dir / name;
        const auto sz = fs::file_size(mca_path);
        if (sz % 4096 != 0) {
            fail(std::string(name) + " size " + std::to_string(sz) +
                 " is not a multiple of the 4096-byte sector size — "
                 "anvil framing corruption");
        }
        if (sz < 8192) {
            fail(std::string(name) + " size " + std::to_string(sz) +
                 " is below the 8192-byte empty-region floor — "
                 "impossible; RegionWriter always emits ≥ 2 sectors");
        }
        const auto slots = count_populated_slots(mca_path);
        if (slots == 0) {
            fail(std::string(name) + " has 0 populated chunk slots — the "
                 "mesh does not populate this region as §3.9 predicted. "
                 "Coord-transform or region-router bug.");
        }
        stats = {sz, slots};
        total_slots += slots;
    }

    // ---- Aggregate lower bound on populated chunks across the 4 regions ----
    // §3.10.2 weak lower bound: ≥ 100 across 4 regions of 40 chunks each
    // (predicted 160 total). CI proxy for §3.7 condition (v) occupancy in
    // [100 000, 400 000]; direct voxel count from Tier-4 helper stderr.
    constexpr std::size_t POPULATED_CHUNKS_LOWER_BOUND = 100;
    if (total_slots < POPULATED_CHUNKS_LOWER_BOUND) {
        fail("total populated chunks across 4 regions = " +
             std::to_string(total_slots) + ", below the §3.10.2 weak lower "
             "bound of " + std::to_string(POPULATED_CHUNKS_LOWER_BOUND) +
             ". Voxelizer likely missed geometry OR mesh input SHA differs "
             "from tests/fixtures/sponza/LICENSE. STOP; investigate per "
             "§3.12 (D).");
    }

    fs::remove_all(base);

    std::printf("PASS: orchestrate_sponza_scale\n"
                "  voxel_size=%.4f, world_origin=(%d, %d, %d)\n"
                "  wall-clock: %.2fs (end-to-end mesh→voxel→matcher→chunk→anvil)\n"
                "  level.dat: %zu bytes\n"
                "  region files (4/4 expected per §3.9):\n",
                static_cast<double>(opts.voxel_size),
                static_cast<int>(opts.world_origin[0]),
                static_cast<int>(opts.world_origin[1]),
                static_cast<int>(opts.world_origin[2]),
                wall_clock_s,
                static_cast<std::size_t>(level_dat_size));
    for (const auto& [name, stats] : per_region) {
        std::printf("    %-12s : %8zu bytes, %4zu populated chunk slots\n",
                    name, stats.size, stats.slots);
    }
    std::printf("  total populated chunks: %zu (>= %zu required per §3.10.2)\n"
                "  NOTE: direct occupied-voxel count (§3.7 v [100000,400000]) "
                "is verified by sponza_scale_tier4_helper's verbose stderr, "
                "not this ctest, to avoid doubling wall-clock via a redundant "
                "voxelize() call.\n",
                total_slots, POPULATED_CHUNKS_LOWER_BOUND);
    return 0;
}
