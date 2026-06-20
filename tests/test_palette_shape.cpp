// Shape test for the palette DB (proposal §3.11(a)).
//
// Drives load_palette_csv against the hand-curated fixture at
// tests/fixtures/palette/minimal_palette.csv and asserts the fixture-scoped
// shape contract:
//
//   * 15 <= db.size() <= 50          (fixture is ~21 rows by design)
//   * every entry: opacity <= 15
//   * every entry: luminance <= 15
//   * every entry: is_full_cube == true
//   * every entry: name starts with "minecraft:"
//   * no two entries share the same name
//
// When the real data/block_palette_db.csv lands in Session 20b the bounds
// should be widened to the proposal §3.11(a) range [500, 750]; the
// per-entry invariants above remain identical.

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>

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

    // Fixture-scoped bounds. The proposal's real-catalog band is [500, 750];
    // the hand-curated fixture is intentionally tiny.
    constexpr std::size_t kLow = 15;
    constexpr std::size_t kHigh = 50;
    if (db.size() < kLow || db.size() > kHigh) {
        fail("db.size() = " + std::to_string(db.size()) +
             " out of fixture-scoped band [" + std::to_string(kLow) +
             ", " + std::to_string(kHigh) + "]");
    }

    std::unordered_set<std::string> seen_names;
    std::size_t entry_index = 0;
    for (const auto& e : db) {
        if (e.opacity > 15) {
            fail("entry " + std::to_string(entry_index) +
                 " name='" + e.name + "' opacity " + std::to_string(e.opacity) +
                 " > 15");
        }
        if (e.luminance > 15) {
            fail("entry " + std::to_string(entry_index) +
                 " name='" + e.name + "' luminance " +
                 std::to_string(e.luminance) + " > 15");
        }
        if (!e.is_full_cube) {
            fail("entry " + std::to_string(entry_index) +
                 " name='" + e.name + "' is_full_cube=false");
        }
        if (e.name.rfind("minecraft:", 0) != 0) {
            fail("entry " + std::to_string(entry_index) +
                 " name='" + e.name + "' does not start with 'minecraft:'");
        }
        if (!seen_names.insert(e.name).second) {
            fail("duplicate name '" + e.name + "'");
        }
        ++entry_index;
    }

    std::printf("PASS: palette shape (%zu entries within fixture band)\n",
                db.size());
    return 0;
}
