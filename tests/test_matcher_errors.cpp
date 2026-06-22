// matcher_errors — proposal §3.13 (g).
//
// Asserts each programmer-misuse case in §3.11 throws matcher_error with a
// message that names the offending condition. Sub-cases:
//   - Empty palette → "empty palette"
//   - Non-finite kd → "non-finite"
//   - Non-finite ke → "non-finite"
//   - Non-finite d  → "non-finite"
//   - Invalid emission_threshold (out of [0, 1]) → "emission_threshold"
//   - Invalid opacity_threshold (out of [0, 1])  → "opacity_threshold"
//   - Batch span size mismatch → "batch size mismatch"

#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace mat = objtomc::matcher;
namespace pal = objtomc::palette;
namespace msh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

void expect_message_contains(const std::string& what,
                             const std::string& needle,
                             const std::string& case_name) {
    if (what.find(needle) == std::string::npos) {
        fail(case_name + ": matcher_error message '" + what +
             "' does not contain expected substring '" + needle + "'");
    }
}

}  // namespace

int main() {
    // (1) Empty palette → matcher_error("empty palette").
    {
        pal::PaletteDB empty_db;
        bool threw = false;
        try {
            mat::Matcher m(empty_db, mat::default_options());
            (void)m;
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "empty palette",
                                    "empty-palette");
        }
        if (!threw) fail("empty palette did not throw matcher_error");
    }

    // (2) Invalid emission_threshold.
    {
        pal::PaletteDB db;
        try {
            db = pal::load_default_palette();
        } catch (const pal::palette_error& e) {
            fail(std::string("load_default_palette threw: ") + e.what());
        }
        mat::MatcherOptions bad = mat::default_options();
        bad.emission_threshold = 1.5f;
        bool threw = false;
        try {
            mat::Matcher m(db, bad);
            (void)m;
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "emission_threshold",
                                    "bad-emission-threshold");
        }
        if (!threw) {
            fail("invalid emission_threshold did not throw matcher_error");
        }
    }

    // (3) Invalid opacity_threshold.
    {
        pal::PaletteDB db;
        try {
            db = pal::load_default_palette();
        } catch (const pal::palette_error& e) {
            fail(std::string("load_default_palette threw: ") + e.what());
        }
        mat::MatcherOptions bad = mat::default_options();
        bad.opacity_threshold = -0.1f;
        bool threw = false;
        try {
            mat::Matcher m(db, bad);
            (void)m;
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "opacity_threshold",
                                    "bad-opacity-threshold");
        }
        if (!threw) {
            fail("invalid opacity_threshold did not throw matcher_error");
        }
    }

    pal::PaletteDB db;
    try {
        db = pal::load_default_palette();
    } catch (const pal::palette_error& e) {
        fail(std::string("load_default_palette threw: ") + e.what());
    }
    mat::Matcher matcher(db, mat::default_options());

    // (4) NaN kd → matcher_error("non-finite ...").
    {
        msh::Material m{};
        m.name = "nan_kd";
        m.kd   = {std::nanf(""), 0.0f, 0.0f};
        m.d    = 1.0f;
        bool threw = false;
        try {
            (void)matcher.match(m);
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "non-finite", "nan-kd");
        }
        if (!threw) fail("NaN kd did not throw matcher_error");
    }

    // (5) NaN ke → matcher_error.
    {
        msh::Material m{};
        m.name = "nan_ke";
        m.kd   = {0.5f, 0.5f, 0.5f};
        m.ke   = {std::nanf(""), 0.0f, 0.0f};
        m.d    = 1.0f;
        bool threw = false;
        try {
            (void)matcher.match(m);
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "non-finite", "nan-ke");
        }
        if (!threw) fail("NaN ke did not throw matcher_error");
    }

    // (6) Inf d → matcher_error.
    {
        msh::Material m{};
        m.name = "inf_d";
        m.kd   = {0.5f, 0.5f, 0.5f};
        m.d    = std::numeric_limits<float>::infinity();
        bool threw = false;
        try {
            (void)matcher.match(m);
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "non-finite", "inf-d");
        }
        if (!threw) fail("Inf d did not throw matcher_error");
    }

    // (7) Batch span size mismatch.
    {
        std::vector<msh::Material> in(2);
        for (auto& m : in) {
            m.kd = {0.5f, 0.5f, 0.5f};
            m.d  = 1.0f;
        }
        std::vector<objtomc::chunk::BlockState> out(3);
        bool threw = false;
        try {
            matcher.match_batch(std::span<const msh::Material>(in),
                                std::span<objtomc::chunk::BlockState>(out));
        } catch (const mat::matcher_error& e) {
            threw = true;
            expect_message_contains(e.what(), "batch size mismatch",
                                    "batch-mismatch");
        }
        if (!threw) {
            fail("batch span size mismatch did not throw matcher_error");
        }
    }

    std::printf("PASS: matcher_errors (7 sub-cases)\n");
    return 0;
}
