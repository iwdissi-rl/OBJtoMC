// orchestrate_cleanup_tiled_equality — v0.5 sub-item (a), Session 68.
//
// THE gate for the tiled-apron cleanup scheme (semantic_reconstruction
// proposal §2): a world produced with cleanup ON through the TILED
// pipeline must be byte-identical to cleanup ON through the MONOLITHIC
// pipeline. Cases are chosen so cleanup decisions cross tile borders:
//
//   A. bunny at world_origin {-20,-60,-20} (straddles the world (0,0)
//      region corner -> 4 tiles): thousands of pinhole fills, many at
//      tile boundaries.
//   B. synthetic scene at world_origin {-40,-60,-40}: a large plate
//      spanning all 4 tiles (survives), a 2-voxel crumb EXACTLY
//      straddling a tile boundary (must be removed identically on both
//      paths — the case the apron exists for), an interior 1-voxel
//      crumb (removed), and a 50-voxel floater (kept, > N=16).
//      Also asserts cleanup actually did something (cleaned world
//      differs from the uncleaned one).

#include "objtomc/orchestrate.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs   = std::filesystem;
namespace orch = objtomc::orch;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::vector<char> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) fail("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    return std::vector<char>(s.begin(), s.end());
}

std::set<fs::path> world_files(const fs::path& world) {
    std::set<fs::path> out;
    out.insert("level.dat");
    const fs::path region = world / "region";
    if (fs::exists(region)) {
        for (const auto& e : fs::directory_iterator(region)) {
            out.insert(fs::path("region") / e.path().filename());
        }
    }
    return out;
}

// Returns true iff the two worlds are byte-identical.
bool worlds_equal(const fs::path& a, const fs::path& b) {
    const auto fa = world_files(a);
    if (fa != world_files(b)) return false;
    for (const auto& rel : fa) {
        if (read_file(a / rel) != read_file(b / rel)) return false;
    }
    return true;
}

// Append an axis-aligned box (12 tris) to an OBJ stream.
void obj_box(std::ostream& out, int& base,
             float x0, float y0, float z0, float x1, float y1, float z1) {
    out << "v " << x0 << ' ' << y0 << ' ' << z0 << "\n"
        << "v " << x1 << ' ' << y0 << ' ' << z0 << "\n"
        << "v " << x0 << ' ' << y1 << ' ' << z0 << "\n"
        << "v " << x1 << ' ' << y1 << ' ' << z0 << "\n"
        << "v " << x0 << ' ' << y0 << ' ' << z1 << "\n"
        << "v " << x1 << ' ' << y0 << ' ' << z1 << "\n"
        << "v " << x0 << ' ' << y1 << ' ' << z1 << "\n"
        << "v " << x1 << ' ' << y1 << ' ' << z1 << "\n";
    static const int F[12][3] = {{1, 5, 2}, {2, 5, 6}, {1, 2, 4}, {1, 4, 3},
                                 {5, 7, 6}, {6, 7, 8}, {1, 3, 7}, {1, 7, 5},
                                 {2, 6, 8}, {2, 8, 4}, {3, 4, 8}, {3, 8, 7}};
    for (const auto& f : F) {
        out << "f " << base + f[0] << ' ' << base + f[1] << ' '
            << base + f[2] << "\n";
    }
    base += 8;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-bunny.obj>\n";
        return 2;
    }
    const fs::path base_dir =
        fs::temp_directory_path() /
        ("objtomc_cleanup_tiled_" + std::to_string(::getpid()));
    fs::remove_all(base_dir);
    fs::create_directories(base_dir);

    // --- Case A: bunny, 4 tiles, cleanup ON: tiled == monolithic -----------
    {
        orch::OrchestrateOptions o = orch::default_options();
        o.voxel_size      = 0.0024437f;
        o.world_origin    = {-20, -60, -20};
        o.spawn_pos       = o.world_origin;
        o.now_unix_s      = 0;
        o.cleanup.enabled = true;

        const fs::path mono = base_dir / "bunny_mono";
        const fs::path tiled = base_dir / "bunny_tiled";
        o.tiling = orch::OrchestrateOptions::TilingMode::OFF;
        orch::orchestrate(argv[1], mono, o);
        o.tiling = orch::OrchestrateOptions::TilingMode::ON;
        orch::orchestrate(argv[1], tiled, o);
        if (!worlds_equal(mono, tiled)) {
            fail("bunny: cleaned tiled world != cleaned monolithic world");
        }
        std::cout << "PASS: bunny 4-tile cleaned equality\n";
    }

    // --- Case B: synthetic boundary-crumb scene ----------------------------
    {
        const fs::path obj = base_dir / "scene.obj";
        {
            std::ofstream out(obj);
            int b = 0;
            // Plate spanning all 4 tiles (world -40..40 with origin -40).
            obj_box(out, b, 0, 0, 0, 80, 2, 80);
            // 2-voxel crumb straddling the world x=0 tile boundary
            // (mesh x 39..41 -> world x -1..1), floating above the plate.
            obj_box(out, b, 39.2f, 6.2f, 10.2f, 40.8f, 6.8f, 10.8f);
            // Interior 1-voxel crumb.
            obj_box(out, b, 10.2f, 6.2f, 30.2f, 10.8f, 6.8f, 30.8f);
            // 50-voxel floater (5x2x5) — survives N=16.
            obj_box(out, b, 50.2f, 6.2f, 50.2f, 54.8f, 7.8f, 54.8f);
        }
        orch::OrchestrateOptions o = orch::default_options();
        o.voxel_size      = 1.0f;
        o.world_origin    = {-40, -60, -40};
        o.spawn_pos       = o.world_origin;
        o.now_unix_s      = 0;
        o.cleanup.enabled = true;

        const fs::path mono  = base_dir / "scene_mono";
        const fs::path tiled = base_dir / "scene_tiled";
        const fs::path dirty = base_dir / "scene_dirty";
        o.tiling = orch::OrchestrateOptions::TilingMode::OFF;
        orch::orchestrate(obj, mono, o);
        o.tiling = orch::OrchestrateOptions::TilingMode::ON;
        orch::orchestrate(obj, tiled, o);
        if (!worlds_equal(mono, tiled)) {
            fail("synthetic: cleaned tiled world != cleaned monolithic world");
        }
        // Prove cleanup engaged: uncleaned world must differ.
        orch::OrchestrateOptions dirty_o = o;
        dirty_o.cleanup.enabled = false;
        dirty_o.tiling = orch::OrchestrateOptions::TilingMode::OFF;
        orch::orchestrate(obj, dirty, dirty_o);
        if (worlds_equal(mono, dirty)) {
            fail("synthetic: cleanup produced no change (crumbs not exercised)");
        }
        std::cout << "PASS: synthetic boundary-crumb cleaned equality "
                     "(+ cleanup provably engaged)\n";
    }

    fs::remove_all(base_dir);
    std::cout << "PASS: orchestrate_cleanup_tiled_equality\n";
    return 0;
}
