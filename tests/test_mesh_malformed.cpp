// Malformed / missing-input unit test (proposal §5.10 (c)).
//
// Asserts mesh::mesh_error (NOT std::runtime_error or std::exception) is
// thrown for: (1) a path that does not exist, (2) a syntactically bad
// fixture file. Type discipline matters — the matcher's recovery path
// branches on this exception type per proposal §5.4.

#include "objtomc/mesh.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace mesh = objtomc::mesh;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Returns "" if the expected mesh_error was thrown; otherwise the failure
// reason. We do not allow any other exception type (catches std::exception
// only to report what else slipped past).
std::string expect_mesh_error(const std::filesystem::path& p,
                              const char* label) {
    try {
        (void)mesh::load_obj(p);
        return std::string(label) +
               ": expected mesh_error, but load_obj returned normally";
    } catch (const mesh::mesh_error&) {
        return "";
    } catch (const std::exception& e) {
        return std::string(label) +
               ": expected mesh_error, got std::exception('" + e.what() + "')";
    } catch (...) {
        return std::string(label) +
               ": expected mesh_error, got non-std::exception throw";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0]
                  << " <missing.obj-path> <malformed.obj-path>\n";
        return 2;
    }

    // (1) Missing-file path. The caller passes a path the test setup
    // guarantees does not exist. Defensive cross-check here anyway.
    const std::filesystem::path missing_path = argv[1];
    if (std::filesystem::exists(missing_path)) {
        fail("missing_path was supposed to be absent: " +
             missing_path.string());
    }
    std::string e1 = expect_mesh_error(missing_path, "missing file");
    if (!e1.empty()) fail(e1);

    // (2) Malformed file (must exist; contents are garbage).
    const std::filesystem::path malformed_path = argv[2];
    if (!std::filesystem::exists(malformed_path)) {
        fail("malformed fixture not found at " + malformed_path.string());
    }
    std::string e2 = expect_mesh_error(malformed_path, "malformed file");
    if (!e2.empty()) fail(e2);

    std::printf("PASS: malformed-input throws mesh_error\n"
                "  missing: '%s'\n"
                "  malformed: '%s'\n",
                missing_path.string().c_str(),
                malformed_path.string().c_str());
    return 0;
}
