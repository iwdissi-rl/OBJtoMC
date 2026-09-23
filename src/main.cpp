// ---------------------------------------------------------------------------
// objtomc CLI binary — hand-rolled argv parser per proposal §1.5(b) /
// §3.7. No third-party CLI library; cxxopts + CLI11 both audited 2026-07-01
// and rejected on code-size grounds (DECISIONS.md 2026-07-01 "Orchestration
// scope (v0.1)").
//
// Spec:
//   Usage: objtomc [options] <input.{obj,gltf,glb}> <world_dir>
//   Mandatory: exactly one of --voxel-size <float> / --target-size <int>
//   Optional:  --axis-convention {y_up,z_up}, --world-origin <x,y,z>,
//              --level-name <str>, --palette-csv <path>,
//              --max-grid-cells <int64>, --verbose/-v, --help/-h
//
// Exit codes (proposal §3.10, Unix convention):
//   0 = success
//   1 = unexpected std::exception
//   2 = usage error (bad CLI args)
//   3 = mesh::mesh_error
//   4 = voxel::voxel_error
//   5 = palette::palette_error
//   6 = matcher::matcher_error
//   7 = chunk::chunk_error
//   8 = anvil::anvil_error
//   9 = orch::orchestrate_error
// ---------------------------------------------------------------------------

#include "objtomc/orchestrate.hpp"
#include "objtomc/anvil.hpp"
#include "objtomc/chunk.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/palette.hpp"
#include "objtomc/voxel.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace orch = objtomc::orch;

class usage_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void print_help(std::ostream& os, const char* argv0) {
    os <<
        "Usage: " << argv0 << " [options] <input.{obj,gltf,glb}> <world_dir>\n"
        "\n"
        "Convert a Wavefront OBJ or glTF 2.0 mesh (.gltf/.glb — Sketchfab's\n"
        "download format) into a vanilla Minecraft Java Edition 1.21.5 world\n"
        "directory. Pipeline: load -> voxelise -> per-voxel texture sample ->\n"
        "match colours to MC block palette -> write region files + level.dat.\n"
        "Format dispatch is by file extension.\n"
        "\n"
        "Sizing (exactly ONE of the two is mandatory):\n"
        "  --voxel-size <float>            Mesh-space units per voxel. Must be > 0.\n"
        "  --target-size <int>             Output size in BLOCKS along the mesh's\n"
        "                                  longest axis; voxel size is derived from\n"
        "                                  the mesh bounding box. Must be >= 1.\n"
        "\n"
        "Optional:\n"
        "  --axis-convention {y_up,z_up}   Input mesh axis (default y_up).\n"
        "  --tiling {auto,on,off}          Streaming tiled output (default auto:\n"
        "                                  tile per region only when the grid\n"
        "                                  exceeds --max-grid-cells; byte-identical\n"
        "                                  to monolithic output either way).\n"
        "  --world-origin <x,y,z>          World-space block coord of voxel (0,0,0).\n"
        "                                  Three comma-separated ints (default 0,-60,0).\n"
        "  --level-name <string>           World name (default \"objtomc world\").\n"
        "  --palette-csv <path>            Override default palette CSV (v0.2 work;\n"
        "                                  v0.1 always uses load_default_palette()).\n"
        "  --max-grid-cells <int64>        Voxel grid cap (default 1073741824 = 1024^3).\n"
        "  --verbose, -v                   Print pipeline progress to stderr.\n"
        "  --help, -h                      Print this help and exit 0.\n"
        "\n"
        "Examples:\n"
        "  " << argv0 << " --voxel-size 0.002 --verbose bunny.obj bunny-world\n"
        "  " << argv0 << " --target-size 128 model.glb model-world\n"
        "\n"
        "Exit codes (per pipeline stage):\n"
        "  0=ok 1=unexpected 2=usage 3=mesh 4=voxel 5=palette\n"
        "  6=matcher 7=chunk 8=anvil 9=orchestrate\n";
}

