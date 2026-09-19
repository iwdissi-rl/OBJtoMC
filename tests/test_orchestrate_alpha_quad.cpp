// orchestrate_alpha_quad — Session 52 per-voxel BLEND alpha
// (baseColorTexture.a) sampling integration test + oracle helper.
//
// Fixture: tests/fixtures/texture/alpha.glb — the 4×4 quad with alphaMode
// BLEND, NO baseColorFactor (defaults [1,1,1,1] — the Sketchfab-standard
// omission Session 52 exists to fix: AlphaBlendModeTest.glb's MatBlend
// rendered fully opaque because its transparency lives entirely in
// baseColorTexture.a), and an embedded 2×1 RGBA texture: left texel
// opaque red (255,0,0,255), right translucent blue (0,0,255,64). With
// u = x/4 and the pinned quad options, voxels vx<2 sample the left texel
// (per-voxel d = 1 → opaque branch, red) and vx≥2 the right texel
// (d = 64/255 ≈ 0.251 < opacity_threshold 0.5 → transparent branch,
// blue).
//
// CTest assertions (shape level — block-exact checks are the oracle's job):
//   0. Loader mapping: load_gltf(alpha.glb) exposes d_from_map_alpha ==
//      true, flat d == 1 (factor alpha defaults 1), and non-empty
//      map_kd_bytes on the single material.
//   1. Branch flip: matcher::match(kd=blue, d=64/255) picks a DIFFERENT
//      block than match(kd=blue, d=1) — proof the low alpha observably
//      flips the transparent branch, so the oracle's block-exact match
//      against d=64/255 predictions can only pass if the pipeline
//      actually multiplied the texel alpha in.
//   2. Determinism: two runs produce byte-identical r.0.0.mca.
//
// Oracle protocol: prints "EXPECT <vx> <vz> <block_name>" for all 16
// voxels, computed independently via matcher::match on Materials whose kd
// is the exact sampled texel colour (red vx<2, blue vx≥2) and whose d is
// 1.0 (vx<2) or 64/255 (vx≥2) — the exact factor × texel product for a
// unit factor. If argv[2] is given the first world is written there and
// kept for the oracle to parse.

#include "objtomc/gltf.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/orchestrate.hpp"
#include "objtomc/palette.hpp"

#include <array>
#include <cmath>
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

constexpr float kRightAlpha = 64.0f / 255.0f;

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

std::vector<char> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) fail("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

orch::OrchestrateOptions quad_options() {
    orch::OrchestrateOptions opts = orch::default_options();
    opts.voxel_size               = 1.0f;
    opts.voxel_grid_dimensions    = {{4, 1, 4}};
    opts.voxel_grid_origin_world  = {{0.0f, 0.0f, 0.0f}};
    opts.world_origin             = {0, -60, 0};
    opts.spawn_pos                = {2, -58, 2};
    opts.level_name               = "objtomc-alpha-quad";
    opts.now_unix_s               = 0;  // byte-determinism
    return opts;
}

msh::Material voxel_material(int vx) {
    msh::Material mat;
    mat.name  = "quadmat";
    mat.kd    = (vx < 2) ? std::array<float, 3>{1.0f, 0.0f, 0.0f}
                         : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    mat.ke    = {0.0f, 0.0f, 0.0f};
    mat.ks    = {0.0f, 0.0f, 0.0f};
    mat.d     = (vx < 2) ? 1.0f : kRightAlpha;
    mat.illum = 2;
    return mat;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <path-to-alpha.glb> [keep-world-dir]\n";
        return 2;
    }
    const fs::path alpha_glb = argv[1];
    const auto opts = quad_options();

    // ---- 0. loader mapping ----
    {
        const msh::Mesh m = objtomc::gltf::load_gltf(alpha_glb);
        if (m.materials.size() != 1) {
            fail("alpha.glb: expected 1 material, got " +
                 std::to_string(m.materials.size()));
        }
        const msh::Material& mat = m.materials[0];
        if (!mat.d_from_map_alpha) {
            fail("alpha.glb: d_from_map_alpha is false — BLEND + "
                 "baseColorTexture was not flagged");
        }
        if (mat.map_kd_bytes.empty()) {
            fail("alpha.glb: map_kd_bytes is empty — baseColorTexture "
                 "was not extracted");
        }
        if (std::fabs(mat.d - 1.0f) > 1e-6f) {
            fail("alpha.glb: flat d = " + std::to_string(mat.d) +
                 ", expected 1 (baseColorFactor omitted defaults alpha 1)");
        }
    }

    // ---- 1. branch flip self-check ----
    const auto db = objtomc::palette::load_default_palette();
    const objtomc::matcher::Matcher m(db, opts.matcher_opts);
    {
        msh::Material low  = voxel_material(2);   // blue, d = 64/255
        msh::Material full = voxel_material(2);
        full.d = 1.0f;
        if (m.match(low).name == m.match(full).name) {
            fail("matcher maps blue at d=64/255 and d=1 to the same "
                 "block — the oracle could not detect a missing alpha "
                 "multiply");
        }
    }

    const auto base = fs::temp_directory_path() /
        ("objtomc_alphaquad_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);

    // ---- 2. determinism ----
    const fs::path world_a = (argc == 3) ? fs::path(argv[2]) : base / "world_a";
    const fs::path world_b = base / "world_b";
    try {
        orch::orchestrate(alpha_glb, world_a, opts);
        orch::orchestrate(alpha_glb, world_b, opts);
    } catch (const std::exception& e) {
        fail(std::string("alpha orchestrate threw: ") + e.what());
    }
    const auto mca_a = read_bytes(world_a / "region" / "r.0.0.mca");
    const auto mca_b = read_bytes(world_b / "region" / "r.0.0.mca");
    if (mca_a != mca_b) fail("alpha runs are not byte-identical");

    // ---- oracle EXPECT lines ----
    for (int vz = 0; vz < 4; ++vz) {
        for (int vx = 0; vx < 4; ++vx) {
            const auto bs = m.match(voxel_material(vx));
            std::printf("EXPECT %d %d %s\n", vx, vz, bs.name.c_str());
        }
    }

    fs::remove_all(base);
    std::printf("PASS: orchestrate_alpha_quad (loader mapping + branch "
                "flip + determinism; 16 EXPECT lines emitted)\n");
    return 0;
}
