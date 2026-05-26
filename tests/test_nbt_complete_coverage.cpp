#include "objtomc/nbt.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

objtomc::nbt::Compound build_coverage_tree() {
    using namespace objtomc::nbt;

    Compound root;
    root.put_byte  ("ByteVal",   static_cast<std::int8_t>(-128));
    root.put_short ("ShortVal",  static_cast<std::int16_t>(32767));
    root.put_int   ("IntVal",    2147483647);
    root.put_long  ("LongVal",   -9223372036854775807LL);
    root.put_float ("FloatVal",  3.14f);
    root.put_double("DoubleVal", 2.718281828459045);
    root.put_byte_array(
        "ByteArr",
        std::vector<std::int8_t>{
            static_cast<std::int8_t>(0x01),
            static_cast<std::int8_t>(0x02),
            static_cast<std::int8_t>(0x03),
        });
    root.put_string("StrVal", "hello");

    List list_val(TagType::Int);
    list_val.push_int(1);
    list_val.push_int(2);
    list_val.push_int(3);
    root.put_list("ListVal", std::move(list_val));

    Compound nested;
    nested.put_int("Nested", 42);
    root.put_compound("CompoundVal", std::move(nested));

    root.put_int_array("IntArr",
        std::vector<std::int32_t>{-1, 0, 1});
    root.put_long_array("LongArr",
        std::vector<std::int64_t>{100LL, 200LL, 300LL});

    return root;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace objtomc::nbt;

    Compound root = build_coverage_tree();
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
            "  %s                  (in-memory smoke check)\n"
            "  %s --dump <path>    (write produced bytes to <path>)\n",
            argv[0], argv[0]);
        return 2;
    }

    // In-memory smoke checks only.  Per research/nbt_api_proposal.md §9 a
    // hand-computed byte fixture across 13 tag types would be too brittle;
    // logical correctness is verified by tests/oracle/test_nbt_complete_coverage.py.
    if (bytes.empty()) {
        std::fprintf(stderr, "FAIL: writer produced empty output\n");
        return 1;
    }
    if (bytes.front() != 0x0A) {
        std::fprintf(stderr,
            "FAIL: first byte is 0x%02X, expected 0x0A (TAG_Compound)\n",
            static_cast<unsigned int>(bytes.front()));
        return 1;
    }
    if (bytes.back() != 0x00) {
        std::fprintf(stderr,
            "FAIL: last byte is 0x%02X, expected 0x00 (TAG_End)\n",
            static_cast<unsigned int>(bytes.back()));
        return 1;
    }

    std::printf(
        "PASS: nbt_complete_coverage produced %zu bytes; envelope is "
        "0x0A ... 0x00 as expected.\n",
        bytes.size());
    std::printf(
        "      (Logical correctness verified by "
        "nbt_complete_coverage_oracle.)\n");
    return 0;
}
