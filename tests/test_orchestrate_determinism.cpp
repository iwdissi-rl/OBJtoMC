// orchestrate_determinism — proposal §3.13(e).
//
// Orchestrate the same OBJ + opts twice (into two temporary world dirs);
// assert the two region files are byte-identical. For level.dat, write
// directly via write_level_dat with deterministic now_unix_ms = 0 (since
// orch::orchestrate uses time(nullptr), the file-from-orchestrate would
// not be byte-stable across runs without this explicit pin).

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    mat.name = "M";
    mat.kd   = {0.5f, 0.5f, 0.5f};
    mat.d    = 1.0f;
    m.materials.push_back(mat);
    return m;
}

void write_obj(const fs::path& path, const msh::Mesh& m) {
    std::ofstream out(path);
    if (!out) fail("can't write temp OBJ");
    for (const auto& p : m.positions)
        out << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    for (const auto& f : m.faces) {
        out << "f " << (f.position_indices[0] + 1)
            << " "  << (f.position_indices[1] + 1)
            << " "  << (f.position_indices[2] + 1) << "\n";
    }
}

std::vector<unsigned char> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("can't read " + path.string());
    const auto size = in.tellg();
    in.seekg(0);
    std::vector<unsigned char> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

}  // namespace

int main() {
    const auto base = fs::temp_directory_path() /
        ("objtomc_det_" + std::to_string(std::rand()));
    fs::remove_all(base);
    const auto obj_path = base / "cube.obj";
    fs::create_directories(base);
    write_obj(obj_path, build_unit_cube());

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size = 0.125f;
    opts.level_name = "det-test";
    // Pin now_unix_s so both runs use the same chunk timestamps + level.dat
    // LastPlayed; otherwise the runs may straddle a wall-clock second boundary
    // and produce different timestamp tables.
    opts.now_unix_s = 0;

    const auto run_dir_a = base / "world_a";
    const auto run_dir_b = base / "world_b";

    try {
        orch::orchestrate(obj_path, run_dir_a, opts);
        orch::orchestrate(obj_path, run_dir_b, opts);
    } catch (const std::exception& e) {
        fail(std::string("orchestrate threw: ") + e.what());
    }

    // ---- region files must be byte-identical ----
    const auto mca_a = run_dir_a / "region" / "r.0.0.mca";
    const auto mca_b = run_dir_b / "region" / "r.0.0.mca";
    if (!fs::exists(mca_a) || !fs::exists(mca_b)) fail("missing region file");

    const auto bytes_a = read_file(mca_a);
    const auto bytes_b = read_file(mca_b);
    if (bytes_a != bytes_b) {
        fail("region files differ between runs (" + std::to_string(bytes_a.size()) +
             " vs " + std::to_string(bytes_b.size()) + " bytes)");
    }

    // ---- level.dat comparison via injected now_unix_ms = 0 ----
    const auto level_a = base / "level_a.dat";
    const auto level_b = base / "level_b.dat";
    orch::write_level_dat(opts, level_a, /*now_unix_ms=*/0);
    orch::write_level_dat(opts, level_b, /*now_unix_ms=*/0);

    if (read_file(level_a) != read_file(level_b)) {
        fail("level.dat differs between two write_level_dat(...) calls "
             "with identical opts + now_unix_ms");
    }

    const auto region_size = bytes_a.size();
    fs::remove_all(base);

    std::printf("PASS: orchestrate_determinism\n"
                "  region byte-identical: %zu bytes; level.dat byte-identical via pinned now_unix_ms=0\n",
                region_size);
    return 0;
}
