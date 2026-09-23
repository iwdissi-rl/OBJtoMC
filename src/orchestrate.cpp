// ---------------------------------------------------------------------------
// Orchestration layer implementation.
//
// Ties OBJ Loading → Voxelisation → Color Matching → Chunk Schema →
// Region File Writing → level.dat into a single C++ entry point. Per the
// approved proposal at research/orchestration_proposal.md (Session 25 +
// 25.1 maintainer edits at commits 4be2673 + f3f1ef4).
//
// Spec references (cited verbatim per the established project convention,
// see DECISIONS.md 2026-07-01 "Orchestration scope (v0.1)"):
//
//   - MC Java Edition level format schema:
//     https://minecraft.wiki/w/Java_Edition_level_format
//     (fetched 2026-07-01; pre-26.1 / 1.21.5 layout).
//
//   - level.dat gzip framing: zlib's deflateInit2(...) "gzip wrapper" mode
//     selected by windowBits = 15 + 16. See zlib manual + RFC 1952. The
//     MC client auto-detects gzip vs zlib vs uncompressed by inspecting
//     the first few bytes; the convention for level.dat is gzip.
//
//   - Implementation-time verification (Session 25.1 maintainer edit step):
//     research/scratch/orchestration/level_dat_diff.md documents the
//     empirical diff between a vanilla MC 1.21.5 auto-generated level.dat
//     and the proposal §3.5 minimum-viable schema. Result: 7 field-groups
//     added beyond §3.5's minimum (6 Border-tuning, WanderingTrader*×2,
//     DragonFight, CustomBossEvents, ScheduledEvents, plus 3 disabled
//     experimental datapacks in DataPacks). Player intentionally omitted
//     per §3.5 explicit decision.
// ---------------------------------------------------------------------------

#include "objtomc/orchestrate.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "objtomc/anvil.hpp"
#include "objtomc/chunk.hpp"
#include "objtomc/matcher.hpp"
#include "objtomc/gltf.hpp"
#include "objtomc/mesh.hpp"
#include "objtomc/nbt.hpp"
#include "objtomc/palette.hpp"
#include "objtomc/texture.hpp"
#include "objtomc/voxel.hpp"
// Project-internal voxel helpers (swap_axes + closest_point_on_triangle_bary)
// for per-voxel UV recovery — see voxel_internal.hpp Session 48 block.
#include "voxel_internal.hpp"

