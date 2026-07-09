// palette_renders_translucent_spot_check — Session 46 (c′) discipline test.
//
// Contract: research/translucency_pool_partitioning_proposal.md §2.5
// (per-entry rationale table) + §6.1 (spot-check specification).
//
// Loads the default 414-entry catalog and asserts renders_translucent for at
// least one entry from each rationale category. Coverage:
//
//   true group (should render as visible see-through surface):
//     glass                — canonical translucent
//     white_stained_glass  — stained glass family
//     red_stained_glass    — stained glass family
//     ice                  — canonical semi-transparent
//     honey_block          — semi-transparent gel
//     copper_grate         — lattice view-through
//     tinted_glass         — opacity=15 but visually translucent
//                            (Session 46 maintainer-specified addition beyond
//                            proposal §2.5's opacity<15-only table)
//
//   false group (should render as solid surface despite low opacity):
//     stone                — canonical solid opaque
//     cobblestone          — canonical solid opaque
//     snow_block           — canonical solid opaque
//     oak_leaves           — cutout foliage, reads solid at distance
//     jungle_leaves        — cutout foliage
//     beacon               — opaque frame with separate beam render
//     cactus               — opaque green cube
//     spawner              — opaque cage with animated internals
//     trial_spawner        — opaque cube with animated internals
//     vault                — opaque cube with animated glow slot
//     ochre_froglight      — solid glowing cube
//     sea_lantern          — solid glowing cube
//
// If any of the above disagree with the loaded palette, the curation drifted
// from the proposal or the CSV was regenerated without applying Session 46
// (c′)'s renders_translucent semantics. STOP + investigate; do NOT silently
// update this test.

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace pal = objtomc::palette;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

struct Expectation {
    const char* name;
    bool        renders_translucent;
    const char* rationale;
};

void check(const pal::PaletteDB& db, const Expectation& x) {
    const auto* e = db.find(x.name);
    if (!e) {
        fail(std::string("block '") + x.name + "' absent from catalog");
    }
    if (e->renders_translucent != x.renders_translucent) {
        fail(std::string(x.name) + " renders_translucent expected " +
             (x.renders_translucent ? "true" : "false") + ", got " +
             (e->renders_translucent ? "true" : "false") +
             " — rationale: " + x.rationale +
             ". Per Session 46 (c′) proposal §2.5, this is a curation-drift "
             "signal. Do NOT silently update; investigate + re-verify.");
    }
    std::printf("  %-32s rt=%s  (%s)\n",
                x.name,
                (e->renders_translucent ? "true " : "false"),
                x.rationale);
}

}  // namespace

int main() {
    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }

    std::printf("palette_renders_translucent_spot_check — Session 46 (c′) "
                "curation contract:\n");

    const std::vector<Expectation> expectations = {
        // true group
        {"minecraft:glass",               true,  "canonical translucent"},
        {"minecraft:white_stained_glass", true,  "stained glass family"},
        {"minecraft:red_stained_glass",   true,  "stained glass family"},
        {"minecraft:ice",                 true,  "canonical semi-transparent"},
        {"minecraft:honey_block",         true,  "semi-transparent gel"},
        {"minecraft:copper_grate",        true,  "lattice view-through"},
        {"minecraft:tinted_glass",        true,  "opacity=15 but visually translucent"},
        // false group
        {"minecraft:stone",               false, "canonical solid opaque"},
        {"minecraft:cobblestone",         false, "canonical solid opaque"},
        {"minecraft:snow_block",          false, "canonical solid opaque"},
        {"minecraft:oak_leaves",          false, "cutout foliage; reads solid"},
        {"minecraft:jungle_leaves",       false, "cutout foliage"},
        {"minecraft:beacon",              false, "opaque frame; beam is separate render"},
        {"minecraft:cactus",              false, "opaque green cube"},
        {"minecraft:spawner",             false, "opaque cage with animated internals"},
        {"minecraft:trial_spawner",       false, "opaque cube with animated internals"},
        {"minecraft:vault",               false, "opaque cube with animated glow slot"},
        {"minecraft:ochre_froglight",     false, "solid glowing cube"},
        {"minecraft:sea_lantern",         false, "solid glowing cube"},
    };
    for (const auto& x : expectations) check(db, x);

    // Aggregate counters — confirm the true/false split matches proposal
    // §2.5 counts (30 true / 384 false out of 414).
    std::size_t n_true = 0;
    for (const auto& e : db) {
        if (e.renders_translucent) ++n_true;
    }
    if (n_true != 30) {
        fail("expected 30 renders_translucent=true entries per §2.5, "
             "got " + std::to_string(n_true));
    }
    if (db.size() != 414) {
        fail("expected 414 palette entries, got " + std::to_string(db.size()));
    }

    std::printf("PASS: palette_renders_translucent_spot_check "
                "(19 entries verified; aggregate 30 true / %zu false out of %zu)\n",
                db.size() - n_true, db.size());
    return 0;
}
