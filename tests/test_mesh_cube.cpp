// Hand-cube unit test (proposal §5.10 (a)).
//
// Locks the byte-exact contract that load_obj("cube.obj") returns the cube
// fixture's expected mesh shape:
//   - 8 positions (the cube's corners)
//   - 6 normals (one per face direction)
//   - 4 uvs    (the four texture corners)
//   - 12 triangles (6 quads × 2 after fan triangulation)
//   - 1 material RedMatte with kd = (1.0, 0.0, 0.0), d = 1.0, illum = 2
//   - every face has material_id == 0

#include "objtomc/mesh.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace mesh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

void expect_eq(std::size_t actual, std::size_t expected, const char* label) {
    if (actual != expected) {
        fail(std::string(label) + " expected " + std::to_string(expected) +
             ", got " + std::to_string(actual));
    }
}

void expect_eq_float(float actual, float expected, const char* label) {
    if (std::fabs(actual - expected) > 1e-6f) {
        fail(std::string(label) + " expected " + std::to_string(expected) +
             ", got " + std::to_string(actual));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-cube.obj>\n";
        return 2;
    }

    const std::filesystem::path cube_path = argv[1];
    const mesh::Mesh m = mesh::load_obj(cube_path);

    expect_eq(m.positions.size(),  8u,  "positions.size()");
    expect_eq(m.normals.size(),    6u,  "normals.size()");
    expect_eq(m.uvs.size(),        4u,  "uvs.size()");
    expect_eq(m.faces.size(),     12u,  "faces.size() (6 quads * 2 tris)");
    expect_eq(m.materials.size(),  1u,  "materials.size()");

    // Every face must reference material id 0.
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        if (m.faces[i].material_id != 0) {
            fail("face[" + std::to_string(i) +
                 "].material_id expected 0, got " +
                 std::to_string(m.faces[i].material_id));
        }
        // Every face's normal/uv indices should be present (the fixture
        // supplies them for every corner).
        for (int c = 0; c < 3; ++c) {
            if (m.faces[i].normal_indices[c] < 0) {
                fail("face[" + std::to_string(i) +
                     "].normal_indices[" + std::to_string(c) +
                     "] missing");
            }
            if (m.faces[i].uv_indices[c] < 0) {
                fail("face[" + std::to_string(i) +
                     "].uv_indices[" + std::to_string(c) + "] missing");
            }
        }
    }

    // Material RedMatte: name and key fields.
    const mesh::Material& mat = m.materials[0];
    if (mat.name != "RedMatte") {
        fail("materials[0].name expected 'RedMatte', got '" + mat.name + "'");
    }
    expect_eq_float(mat.kd[0], 1.0f, "kd[0]");
    expect_eq_float(mat.kd[1], 0.0f, "kd[1]");
    expect_eq_float(mat.kd[2], 0.0f, "kd[2]");
    expect_eq_float(mat.d,    1.0f, "d (alpha)");
    if (mat.illum != 2) fail("illum expected 2, got " + std::to_string(mat.illum));
    if (!mat.map_kd.empty()) {
        fail("map_kd expected empty, got '" + mat.map_kd + "'");
    }

    std::printf("PASS: cube fixture loaded\n"
                "  positions=%zu normals=%zu uvs=%zu faces=%zu materials=%zu\n"
                "  materials[0]: name='%s' kd=(%.3f, %.3f, %.3f) d=%.3f illum=%d\n",
                m.positions.size(), m.normals.size(), m.uvs.size(),
                m.faces.size(), m.materials.size(),
                mat.name.c_str(),
                static_cast<double>(mat.kd[0]),
                static_cast<double>(mat.kd[1]),
                static_cast<double>(mat.kd[2]),
                static_cast<double>(mat.d), mat.illum);
    return 0;
}
