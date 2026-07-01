// orchestrate_transparent_scene — transparent Tier-4 proposal §3.7.1.
//
// Orchestrate the hand-written transparent-scene fixture end-to-end into a
// temporary world dir. Asserts level.dat exists; exactly 1 region file;
// region file passes a basic anvil-framing sanity check (4096-aligned,
// >= 8192 bytes, non-zero location-table slots).
//
// NO MC client check — that's the Session 34 maintainer-run Tier-4 manual
// gate. NO level.dat field assertions — that's orchestrate_level_dat_
// oracle's job. This ctest verifies that the full pipeline runs cleanly on
// the transparent-branch fixture and emits a 1-region world dir. Mirrors
// orchestrate_cornell_box / orchestrate_stanford_bunny structure verbatim.

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-scene.obj>\n";
        return 2;
    }
    const fs::path obj_path = argv[1];

    const auto base = fs::temp_directory_path() /
        ("objtomc_trans_" + std::to_string(std::rand()));
    fs::remove_all(base);
    const auto world_dir = base / "world";

    // Proposal §3.3 orchestration parameters.
    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size   = 0.0313f;            // 2 OBJ-units / 64 voxels
    opts.world_origin = {0, -60, 0};        // matches bunny + Cornell Box
    opts.spawn_pos    = {32, -59, 4};       // standing on floor, before front walls
    opts.level_name   = "objtomc-transparent-tier4";
    opts.now_unix_s   = 0;                  // pin for determinism in CI

    try {
        orch::orchestrate(obj_path, world_dir, opts);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }

    if (!fs::exists(world_dir / "level.dat")) fail("level.dat missing");
    const auto level_dat_size = fs::file_size(world_dir / "level.dat");
    if (level_dat_size < 200) {
        fail("level.dat suspiciously small: " + std::to_string(level_dat_size));
    }

    const auto region_dir = world_dir / "region";
    if (!fs::is_directory(region_dir)) fail("region/ subdir missing");

    std::size_t mca_count = 0;
    fs::path the_mca;
    for (const auto& entry : fs::directory_iterator(region_dir)) {
        if (entry.path().extension() == ".mca") {
            ++mca_count;
            the_mca = entry.path();
        }
    }
    if (mca_count != 1) {
        fail("expected 1 region file, got " + std::to_string(mca_count));
    }
    if (the_mca.filename() != "r.0.0.mca") {
        fail("unexpected region filename: " + the_mca.filename().string());
    }

    // Anvil sanity: 4096-aligned, >= 8192 bytes, at least 1 non-zero location.
    const auto mca_size = fs::file_size(the_mca);
    if (mca_size % 4096 != 0) {
        fail("region file size " + std::to_string(mca_size) +
             " is not a multiple of the 4096-byte sector size");
    }
    if (mca_size < 8192) {
        fail("region file size " + std::to_string(mca_size) +
             " is below the 8192-byte empty-region floor");
    }

    std::ifstream in(the_mca, std::ios::binary);
    std::vector<unsigned char> location_table(4096);
    in.read(reinterpret_cast<char*>(location_table.data()), 4096);

    std::size_t non_zero_slots = 0;
    for (std::size_t i = 0; i < 1024; ++i) {
        const auto* p = location_table.data() + 4 * i;
        const std::uint32_t loc =
            (static_cast<std::uint32_t>(p[0]) << 24) |
            (static_cast<std::uint32_t>(p[1]) << 16) |
            (static_cast<std::uint32_t>(p[2]) << 8)  |
            (static_cast<std::uint32_t>(p[3]));
        if (loc != 0) ++non_zero_slots;
    }
    if (non_zero_slots < 1) {
        fail("region location table has 0 non-zero slots — no chunks written");
    }
    if (non_zero_slots > 1024) {
        fail("region location table has " + std::to_string(non_zero_slots) +
             " non-zero slots (> 1024 max)");
    }

    fs::remove_all(base);

    std::printf("PASS: orchestrate_transparent_scene\n"
                "  voxel_size=%.4f, 1 region file (r.0.0.mca, %zu bytes, "
                "%zu populated chunk slots)\n"
                "  level.dat=%zu bytes\n",
                static_cast<double>(opts.voxel_size),
                static_cast<std::size_t>(mca_size),
                non_zero_slots,
                static_cast<std::size_t>(level_dat_size));
    return 0;
}
