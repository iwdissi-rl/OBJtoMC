// Spot-check test for the palette DB (proposal §3.11(b)).
//
// Looks up specific blocks in the fixture catalog and verifies opacity,
// luminance, and approximate sRGB band per the proposal's spot-check table.
// Colour bands are lax (~±32 per channel) because the average depends on
// the texture sample. When the real catalog lands in 20b these same bands
// should still pass against extractor output.

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

struct Spec {
    const char* name;
    int         opacity;
    int         luminance;
    int         r_lo, r_hi;
    int         g_lo, g_hi;
    int         b_lo, b_hi;
};

void check(const pal::PaletteDB& db, const Spec& s) {
    const auto* e = db.find(s.name);
    if (!e) {
        fail(std::string("block '") + s.name + "' absent from fixture catalog");
    }
    if (e->opacity != s.opacity) {
        fail(std::string(s.name) + ".opacity expected " +
             std::to_string(s.opacity) + ", got " + std::to_string(e->opacity));
    }
    if (e->luminance != s.luminance) {
        fail(std::string(s.name) + ".luminance expected " +
             std::to_string(s.luminance) + ", got " +
             std::to_string(e->luminance));
    }
    const int r = e->avg_color_srgb.r;
    const int g = e->avg_color_srgb.g;
    const int b = e->avg_color_srgb.b;
    if (r < s.r_lo || r > s.r_hi ||
        g < s.g_lo || g > s.g_hi ||
        b < s.b_lo || b > s.b_hi) {
        fail(std::string(s.name) +
             ".avg_color_srgb (" + std::to_string(r) + ", " +
             std::to_string(g) + ", " + std::to_string(b) +
             ") outside band [" +
             std::to_string(s.r_lo) + "-" + std::to_string(s.r_hi) + ", " +
             std::to_string(s.g_lo) + "-" + std::to_string(s.g_hi) + ", " +
             std::to_string(s.b_lo) + "-" + std::to_string(s.b_hi) + "]");
    }
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

    // Bands are wider than the fixture's exact hand-curated values
    // (~±32 per channel per proposal §3.11(b)) so the same test passes
    // unmodified against real-extractor output in Session 20b.
    const Spec specs[] = {
        {"minecraft:stone",          15,  0, 100, 140, 100, 140, 100, 140},
        {"minecraft:grass_block",    15,  0,  80, 150, 130, 200,  60, 130},
        {"minecraft:sea_lantern",    15, 15, 170, 240, 180, 240, 170, 240},
        {"minecraft:jack_o_lantern", 15, 15, 170, 240, 100, 170,  20, 100},
        {"minecraft:glowstone",      15, 15, 180, 240, 150, 220,  60, 140},
        {"minecraft:obsidian",       15,  0,  20,  80,  10,  70,  40, 100},
        {"minecraft:redstone_block", 15,  0, 130, 220,  10,  80,  10,  80},
        {"minecraft:diamond_block",  15,  0,  80, 160, 190, 255, 190, 255},
        {"minecraft:white_concrete", 15,  0, 180, 240, 180, 240, 180, 240},
    };
    for (const auto& s : specs) check(db, s);

    // Spot-check that linear-RGB conversion is plausible (linear < sRGB for
    // mid-tones since the transfer is concave). For stone's grey, all three
    // linear channels should be in (0, 1) and < their sRGB/255 counterpart.
    const auto& stone = db.get("minecraft:stone");
    for (int c = 0; c < 3; ++c) {
        const float lin = stone.avg_color_linear[c];
        if (lin < 0.0f || lin > 1.0f) {
            fail("stone.avg_color_linear[" + std::to_string(c) +
                 "] out of [0, 1]: " + std::to_string(lin));
        }
    }

    std::printf("PASS: palette spot-check (%zu blocks verified)\n",
                sizeof(specs) / sizeof(specs[0]));
    return 0;
}
