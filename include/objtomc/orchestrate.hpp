#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

#include "objtomc/chunk.hpp"     // chunk::WorldHeight
#include "objtomc/cleanup.hpp"   // cleanup::CleanupOptions
#include "objtomc/matcher.hpp"   // matcher::MatcherOptions
#include "objtomc/mesh.hpp"      // mesh::Mesh (voxel_size_for_target)
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
    // Exactly ONE of voxel_size / target_size must be set (Session 50,
    // v0.3 item #3): voxel_size > 0 gives mesh-units-per-voxel directly;
    // target_size gives the desired output size in BLOCKS along the mesh
    // AABB's longest axis and orchestrate derives voxel_size from the
    // loaded mesh via voxel_size_for_target(). Setting both (voxel_size
    // != 0 while target_size is engaged) throws orchestrate_error.
    float                                    voxel_size;            // 0 = unset
    std::optional<std::int32_t>              target_size;           // default nullopt
    voxel::AxisConvention                    axis_convention;       // default Y_UP
    std::optional<std::array<int,   3>>      voxel_grid_dimensions; // default = auto from mesh AABB
    std::optional<std::array<float, 3>>      voxel_grid_origin_world; // default = mesh AABB minimum
    std::int64_t                             max_grid_cells;        // default = voxel::MAX_GRID_CELLS_DEFAULT

    // ----- Voxel → block coord transform -----------------------------------
    std::array<std::int32_t, 3>              world_origin;          // default {0, -60, 0}

    // ----- Tiling (Session 57, v0.4 — research/scale_v04_proposal.md §3.5) --
    // AUTO (default): monolithic pipeline when the full voxel grid fits
    //   max_grid_cells (every pre-v0.4 invocation byte-stable by
    //   construction); region-aligned tiled streaming otherwise.
    // ON:  always tile — one 512-block-aligned XZ window per Anvil region;
    //   per tile: voxelize → match → build → write ONE region file → free.
    // OFF: always monolithic; an oversized grid keeps throwing the
    //   pre-v0.4 voxel_error.
    // Tiled and monolithic output are byte-identical (ctest
    // orchestrate_tiled_equality). In tiled mode max_grid_cells caps the
    // PER-TILE grid, not the whole world.
    enum class TilingMode { AUTO, ON, OFF };
    TilingMode                               tiling;                // default AUTO

    // ----- World height (Session 63, v0.4 item #2) --------------------------
    // nullopt (default) = AUTO: the vanilla (-64, 384) dimension when the
    // model's Y span fits it; otherwise the smallest multiple-of-16
    // extension that fits (up to the vanilla datapack limits: min_y >=
    // -2032, min_y + height <= 2032). Set explicitly to pin. Whenever the
    // resolved value is non-default, orchestrate embeds a world datapack
    // (datapacks/objtomc_height/) overriding minecraft:overworld's
    // dimension_type and lists it in level.dat DataPacks.Enabled — the
    // world stays vanilla-loadable. NOTE: per the 1.21.2+ behaviour
    // recorded in research/extended_height_proposal.md, removing a height
    // datapack corrupts the world — the height is permanent per world.
    std::optional<chunk::WorldHeight>        world_height;          // default nullopt (AUTO)

    // ----- Deterministic voxel cleanup (Session 67, v0.5 sub-item (a)) ------
    // Pinhole fill + crumb removal on the voxel grid before matching
    // (research/semantic_reconstruction_proposal.md §2). Default OFF —
    // the default pipeline stays byte-identical. Works in both monolithic
    // and tiled modes; tiled cleanup uses the Session 68 apron scheme and
    // is byte-identical to monolithic cleanup (ctest
    // orchestrate_cleanup_tiled_equality).
    cleanup::CleanupOptions                  cleanup;

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
// voxel_size_for_target — the OrchestrateOptions::target_size derivation,
// exposed so tests (and callers wanting the number up front) share the one
// implementation orchestrate() uses.
//
// Returns the voxel_size (mesh units per voxel) that makes the auto-sized
// voxel grid exactly target_size cells along the mesh AABB's longest axis:
// longest_extent / target_size, widened by a 1e-5 relative headroom factor
// so the voxel layer's ceil(extent / voxel_size) cannot round up to
// target_size + 1 under float division. Exact for target_size < 100000 —
// far beyond the reachable grid (max_grid_cells caps ~1024 per axis) and
// MC's 384-block world height. Axis convention is irrelevant: Y_UP/Z_UP
// permute axes, which cannot change the longest extent.
//
// Throws orchestrate_error if target_size < 1, the mesh has no positions,
// or the longest AABB extent is non-finite or zero (degenerate point mesh).
// ---------------------------------------------------------------------------
float voxel_size_for_target(const mesh::Mesh& m, std::int32_t target_size);

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
