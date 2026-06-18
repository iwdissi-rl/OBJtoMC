// Two-material MTL-bearing unit test (proposal §5.10 (b)).
//
// Locks: 2 materials present after load_obj, per-face material assignment
// correct (face 0 -> MatA, face 1 -> MatB), Kd/Ke/d round-trip exactly,
// map_Kd round-trips with no path normalisation.

#include "objtomc/mesh.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace mesh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
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
        std::cerr << "usage: " << argv[0] << " <path-to-materials.obj>\n";
        return 2;
    }

    const std::filesystem::path obj_path = argv[1];
    const mesh::Mesh m = mesh::load_obj(obj_path);

    if (m.materials.size() != 2) {
        fail("materials.size() expected 2, got " +
             std::to_string(m.materials.size()));
    }
    if (m.faces.size() != 2) {
        fail("faces.size() expected 2, got " +
             std::to_string(m.faces.size()));
    }

    // Look up by name so the test does not depend on internal MTL ordering
    // (tinyobjloader currently preserves declaration order but we treat that
    // as an implementation detail).
    std::unordered_map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < m.materials.size(); ++i) {
        by_name[m.materials[i].name] = i;
    }
    if (!by_name.count("MatA")) fail("MatA missing");
    if (!by_name.count("MatB")) fail("MatB missing");

    const mesh::Material& a = m.materials[by_name["MatA"]];
    const mesh::Material& b = m.materials[by_name["MatB"]];

    expect_eq_float(a.kd[0], 0.25f, "MatA.kd[0]");
    expect_eq_float(a.kd[1], 0.50f, "MatA.kd[1]");
    expect_eq_float(a.kd[2], 0.75f, "MatA.kd[2]");
    expect_eq_float(a.ke[0], 0.10f, "MatA.ke[0]");
    expect_eq_float(a.ke[1], 0.20f, "MatA.ke[1]");
    expect_eq_float(a.ke[2], 0.30f, "MatA.ke[2]");
    expect_eq_float(a.d,     0.5f,  "MatA.d");
    if (a.illum != 2) fail("MatA.illum expected 2");
    if (a.map_kd != "textures/matA_diffuse.png") {
        fail("MatA.map_kd round-trip failed: '" + a.map_kd + "'");
    }

    expect_eq_float(b.kd[0], 0.90f, "MatB.kd[0]");
    expect_eq_float(b.kd[1], 0.10f, "MatB.kd[1]");
    expect_eq_float(b.kd[2], 0.05f, "MatB.kd[2]");
    expect_eq_float(b.ke[0], 0.0f,  "MatB.ke[0]");
    expect_eq_float(b.ke[1], 0.0f,  "MatB.ke[1]");
    expect_eq_float(b.ke[2], 0.0f,  "MatB.ke[2]");
    expect_eq_float(b.d,     1.0f,  "MatB.d");
    if (b.illum != 1) fail("MatB.illum expected 1");
    if (b.map_kd != "./textures/matB_diffuse.png") {
        fail("MatB.map_kd round-trip failed (path normalisation?): '" +
             b.map_kd + "'");
    }

    // Per-face material assignment: face 0 should bind MatA, face 1 -> MatB.
    const std::int32_t face0_id = m.faces[0].material_id;
    const std::int32_t face1_id = m.faces[1].material_id;
    if (face0_id < 0 || face1_id < 0) {
        fail("face material_id is -1 (expected both faces to have a material)");
    }
    if (m.materials[static_cast<std::size_t>(face0_id)].name != "MatA") {
        fail("face[0] expected MatA, got '" +
             m.materials[static_cast<std::size_t>(face0_id)].name + "'");
    }
    if (m.materials[static_cast<std::size_t>(face1_id)].name != "MatB") {
        fail("face[1] expected MatB, got '" +
             m.materials[static_cast<std::size_t>(face1_id)].name + "'");
    }

    std::printf("PASS: materials fixture loaded\n"
                "  materials=%zu faces=%zu\n"
                "  MatA: kd=(0.25,0.50,0.75) ke=(0.10,0.20,0.30) d=0.5 map_kd='%s'\n"
                "  MatB: kd=(0.90,0.10,0.05) ke=(0,0,0) d=1.0 map_kd='%s'\n"
                "  face[0]->MatA face[1]->MatB\n",
                m.materials.size(), m.faces.size(),
                a.map_kd.c_str(), b.map_kd.c_str());
    return 0;
}
