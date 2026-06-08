// Helper binary for the anvil_full_region_roundtrip oracle (tier 3).
//
// CLI:
//   --chunks-manifest <path>  tab-separated, one chunk per line:
//                             chunk_x\tchunk_z\tabs_path_to_decompressed_nbt
//   --out <path>              destination .mca path
//
// Reads the manifest, pre-loads every chunk's NBT bytes from disk, then
// builds ONE RegionWriter(0, 0), calls add_chunk for each entry, and
// writes the region via nbt::OstreamWriter over std::ofstream. The
// wall-clock window covers add_chunk + write() only — manifest parsing
// and per-chunk file I/O happen before the clock starts, per the
// Session-13 prompt's "honest timing" constraint.
//
// On success, prints:
//   chunks written, output file size (bytes + sectors),
//   compressed payload bytes (sum of declared lengths minus scheme bytes),
//   wall-clock seconds.
//
// On add_chunk failure (out-of-region coord, duplicate, sealed, etc.),
// reports the (chunk_x, chunk_z) that triggered it and exits non-zero
// without silently skipping.

#include "objtomc/anvil.hpp"
#include "objtomc/nbt.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --chunks-manifest <path> --out <out.mca>\n", argv0);
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

struct ManifestEntry {
    std::int32_t              chunk_x;
    std::int32_t              chunk_z;
    std::vector<std::uint8_t> nbt_bytes;
};

bool parse_manifest(const char* path,
                    std::vector<ManifestEntry>& out) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "ERROR: cannot open manifest %s\n", path);
        return false;
    }
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (line.empty()) {
            continue;
        }
        const std::size_t t1 = line.find('\t');
        if (t1 == std::string::npos) {
            std::fprintf(stderr,
                "ERROR: manifest line %zu missing first tab: %s\n",
                line_no, line.c_str());
            return false;
        }
        const std::size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) {
            std::fprintf(stderr,
                "ERROR: manifest line %zu missing second tab: %s\n",
                line_no, line.c_str());
            return false;
        }

        ManifestEntry e;
        try {
            e.chunk_x = static_cast<std::int32_t>(
                std::stoi(line.substr(0, t1)));
            e.chunk_z = static_cast<std::int32_t>(
                std::stoi(line.substr(t1 + 1, t2 - t1 - 1)));
        } catch (const std::exception& ex) {
            std::fprintf(stderr,
                "ERROR: manifest line %zu coord parse failed: %s\n",
                line_no, ex.what());
            return false;
        }

        const std::string nbt_path = line.substr(t2 + 1);
        if (!read_file(nbt_path.c_str(), e.nbt_bytes)) {
            return false;
        }
        if (e.nbt_bytes.empty()) {
            std::fprintf(stderr,
                "ERROR: manifest line %zu NBT file %s is empty\n",
                line_no, nbt_path.c_str());
            return false;
        }

        out.push_back(std::move(e));
    }
    return true;
}