namespace objtomc::orch {

// ===========================================================================
// default_options — v0.1 defaults per proposal §3.2.
// ===========================================================================
OrchestrateOptions default_options() noexcept {
    OrchestrateOptions o{};
    o.voxel_size              = 0.0f;                                // caller sets this…
    o.target_size             = std::nullopt;                        // …or this (exactly one)
    o.axis_convention         = voxel::AxisConvention::Y_UP;
    o.voxel_grid_dimensions   = std::nullopt;
    o.voxel_grid_origin_world = std::nullopt;
    o.max_grid_cells          = voxel::MAX_GRID_CELLS_DEFAULT;
    o.world_origin            = {0, -60, 0};
    o.matcher_opts            = matcher::default_options();
    o.palette_csv             = std::nullopt;
    o.level_name              = "objtomc world";
    o.spawn_pos               = {0, -60, 0};
    o.game_type               = 1;     // creative
    o.difficulty              = 0;     // peaceful
    o.allow_commands          = true;
    o.day_time                = 6000;  // noon
    o.now_unix_s              = std::nullopt;  // live wall clock by default
    o.verbose                 = false;
    return o;
}

namespace {

// ===========================================================================
// Coordinate-transform helpers per proposal §3.3 / §3.4.
//
// The voxel grid is indexed by (vx, vy, vz) ∈ [0, dim_axis). After
// applying world_origin, the world-space block coordinate is
//   (bx, by, bz) = world_origin + (vx, vy, vz).
//
// Chunk-X / chunk-Z = world_X / world_Z divided by 16, ARITHMETIC shift
// (signed). C++20 guarantees `x >> n` on a signed int does sign-extension,
// so negative world coords correctly map to negative chunk coords
// (verified end-to-end by DECISIONS.md 2026-06-17 "chunkschema Tier-4
// PASS at chunk (-7, -3)" via Session 14's chunk-schema Tier-4-B).
//
// Local x/z within a chunk = world_X bitwise-AND 15 (= modulo-16 for both
// positive and negative). Same for local_z.
//
// Region-X / region-Z = chunk-X / chunk-Z divided by 32 (= >> 5).
// ===========================================================================
struct WorldPos {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

// sRGB transfer pair (IEC 61966-2-1), for the per-voxel emissive product
// (Session 51): glTF combines emissiveFactor and emissiveTexture in
// LINEAR space, while Material.ke and sample_nearest are sRGB-gamma.
inline float srgb_decode(float c) noexcept {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float srgb_encode(float c) noexcept {
    if (c <= 0.0031308f) return 12.92f * c;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline WorldPos voxel_to_world(int vx, int vy, int vz,
                               const std::array<std::int32_t, 3>& world_origin) noexcept {
    return WorldPos{
        world_origin[0] + vx,
        world_origin[1] + vy,
        world_origin[2] + vz};
}

inline std::int32_t world_to_chunk(std::int32_t world_xz) noexcept {
    return world_xz >> 4;   // arithmetic shift: -1 >> 4 == -1 not 0 (C++20 guaranteed)
}
inline std::int32_t world_to_local(std::int32_t world_xz) noexcept {
    // (world_xz & 15) yields the local-within-chunk index for both
    // positive and negative coords. Verified at chunk (-7, -3) per Session 14.
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(world_xz) & 0xFu);
}
inline std::int32_t chunk_to_region(std::int32_t chunk_xz) noexcept {
    return chunk_xz >> 5;
}

// ===========================================================================
// build_level_dat_compound — per proposal §3.5 + the Session 25.1
// implementation-time-verification additions documented in
// research/scratch/orchestration/level_dat_diff.md.
//
// Returns the Compound that will be serialised to NBT bytes, wrapped in a
// "" (empty) name at the file root, then gzip-compressed.
// ===========================================================================
nbt::Compound build_level_dat_compound(const OrchestrateOptions& opts,
                                       std::int64_t              now_unix_ms) {
    nbt::Compound data;

    // ---- Identity / version fields (always required) ----
    data.put_int("DataVersion", LEVEL_DAT_DATA_VERSION);
    data.put_int("version",     19133);    // Anvil-era format flag; constant since 1.13

    {
        nbt::Compound version_compound;
        version_compound.put_int   ("Id",       LEVEL_DAT_DATA_VERSION);
        version_compound.put_string("Name",     "1.21.5");
        version_compound.put_string("Series",   "main");
        version_compound.put_byte  ("Snapshot", 0);
        data.put_compound("Version", std::move(version_compound));
    }

    data.put_string("LevelName",   opts.level_name);
    data.put_long  ("LastPlayed",  now_unix_ms);
    data.put_byte  ("initialized", 1);

    // ---- Spawn point (pre-26.1 schema uses separate Int fields) ----
    data.put_int  ("SpawnX",     opts.spawn_pos[0]);
    data.put_int  ("SpawnY",     opts.spawn_pos[1]);
    data.put_int  ("SpawnZ",     opts.spawn_pos[2]);
    data.put_float("SpawnAngle", 0.0f);

    // ---- Game state ----
    data.put_int ("GameType",         opts.game_type);
    data.put_byte("Difficulty",       opts.difficulty);
    data.put_byte("DifficultyLocked", 1);
    data.put_byte("allowCommands",    opts.allow_commands ? 1 : 0);
    data.put_byte("hardcore",         0);
    data.put_long("Time",             0);
    data.put_long("DayTime",          opts.day_time);

    // ---- Weather (no rain, no thunder) ----
    data.put_byte("raining",          0);
    data.put_int ("rainTime",         0);
    data.put_byte("thundering",       0);
    data.put_int ("thunderTime",      0);
    data.put_int ("clearWeatherTime", 0);

    // ---- World border (core 3 fields + 6 tuning fields from §3.5 diff) ----
    data.put_double("BorderCenterX",        0.0);
    data.put_double("BorderCenterZ",        0.0);
    data.put_double("BorderSize",           60'000'000.0);
    // The next 6 are added per the Session 25.1 / 26 §3.5 implementation-time
    // verification: vanilla MC 1.21.5 always writes them; absence may cause
    // [Server thread/WARN] lines on first load (PASS-criterion-iv blocker).
    data.put_double("BorderSafeZone",       5.0);
    data.put_double("BorderWarningBlocks",  5.0);
    data.put_double("BorderWarningTime",    15.0);
    data.put_double("BorderDamagePerBlock", 0.2);
    data.put_double("BorderSizeLerpTarget", 60'000'000.0);
    data.put_long  ("BorderSizeLerpTime",   0);

    // ---- Wandering trader (added per §3.5 diff; vanilla always writes) ----
    data.put_int("WanderingTraderSpawnChance", 25);
    data.put_int("WanderingTraderSpawnDelay",  24'000);

    // ---- DragonFight (added per §3.5 diff; vanilla writes by default) ----
    {
        nbt::Compound dragon_fight;
        dragon_fight.put_byte("NeedsStateScanning", 1);
        // Gateways: empty list of TAG_Int (vanilla writes this).
        nbt::List gateways{nbt::TagType::Int};
        dragon_fight.put_list("Gateways", std::move(gateways));
        dragon_fight.put_byte("DragonKilled",     0);
        dragon_fight.put_byte("PreviouslyKilled", 0);
        data.put_compound("DragonFight", std::move(dragon_fight));
    }

    // ---- CustomBossEvents + ScheduledEvents (added per §3.5 diff; both empty) ----
    data.put_compound("CustomBossEvents", nbt::Compound{});
    data.put_list("ScheduledEvents", nbt::List{nbt::TagType::Compound});

    // ---- GameRules (proposal §3.5 overrides; MC fills the rest with defaults) ----
    {
        nbt::Compound rules;
        // All values are TAG_String per the MC convention (even boolean rules).
        rules.put_string("doDaylightCycle",     "false");
        rules.put_string("doWeatherCycle",      "false");
        rules.put_string("doMobSpawning",       "false");
        rules.put_string("keepInventory",       "true");
        rules.put_string("commandBlockOutput",  "true");
        rules.put_string("sendCommandFeedback", "true");
        data.put_compound("GameRules", std::move(rules));
    }

    // ---- DataPacks (extended per §3.5 diff: vanilla 1.21.5 disables 3 experimental packs) ----
    {
        nbt::Compound packs;
        nbt::List enabled{nbt::TagType::String};
        enabled.push_string("vanilla");
        packs.put_list("Enabled", std::move(enabled));

        nbt::List disabled{nbt::TagType::String};
        // Vanilla MC 1.21.5 ships these 3 experimental packs disabled by default.
        // Including the same list avoids MC re-adding them on first load
        // (which would create a save-state diff on first close).
        disabled.push_string("minecart_improvements");
        disabled.push_string("redstone_experiments");
        disabled.push_string("trade_rebalance");
        packs.put_list("Disabled", std::move(disabled));
        data.put_compound("DataPacks", std::move(packs));
    }

    // ---- ServerBrands ----
    {
        nbt::List brands{nbt::TagType::String};
        brands.push_string("vanilla");
        data.put_list("ServerBrands", std::move(brands));
    }

    data.put_byte("WasModded", 0);

    // ---- WorldGenSettings (load-bearing: void preset for all 3 dimensions) ----
    {
        nbt::Compound wgs;
        wgs.put_long("seed",              0);
        wgs.put_byte("generate_features", 0);
        wgs.put_byte("bonus_chest",       0);

        nbt::Compound dimensions;
        const std::array<std::pair<const char*, const char*>, 3> dims = {{
            {"minecraft:overworld",  "minecraft:plains"},
            {"minecraft:the_nether", "minecraft:nether_wastes"},
            {"minecraft:the_end",    "minecraft:the_end"},
        }};
        for (const auto& [dim_id, biome] : dims) {
            nbt::Compound dim;
            dim.put_string("type", dim_id);

            nbt::Compound generator;
            generator.put_string("type", "minecraft:flat");

            nbt::Compound settings;
            settings.put_string("biome",    biome);
            settings.put_byte  ("lakes",    0);
            settings.put_byte  ("features", 0);
            // Empty layers list = void dimension. The TAG_Compound element
            // type matches how MC writes non-empty flat configs.
            settings.put_list("layers", nbt::List{nbt::TagType::Compound});
            settings.put_list("structure_overrides", nbt::List{nbt::TagType::String});

            generator.put_compound("settings", std::move(settings));
            dim.put_compound("generator", std::move(generator));
            dimensions.put_compound(dim_id, std::move(dim));
        }
        wgs.put_compound("dimensions", std::move(dimensions));

        data.put_compound("WorldGenSettings", std::move(wgs));
    }

    // ---- Wrap in the outer "Data" compound ----
    nbt::Compound root;
    root.put_compound("Data", std::move(data));
    return root;
}

// ===========================================================================
// gzip_compress — wrap raw bytes in a gzip stream per zlib's deflateInit2()
// "gzip wrapper" mode (windowBits = 15 + 16). See RFC 1952 + zlib manual.
// ===========================================================================
std::vector<std::uint8_t> gzip_compress(const std::vector<std::uint8_t>& in) {
    z_stream zs{};
    // windowBits = 15 (max window) + 16 (gzip wrapper, per zlib docs).
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw orchestrate_error("gzip_compress: deflateInit2 failed");
    }

    zs.next_in  = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());

    std::vector<std::uint8_t> out;
    out.resize(deflateBound(&zs, static_cast<uLong>(in.size())) + 32);
    zs.next_out  = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        throw orchestrate_error("gzip_compress: deflate did not reach STREAM_END (rc=" +
                                std::to_string(rc) + ")");
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

// ===========================================================================
// Validation helpers per proposal §3.1 / §3.3.
// ===========================================================================
void validate_options(const OrchestrateOptions& opts) {
    if (opts.target_size.has_value()) {
        if (opts.voxel_size != 0.0f) {
            throw orchestrate_error(
                "OrchestrateOptions::voxel_size and ::target_size are mutually "
                "exclusive; leave voxel_size at 0 (unset) when target_size is used");
        }
        if (*opts.target_size < 1) {
            throw orchestrate_error(
                "OrchestrateOptions::target_size must be >= 1, got " +
                std::to_string(*opts.target_size));
        }
    } else if (!std::isfinite(opts.voxel_size) || opts.voxel_size <= 0.0f) {
        throw orchestrate_error(
            "OrchestrateOptions::voxel_size must be positive and finite, got " +
            std::to_string(opts.voxel_size) +
            " (set voxel_size > 0 or target_size)");
    }
    if (opts.max_grid_cells <= 0) {
        throw orchestrate_error(
            "OrchestrateOptions::max_grid_cells must be positive, got " +
            std::to_string(opts.max_grid_cells));
    }
}

void validate_world_dir(const std::filesystem::path& world_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(world_dir, ec)) {
        if (!fs::is_directory(world_dir, ec)) {
            throw orchestrate_error("world_dir exists but is not a directory: " +
                                    world_dir.string());
        }
        const auto it_begin = fs::directory_iterator(world_dir, ec);
        if (ec) {
            throw orchestrate_error("world_dir cannot be read: " +
                                    world_dir.string() + " (" + ec.message() + ")");
        }
        if (it_begin != fs::directory_iterator{}) {
            throw orchestrate_error(
                "world_dir already exists and is non-empty: " + world_dir.string() +
                " (refusing to silently overwrite an existing world)");
        }
    }
}

void validate_y_range(int dim_y, const std::array<std::int32_t, 3>& world_origin) {
    const std::int64_t y_lo = world_origin[1];
    const std::int64_t y_hi = static_cast<std::int64_t>(world_origin[1]) + dim_y;
    if (y_lo < chunk::WORLD_BOTTOM_Y) {
        throw orchestrate_error(
            "world_origin[1] (" + std::to_string(world_origin[1]) +
            ") is below WORLD_BOTTOM_Y (" +
            std::to_string(chunk::WORLD_BOTTOM_Y) + ")");
    }
    if (y_hi > chunk::WORLD_TOP_Y) {
        throw orchestrate_error(
            "world_origin[1] + voxel_grid_dim_y (" + std::to_string(y_hi) +
            ") exceeds WORLD_TOP_Y (" +
            std::to_string(chunk::WORLD_TOP_Y) +
            "); reduce dim_y or pick a smaller voxel-grid Y extent");
    }
}

// Bucket entry stored once per voxel during the routing pass (§3.4).
struct VoxelEntry {
    std::int32_t local_x;
    std::int32_t world_y;
    std::int32_t local_z;
    chunk::BlockState state;
};
using ChunkKey  = std::pair<std::int32_t, std::int32_t>;   // (chunk_x, chunk_z)
using RegionKey = std::pair<std::int32_t, std::int32_t>;   // (region_x, region_z)

struct PairHash {
    std::size_t operator()(const std::pair<std::int32_t, std::int32_t>& p) const noexcept {
        return std::hash<std::int64_t>{}(
            (static_cast<std::int64_t>(p.first) << 32) ^
            static_cast<std::uint32_t>(p.second));
    }
};

// Hash for the per-chunk (lx, wy, lz) lookup the BlockProvider consults.
struct LocalPosHash {
    std::size_t operator()(const std::tuple<int, int, int>& t) const noexcept {
        const auto& [a, b, c] = t;
        return std::hash<std::int64_t>{}(
            (static_cast<std::int64_t>(a) << 32) ^
            (static_cast<std::int64_t>(b) << 16) ^
            static_cast<std::uint32_t>(c));
    }
};

}  // namespace

// ===========================================================================
// write_level_dat — public helper. Builds the compound + gzip-wraps + writes.
// ===========================================================================
void write_level_dat(const OrchestrateOptions&     opts,
                     const std::filesystem::path&  level_dat_path,
                     std::int64_t                  now_unix_ms) {
    nbt::Compound root = build_level_dat_compound(opts, now_unix_ms);
    // Serialise to uncompressed NBT (empty root name per MC's level.dat convention).
    const auto uncompressed = nbt::to_bytes("", nbt::Tag(std::move(root)));
    const auto gzipped      = gzip_compress(uncompressed);

    std::ofstream out(level_dat_path, std::ios::binary);
    if (!out) {
        throw orchestrate_error("write_level_dat: cannot open for writing: " +
                                level_dat_path.string());
    }
    out.write(reinterpret_cast<const char*>(gzipped.data()),
              static_cast<std::streamsize>(gzipped.size()));
    if (!out) {
        throw orchestrate_error("write_level_dat: write failed: " +
                                level_dat_path.string());
    }
}

// ===========================================================================
// voxel_size_for_target — target_size → voxel_size derivation (Session 50,
// v0.3 item #3). Contract in orchestrate.hpp.
// ===========================================================================
float voxel_size_for_target(const mesh::Mesh& m, std::int32_t target_size) {
    if (target_size < 1) {
        throw orchestrate_error("voxel_size_for_target: target_size must be >= 1, got " +
                                std::to_string(target_size));
    }
    if (m.positions.empty()) {
        throw orchestrate_error("voxel_size_for_target: mesh has no positions");
    }
    std::array<float, 3> lo = m.positions[0];
    std::array<float, 3> hi = m.positions[0];
    for (const auto& p : m.positions) {
        for (int k = 0; k < 3; ++k) {
            if (p[k] < lo[k]) lo[k] = p[k];
            if (p[k] > hi[k]) hi[k] = p[k];
        }
    }
    const float longest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
    if (!std::isfinite(longest) || longest <= 0.0f) {
        throw orchestrate_error(
            "voxel_size_for_target: mesh AABB longest extent must be positive "
            "and finite, got " + std::to_string(longest));
    }
    // 1e-5 relative headroom keeps ceil(extent / voxel_size) at exactly
    // target_size despite float division rounding (see header contract).
    return longest / static_cast<float>(target_size) * (1.0f + 1e-5f);
}

// ===========================================================================
// orchestrate — the primary entry point. Per proposal §3.1 / §3.4 / §3.6.
// ===========================================================================
void orchestrate(const std::filesystem::path& obj_path,
                 const std::filesystem::path& world_dir,
                 const OrchestrateOptions&    opts) {
    namespace fs = std::filesystem;

    auto log = [&](const std::string& msg) {
        if (opts.verbose) std::cerr << msg << "\n";
    };

    validate_options(opts);
    validate_world_dir(world_dir);

    // Resolve the orchestration's reference timestamp ONCE so the per-chunk
    // anvil timestamps, the level.dat LastPlayed, and any other timestamp
    // we emit all agree. Pinning at the top of orchestrate() is what makes
    // a single run byte-deterministic across all output files. The optional
    // OrchestrateOptions::now_unix_s lets callers pin further across runs
    // (used by the determinism ctest + future reproducible-build needs).
    const std::int64_t now_unix_s = opts.now_unix_s.value_or(
        static_cast<std::int64_t>(std::time(nullptr)));
    const std::int64_t now_unix_ms = now_unix_s * 1000LL;

    // ---- 1. Load mesh (format dispatch on extension, Session 49) ----
    std::string ext = obj_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    const bool is_gltf = (ext == ".gltf" || ext == ".glb");
    log("[mesh]    loading " + obj_path.string() +
        (is_gltf ? " (glTF)" : " (OBJ)"));
    const mesh::Mesh m =
        is_gltf ? gltf::load_gltf(obj_path) : mesh::load_obj(obj_path);
    log("[mesh]    loaded " + std::to_string(m.positions.size()) + " positions, " +
        std::to_string(m.faces.size()) + " faces, " +
        std::to_string(m.materials.size()) + " materials");

    // ---- 2. Voxelise ----
    // target_size (Session 50): derive voxel_size from the loaded mesh so
    // the auto-sized grid is exactly target_size blocks along the AABB's
    // longest axis. validate_options() already guaranteed exactly one of
    // voxel_size / target_size is set.
    const float voxel_size = opts.target_size.has_value()
        ? voxel_size_for_target(m, *opts.target_size)
        : opts.voxel_size;
    if (opts.target_size.has_value()) {
        log("[voxel]   target_size=" + std::to_string(*opts.target_size) +
            " blocks -> voxel_size=" + std::to_string(voxel_size));
    }

    voxel::VoxelizerOptions vopts = voxel::default_options();
    vopts.voxel_size      = voxel_size;
    vopts.axis_convention = opts.axis_convention;
    vopts.dimensions      = opts.voxel_grid_dimensions;
    vopts.origin_world    = opts.voxel_grid_origin_world;
    vopts.max_grid_cells  = opts.max_grid_cells;

    log("[voxel]   voxelizing at voxel_size=" + std::to_string(voxel_size));
    const voxel::VoxelGrid grid = voxel::voxelize_mesh(m, vopts);
    const auto dims = grid.dimensions();
    log("[voxel]   grid dims = " + std::to_string(dims[0]) + " x " +
        std::to_string(dims[1]) + " x " + std::to_string(dims[2]));

    // ---- 3. Y-range validation against MC world height ----
    validate_y_range(dims[1], opts.world_origin);

    const std::size_t occ = grid.occupied_count();
    log("[voxel]   occupied = " + std::to_string(occ));

    if (occ == 0) {
        // Degenerate but valid input: write a world directory with no chunks +
        // a level.dat. The world will load as empty void.
        log("[orch]    warning: no occupied voxels; writing empty world");
    }

    // ---- 4. Drain voxel grid into MatchInput + position arrays ----
    // MatchInput (28 B) instead of full mesh::Material copies (~200 B with
    // its string/byte-vector members) — v0.4 §3.4. Same (kd, ke, d) floats
    // flow to match_batch, so output is byte-identical (ctest
    // match_input_equivalence gates the overload pair).
    std::vector<matcher::MatchInput> mats;
    std::vector<WorldPos>            positions;
    mats.reserve(occ);
    positions.reserve(occ);

    // mesh.materials may be empty (e.g. the bunny fixture has no MTL); the
    // voxelizer coerces face.material_id == -1 → 0 on storage. So material_id
    // values in the grid are always ≥ 0, but materials() may be empty. In that
    // case we synthesise a default input per voxel so the matcher's input is
    // well-defined (default = mid-gray opaque non-emissive per
    // matcher_proposal.md §3.4).
    const bool materials_empty = grid.materials().empty();
    const matcher::MatchInput default_input{
        {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 0.0f}, 1.0f};

    // ---- 4a. Per-voxel texture sampling (Session 48, v0.3 item #1) ----
    // Resolve + decode each material's map_Kd once. map_Kd paths are
    // relative to the .mtl location, which the MTL convention co-locates
    // with the .obj; backslash separators (Windows-authored MTLs are
    // common in the wild) are normalised to '/'. A missing/undecodable
    // texture warns once (TextureCache) and that material falls back to
    // its flat Kd — meshes with no textures on disk (e.g. the repo's
    // Sponza fixture) take the pre-Session-48 path bit-for-bit.
    texture::TextureCache tex_cache;
    std::vector<const texture::Image*> mat_tex(grid.materials().size(), nullptr);
    std::vector<const texture::Image*> mat_tex_ke(grid.materials().size(), nullptr);
    if (!m.uvs.empty()) {
        // Shared resolver for the kd and ke texture slots. `prefix`
        // namespaces the embedded-bytes cache keys so a kd and a ke image
        // with colliding display names on the same material stay distinct.
        const auto resolve = [&](std::size_t i, const std::string& name,
                                 const std::vector<std::uint8_t>& bytes,
                                 const char* prefix) -> const texture::Image* {
            if (!bytes.empty()) {
                // Embedded texture (GLB buffer view / glTF data URI,
                // Session 49): decode from the carried bytes. Key is
                // index-scoped — distinct per material, never a path.
                return tex_cache.load_from_memory(
                    std::string(prefix) + std::to_string(i) + ":" + name,
                    bytes.data(), bytes.size());
            }
            if (name.empty()) return nullptr;
            std::string p = name;
            std::replace(p.begin(), p.end(), '\\', '/');
            fs::path tp(p);
            if (tp.is_relative()) tp = obj_path.parent_path() / tp;
            return tex_cache.load(tp);
        };
        for (std::size_t i = 0; i < grid.materials().size(); ++i) {
            const mesh::Material& gm = grid.materials()[i];
            mat_tex[i]    = resolve(i, gm.map_kd, gm.map_kd_bytes, "embedded:");
            mat_tex_ke[i] = resolve(i, gm.map_ke, gm.map_ke_bytes, "embedded_ke:");
        }
    }
    const auto has_any = [](const std::vector<const texture::Image*>& v) {
        return std::any_of(v.begin(), v.end(),
                           [](const texture::Image* t) { return t != nullptr; });
    };
    const bool any_texture = has_any(mat_tex) || has_any(mat_tex_ke);
    if (any_texture) log("[texture] per-voxel texture sampling active");

    const auto grid_origin = grid.origin_world();
    const float vs = grid.voxel_size();

    grid.for_each_voxel([&](int vx, int vy, int vz, std::int32_t mid) {
        if (materials_empty) {
            mats.push_back(default_input);
        } else {
            const mesh::Material& gm =
                grid.materials()[static_cast<std::size_t>(mid)];
            matcher::MatchInput mat{gm.kd, gm.ke, gm.d};
            const texture::Image* tex =
                any_texture ? mat_tex[static_cast<std::size_t>(mid)] : nullptr;
            const texture::Image* tex_ke =
                any_texture ? mat_tex_ke[static_cast<std::size_t>(mid)] : nullptr;
            if (tex != nullptr || tex_ke != nullptr) {
                // Recover the UV at this voxel's winning triangle: the
                // closest point on the triangle to the voxel centre is
                // exactly the point the disambiguation measured (same
                // arithmetic via closest_point_on_triangle_bary), and its
                // barycentric (s, t) interpolates the corner UVs.
                const auto tri = grid.triangle_at_unchecked(vx, vy, vz);
                const auto& face = m.faces[static_cast<std::size_t>(tri)];
                if (face.uv_indices[0] >= 0 &&
                    face.uv_indices[1] >= 0 &&
                    face.uv_indices[2] >= 0) {
                    const auto v0 = voxel::detail::swap_axes(
                        m.positions[face.position_indices[0]], vopts.axis_convention);
                    const auto v1 = voxel::detail::swap_axes(
                        m.positions[face.position_indices[1]], vopts.axis_convention);
                    const auto v2 = voxel::detail::swap_axes(
                        m.positions[face.position_indices[2]], vopts.axis_convention);
                    const std::array<float, 3> box_centre{
                        grid_origin[0] + (static_cast<float>(vx) + 0.5f) * vs,
                        grid_origin[1] + (static_cast<float>(vy) + 0.5f) * vs,
                        grid_origin[2] + (static_cast<float>(vz) + 0.5f) * vs};
                    float s = 0.0f;
                    float t = 0.0f;
                    voxel::detail::closest_point_on_triangle_bary(
                        box_centre, v0, v1, v2, s, t);
                    const float w0 = 1.0f - s - t;
                    const auto& uv0 = m.uvs[static_cast<std::size_t>(face.uv_indices[0])];
                    const auto& uv1 = m.uvs[static_cast<std::size_t>(face.uv_indices[1])];
                    const auto& uv2 = m.uvs[static_cast<std::size_t>(face.uv_indices[2])];
                    const float u = w0 * uv0[0] + s * uv1[0] + t * uv2[0];
                    const float v = w0 * uv0[1] + s * uv1[1] + t * uv2[1];
                    // Sampled texel replaces the flat Kd; illum is
                    // inherited from the parent material.
                    if (tex != nullptr) {
                        mat.kd = texture::sample_nearest(*tex, u, v);
                        // BLEND alpha-in-texture (Session 52): effective
                        // alpha = baseColorFactor.a × texel.a. Alpha is
                        // LINEAR on both sides (the sRGB transfer never
                        // applies to alpha), so multiply directly — no
                        // decode/encode. Without this, Sketchfab BLEND
                        // materials with default factor alpha 1 render
                        // fully opaque.
                        if (gm.d_from_map_alpha) {
                            mat.d *= texture::sample_alpha(*tex, u, v);
                        }
                    }
                    // Emissive mask (Session 51): glTF emission is
                    // emissiveFactor × emissiveTexture in LINEAR space.
                    // mat.ke already carries encode(factor × strength), so
                    // decode both sides, multiply, re-encode. Black texels
                    // drive ke → 0, dropping the voxel out of the matcher's
                    // emissive branch (the DamagedHelmet all-froglight
                    // failure mode).
                    if (tex_ke != nullptr) {
                        const auto ke_s = texture::sample_nearest(*tex_ke, u, v);
                        for (std::size_t c = 0; c < 3; ++c) {
                            mat.ke[c] = srgb_encode(srgb_decode(mat.ke[c]) *
                                                    srgb_decode(ke_s[c]));
                        }
                    }
                }
            }
            mats.push_back(std::move(mat));
        }
        positions.push_back(voxel_to_world(vx, vy, vz, opts.world_origin));
    });

    // ---- 5. Match materials → BlockStates ----
    // v0.1 limitation: load_palette_csv() requires a blocks.json path which
    // the palette layer bakes in privately via OBJTOMC_VENDOR_MINECRAFT_DATA_DIR.
    // The CLI's --palette-csv flag is accepted but ignored in v0.1 with a
    // warning. Plumbing the baked path into a public API is v0.2 work.
    log("[matcher] loading palette");
    if (opts.palette_csv.has_value()) {
        std::cerr << "[matcher] warning: --palette-csv (" << opts.palette_csv->string()
                  << ") is v0.2 work; using load_default_palette() for v0.1\n";
    }
    const palette::PaletteDB db = palette::load_default_palette();

    matcher::Matcher matcher_(db, opts.matcher_opts);
    log("[matcher] matching " + std::to_string(mats.size()) + " voxels");

    std::vector<chunk::BlockState> states(mats.size());
    matcher_.match_batch(mats, states);

    // ---- 6. Bucket by chunk (§3.4 two-pass routing) ----
    std::unordered_map<ChunkKey, std::vector<VoxelEntry>, PairHash> chunks;
    chunks.reserve(occ / 100 + 16);   // rough — most voxels share chunks

    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i];
        const auto cx = world_to_chunk(p.x);
        const auto cz = world_to_chunk(p.z);
        const auto lx = world_to_local(p.x);
        const auto lz = world_to_local(p.z);
        chunks[{cx, cz}].push_back(
            VoxelEntry{lx, p.y, lz, std::move(states[i])});
    }

