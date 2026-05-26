#include "objtomc/nbt.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t kExpected[] = {
    // Root TAG_Compound, name = ""
    0x0A,
    0x00, 0x00,

    // TAG_Int "DataVersion" = 4325
    0x03,
    0x00, 0x0B,
    0x44, 0x61, 0x74, 0x61, 0x56, 0x65, 0x72, 0x73, 0x69, 0x6F, 0x6E,   // "DataVersion"
    0x00, 0x00, 0x10, 0xE5,                                              // 4325

    // TAG_String "Status" = "minecraft:full"
    0x08,
    0x00, 0x06,
    0x53, 0x74, 0x61, 0x74, 0x75, 0x73,                                  // "Status"
    0x00, 0x0E,
    0x6D, 0x69, 0x6E, 0x65, 0x63, 0x72, 0x61,
    0x66, 0x74, 0x3A, 0x66, 0x75, 0x6C, 0x6C,                            // "minecraft:full"

    // TAG_List of TAG_Long "Position" = [42, -7]
    0x09,
    0x00, 0x08,
    0x50, 0x6F, 0x73, 0x69, 0x74, 0x69, 0x6F, 0x6E,                      // "Position"
    0x04,                                                                 // element type = TAG_Long
    0x00, 0x00, 0x00, 0x02,                                              // list length = 2
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,                      //  42
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF9,                      // -7

    // TAG_End of root compound
    0x00,
};

static_assert(sizeof(kExpected) == 79,
              "expected fixture must be exactly 79 bytes per proposal §5");

void dump_hex_diff(const std::vector<std::uint8_t>& actual,
                   const std::uint8_t* expected,
                   std::size_t expected_len) {
    const std::size_t max_len =
        actual.size() > expected_len ? actual.size() : expected_len;
    std::printf("  offset  actual  expected  diff\n");
    for (std::size_t i = 0; i < max_len; ++i) {
        const bool has_a = (i < actual.size());
        const bool has_e = (i < expected_len);
        char a_str[4] = "--";
        char e_str[4] = "--";
        if (has_a) {
            std::snprintf(a_str, sizeof(a_str), "%02X",
                          static_cast<unsigned int>(actual[i]));
        }
        if (has_e) {
            std::snprintf(e_str, sizeof(e_str), "%02X",
                          static_cast<unsigned int>(expected[i]));
        }
        const bool mismatch = (has_a != has_e) ||
                              (has_a && has_e && actual[i] != expected[i]);
        std::printf("  0x%04zx  %s      %s        %s\n",
                    i, a_str, e_str, mismatch ? "<-- DIFF" : "");
    }
    std::printf("  actual length = %zu, expected length = %zu\n",
                actual.size(), expected_len);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace objtomc::nbt;

    Compound root;
    root.put_int   ("DataVersion", 4325);
    root.put_string("Status",      "minecraft:full");

    List positions(TagType::Long);
    positions.push_long( 42);
    positions.push_long(-7);
    root.put_list("Position", std::move(positions));

    const std::vector<std::uint8_t> bytes =
        to_bytes("", Tag(std::move(root)));

    if (argc == 3 && std::string_view(argv[1]) == "--dump") {
        std::ofstream out(argv[2], std::ios::binary);
        if (!out) {
            std::fprintf(stderr,
                "ERROR: failed to open %s for writing\n", argv[2]);
            return 2;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::fprintf(stderr, "ERROR: write to %s failed\n", argv[2]);
            return 2;
        }
        return 0;
    }

    if (argc != 1) {
        std::fprintf(stderr,
            "usage:\n"
            "  %s                  (in-memory assertion against the "
            "proposal §5 fixture)\n"
            "  %s --dump <path>    (write produced bytes to <path>; "
            "no assertion)\n",
            argv[0], argv[0]);
        return 2;
    }

    const bool size_match = (bytes.size() == sizeof(kExpected));
    const bool content_match = size_match &&
        std::equal(bytes.begin(), bytes.end(),
                   std::begin(kExpected), std::end(kExpected));

    if (!content_match) {
        std::printf(
            "FAIL: nbt_worked_example produced bytes that do not match the "
            "proposal §5 fixture.\n");
        dump_hex_diff(bytes, kExpected, sizeof(kExpected));
        assert(content_match);
        return 1;
    }

    std::printf(
        "PASS: nbt_worked_example produced %zu bytes matching the §5 "
        "fixture exactly.\n",
        bytes.size());
    return 0;
}
