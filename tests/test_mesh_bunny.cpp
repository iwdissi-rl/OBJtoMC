// Real-artist-mesh "Stanford bunny" load test (proposal §5.10 (d)).
//
// Counts pinned against the vendored fixture at
// tests/fixtures/mesh/bunny.obj. Fixture provenance + SHA-256 recorded
// in DECISIONS.md 2026-06-18 "Stanford bunny test fixture provenance".
// The bunny copy in our tree is the full 35947-vertex / 69451-face
// Stanford bunny (not the 8146-vertex decimated copy mentioned as an
// example in the proposal §5.10 (d)).
//
// The fixture has no normals, no UVs, no MTL. This locks the wrapper's
// "absent attributes stay empty" branch from proposal §5.1.

#include "objtomc/mesh.hpp"

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <path-to-bunny.obj>\n";
        return 2;
    }
    const std::filesystem::path bunny_path = argv[1];
    const mesh::Mesh m = mesh::load_obj(bunny_path);

    constexpr std::size_t kExpectedPositions = 35947;
    constexpr std::size_t kExpectedFaces     = 69451;

    if (m.positions.size() != kExpectedPositions) {
        fail("positions.size() expected " +
             std::to_string(kExpectedPositions) + ", got " +
             std::to_string(m.positions.size()));
    }
    if (m.faces.size() != kExpectedFaces) {
        fail("faces.size() expected " + std::to_string(kExpectedFaces) +
             ", got " + std::to_string(m.faces.size()));
    }
    if (!m.normals.empty()) {
        fail("normals.size() expected 0 (fixture has no vn lines), got " +
             std::to_string(m.normals.size()));
    }
    if (!m.uvs.empty()) {
        fail("uvs.size() expected 0 (fixture has no vt lines), got " +
             std::to_string(m.uvs.size()));
    }
    if (!m.materials.empty()) {
        fail("materials.size() expected 0 (fixture has no mtllib), got " +
             std::to_string(m.materials.size()));
    }

    // Faces have no normals / no UVs / no material — confirm the -1
    // sentinels are populated as the wrapper documents.
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        if (m.faces[i].material_id != -1) {
            fail("face[" + std::to_string(i) +
                 "].material_id expected -1, got " +
                 std::to_string(m.faces[i].material_id));
        }
        for (int c = 0; c < 3; ++c) {
            if (m.faces[i].normal_indices[c] != -1) {
                fail("face[" + std::to_string(i) + "].normal_indices[" +
                     std::to_string(c) + "] expected -1");
            }
            if (m.faces[i].uv_indices[c] != -1) {
                fail("face[" + std::to_string(i) + "].uv_indices[" +
                     std::to_string(c) + "] expected -1");
            }
        }
    }

    std::printf("PASS: Stanford bunny loaded\n"
                "  positions=%zu faces=%zu normals=%zu uvs=%zu materials=%zu\n",
                m.positions.size(), m.faces.size(),
                m.normals.size(), m.uvs.size(), m.materials.size());
    return 0;
}