    log("[chunk]   routing " + std::to_string(occ) + " voxels into " +
        std::to_string(chunks.size()) + " chunks");

    // ---- 7. Sort chunk keys for byte-deterministic output (§3.4 note) ----
    std::vector<ChunkKey> chunk_keys;
    chunk_keys.reserve(chunks.size());
    for (const auto& kv : chunks) chunk_keys.push_back(kv.first);
    std::sort(chunk_keys.begin(), chunk_keys.end());

    // ---- 8. Group chunks by region, write each region ----
    std::unordered_map<RegionKey, std::vector<ChunkKey>, PairHash> regions;
    for (const auto& ck : chunk_keys) {
        const auto rx = chunk_to_region(ck.first);
        const auto rz = chunk_to_region(ck.second);
        regions[{rx, rz}].push_back(ck);
    }

    // Sort region keys too, for deterministic file-system write order.
    std::vector<RegionKey> region_keys;
    region_keys.reserve(regions.size());
    for (const auto& kv : regions) region_keys.push_back(kv.first);
    std::sort(region_keys.begin(), region_keys.end());

    log("[anvil]   writing " + std::to_string(regions.size()) + " region file(s)");

    // ---- 9. Create world_dir + world_dir/region/ ----
    {
        std::error_code ec;
        fs::create_directories(world_dir, ec);
        if (ec) {
            throw orchestrate_error("cannot create world_dir " +
                                    world_dir.string() + ": " + ec.message());
        }
        fs::create_directories(world_dir / WORLD_DIR_REGION_SUBDIR, ec);
        if (ec) {
            throw orchestrate_error("cannot create world_dir/region: " + ec.message());
        }
    }

