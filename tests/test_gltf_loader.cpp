// gltf_loader — Session 49 v0.3 item #2 unit test.
//
// Exercises gltf::load_gltf against the three generated fixtures
// (tests/fixtures/texture/gen_gltf_fixtures.py):
//
//   quad.gltf      — geometry/UV exactness vs the known quad.obj layout:
//                    identical positions, UVs V-flipped back to OBJ's
//                    bottom-left convention, indexed faces, external
//                    texture URI in map_kd (map_kd_bytes empty).
//   quad.glb       — same geometry from the GLB binary form; checker.png
//                    EMBEDDED: map_kd_bytes must equal the on-disk
//                    checker.png bytes exactly.
//   materials.gltf — PBR→Material mapping: baseColorFactor linear→sRGB
//                    (1,0,0)→(1,0,0); alphaMode BLEND → d = baseColor.a;
//                    emissiveFactor × KHR_materials_emissive_strength
//                    clamped then sRGB-encoded; non-indexed primitives.
//   missing file   — throws mesh::mesh_error.

#include "objtomc/gltf.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs  = std::filesystem;
namespace msh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

void expect_near(float got, float want, const std::string& what) {
    if (std::fabs(got - want) > 1e-5f) {
        fail(what + ": got " + std::to_string(got) + " want " +
             std::to_string(want));
    }
}

