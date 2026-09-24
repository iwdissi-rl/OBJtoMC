// orchestrate_extended_height — Session 63 (v0.4 item #2) end-to-end gate.
//
// 1. A 1×500×1 column mesh at default world_origin {0,-60,0} needs Y
//    [-60, 440) — beyond vanilla — so AUTO must resolve WorldHeight
//    {-64, 512} (smallest multiple-of-16 extension containing both the
//    vanilla span and the model): the world gains
//    datapacks/objtomc_height/{pack.mcmeta (pack_format 71),
//    data/minecraft/dimension_type/overworld.json (height 512, min_y
//    -64)}, level.dat's DataPacks.Enabled lists "file/objtomc_height",
//    and the chunk NBT's yPos is -4 (min_y unchanged).
// 2. The same mesh with an EXPLICIT WorldHeight{-2032, 4064} produces
//    yPos -127 chunks and a 4064/−2032 overworld.json.
// 3. A small default-fitting cube produces NO datapacks dir and NO pack
//    entry — the pre-63 world layout, byte-stability guarded by the rest
//    of the suite.

#include "objtomc/chunk.hpp"
#include "objtomc/orchestrate.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs   = std::filesystem;
namespace orch = objtomc::orch;
namespace chk  = objtomc::chunk;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) fail("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// gunzip a whole file (level.dat) into a byte string.
std::string gunzip_file(const fs::path& p) {
    const std::string raw = read_text(p);
    std::string out(1u << 20, '\0');
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) fail("inflateInit2");
    zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
    zs.avail_in  = static_cast<uInt>(raw.size());
    zs.next_out  = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());
    const int rc = inflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) fail("inflate rc=" + std::to_string(rc));
    out.resize(zs.total_out);
    inflateEnd(&zs);
    return out;
}

// Decompress the first chunk of a region file and return its raw NBT.
std::string first_chunk_nbt(const fs::path& region) {
    const std::string raw = read_text(region);
    for (std::size_t ci = 0; ci < 1024; ++ci) {
        const std::size_t off =
            (static_cast<std::size_t>(static_cast<unsigned char>(raw[ci * 4])) << 16) |
            (static_cast<std::size_t>(static_cast<unsigned char>(raw[ci * 4 + 1])) << 8) |
            static_cast<std::size_t>(static_cast<unsigned char>(raw[ci * 4 + 2]));
        if (off == 0) continue;
        const std::size_t base = off * 4096;
        std::string out(1u << 24, '\0');
        z_stream zs{};
        if (inflateInit(&zs) != Z_OK) fail("inflateInit");
        zs.next_in   = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data() + base + 5));
        zs.avail_in  = static_cast<uInt>(raw.size() - base - 5);
        zs.next_out  = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        const int rc = inflate(&zs, Z_FINISH);
        if (rc != Z_STREAM_END) fail("chunk inflate rc=" + std::to_string(rc));
        out.resize(zs.total_out);
        inflateEnd(&zs);
        return out;
    }
    fail("no chunk in " + region.string());
}

