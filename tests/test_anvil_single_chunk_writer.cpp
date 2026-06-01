// Helper binary for the anvil_single_chunk_roundtrip oracle test.
//
// CLI:
//   --chunk-nbt <in>   path to a file containing raw decompressed NBT bytes
//                      for one chunk (the test extracts this from the
//                      fixture using zlib.decompress on the raw sector
//                      bytes; see tests/oracle/test_anvil_single_chunk_
//                      roundtrip.py).
//   --out <out>        path where the produced .mca region is written.
//
// Builds a one-chunk RegionWriter at (0, 0), adds the chunk via
// add_chunk(0, 0, std::move(bytes)), and writes the region to disk
// using nbt::OstreamWriter over an std::ofstream in binary mode.
// Verifies the resulting file is non-empty and 4 KiB-aligned.

#include "objtomc/anvil.hpp"
#include "objtomc/nbt.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --chunk-nbt <in> --out <out.mca>\n",
        argv0);
}

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open %s for reading\n", path);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* chunk_nbt_path = nullptr;
    const char* out_path       = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--chunk-nbt" && i + 1 < argc) {
            chunk_nbt_path = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::fprintf(stderr, "ERROR: unknown arg %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (chunk_nbt_path == nullptr || out_path == nullptr) {
        usage(argv[0]);
        return 2;
    }

    std::vector<std::uint8_t> nbt_bytes;
    if (!read_file(chunk_nbt_path, nbt_bytes)) {
        return 2;
    }
    if (nbt_bytes.empty()) {
        std::fprintf(stderr,
            "ERROR: input file %s is empty\n", chunk_nbt_path);
        return 2;
    }

    try {
        objtomc::anvil::RegionWriter region(0, 0);
        region.add_chunk(0, 0, std::move(nbt_bytes));

        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr,
                "ERROR: cannot open %s for writing\n", out_path);
            return 2;
        }
        objtomc::nbt::OstreamWriter sink(out);
        region.write(sink);
        out.close();

        std::ifstream check(out_path, std::ios::binary | std::ios::ate);
        if (!check) {
            std::fprintf(stderr,
                "ERROR: cannot reopen %s for size check\n", out_path);
            return 2;
        }
        const auto file_size =
            static_cast<std::size_t>(check.tellg());
        if (file_size == 0u ||
            file_size % objtomc::anvil::SECTOR_SIZE != 0u) {
            std::fprintf(stderr,
                "ERROR: %s is %zu bytes (must be a positive multiple of "
                "%zu)\n",
                out_path, file_size,
                objtomc::anvil::SECTOR_SIZE);
            return 1;
        }

        std::printf("PASS: wrote %zu bytes (%zu sectors) to %s\n",
                    file_size,
                    file_size / objtomc::anvil::SECTOR_SIZE,
                    out_path);
        return 0;
    } catch (const objtomc::anvil::anvil_error& e) {
        std::fprintf(stderr, "ERROR: anvil_error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