// ---------------------------------------------------------------------------
// parse_world_origin — "x,y,z" → array<int, 3>. Throws usage_error on bad
// format.
// ---------------------------------------------------------------------------
std::array<std::int32_t, 3> parse_world_origin(std::string_view s) {
    std::array<std::int32_t, 3> out{};
    std::string buf{s};
    std::stringstream ss(buf);
    std::string tok;
    int i = 0;
    while (std::getline(ss, tok, ',')) {
        if (i >= 3) {
            throw usage_error("--world-origin expects exactly 3 comma-separated ints");
        }
        try {
            std::size_t pos = 0;
            const long v = std::stol(tok, &pos);
            if (pos != tok.size()) {
                throw usage_error("--world-origin: '" + tok + "' is not an integer");
            }
            out[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(v);
        } catch (const std::invalid_argument&) {
            throw usage_error("--world-origin: '" + tok + "' is not an integer");
        } catch (const std::out_of_range&) {
            throw usage_error("--world-origin: '" + tok + "' is out of int32 range");
        }
        ++i;
    }
    if (i != 3) {
        throw usage_error("--world-origin expects exactly 3 comma-separated ints (got " +
                          std::to_string(i) + ")");
    }
    return out;
}

// ---------------------------------------------------------------------------
// Parsed CLI bundle.
// ---------------------------------------------------------------------------
struct ParsedCli {
    bool                          show_help = false;
    std::filesystem::path         obj_path;
    std::filesystem::path         world_dir;
    orch::OrchestrateOptions      opts = orch::default_options();
};

ParsedCli parse_cli(int argc, char** argv) {
    ParsedCli cli;
    std::vector<std::string_view> positionals;
    bool voxel_size_seen  = false;
    bool target_size_seen = false;

    auto next_arg = [&](int& i, std::string_view flag) -> std::string_view {
        if (i + 1 >= argc) {
            throw usage_error(std::string("--") + std::string(flag) +
                              " requires a value");
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--help" || a == "-h") {
            cli.show_help = true;
        } else if (a == "--verbose" || a == "-v") {
            cli.opts.verbose = true;
        } else if (a == "--voxel-size") {
            const auto v = next_arg(i, "voxel-size");
            try {
                std::size_t pos = 0;
                const std::string s{v};
                cli.opts.voxel_size = std::stof(s, &pos);
                if (pos != s.size()) {
                    throw usage_error("--voxel-size: '" + s +
                                      "' is not a valid float");
                }
            } catch (const std::invalid_argument&) {
                throw usage_error(std::string("--voxel-size: '") + std::string(v) +
                                  "' is not a valid float");
            } catch (const std::out_of_range&) {
                throw usage_error(std::string("--voxel-size: '") + std::string(v) +
                                  "' is out of float range");
            }
            voxel_size_seen = true;
        } else if (a == "--target-size") {
            const auto v = next_arg(i, "target-size");
            try {
                std::size_t pos = 0;
                const std::string s{v};
                const long ts = std::stol(s, &pos);
                if (pos != s.size()) {
                    throw usage_error("--target-size: '" + s +
                                      "' is not an integer");
                }
                if (ts < 1 || ts > 100000) {
                    throw usage_error("--target-size must be in [1, 100000], got " + s);
                }
                cli.opts.target_size = static_cast<std::int32_t>(ts);
            } catch (const std::invalid_argument&) {
                throw usage_error(std::string("--target-size: '") + std::string(v) +
                                  "' is not an integer");
            } catch (const std::out_of_range&) {
                throw usage_error(std::string("--target-size: '") + std::string(v) +
                                  "' is out of range");
            }
            target_size_seen = true;
        } else if (a == "--tiling") {
            const auto v = next_arg(i, "tiling");
            using TM = objtomc::orch::OrchestrateOptions::TilingMode;
            if (v == "auto") {
                cli.opts.tiling = TM::AUTO;
            } else if (v == "on") {
                cli.opts.tiling = TM::ON;
            } else if (v == "off") {
                cli.opts.tiling = TM::OFF;
            } else {
                throw usage_error(std::string("--tiling: '") + std::string(v) +
                                  "' is not one of {auto, on, off}");
            }
        } else if (a == "--axis-convention") {
            const auto v = next_arg(i, "axis-convention");
            if (v == "y_up" || v == "Y_UP") {
                cli.opts.axis_convention = objtomc::voxel::AxisConvention::Y_UP;
            } else if (v == "z_up" || v == "Z_UP") {
                cli.opts.axis_convention = objtomc::voxel::AxisConvention::Z_UP;
            } else {
                throw usage_error(std::string("--axis-convention: '") + std::string(v) +
                                  "' is not one of {y_up, z_up}");
            }
        } else if (a == "--world-origin") {
            cli.opts.world_origin = parse_world_origin(next_arg(i, "world-origin"));
            // If spawn_pos hasn't been explicitly set, mirror world_origin to it.
            cli.opts.spawn_pos    = cli.opts.world_origin;
        } else if (a == "--level-name") {
            cli.opts.level_name = std::string(next_arg(i, "level-name"));
        } else if (a == "--palette-csv") {
            cli.opts.palette_csv = std::filesystem::path(next_arg(i, "palette-csv"));
        } else if (a == "--max-grid-cells") {
            const auto v = next_arg(i, "max-grid-cells");
            try {
                std::size_t pos = 0;
                const std::string s{v};
                cli.opts.max_grid_cells = std::stoll(s, &pos);
                if (pos != s.size()) {
                    throw usage_error("--max-grid-cells: '" + s + "' is not an integer");
                }
            } catch (const std::invalid_argument&) {
                throw usage_error(std::string("--max-grid-cells: '") + std::string(v) +
                                  "' is not an integer");
            } catch (const std::out_of_range&) {
                throw usage_error(std::string("--max-grid-cells: '") + std::string(v) +
                                  "' is out of int64 range");
            }
        } else if (!a.empty() && a[0] == '-') {
            throw usage_error("unknown flag: " + std::string(a));
        } else {
            positionals.push_back(a);
        }
    }

    if (cli.show_help) return cli;

    if (positionals.size() != 2) {
        throw usage_error("usage requires exactly 2 positional args "
                          "(<input.{obj,gltf,glb}> <world_dir>); got " +
                          std::to_string(positionals.size()));
    }
    if (voxel_size_seen && target_size_seen) {
        throw usage_error("--voxel-size and --target-size are mutually exclusive; "
                          "give exactly one");
    }
    if (!voxel_size_seen && !target_size_seen) {
        throw usage_error("exactly one of --voxel-size / --target-size is mandatory");
    }
    cli.obj_path  = std::filesystem::path(positionals[0]);
    cli.world_dir = std::filesystem::path(positionals[1]);
    return cli;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        ParsedCli cli = parse_cli(argc, argv);
        if (cli.show_help) {
            print_help(std::cout, argv[0]);
            return 0;
        }
        orch::orchestrate(cli.obj_path, cli.world_dir, cli.opts);
        if (cli.opts.verbose) std::cerr << "done\n";
        return 0;
    } catch (const usage_error& e) {
        std::cerr << "usage error: " << e.what() << "\n";
        std::cerr << "Try '" << argv[0] << " --help' for usage.\n";
        return 2;
    } catch (const objtomc::mesh::mesh_error& e) {
        std::cerr << "mesh error: " << e.what() << "\n";
        return 3;
    } catch (const objtomc::voxel::voxel_error& e) {
        std::cerr << "voxel error: " << e.what() << "\n";
        return 4;
    } catch (const objtomc::palette::palette_error& e) {
        std::cerr << "palette error: " << e.what() << "\n";
        return 5;
    } catch (const objtomc::matcher::matcher_error& e) {
        std::cerr << "matcher error: " << e.what() << "\n";
        return 6;
    } catch (const objtomc::chunk::chunk_error& e) {
        std::cerr << "chunk error: " << e.what() << "\n";
        return 7;
    } catch (const objtomc::anvil::anvil_error& e) {
        std::cerr << "anvil error: " << e.what() << "\n";
        return 8;
    } catch (const orch::orchestrate_error& e) {
        std::cerr << "orchestrate error: " << e.what() << "\n";
        return 9;
    } catch (const std::exception& e) {
        std::cerr << "unexpected error: " << e.what() << "\n";
        return 1;
    }
}
