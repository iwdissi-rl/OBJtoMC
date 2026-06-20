// Malformed CSV test for the palette DB (proposal §3.11(d)).
//
// Four sub-cases, each must throw palette_error (NOT std::runtime_error or
// std::exception or a generic throw):
//
//   (1) path that doesn't exist
//   (2) wrong column count
//   (3) opacity out of [0, 15]
//   (4) non-numeric luminance

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace pal = objtomc::palette;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Returns "" on success (palette_error was thrown), else the failure reason.
std::string expect_palette_error(const std::filesystem::path& csv,
                                 const std::filesystem::path& blocks_json,
                                 const char* label) {
    try {
        (void)pal::load_palette_csv(csv, blocks_json);
        return std::string(label) +
               ": expected palette_error, but load_palette_csv returned normally";
    } catch (const pal::palette_error&) {
        return "";
    } catch (const std::exception& e) {
        return std::string(label) +
               ": expected palette_error, got std::exception('" + e.what() + "')";
    } catch (...) {
        return std::string(label) +
               ": expected palette_error, got non-std::exception throw";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: " << argv[0]
                  << " <missing.csv> <wrong_cols.csv> <opacity_oob.csv>"
                  << " <nonnum_lum.csv> <blocks.json>\n";
        return 2;
    }
    const std::filesystem::path missing       = argv[1];
    const std::filesystem::path wrong_cols    = argv[2];
    const std::filesystem::path opacity_oob   = argv[3];
    const std::filesystem::path nonnum_lum    = argv[4];
    const std::filesystem::path blocks_json   = argv[5];

    if (std::filesystem::exists(missing)) {
        fail("missing-path fixture was supposed to be absent: " +
             missing.string());
    }
    if (!std::filesystem::exists(wrong_cols)) {
        fail("wrong-cols fixture not found at " + wrong_cols.string());
    }
    if (!std::filesystem::exists(opacity_oob)) {
        fail("opacity-oob fixture not found at " + opacity_oob.string());
    }
    if (!std::filesystem::exists(nonnum_lum)) {
        fail("nonnum-lum fixture not found at " + nonnum_lum.string());
    }

    std::string e1 = expect_palette_error(missing, blocks_json, "missing-path");
    if (!e1.empty()) fail(e1);
    std::string e2 = expect_palette_error(wrong_cols, blocks_json, "wrong-column-count");
    if (!e2.empty()) fail(e2);
    std::string e3 = expect_palette_error(opacity_oob, blocks_json, "opacity-out-of-range");
    if (!e3.empty()) fail(e3);
    std::string e4 = expect_palette_error(nonnum_lum, blocks_json, "non-numeric-luminance");
    if (!e4.empty()) fail(e4);

    std::printf("PASS: palette malformed CSV throws palette_error (4 sub-cases)\n");
    return 0;
}
