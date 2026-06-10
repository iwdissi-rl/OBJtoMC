// Session 15c — heightmap predicate-scan stress test.
//
// Constructs ChunkBuilder(0, 0, "minecraft:plains"), builds with a
// provider that places stone in a triangular pattern across the
// section Y=-4 (world Y ∈ [-60, -50]):
//
//   stone iff (lx + lz <= world_y - WORLD_BOTTOM_Y)
//             && world_y ∈ [-60, -50]
//
// air elsewhere. The (lx, lz) plane crossed against the world_y range
// produces a non-uniform heightmap pattern (some columns have a top,
// others are pure air) — the multi-column variation the Superflat
// fixture didn't exercise, so this test is the first cross-check on
// the (lz * 16 + lx) heightmap indexing convention from proposal §3.5.
//
// The Python oracle re-derives expected heightmap values per column
// and verifies all four heightmaps unpack to the same pattern.

#include "objtomc/chunk.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

using objtomc::chunk::BlockState;
using objtomc::chunk::ChunkBuilder;
using objtomc::chunk::WORLD_BOTTOM_Y;

namespace {

int run_dump(const std::string& out_path) {
    ChunkBuilder cb(0, 0, "minecraft:plains");
    cb.build([](int lx, int wy, int lz) -> BlockState {
        if (wy >= -60 && wy <= -50 &&
            (lx + lz) <= (wy - WORLD_BOTTOM_Y)) {
            return BlockState{"minecraft:stone", {}};
        }
        return BlockState::air();
    });

    std::vector<std::uint8_t> bytes = cb.to_bytes();
    if (bytes.empty()) {
        std::fprintf(stderr, "FAIL: to_bytes() returned empty buffer\n");
        return 1;
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr,
            "FAIL: could not open %s for writing\n", out_path.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr,
            "FAIL: write/close failed for %s\n", out_path.c_str());
        return 1;
    }

    std::printf("PASS: dumped staircase-heightmap chunk NBT (%zu bytes) "
                "to %s\n", bytes.size(), out_path.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--out") == 0) {
        return run_dump(argv[2]);
    }
    std::fprintf(stderr,
        "usage: %s --out PATH  (dump staircase-heightmap chunk NBT to PATH)\n",
        argv[0]);
    return 2;
}