    // ---- 10. Per-region: build + serialise + write ----
    for (const auto& rkey : region_keys) {
        anvil::RegionWriter writer(rkey.first, rkey.second);
        const auto& chunks_in_region = regions.at(rkey);

        // chunks_in_region is already in sorted order because chunk_keys was
        // sorted before grouping (region grouping preserves the relative
        // order of its source keys).
        for (const auto& ck : chunks_in_region) {
            const auto& entries = chunks.at(ck);

            // Per-chunk (lx, wy, lz) → BlockState lookup map for the
            // BlockProvider lambda.
            std::unordered_map<std::tuple<int, int, int>,
                               chunk::BlockState, LocalPosHash> per_chunk;
            per_chunk.reserve(entries.size());
            for (const auto& e : entries) {
                per_chunk[{e.local_x, e.world_y, e.local_z}] = e.state;
            }

            chunk::ChunkBuilder builder(ck.first, ck.second, "minecraft:plains");
            builder.build([&](int lx, int wy, int lz) -> chunk::BlockState {
                const auto it = per_chunk.find({lx, wy, lz});
                return (it != per_chunk.end()) ? it->second
                                               : chunk::BlockState::air();
            });

            writer.add_chunk(ck.first, ck.second, builder.to_bytes(),
                             static_cast<std::int32_t>(now_unix_s));
        }

        const auto region_path = world_dir / WORLD_DIR_REGION_SUBDIR /
            ("r." + std::to_string(rkey.first) + "." +
             std::to_string(rkey.second) + ".mca");
        std::ofstream out(region_path, std::ios::binary);
        if (!out) {
            throw orchestrate_error("cannot open region file for writing: " +
                                    region_path.string());
        }
        nbt::OstreamWriter w(out);
        writer.write(w);
        log("[anvil]   wrote " + region_path.string());
    }

    // ---- 11. Write level.dat ----
    const auto level_dat_path = world_dir / "level.dat";
    write_level_dat(opts, level_dat_path, now_unix_ms);
    log("[level]   wrote " + level_dat_path.string());

    log("[orch]    done");
}

}  // namespace objtomc::orch
