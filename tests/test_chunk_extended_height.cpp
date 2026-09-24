// chunk_extended_height — Session 63 (v0.4 item #2) helper binary.
//
// Two modes:
//   no args   : WorldHeight::validate() error paths — non-multiple-of-16,
//               height out of [16, 4064], min_y < -2032, min_y+height >
//               2032 all throw chunk_error; the default validates; the
//               ground-truth (-2032, 4064) validates.
//   --out <p> : build a chunk at WorldHeight{-2032, 4064} (the Session 61
//               ground-truth dimensions) containing a single
//               minecraft:oak_log at world (14, 592, 7) — the same
//               world Y / column as the top of the maintainer-built capture
//               column — and write the uncompressed chunk NBT to <p>.
//               The Python oracle (test_chunk_extended_height_oracle.py)
//               parses it with twoolie/NBT and asserts the parametrised
//               schema against the captured real chunk's facts: yPos,
//               full contiguous section span, 12-bit / 52-long
//               heightmaps, and the WORLD_SURFACE encoding at the block's
//               column.

#include "objtomc/chunk.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace chk = objtomc::chunk;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

void expect_invalid(chk::WorldHeight wh, const std::string& what) {
    try {
        wh.validate();
    } catch (const chk::chunk_error&) {
        std::cout << "PASS: " << what << " rejected\n";
        return;
    }
    fail(what + " was NOT rejected");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        // Error paths.
        expect_invalid({-60, 384},   "min_y not multiple of 16");
        expect_invalid({-64, 380},   "height not multiple of 16");
        expect_invalid({-64, 0},     "height < 16");
        expect_invalid({-64, 4080},  "height > 4064");
        expect_invalid({-2048, 384}, "min_y < -2032");
        expect_invalid({-64, 2112},  "min_y + height > 2032");
        chk::WorldHeight{}.validate();               // default OK
        chk::WorldHeight{-2032, 4064}.validate();    // ground-truth OK
        chk::WorldHeight{-64, 1024}.validate();      // HH 1024 variant OK
        if (chk::WorldHeight{}.heightmap_bits() != 9u) {
            fail("default heightmap_bits != 9");
        }
        if ((chk::WorldHeight{-2032, 4064}.heightmap_bits()) != 12u) {
            fail("4064 heightmap_bits != 12");
        }
        std::cout << "PASS: WorldHeight validation + bit widths\n";
        return 0;
    }

    if (argc != 3 || std::string(argv[1]) != "--out") {
        std::cerr << "usage: " << argv[0] << " [--out <path>]\n";
        return 2;
    }

    const chk::WorldHeight wh{-2032, 4064};
    chk::ChunkBuilder builder(2, -16, "minecraft:plains", wh);
    chk::BlockState log{"minecraft:oak_log", {{"axis", "y"}}};
    builder.build([&](int lx, int wy, int lz) -> chk::BlockState {
        if (lx == 14 && wy == 592 && lz == 7) return log;
        return chk::BlockState::air();
    });
    const std::vector<std::uint8_t> bytes = builder.to_bytes();

    std::ofstream out(argv[2], std::ios::binary);
    if (!out) fail(std::string("cannot open ") + argv[2]);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    std::cout << "wrote " << bytes.size() << " bytes\n";
    return 0;
}
