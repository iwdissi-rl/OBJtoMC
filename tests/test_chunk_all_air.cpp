// Session 15a — all-air chunk path.
//
// Two modes, dispatched on argv:
//
//   ./test_chunk_all_air
//       Sanity checks the three programmer-misuse error paths on
//       ChunkBuilder. Prints "PASS: ChunkBuilder error paths" and
//       exits 0 on success.
//
//   ./test_chunk_all_air --out <path>
//       Constructs ChunkBuilder(0, 0, "minecraft:plains"), builds with
//       a trivial all-air provider lambda, calls to_bytes() and writes
//       the result to <path>. Prints a one-line "PASS" summary and
//       exits 0. This is the mode the Python oracle invokes.

#include "objtomc/chunk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

using objtomc::chunk::BlockState;
using objtomc::chunk::ChunkBuilder;
using objtomc::chunk::chunk_error;

namespace {

int run_error_paths() {
    // 1) Empty biome_id at construction must throw.
    {
        bool threw = false;
        try {
            ChunkBuilder cb(0, 0, "");
            (void)cb;
        } catch (const chunk_error&) {
            threw = true;
        }
        if (!threw) {
            std::fprintf(stderr,
                "FAIL: ChunkBuilder(0,0,\"\") did not throw chunk_error\n");
            return 1;
        }
    }

    // 2) to_bytes() before build() must throw.
    {
        ChunkBuilder cb(0, 0, "minecraft:plains");
        bool threw = false;
        try {
            (void)cb.to_bytes();
        } catch (const chunk_error&) {
            threw = true;
        }
        if (!threw) {
            std::fprintf(stderr,
                "FAIL: to_bytes() before build() did not throw chunk_error\n");
            return 1;
        }
    }

    // 3) Calling build() twice must throw on the second call.
    {
        ChunkBuilder cb(0, 0, "minecraft:plains");
        cb.build([](int, int, int) { return BlockState::air(); });
        bool threw = false;
        try {
            cb.build([](int, int, int) { return BlockState::air(); });
        } catch (const chunk_error&) {
            threw = true;
        }
        if (!threw) {
            std::fprintf(stderr,
                "FAIL: second build() did not throw chunk_error\n");
            return 1;
        }
    }

    std::printf("PASS: ChunkBuilder error paths\n");
    return 0;
}

int run_dump(const std::string& out_path) {
    ChunkBuilder cb(0, 0, "minecraft:plains");
    cb.build([](int, int, int) { return BlockState::air(); });
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

    std::printf("PASS: dumped all-air chunk NBT (%zu bytes) to %s\n",
                bytes.size(), out_path.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        return run_error_paths();
    }
    if (argc == 3 && std::strcmp(argv[1], "--out") == 0) {
        return run_dump(argv[2]);
    }
    std::fprintf(stderr,
        "usage: %s             (run error-path sanity checks)\n"
        "       %s --out PATH  (dump all-air chunk NBT to PATH)\n",
        argv[0], argv[0]);
    return 2;
}
