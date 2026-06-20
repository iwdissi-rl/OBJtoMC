// Version-mismatch test for the palette DB (proposal §3.11(e)).
//
// Passes a CSV with a deliberately wrong "# mc_version: 1.21.4" header
// alongside the vendored 1.21.5 blocks.json. The loader's version-assert
// must throw palette_error with a message that mentions "version" so a
// future maintainer reading the error knows to bump the CSV header.
//
// Without this test, a future maintainer who bumps PrismarineJS but forgets
// to bump the CSV header could ship a silently inconsistent catalog.

#include "objtomc/palette.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

bool contains_ci(const std::string& haystack, const char* needle) {
    std::string lower;
    lower.reserve(haystack.size());
    for (char c : haystack) {
        lower.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return lower.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <version_mismatch.csv> <blocks.json>\n";
        return 2;
    }
    const std::filesystem::path csv_path = argv[1];
    const std::filesystem::path blocks_json_path = argv[2];

    if (!std::filesystem::exists(csv_path)) {
        fail("version-mismatch fixture not found at " + csv_path.string());
    }

    std::string caught_what;
    try {
        (void)pal::load_palette_csv(csv_path, blocks_json_path);
        fail("expected palette_error from version-mismatch CSV, but "
             "load_palette_csv returned normally");
    } catch (const pal::palette_error& e) {
        caught_what = e.what();
    } catch (const std::exception& e) {
        fail(std::string("expected palette_error, got std::exception('") +
             e.what() + "')");
    }

    // Message must mention "version" so a future maintainer can diagnose
    // the failure without reading the loader source.
    if (!contains_ci(caught_what, "version")) {
        fail("palette_error message lacks the word 'version': '" +
             caught_what + "'");
    }
    // And it should mention the bad declared version so the maintainer
    // sees which header line to update.
    if (caught_what.find("1.21.4") == std::string::npos) {
        fail("palette_error message lacks the declared bad version '1.21.4': '" +
             caught_what + "'");
    }

    std::printf("PASS: palette version-mismatch throws palette_error\n"
                "  message: %s\n", caught_what.c_str());
    return 0;
}
