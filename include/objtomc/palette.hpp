#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace objtomc::palette {

// ---------------------------------------------------------------------------
// RGB — three uint8 channels in sRGB space (the "human-readable" colour
// representation; gamma-decoded counterpart lives on the entry itself).
// ---------------------------------------------------------------------------
struct RGB {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

inline bool operator==(RGB a, RGB b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
inline bool operator!=(RGB a, RGB b) noexcept { return !(a == b); }

// ---------------------------------------------------------------------------
// BlockPaletteEntry — one row of the catalogue. Fields:
//
//   name             "minecraft:stone" — canonical MC block id, namespaced
//   properties       empty in v0.1; reserved for future property-variant
//                    coverage (oak_stairs[facing=north,...] etc.)
//   avg_color_srgb   diffuse-texture average colour in sRGB (0..255 per channel)
//   avg_color_linear same colour gamma-decoded to linear-RGB float [0, 1]
//   opacity          MC's filterLight, 0..15 (15 = blocks all light)
//   luminance        MC's emitLight,   0..15 (0 = emits no light)
//   is_full_cube     true iff the block occupies a full 1x1x1 voxel
//                    (slabs/stairs/fences are NOT full cubes; v0.1 matcher
//                    restricts to is_full_cube == true)
//   texture_path     relative path to the principal diffuse texture
//                    (e.g. "block/stone.png"); empty if not recorded
// ---------------------------------------------------------------------------
struct BlockPaletteEntry {
    std::string                         name;
    std::map<std::string, std::string>  properties;
    RGB                                 avg_color_srgb;
    std::array<float, 3>                avg_color_linear;
    std::uint8_t                        opacity;
    std::uint8_t                        luminance;
    bool                                is_full_cube;
    std::string                         texture_path;
};

// ---------------------------------------------------------------------------
// Errors. Programmer misuse (unknown name lookup) and IO failures (catalogue
// not found, malformed CSV, version mismatch) both surface as a single
// exception type. The caller's recovery path is the same ("the catalogue is
// unusable; the user must re-extract or provide a different path").
// ---------------------------------------------------------------------------
class palette_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// PaletteDB — immutable catalogue of MC 1.21.5 blocks. Construction is via
// the load_* free functions below; the class itself has no public mutators.
//
// Iteration: range-for over the DB yields entries in catalogue insertion
// order (= the CSV row order, which is sorted by minecraft-data block id).
//
// Lookup: get(name) is O(1) average via the internal name->index map;
// throws palette_error on miss. find(name) returns nullptr on miss.
// ---------------------------------------------------------------------------
class PaletteDB {
public:
    using const_iterator = std::vector<BlockPaletteEntry>::const_iterator;

    PaletteDB() = default;

    std::size_t                size() const noexcept;
    const BlockPaletteEntry&   get(const std::string& name) const;
    const BlockPaletteEntry*   find(const std::string& name) const noexcept;
    const_iterator             begin() const noexcept;
    const_iterator             end()   const noexcept;

    // Construction is via the load_* functions below; this is exposed so
    // tests + the extractor's "build then assert" pattern can synthesise
    // a DB directly without round-tripping through CSV.
    explicit PaletteDB(std::vector<BlockPaletteEntry> entries);

private:
    std::vector<BlockPaletteEntry>                       entries_;
    std::unordered_map<std::string, std::size_t>         name_index_;
};

// ---------------------------------------------------------------------------
// load_default_palette — convenience loader that resolves the committed
// catalogue files at their canonical project-root-relative paths:
//
//   data/block_palette_db.csv
//   third_party/minecraft-data/pc/1.21.5/blocks.json
//
// The CMake build bakes the project root into a string constant so this
// works regardless of the caller's cwd. Throws palette_error on any IO
// failure or version mismatch.
// ---------------------------------------------------------------------------
PaletteDB load_default_palette();

// ---------------------------------------------------------------------------
// load_palette_csv — explicit loader for tests + future tooling that wants
// to point at a non-default catalogue (e.g. a 1.21.6 CSV when that lands,
// or a fixture CSV under tests/fixtures/palette/). blocks_json_path must
// point at the matching minecraft-data version's blocks.json so the loader
// can validate names + assert the version match.
// ---------------------------------------------------------------------------
PaletteDB load_palette_csv(const std::filesystem::path& csv_path,
                           const std::filesystem::path& blocks_json_path);

}  // namespace objtomc::palette
