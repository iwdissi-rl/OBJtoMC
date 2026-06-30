// orchestrate_region_count — proposal §3.13(d).
//
// Predict the number of region files produced for synthetic meshes with
// controlled dimensions. 3 sub-cases:
//
//   (1) Single-voxel mesh at world_origin = {0, -60, 0} → chunks (0, 0)
//       → 1 region file (r.0.0.mca).
//   (2) 33-voxel-wide mesh in X straddling chunk-X boundaries
//       (world_origin = {0, -60, 0}, dim_x = 33) → chunks 0..2 in X →
//       still 1 region file (r.0.0.mca).
//   (3) 513-wide mesh in X (dim_x = 513) → chunks 0..32 in X (chunk 32
//       belongs to region 1) → 2 region files in X: r.0.0.mca + r.1.0.mca.
//
// Built by emitting synthetic narrow rod meshes inline.

#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

namespace fs   = std::filesystem;
namespace msh  = objtomc::mesh;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Build a thin rod of length `n` voxels along X at Y=0..1, Z=0..1,
// scaled by `voxel_size`. Two triangles per slice cover the rod's top face.
msh::Mesh build_rod(int n, float voxel_size) {
    msh::Mesh m;
    // Two horizontal triangles spanning the rod's top XY plane.
    const float xlen = static_cast<float>(n) * voxel_size;
    const float yh   = 1.0f * voxel_size;       // unit height in voxels
    m.positions = {
        {0.0f, 0.0f, 0.0f}, {xlen, 0.0f, 0.0f},
        {0.0f, yh,   0.0f}, {xlen, yh,   0.0f},
    };
    msh::Face f0{};
    f0.position_indices = {0, 1, 3};
    f0.normal_indices   = {-1, -1, -1};
    f0.uv_indices       = {-1, -1, -1};
    f0.material_id      = 0;
    m.faces.push_back(f0);

    msh::Face f1{};
    f1.position_indices = {0, 3, 2};
    f1.normal_indices   = {-1, -1, -1};
    f1.uv_indices       = {-1, -1, -1};
    f1.material_id      = 0;
    m.faces.push_back(f1);

    msh::Material mat{};
    mat.name = "RodMat";
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

std::set<std::string> region_filenames(const fs::path& world_dir) {
    std::set<std::string> out;
    const auto region_dir = world_dir / "region";
    if (!fs::is_directory(region_dir)) return out;
    for (const auto& entry : fs::directory_iterator(region_dir)) {
        if (entry.path().extension() == ".mca")
            out.insert(entry.path().filename().string());
    }
    return out;
}

// Run one sub-case.
void run_case(const std::string& label,
              int n_voxels_x,
              const std::set<std::string>& expected_regions) {
    const auto base = fs::temp_directory_path() /
        ("objtomc_rc_" + std::to_string(std::rand()));
    fs::remove_all(base);
    const auto obj_path  = base / "rod.obj";
    const auto world_dir = base / "world";
    fs::create_directories(base);

    constexpr float vs = 1.0f;       // 1 OBJ-unit = 1 voxel (= 1 block)
    write_obj(obj_path, build_rod(n_voxels_x, vs));

    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size              = vs;
    opts.voxel_grid_dimensions   = std::array<int, 3>{n_voxels_x, 1, 1};
    opts.voxel_grid_origin_world = std::array<float, 3>{0.0f, 0.0f, 0.0f};
    opts.world_origin            = {0, -60, 0};
    opts.spawn_pos               = {0, -60, 0};

    try {
        orch::orchestrate(obj_path, world_dir, opts);
    } catch (const std::exception& e) {
        fail(label + ": orchestrate threw: " + e.what());
    }

    const auto produced = region_filenames(world_dir);
    if (produced != expected_regions) {
        std::string got;
        for (const auto& s : produced) got += " " + s;
        std::string want;
        for (const auto& s : expected_regions) want += " " + s;
        fail(label + ": region files mismatch — expected:" + want + "  got:" + got);
    }
    std::printf("  case %s: produced %zu region(s)\n", label.c_str(), produced.size());

    fs::remove_all(base);
}

}  // namespace

int main() {
    // (1) 1-voxel mesh → chunks (0, 0) → r.0.0.mca only.
    run_case("1-voxel", 1, {"r.0.0.mca"});

    // (2) 33-voxel-wide mesh → chunks (0,0), (1,0), (2,0) → still 1 region.
    run_case("33-voxel-wide", 33, {"r.0.0.mca"});

    // (3) 513-wide → chunks 0..32 in X; chunk 32 is in region 1.
    //     Voxel x=513 means worlds X 0..512; chunk_x = floor(512/16) = 32.
    //     Region_x = floor(32 / 32) = 1 → both r.0.0.mca + r.1.0.mca.
    run_case("513-voxel-wide", 513, {"r.0.0.mca", "r.1.0.mca"});

    std::printf("PASS: orchestrate_region_count (3 sub-cases)\n");
    return 0;
}
