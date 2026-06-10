// Session 15b — single-stone chunk dump.
//
// Constructs ChunkBuilder(0, 0, "minecraft:plains"), builds with a
// provider that returns stone at the single world position (8, -61, 8)
// and air everywhere else, then writes the uncompressed NBT bytes to
// the path given by --out.
//
// This is the §3.12.b byte-pattern test artifact from
// research/chunk_schema_proposal.md. The Python oracle drives this
// binary and asserts the palette + bit-packed data contract.

#include "objtomc/chunk.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

using objtomc::chunk::BlockState;
using objtomc::chunk::ChunkBuilder;

namespace {

int run_dump(const std::string& out_path) {
    ChunkBuilder cb(0, 0, "minecraft:plains");
    cb.build([](int lx, int wy, int lz) -> BlockState {
        if (lx == 8 && wy == -61 && lz == 8) {
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

    std::printf("PASS: dumped single-stone chunk NBT (%zu bytes) to %s\n",
                bytes.size(), out_path.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--out") == 0) {
        return run_dump(argv[2]);
    }
    std::fprintf(stderr,
        "usage: %s --out PATH  (dump single-stone chunk NBT to PATH)\n",
        argv[0]);
    return 2;
}
