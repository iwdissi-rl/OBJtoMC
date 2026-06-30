// orchestrate_unit_cube_smoke — proposal §3.13(a).
//
// Orchestrate a synthetic 12-triangle unit cube end-to-end into a
// temporary world dir. Assert the world dir exists + level.dat is present
// (gzip-detectable from first bytes) + r.0.0.mca is present with sensible
// size. NO MC client check — that's §3.13(g)'s Tier-4 manual gate.

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

msh::Mesh build_unit_cube() {
    msh::Mesh m;
    m.positions = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f},
    };
    const std::array<std::array<std::uint32_t, 3>, 12> tris = {{
        {0, 4, 1}, {1, 4, 5},
        {0, 1, 3}, {0, 3, 2},
        {4, 6, 5}, {5, 6, 7},
        {0, 2, 6}, {0, 6, 4},
        {1, 5, 7}, {1, 7, 3},
        {2, 3, 7}, {2, 7, 6},
    }};
    for (const auto& t : tris) {
        msh::Face f{};
        f.position_indices = t;
        f.normal_indices   = {-1, -1, -1};
        f.uv_indices       = {-1, -1, -1};
        f.material_id      = 0;
        m.faces.push_back(f);
    }
    msh::Material mat{};
    mat.name = "CubeMat";
    mat.kd   = {0.5f, 0.5f, 0.5f};
    mat.d    = 1.0f;
    m.materials.push_back(mat);
    return m;
}

void write_temp_obj(const fs::path& path, const msh::Mesh& m) {
    std::ofstream out(path);
    if (!out) fail("can't open temp OBJ for writing");
    for (const auto& p : m.positions)
        out << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    for (const auto& f : m.faces) {
        out << "f " << (f.position_indices[0] + 1)
            << " "  << (f.position_indices[1] + 1)
            << " "  << (f.position_indices[2] + 1) << "\n";
    }
}

}  // namespace

int main() {
    // Set up a fresh tmpdir.
    const auto base = fs::temp_directory_path() /
        ("objtomc_smoke_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    const auto obj_path   = base / "cube.obj";
    const auto world_dir  = base / "world";
    write_temp_obj(obj_path, build_unit_cube());

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size = 0.125f;     // 8x8x8 grid

    try {
        orch::orchestrate(obj_path, world_dir, opts);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }

    // ---- Assertions ----
    if (!fs::exists(world_dir)) fail("world_dir not created");
    if (!fs::is_directory(world_dir)) fail("world_dir is not a directory");

    const auto level_dat = world_dir / "level.dat";
    if (!fs::exists(level_dat)) fail("level.dat missing");
    if (fs::file_size(level_dat) < 200) fail("level.dat suspiciously small");

    // First 2 bytes should be the gzip magic 0x1F 0x8B.
    {
        std::ifstream in(level_dat, std::ios::binary);
        unsigned char buf[2] = {0, 0};
        in.read(reinterpret_cast<char*>(buf), 2);
        if (buf[0] != 0x1F || buf[1] != 0x8B) {
            fail("level.dat does not start with gzip magic (0x1F 0x8B); got " +
                 std::to_string(buf[0]) + ", " + std::to_string(buf[1]));
        }
    }

    const auto region_dir = world_dir / "region";
    if (!fs::is_directory(region_dir)) fail("region/ subdir missing");

    const auto r000 = region_dir / "r.0.0.mca";
    if (!fs::exists(r000)) fail("region/r.0.0.mca missing");
    // Region file must be at least 8192 bytes (the empty-region size).
    if (fs::file_size(r000) < 8192) fail("region file too small (< 8192 bytes)");

    // No spurious other region files.
    std::size_t mca_count = 0;
    for (const auto& entry : fs::directory_iterator(region_dir)) {
        if (entry.path().extension() == ".mca") ++mca_count;
    }
    if (mca_count != 1) {
        fail("expected exactly 1 .mca file, got " + std::to_string(mca_count));
    }

    const std::size_t ld_size = static_cast<std::size_t>(fs::file_size(level_dat));
    const std::size_t mca_size = static_cast<std::size_t>(fs::file_size(r000));
    fs::remove_all(base);
    std::printf("PASS: orchestrate_unit_cube_smoke\n"
                "  world_dir+level.dat+region/r.0.0.mca all present\n"
                "  level.dat size=%zu region size=%zu\n",
                ld_size, mca_size);
    return 0;
}
