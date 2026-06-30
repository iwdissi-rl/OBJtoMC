// orchestrate_level_dat_writer — C++ side of the orchestrate_level_dat_oracle.
//
// Usage: test_orchestrate_level_dat_writer <output_level_dat_path>
//
// Calls orch::write_level_dat(...) with deterministic LastPlayed=0 so the
// Python oracle's round-trip read produces byte-stable assertions.

#include "objtomc/orchestrate.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace orch = objtomc::orch;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <output_level_dat_path>\n";
        return 2;
    }
    const std::filesystem::path out_path = argv[1];

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size = 1.0f;        // not used by write_level_dat but must be > 0 for validation
    opts.level_name = "objtomc world";
    opts.spawn_pos  = {0, -60, 0};

    try {
        orch::write_level_dat(opts, out_path, /*now_unix_ms=*/0);
    } catch (const std::exception& e) {
        std::cerr << "write_level_dat threw: " << e.what() << "\n";
        return 1;
    }

    std::printf("wrote %s\n", out_path.string().c_str());
    return 0;
}
