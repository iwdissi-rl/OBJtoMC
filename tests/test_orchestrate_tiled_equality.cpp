// orchestrate_tiled_equality — v0.4 §5.1, the load-bearing gate
// (research/scale_v04_proposal.md; Session 57).
//
// Runs orch::orchestrate() twice per case — TilingMode::OFF (monolithic)
// and TilingMode::ON (region-aligned tiled streaming) — into separate
// world dirs with now_unix_s pinned to 0, then asserts the two world
// directories contain the SAME file set with byte-identical contents
// (level.dat + every region/r.*.mca). Any divergence is a real bug per
// the first-run-mismatch triage discipline.
//
// Cases:
//   1. cube.obj, default world_origin {0,-60,0} — single-tile world.
//   2. bunny.obj, world_origin {-20,-60,-20} — the model straddles the
//      world (0,0) region corner, producing 4 region files across all
//      four quadrants AND a non-multiple-of-512 origin, so the aligned
//      decomposition's clipped first tiles + floor-division at negative
//      coordinates are all load-bearing.
//   3. quad.glb (embedded checker texture), pinned mid-cell frame — the
//      per-voxel UV-recovery path through the tiled branch (global-index
//      box centres) byte-matches the monolithic branch.

#include "objtomc/orchestrate.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs   = std::filesystem;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::vector<char> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) fail("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    return std::vector<char>(s.begin(), s.end());
}

// Collect the world's payload files (level.dat + region/*.mca) as
// world-relative paths.
std::set<fs::path> world_files(const fs::path& world) {
    std::set<fs::path> out;
    out.insert("level.dat");
    const fs::path region = world / "region";
    if (fs::exists(region)) {
        for (const auto& e : fs::directory_iterator(region)) {
            out.insert(fs::path("region") / e.path().filename());
        }
    }
    return out;
}

void check_case(const fs::path& input, orch::OrchestrateOptions opts,
                std::size_t expect_regions, const std::string& label) {
    const fs::path base =
        fs::temp_directory_path() /
        ("objtomc_tiled_eq_" + std::to_string(::getpid()) + "_" + label);
    const fs::path mono_dir  = base.string() + "_mono";
    const fs::path tiled_dir = base.string() + "_tiled";
    fs::remove_all(mono_dir);
    fs::remove_all(tiled_dir);

    opts.now_unix_s = 0;   // byte-determinism across the two runs

    opts.tiling = orch::OrchestrateOptions::TilingMode::OFF;
    orch::orchestrate(input, mono_dir, opts);
    opts.tiling = orch::OrchestrateOptions::TilingMode::ON;
    orch::orchestrate(input, tiled_dir, opts);

    const auto mono_files  = world_files(mono_dir);
    const auto tiled_files = world_files(tiled_dir);
    if (mono_files != tiled_files) {
        fail(label + ": file sets differ (mono " +
             std::to_string(mono_files.size()) + " vs tiled " +
             std::to_string(tiled_files.size()) + " files)");
    }
    const std::size_t regions = mono_files.size() - 1;   // minus level.dat
    if (regions != expect_regions) {
        fail(label + ": expected " + std::to_string(expect_regions) +
             " region files, got " + std::to_string(regions));
    }
    for (const auto& rel : mono_files) {
        const auto a = read_file(mono_dir / rel);
        const auto b = read_file(tiled_dir / rel);
        if (a != b) {
            fail(label + ": " + rel.string() +
                 " differs between monolithic and tiled output (" +
                 std::to_string(a.size()) + " vs " +
                 std::to_string(b.size()) + " bytes)");
        }
    }
    std::cout << "PASS: " << label << " (" << regions
              << " region file(s) + level.dat byte-identical)\n";

    fs::remove_all(mono_dir);
    fs::remove_all(tiled_dir);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: " << argv[0]
                  << " <cube.obj> <bunny.obj> <quad.glb>\n";
        return 2;
    }

    // Case 1: cube, default origin — single region, single tile.
    {
        orch::OrchestrateOptions o = orch::default_options();
        o.voxel_size = 0.25f;
        check_case(argv[1], o, 1, "cube-single-region");
    }

    // Case 2: bunny straddling the (0,0) region corner — 4 regions,
    // negative chunks, clipped non-aligned first tiles.
    {
        orch::OrchestrateOptions o = orch::default_options();
        o.voxel_size   = 0.0024437f;   // ~64 cells along the longest extent
        o.world_origin = {-20, -60, -20};
        o.spawn_pos    = o.world_origin;
        check_case(argv[2], o, 4, "bunny-four-regions");
    }

    // Case 3: textured quad (embedded checker) — same pinned mid-cell
    // frame as orchestrate_textured_quad, exercising UV recovery through
    // the tiled branch.
    {
        orch::OrchestrateOptions o = orch::default_options();
        o.voxel_size              = 1.0f;
        o.voxel_grid_dimensions   = std::array<int, 3>{4, 1, 4};
        o.voxel_grid_origin_world = std::array<float, 3>{0.0f, 0.0f, 0.0f};
        check_case(argv[3], o, 1, "textured-quad");
    }

    std::cout << "PASS: orchestrate_tiled_equality\n";
    return 0;
}
