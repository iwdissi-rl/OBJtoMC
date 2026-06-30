// orchestrate_cli_errors — proposal §3.13(f).
//
// Spawn the objtomc binary with bad inputs; assert exit codes match
// proposal §3.10 + stderr contains an expected substring per case.
//
// Sub-cases:
//   1) no args                       → exit 2 (usage), stderr "voxel-size" or "positional"
//   2) --voxel-size missing         → exit 2 (usage), stderr "voxel-size"
//   3) bad obj path                  → exit 3 (mesh)
//   4) world_dir already exists      → exit 9 (orchestrate)
//   5) bad --axis-convention value  → exit 2 (usage), stderr "axis-convention"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

[[noreturn]] void fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << "\n";
    std::exit(1);
}

// Run cmd via popen, capture stdout+stderr merged, return (exit code, output).
std::pair<int, std::string> run(const std::string& cmd) {
    std::string output;
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) fail("popen failed");
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) output += buf;
    int status = pclose(p);
    if (WIFEXITED(status)) return {WEXITSTATUS(status), output};
    return {-1, output};
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

void expect_exit(int actual, int expected, const std::string& label) {
    if (actual != expected) {
        fail(label + ": expected exit " + std::to_string(expected) +
             ", got " + std::to_string(actual));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <path-to-objtomc> <good-obj-fixture>\n";
        return 2;
    }
    const std::string bin     = argv[1];
    const std::string good_obj = argv[2];

    // Set up a tmpdir for the (4) case.
    const auto base = fs::temp_directory_path() /
        ("objtomc_clierr_" + std::to_string(std::rand()));
    fs::remove_all(base);
    fs::create_directories(base);
    const auto pre_existing = base / "world";
    fs::create_directories(pre_existing);
    // Drop a marker file so the dir is non-empty.
    { std::ofstream(pre_existing / "marker") << "x"; }

    // ---- (1) no args → exit 2 ----
    {
        auto [code, out] = run(bin);
        expect_exit(code, 2, "(1) no args");
        if (!contains(out, "usage") && !contains(out, "positional") &&
            !contains(out, "voxel-size")) {
            fail("(1) no args: stderr missing usage hint; got: " + out);
        }
        std::printf("  (1) no args:                exit %d ✓\n", code);
    }

    // ---- (2) --voxel-size missing ----
    {
        auto [code, out] = run(bin + " " + good_obj + " " + (base / "w2").string());
        expect_exit(code, 2, "(2) missing voxel-size");
        if (!contains(out, "voxel-size")) {
            fail("(2) missing voxel-size: stderr should name 'voxel-size': " + out);
        }
        std::printf("  (2) missing voxel-size:    exit %d ✓\n", code);
    }

    // ---- (3) bad obj path → exit 3 ----
    {
        auto [code, out] = run(bin + " --voxel-size 0.125 " +
                               (base / "does_not_exist.obj").string() + " " +
                               (base / "w3").string());
        expect_exit(code, 3, "(3) bad obj path");
        std::printf("  (3) bad obj path:           exit %d ✓\n", code);
    }

    // ---- (4) world_dir already exists + non-empty → exit 9 ----
    {
        auto [code, out] = run(bin + " --voxel-size 0.125 " + good_obj + " " +
                               pre_existing.string());
        expect_exit(code, 9, "(4) world_dir exists");
        if (!contains(out, "world_dir")) {
            fail("(4) world_dir exists: stderr should name 'world_dir': " + out);
        }
        std::printf("  (4) world_dir non-empty:    exit %d ✓\n", code);
    }

    // ---- (5) bad --axis-convention value → exit 2 ----
    {
        auto [code, out] = run(bin + " --voxel-size 0.125 --axis-convention diagonal " +
                               good_obj + " " + (base / "w5").string());
        expect_exit(code, 2, "(5) bad axis-convention");
        if (!contains(out, "axis-convention")) {
            fail("(5) bad axis-convention: stderr should name flag: " + out);
        }
        std::printf("  (5) bad axis-convention:    exit %d ✓\n", code);
    }

    fs::remove_all(base);
    std::printf("PASS: orchestrate_cli_errors (5 sub-cases)\n");
    return 0;
}
