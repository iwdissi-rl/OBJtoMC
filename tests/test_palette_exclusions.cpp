// Exclusion-list test for the palette DB (proposal §3.11(c)).
//
// Asserts that the catalog does NOT contain admin/debug/fluid/non-full-cube
// blocks. The extractor's exclusion list (proposal §3.7) is what enforces
// this; the test catches drift.

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace pal = objtomc::palette;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <minimal_palette.csv> <blocks.json>\n";
        return 2;
    }
    const std::filesystem::path csv_path = argv[1];
    const std::filesystem::path blocks_json_path = argv[2];

    pal::PaletteDB db;
    try {
        db = pal::load_palette_csv(csv_path, blocks_json_path);
    } catch (const pal::palette_error& e) {
        fail(std::string("load_palette_csv threw: ") + e.what());
    }

    // Per proposal §3.11(c) — these names must NOT appear in the catalog.
    const char* excluded[] = {
        "minecraft:air",
        "minecraft:water",
        "minecraft:lava",
        "minecraft:barrier",
        "minecraft:light",
        "minecraft:structure_void",
        "minecraft:structure_block",
        "minecraft:jigsaw",
        "minecraft:command_block",
        "minecraft:oak_slab",
        "minecraft:oak_stairs",
        "minecraft:oak_fence",
        "minecraft:glass_pane",
        "minecraft:iron_bars",
    };
    std::size_t checked = 0;
    for (const char* name : excluded) {
        if (db.find(name) != nullptr) {
            fail(std::string("excluded block '") + name +
                 "' is present in the catalog (exclusion-list drift)");
        }
        ++checked;
    }

    std::printf("PASS: palette exclusions (%zu names absent)\n", checked);
    return 0;
}
