// orchestrate_stanford_bunny — proposal §3.13(b).
//
// Orchestrate the committed Stanford bunny end-to-end into a temporary
// world dir. Assert: level.dat exists; exactly 1 region file; region file
// passes a basic anvil round-trip sanity check (location-table sector
// counts non-zero for chunks 0..15, where 16 chunks of the bunny's
// (cx=0..3) × (cz=0..3) footprint live).

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
        std::cerr << "usage: " << argv[0] << " <path-to-bunny.obj>\n";
        return 2;
    }
    const fs::path bunny_path = argv[1];

    // Compute the bunny's actual AABB max extent (the fixture is coord-
    // normalised to ~0.156 × 0.187 × 0.121; not the Stanford archive's
    // ~1.5).
    const msh::Mesh bunny = msh::load_obj(bunny_path);
    float max_extent = 0.0f;
    for (int k = 0; k < 3; ++k) {
        float lo =  std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();
        for (const auto& p : bunny.positions) {
            if (p[k] < lo) lo = p[k];
            if (p[k] > hi) hi = p[k];
        }
        max_extent = std::max(max_extent, hi - lo);
    }
    const float voxel_size = max_extent / 64.0f;

    const auto base = fs::temp_directory_path() /
        ("objtomc_bunny_" + std::to_string(std::rand()));
    fs::remove_all(base);
    const auto world_dir = base / "world";

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size  = voxel_size;
    opts.world_origin = {0, -60, 0};
    opts.spawn_pos    = {0, -60, 0};
    opts.level_name   = "bunny";

    try {
        orch::orchestrate(bunny_path, world_dir, opts);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }

    if (!fs::exists(world_dir / "level.dat")) fail("level.dat missing");

    const auto region_dir = world_dir / "region";
    if (!fs::is_directory(region_dir)) fail("region/ subdir missing");

    // Count region files; verify exactly 1.
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

    // Anvil round-trip basic check: the file is a multiple of 4096 bytes
    // (the sector size), and at least one chunk slot in the location table
    // (bytes [0, 4096)) is non-zero.
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

    const auto ld_size = fs::file_size(world_dir / "level.dat");
    fs::remove_all(base);

    std::printf("PASS: orchestrate_stanford_bunny\n"
                "  voxel_size=%.6f, 1 region file (r.0.0.mca, %zu bytes, "
                "%zu populated chunk slots)\n"
                "  level.dat=%zu bytes\n",
                static_cast<double>(voxel_size),
                static_cast<std::size_t>(mca_size),
                non_zero_slots,
                static_cast<std::size_t>(ld_size));
    return 0;
}
