// Real-catalog shape test for the palette DB (proposal §3.11(a), bound
// recalibrated by DECISIONS.md 2026-06-20 "Block Palette DB §3.11(a) bound
// recalibration").
//
// Drives load_default_palette() — which resolves the committed
// data/block_palette_db.csv via the OBJTOMC_PALETTE_DATA_DIR macro baked into
// src/palette.cpp at compile-time — and asserts the empirically-calibrated
// shape contract for the MC 1.21.5 vanilla catalog:
//
//   * 400 <= db.size() <= 450  (extracted count is 414 after Session 20a.6's
//                               model-JSON-driven is_full_cube re-derivation;
//                               band tight enough to catch real drift while
//                               accommodating single-digit deltas from a
//                               future MC version bump)
//   * every entry: opacity <= 15
//   * every entry: luminance <= 15
//   * every entry: is_full_cube == true
//   * every entry: name starts with "minecraft:"
//   * no two entries share the same name
//
// The per-entry invariants are identical to the fixture-based palette_shape
// test (tests/test_palette_shape.cpp); only the size band differs. This test
// is additive to that one — both run in CI; the fixture exercises
// load_palette_csv(path) against a hand-curated 21-row CSV, this one
// exercises load_default_palette() against the real 414-row catalog.

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
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

int main() {
    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }

    // Empirically-calibrated bounds. See DECISIONS.md 2026-06-20 "Block
    // Palette DB §3.11(a) bound recalibration: 414 entries empirical".
    constexpr std::size_t kLow = 400;
    constexpr std::size_t kHigh = 450;
    if (db.size() < kLow || db.size() > kHigh) {
        fail("db.size() = " + std::to_string(db.size()) +
             " out of empirical band [" + std::to_string(kLow) +
             ", " + std::to_string(kHigh) + "]");
    }

    std::unordered_set<std::string> seen_names;
    std::size_t entry_index = 0;
    for (const auto& e : db) {
        if (e.opacity > 15) {
            fail("entry " + std::to_string(entry_index) +
                 " name='" + e.name + "' opacity " +
                 std::to_string(e.opacity) + " > 15");
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

    std::printf(
        "PASS: palette default catalog shape (%zu entries within band [%zu, %zu])\n",
        db.size(), kLow, kHigh);
    return 0;
}