void check_quad_geometry(const msh::Mesh& m, const std::string& tag) {
    if (m.positions.size() != 4) {
        fail(tag + ": expected 4 positions, got " +
             std::to_string(m.positions.size()));
    }
    const float want_pos[4][3] = {
        {0, 0.5f, 0}, {4, 0.5f, 0}, {4, 0.5f, 4}, {0, 0.5f, 4}};
    for (std::size_t i = 0; i < 4; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (m.positions[i][static_cast<std::size_t>(c)] !=
                want_pos[i][c]) {
                fail(tag + ": position " + std::to_string(i) + " differs");
            }
        }
    }
    if (m.uvs.size() != 4) fail(tag + ": expected 4 uvs");
    // glTF fixture stores top-left-origin texcoords; the loader's V flip
    // must reproduce quad.obj's vt values EXACTLY (0/1 are exact floats).
    const float want_uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (std::size_t i = 0; i < 4; ++i) {
        if (m.uvs[i][0] != want_uv[i][0] || m.uvs[i][1] != want_uv[i][1]) {
            fail(tag + ": uv " + std::to_string(i) + " differs (got " +
                 std::to_string(m.uvs[i][0]) + "," +
                 std::to_string(m.uvs[i][1]) + ")");
        }
    }
    if (m.faces.size() != 2) fail(tag + ": expected 2 faces");
    const std::uint32_t want_tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (std::size_t f = 0; f < 2; ++f) {
        for (std::size_t c = 0; c < 3; ++c) {
            if (m.faces[f].position_indices[c] != want_tri[f][c]) {
                fail(tag + ": face " + std::to_string(f) + " corner " +
                     std::to_string(c) + " position index differs");
            }
            if (m.faces[f].uv_indices[c] !=
                static_cast<std::int32_t>(want_tri[f][c])) {
                fail(tag + ": face " + std::to_string(f) + " uv index differs");
            }
            if (m.faces[f].normal_indices[c] != -1) {
                fail(tag + ": expected normal index -1");
            }
        }
        if (m.faces[f].material_id != 0) fail(tag + ": material_id != 0");
    }
    if (m.materials.size() != 1) fail(tag + ": expected 1 material");
    const auto& mat = m.materials[0];
    if (mat.name != "quadmat") fail(tag + ": material name " + mat.name);
    // baseColorFactor 0.21404114 linear → ~0.5 sRGB (quad.mtl Kd parity).
    for (int c = 0; c < 3; ++c) {
        if (std::fabs(mat.kd[static_cast<std::size_t>(c)] - 0.5f) > 1e-3f) {
            fail(tag + ": kd not ~0.5");
        }
    }
    expect_near(mat.d, 1.0f, tag + ": d");
    for (int c = 0; c < 3; ++c) {
        expect_near(mat.ke[static_cast<std::size_t>(c)], 0.0f, tag + ": ke");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <texture-fixtures-dir>\n";
        return 2;
    }
    const fs::path dir = argv[1];

    // --- quad.gltf: geometry + external texture URI ------------------------
    const msh::Mesh q = objtomc::gltf::load_gltf(dir / "quad.gltf");
    check_quad_geometry(q, "quad.gltf");
    if (q.materials[0].map_kd != "checker.png") {
        fail("quad.gltf: map_kd = '" + q.materials[0].map_kd + "'");
    }
    if (!q.materials[0].map_kd_bytes.empty()) {
        fail("quad.gltf: expected no embedded bytes");
    }

    // --- quad.glb: same geometry, embedded texture bytes -------------------
    const msh::Mesh g = objtomc::gltf::load_gltf(dir / "quad.glb");
    check_quad_geometry(g, "quad.glb");
    std::ifstream png_in(dir / "checker.png", std::ios::binary);
    const std::vector<std::uint8_t> png_bytes(
        (std::istreambuf_iterator<char>(png_in)),
        std::istreambuf_iterator<char>());
    if (g.materials[0].map_kd_bytes != png_bytes) {
        fail("quad.glb: embedded bytes differ from checker.png (" +
             std::to_string(g.materials[0].map_kd_bytes.size()) + " vs " +
             std::to_string(png_bytes.size()) + " bytes)");
    }

    // --- materials.gltf: PBR → Material mapping ----------------------------
    const msh::Mesh mm = objtomc::gltf::load_gltf(dir / "materials.gltf");
    if (mm.materials.size() != 3) fail("materials.gltf: expected 3 materials");
    if (mm.faces.size() != 3) fail("materials.gltf: expected 3 faces");
    for (std::size_t f = 0; f < 3; ++f) {
        if (mm.faces[f].material_id != static_cast<std::int32_t>(f)) {
            fail("materials.gltf: face material_id mismatch");
        }
        for (std::size_t c = 0; c < 3; ++c) {
            if (mm.faces[f].uv_indices[c] != -1) {
                fail("materials.gltf: expected uv index -1 (no texcoords)");
            }
        }
    }
    const auto& red = mm.materials[0];
    if (red.name != "opaque_red") fail("material 0 name");
    expect_near(red.kd[0], 1.0f, "opaque_red kd.r");     // encode(1) = 1
    expect_near(red.kd[1], 0.0f, "opaque_red kd.g");     // encode(0) = 0
    expect_near(red.kd[2], 0.0f, "opaque_red kd.b");
    expect_near(red.d, 1.0f, "opaque_red d");

    const auto& blend = mm.materials[1];
    if (blend.name != "blend_half") fail("material 1 name");
    expect_near(blend.d, 0.4f, "blend_half d");          // BLEND → alpha
    expect_near(blend.kd[2], 1.0f, "blend_half kd.b");

    const auto& emis = mm.materials[2];
    if (emis.name != "emissive_green") fail("material 2 name");
    // emissiveFactor 0.5 × strength 2.0 = 1.0 linear, clamped → sRGB 1.0.
    expect_near(emis.ke[1], 1.0f, "emissive_green ke.g");
    expect_near(emis.ke[0], 0.0f, "emissive_green ke.r");
    expect_near(emis.d, 1.0f, "emissive_green d");

    // --- missing file → mesh_error -----------------------------------------
    bool threw = false;
    try {
        (void)objtomc::gltf::load_gltf(dir / "no_such_asset.gltf");
    } catch (const msh::mesh_error&) {
        threw = true;
    }
    if (!threw) fail("missing file did not throw mesh_error");

    std::cout << "PASS: gltf_loader (quad.gltf geometry/UV-flip/URI, "
                 "quad.glb embedded bytes, materials.gltf PBR mapping, "
                 "missing-file error)\n";
    return 0;
}
