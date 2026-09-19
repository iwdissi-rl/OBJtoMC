// texture_sampling — Session 48 v0.3 item #1 unit test.
//
// Exercises the texture layer directly against the committed 2×2 checker
// fixture (tests/fixtures/texture/checker.png — top row green|white,
// bottom row red|blue, hand-generated RGB8 PNG):
//   1. TextureCache::load decodes it to a 2×2 RGBA8 Image with exact
//      texels (alpha-less source → alpha bytes all 255 — Session 52).
//   2. sample_nearest quadrant mapping honours the V flip (v=0 → image
//      BOTTOM row per OBJ's bottom-left UV origin).
//   3. Repeat wrap: u/v shifted by whole turns (±1, ±2) resample the same
//      texel; v=1.0 wraps to vw=0 (bottom row).
//   4. Missing file → nullptr (flat-Kd fallback contract), and the failure
//      is cached (second load also nullptr).
//   5. Cache identity: same path twice → same Image pointer.
//   6. sample_alpha returns exactly 1.0 for the alpha-less source at every
//      quadrant (Session 52 contract).

#include "objtomc/texture.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs  = std::filesystem;
namespace tex = objtomc::texture;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

void expect_texel(const tex::Image& img, float u, float v,
                  const std::array<float, 3>& want, const std::string& what) {
    const auto got = tex::sample_nearest(img, u, v);
    for (int c = 0; c < 3; ++c) {
        if (std::fabs(got[static_cast<std::size_t>(c)] -
                      want[static_cast<std::size_t>(c)]) > 1e-6f) {
            fail(what + ": channel " + std::to_string(c) + " got " +
                 std::to_string(got[static_cast<std::size_t>(c)]) +
                 " want " + std::to_string(want[static_cast<std::size_t>(c)]));
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-checker.png>\n";
        return 2;
    }
    const fs::path png_path = argv[1];

    tex::TextureCache cache;

    // --- 1. decode ---------------------------------------------------------
    const tex::Image* img = cache.load(png_path);
    if (img == nullptr) fail("checker.png failed to load");
    if (img->width != 2 || img->height != 2) {
        fail("expected 2x2, got " + std::to_string(img->width) + "x" +
             std::to_string(img->height));
    }
    if (img->rgba.size() != 16) {
        fail("expected 16 bytes RGBA, got " + std::to_string(img->rgba.size()));
    }
    // Row 0 (top): green, white. Row 1 (bottom): red, blue. Alpha-less
    // source expands with alpha = 255 (Session 52).
    const std::array<std::uint8_t, 16> want_bytes = {
        0, 255, 0, 255,  255, 255, 255, 255,
        255, 0, 0, 255,  0, 0, 255, 255,
    };
    for (std::size_t i = 0; i < 16; ++i) {
        if (img->rgba[i] != want_bytes[i]) {
            fail("texel byte " + std::to_string(i) + " got " +
                 std::to_string(img->rgba[i]) + " want " +
                 std::to_string(want_bytes[i]));
        }
    }

    // --- 2. quadrant sampling with V flip ----------------------------------
    const std::array<float, 3> red   = {1.0f, 0.0f, 0.0f};
    const std::array<float, 3> green = {0.0f, 1.0f, 0.0f};
    const std::array<float, 3> blue  = {0.0f, 0.0f, 1.0f};
    const std::array<float, 3> white = {1.0f, 1.0f, 1.0f};

    // v = 0.25 → image bottom row (red|blue); v = 0.75 → top row (green|white).
    expect_texel(*img, 0.25f, 0.25f, red,   "u=.25 v=.25 (bottom-left)");
    expect_texel(*img, 0.75f, 0.25f, blue,  "u=.75 v=.25 (bottom-right)");
    expect_texel(*img, 0.25f, 0.75f, green, "u=.25 v=.75 (top-left)");
    expect_texel(*img, 0.75f, 0.75f, white, "u=.75 v=.75 (top-right)");

    // --- 3. repeat wrap -----------------------------------------------------
    expect_texel(*img,  1.25f,  0.25f, red,  "u=1.25 wraps to .25");
    expect_texel(*img, -0.75f,  0.25f, red,  "u=-0.75 wraps to .25");
    expect_texel(*img,  0.25f,  2.75f, green, "v=2.75 wraps to .75");
    expect_texel(*img,  0.25f, -1.25f, green, "v=-1.25 wraps to .75");
    // v = 1.0 wraps to vw = 0 exactly → bottom row; (1 - 0) * height clamps.
    expect_texel(*img,  0.0f,   1.0f,  red,  "v=1.0 wraps to bottom row");
    expect_texel(*img,  0.0f,   0.0f,  red,  "v=0.0 is bottom row");

    // --- 4. missing file → nullptr, cached ---------------------------------
    const fs::path missing = png_path.parent_path() / "no_such_texture.png";
    std::cerr << "(expected warning follows)\n";
    if (cache.load(missing) != nullptr) fail("missing file returned non-null");
    if (cache.load(missing) != nullptr) fail("cached failure returned non-null");

    // --- 5. cache identity --------------------------------------------------
    if (cache.load(png_path) != img) fail("second load returned different pointer");

    // --- 6. sample_alpha on an alpha-less source = exactly 1.0 --------------
    for (const float u : {0.25f, 0.75f}) {
        for (const float v : {0.25f, 0.75f}) {
            const float a = tex::sample_alpha(*img, u, v);
            if (a != 1.0f) {
                fail("sample_alpha(u=" + std::to_string(u) + ", v=" +
                     std::to_string(v) + ") = " + std::to_string(a) +
                     ", expected exactly 1.0 for RGB source");
            }
        }
    }

    std::cout << "PASS: texture_sampling (decode, quadrants, V-flip, wrap, "
                 "missing-file fallback, cache identity, alpha=1)\n";
    return 0;
}