// Sum the declared compressed-payload lengths from the OUTPUT file's
// location table. declared_length includes the 1-byte compression scheme;
// the compressed payload itself is (declared_length - 1) bytes. Sums
// only populated entries.
bool sum_compressed_payload(const char* path, std::size_t& total_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr,
            "ERROR: cannot reopen %s for compressed-payload sum\n", path);
        return false;
    }
    std::vector<std::uint8_t> all{
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>{}};
    if (all.size() < 8192) {
        std::fprintf(stderr,
            "ERROR: %s is shorter than 8192 bytes\n", path);
        return false;
    }

    std::size_t total = 0;
    for (std::size_t i = 0; i < 1024; ++i) {
        const std::size_t off = i * 4;
        const std::uint32_t sector_offset =
            (static_cast<std::uint32_t>(all[off    ]) << 16) |
            (static_cast<std::uint32_t>(all[off + 1]) <<  8) |
             static_cast<std::uint32_t>(all[off + 2]);
        const std::uint8_t sector_count = all[off + 3];
        if (sector_offset == 0 && sector_count == 0) {
            continue;
        }
        const std::size_t sb =
            static_cast<std::size_t>(sector_offset) * 4096u;
        if (sb + 5 > all.size()) {
            std::fprintf(stderr,
                "ERROR: chunk slot %zu sector starts past EOF\n", i);
            return false;
        }
        const std::uint32_t declared_length =
            (static_cast<std::uint32_t>(all[sb    ]) << 24) |
            (static_cast<std::uint32_t>(all[sb + 1]) << 16) |
            (static_cast<std::uint32_t>(all[sb + 2]) <<  8) |
             static_cast<std::uint32_t>(all[sb + 3]);
        if (declared_length == 0) {
            std::fprintf(stderr,
                "ERROR: chunk slot %zu declared_length=0\n", i);
            return false;
        }
        total += static_cast<std::size_t>(declared_length) - 1u;
    }
    total_out = total;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* manifest_path = nullptr;
    const char* out_path      = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--chunks-manifest" && i + 1 < argc) {
            manifest_path = argv[++i];
        } else if (a == "--out" && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            std::fprintf(stderr, "ERROR: unknown arg %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (manifest_path == nullptr || out_path == nullptr) {
        usage(argv[0]);
        return 2;
    }

    std::vector<ManifestEntry> entries;
    if (!parse_manifest(manifest_path, entries)) {
        return 2;
    }
    if (entries.empty()) {
        std::fprintf(stderr,
            "ERROR: manifest %s has no entries\n", manifest_path);
        return 2;
    }

    try {
        objtomc::anvil::RegionWriter region(0, 0);

        // Honest timing window: add_chunk loop + write() only.
        const auto t0 = std::chrono::steady_clock::now();

        for (auto& e : entries) {
            try {
                region.add_chunk(e.chunk_x, e.chunk_z,
                                 std::move(e.nbt_bytes));
            } catch (const objtomc::anvil::anvil_error& ex) {
                std::fprintf(stderr,
                    "ERROR: add_chunk(%d, %d) threw: %s\n",
                    e.chunk_x, e.chunk_z, ex.what());
                return 1;
            }
        }

        if (region.chunk_count() != entries.size()) {
            std::fprintf(stderr,
                "ERROR: chunk_count()=%zu after loop, expected %zu\n",
                region.chunk_count(), entries.size());
            return 1;
        }

        std::ofstream out(out_path, std::ios::binary);
        if (!out) {
            std::fprintf(stderr,
                "ERROR: cannot open %s for writing\n", out_path);
            return 2;
        }
        objtomc::nbt::OstreamWriter sink(out);
        region.write(sink);
        out.close();

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsed_s =
            std::chrono::duration<double>(t1 - t0).count();

        // Post-write stats (outside the timed window).
        std::ifstream check(out_path, std::ios::binary | std::ios::ate);
        if (!check) {
            std::fprintf(stderr,
                "ERROR: cannot reopen %s for size check\n", out_path);
            return 2;
        }
        const auto file_size = static_cast<std::size_t>(check.tellg());
        check.close();
        if (file_size == 0u ||
            file_size % objtomc::anvil::SECTOR_SIZE != 0u) {
            std::fprintf(stderr,
                "ERROR: %s is %zu bytes (must be positive multiple of "
                "%zu)\n",
                out_path, file_size, objtomc::anvil::SECTOR_SIZE);
            return 1;
        }

        std::size_t compressed_payload_bytes = 0;
        if (!sum_compressed_payload(out_path,
                                    compressed_payload_bytes)) {
            return 1;
        }

        std::printf("PASS: wrote %zu chunks to %s\n",
                    entries.size(), out_path);
        std::printf("  output size       : %zu bytes (%zu sectors of %zu)\n",
                    file_size,
                    file_size / objtomc::anvil::SECTOR_SIZE,
                    objtomc::anvil::SECTOR_SIZE);
        std::printf("  compressed payload: %zu bytes "
                    "(sum of declared lengths minus scheme bytes)\n",
                    compressed_payload_bytes);
        std::printf("  wall-clock        : %.3f s "
                    "(add_chunk + write only)\n",
                    elapsed_s);
        return 0;
    } catch (const objtomc::anvil::anvil_error& e) {
        std::fprintf(stderr, "ERROR: anvil_error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
