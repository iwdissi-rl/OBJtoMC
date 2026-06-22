// matcher_tie_break — proposal §3.13 (e).
//
// Loads a fixture catalog with two entries (granite, stone) at identical
// avg_color; matches a Material with that colour; asserts the alphabetical
// tie-break (§3.12) picks 'granite' over 'stone'.
//
// Drives load_palette_csv() against tests/fixtures/palette/tie_break_palette.csv
// (paths injected via argv from CMake, mirroring the palette-layer tests).

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace mat = objtomc::matcher;
namespace pal = objtomc::palette;
namespace msh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <tie_break_palette.csv> <blocks.json>\n";
        return 2;
    }
    const std::filesystem::path csv_path         = argv[1];
    const std::filesystem::path blocks_json_path = argv[2];

    pal::PaletteDB db;
    try {
        db = pal::load_palette_csv(csv_path, blocks_json_path);
    } catch (const pal::palette_error& e) {
        fail(std::string("load_palette_csv threw: ") + e.what());
    }
    if (db.size() != 2) {
        fail("tie_break_palette must have exactly 2 entries; got " +
             std::to_string(db.size()));
    }

    mat::Matcher matcher(db, mat::default_options());

    // Target colour matches the fixture's mid-gray exactly so Oklab
    // distance is zero against BOTH entries. Tie-break order is then
    // opacity (both 15) → luminance (both 0) → alphabetical name
    // (g < s → granite).
    msh::Material m{};
    m.name = "tie_break_target";
    m.kd   = {120.0f / 255.0f, 120.0f / 255.0f, 120.0f / 255.0f};
    m.ke   = {0.0f, 0.0f, 0.0f};
    m.d    = 1.0f;

    objtomc::chunk::BlockState result;
    try {
        result = matcher.match(m);
    } catch (const mat::matcher_error& e) {
        fail(std::string("matcher.match threw: ") + e.what());
    }

    if (result.name != "minecraft:granite") {
        fail("tie-break expected 'minecraft:granite' (alphabetically first), "
             "got '" + result.name + "'");
    }

    std::printf("PASS: matcher_tie_break (granite < stone)\n");
    return 0;
}