// Find the big-endian int following the NBT name "yPos".
std::int32_t chunk_ypos(const std::string& nbt) {
    const std::string key = "yPos";
    const auto pos = nbt.find(key);
    if (pos == std::string::npos) fail("yPos not found in chunk NBT");
    const std::size_t v = pos + key.size();
    return static_cast<std::int32_t>(
        (static_cast<std::uint32_t>(static_cast<unsigned char>(nbt[v]))     << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(nbt[v + 1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(nbt[v + 2])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(nbt[v + 3])));
}

fs::path write_column_obj(const fs::path& dir, float height) {
    const fs::path p = dir / "column.obj";
    std::ofstream out(p);
    out << "v 0 0 0\nv 1 0 0\nv 0 " << height << " 0\nv 1 " << height
        << " 0\nv 0 0 1\nv 1 0 1\nv 0 " << height << " 1\nv 1 " << height
        << " 1\n"
        << "f 1 5 2\nf 2 5 6\nf 1 2 4\nf 1 4 3\nf 5 7 6\nf 6 7 8\n"
        << "f 1 3 7\nf 1 7 5\nf 2 6 8\nf 2 8 4\nf 3 4 8\nf 3 8 7\n";
    return p;
}

}  // namespace

int main() {
    const fs::path base =
        fs::temp_directory_path() /
        ("objtomc_extheight_" + std::to_string(::getpid()));
    fs::remove_all(base);
    fs::create_directories(base);

    const fs::path tall_obj = write_column_obj(base, 500.0f);

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size = 1.0f;
    opts.now_unix_s = 0;

    // --- 1. AUTO extension -------------------------------------------------
    {
        const fs::path world = base / "w_auto";
        orch::orchestrate(tall_obj, world, opts);

        const fs::path pack = world / "datapacks" / "objtomc_height";
        const std::string mcmeta = read_text(pack / "pack.mcmeta");
        if (mcmeta.find("\"pack_format\": 71") == std::string::npos) {
            fail("pack.mcmeta missing pack_format 71");
        }
        const std::string dim = read_text(
            pack / "data" / "minecraft" / "dimension_type" / "overworld.json");
        if (dim.find("\"height\": 512") == std::string::npos ||
            dim.find("\"min_y\": -64") == std::string::npos ||
            dim.find("\"logical_height\": 512") == std::string::npos) {
            fail("overworld.json wrong height trio for AUTO {-64,512}:\n" + dim);
        }
        const std::string level = gunzip_file(world / "level.dat");
        if (level.find("file/objtomc_height") == std::string::npos) {
            fail("level.dat missing file/objtomc_height in DataPacks");
        }
        if (chunk_ypos(first_chunk_nbt(world / "region" / "r.0.0.mca")) != -4) {
            fail("AUTO chunk yPos != -4");
        }
        std::cout << "PASS: AUTO extension to {-64, 512} (datapack + "
                     "level.dat entry + yPos -4)\n";
    }

    // --- 2. explicit ground-truth dimensions -------------------------------
    {
        const fs::path world = base / "w_4064";
        orch::OrchestrateOptions o = opts;
        o.world_height = chk::WorldHeight{-2032, 4064};
        orch::orchestrate(tall_obj, world, o);
        const std::string dim = read_text(
            world / "datapacks" / "objtomc_height" / "data" / "minecraft" /
            "dimension_type" / "overworld.json");
        if (dim.find("\"height\": 4064") == std::string::npos ||
            dim.find("\"min_y\": -2032") == std::string::npos) {
            fail("overworld.json wrong for explicit {-2032,4064}");
        }
        if (chunk_ypos(first_chunk_nbt(world / "region" / "r.0.0.mca")) != -127) {
            fail("explicit-4064 chunk yPos != -127");
        }
        std::cout << "PASS: explicit {-2032, 4064} (yPos -127, ground-truth "
                     "dimensions)\n";
    }

    // --- 3. default-fitting mesh: pre-63 layout ----------------------------
    {
        const fs::path small_obj = write_column_obj(base, 4.0f);
        const fs::path world = base / "w_default";
        orch::orchestrate(small_obj, world, opts);
        if (fs::exists(world / "datapacks")) {
            fail("default-fitting world grew a datapacks dir");
        }
        const std::string level = gunzip_file(world / "level.dat");
        if (level.find("objtomc_height") != std::string::npos) {
            fail("default-fitting level.dat mentions objtomc_height");
        }
        if (chunk_ypos(first_chunk_nbt(world / "region" / "r.0.0.mca")) != -4) {
            fail("default chunk yPos != -4");
        }
        std::cout << "PASS: default-fitting world has the pre-63 layout\n";
    }

    fs::remove_all(base);
    std::cout << "PASS: orchestrate_extended_height\n";
    return 0;
}
