#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "objtomc/matcher.hpp"   // matcher::MatcherOptions
#include "objtomc/voxel.hpp"     // voxel::AxisConvention

namespace objtomc::orch {

// ---------------------------------------------------------------------------
// MC 1.21.5 schema constants exposed for callers + tests.
// ---------------------------------------------------------------------------
static constexpr std::int32_t  LEVEL_DAT_DATA_VERSION  = 4325;
static constexpr const char*   WORLD_DIR_REGION_SUBDIR = "region";

// ---------------------------------------------------------------------------
// OrchestrateOptions — v0.1 hardcoded defaults (default_options()), tunable
// by the caller. voxel_size is mandatory (default_options() leaves it 0 and
// orchestrate() throws); everything else has a sensible default.
// ---------------------------------------------------------------------------
struct OrchestrateOptions {
    // ----- Voxelization ----------------------------------------------------
    float                                    voxel_size;            // mandatory, > 0
    voxel::AxisConvention                    axis_convention;       // default Y_UP
    std::optional<std::array<int,   3>>      voxel_grid_dimensions; // default = auto from mesh AABB
    std::optional<std::array<float, 3>>      voxel_grid_origin_world; // default = mesh AABB minimum
    std::int64_t                             max_grid_cells;        // default = voxel::MAX_GRID_CELLS_DEFAULT

    // ----- Voxel → block coord transform -----------------------------------
    std::array<std::int32_t, 3>              world_origin;          // default {0, -60, 0}

    // ----- Matcher (forwarded verbatim) -----------------------------------
    matcher::MatcherOptions                  matcher_opts;

    // ----- Palette source --------------------------------------------------
    std::optional<std::filesystem::path>     palette_csv;           // empty → load_default_palette()

    // ----- level.dat fields ------------------------------------------------
    std::string                              level_name;            // default "objtomc world"
    std::array<std::int32_t, 3>              spawn_pos;             // default = world_origin
    std::int32_t                             game_type;             // default 1 (creative)
    std::int8_t                              difficulty;            // default 0 (peaceful)
    bool                                     allow_commands;        // default true
    std::int64_t                             day_time;              // default 6000 (noon)

    // ----- Determinism / reproducibility ----------------------------------
    // If set, this Unix epoch seconds value is used for the per-chunk
    // anvil timestamp table AND (multiplied by 1000) for level.dat's
    // LastPlayed field, in place of std::time(nullptr). Tests pin this
    // to a fixed value to make region + level.dat files byte-deterministic
    // across runs; library callers wanting reproducible-build artifacts
    // can pin to a SOURCE_DATE_EPOCH-style value. nullopt → live wall clock.
    std::optional<std::int64_t>              now_unix_s;            // default nullopt

    // ----- Progress reporting ---------------------------------------------
    bool                                     verbose;               // default false
};

OrchestrateOptions default_options() noexcept;

// ---------------------------------------------------------------------------
// Errors. Programmer-misuse and orchestration-specific failures both surface
// as a single orchestration exception type. Per-layer errors (mesh_error,
// voxel_error, matcher_error, chunk_error, anvil_error, palette_error)
// propagate as their own types — callers can catch each separately or the
// parent std::runtime_error.
// ---------------------------------------------------------------------------
class orchestrate_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// orchestrate — the primary entry point. Reads obj_path, voxelises, matches
// against the palette, routes voxels to chunks, builds chunk NBT, writes
// regions + level.dat into world_dir.
//
// world_dir must not exist OR must be empty. If world_dir exists and
// contains files, orchestrate_error is thrown.
//
// Throws on any layer's error type (see comment above) or orchestrate_error
// for orchestration-specific failures (Y range overflow, world_dir
// conflict, etc.).
//
// Implementation reference: research/orchestration_proposal.md §3.
// ---------------------------------------------------------------------------
void orchestrate(const std::filesystem::path& obj_path,
                 const std::filesystem::path& world_dir,
                 const OrchestrateOptions&    opts);

// ---------------------------------------------------------------------------
// write_level_dat — helper exposed for testing. Builds the level.dat NBT
// tree per §3.5 of the proposal and writes it gzip-compressed to disk.
//
// now_unix_ms is the value for the LastPlayed field. Tests pass a fixed
// value (e.g. 0) to make the resulting file byte-deterministic.
// ---------------------------------------------------------------------------
void write_level_dat(const OrchestrateOptions&     opts,
                     const std::filesystem::path&  level_dat_path,
                     std::int64_t                  now_unix_ms);

}  // namespace objtomc::orch
