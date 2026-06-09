#include "objtomc/chunk.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "objtomc/nbt.hpp"

namespace objtomc::chunk {

namespace {

// One immutable air state, returned by reference from BlockState::air().
const BlockState& air_singleton() noexcept {
    static const BlockState kAir{"minecraft:air", {}};
    return kAir;
}

// Build the per-section compound for an all-air section at the given
// section index (0..SECTIONS_PER_CHUNK-1). Single-entry palette, no `data`
// field — the spec lets us omit `data` when the palette has one entry
// (research/chunk_schema_proposal.md §3.4).
nbt::Compound make_all_air_section(int section_index,
                                   const std::string& biome_id) {
    nbt::Compound section;
    const int section_y = section_index - 4;
    section.put_byte("Y", static_cast<std::int8_t>(section_y));

    {
        nbt::Compound block_states;
        nbt::List palette(nbt::TagType::Compound);
        nbt::Compound air_entry;
        air_entry.put_string("Name", "minecraft:air");
        palette.push_compound(std::move(air_entry));
        block_states.put_list("palette", std::move(palette));
        section.put_compound("block_states", std::move(block_states));
    }

    {
        nbt::Compound biomes;
        nbt::List palette(nbt::TagType::String);
        palette.push_string(biome_id);
        biomes.put_list("palette", std::move(palette));
        section.put_compound("biomes", std::move(biomes));
    }

    return section;
}

}  // namespace

const BlockState& BlockState::air() noexcept { return air_singleton(); }

ChunkBuilder::ChunkBuilder(std::int32_t chunk_x,
                           std::int32_t chunk_z,
                           std::string  biome_id)
    : chunk_x_(chunk_x), chunk_z_(chunk_z), biome_id_(std::move(biome_id)) {
    if (biome_id_.empty()) {
        throw chunk_error("ChunkBuilder: biome_id must not be empty");
    }
}

std::int32_t       ChunkBuilder::chunk_x()  const noexcept { return chunk_x_;  }
std::int32_t       ChunkBuilder::chunk_z()  const noexcept { return chunk_z_;  }
const std::string& ChunkBuilder::biome_id() const noexcept { return biome_id_; }
bool               ChunkBuilder::built()    const noexcept { return built_;    }

void ChunkBuilder::build_erased(ProviderFn provider) {
    if (built_) {
        throw chunk_error("ChunkBuilder::build called twice on same instance");
    }

    // Slice 15a — all-air short-circuit. The provider is captured by the
    // public template's std::function wrapper but is NOT invoked here:
    // palette construction, bit-packing, and heightmap predicate scans
    // arrive in 15b/15c per research/chunk_schema_proposal.md §6. For
    // 15a we emit the all-air structural skeleton directly, which is
    // already enough to exercise the §3.12 (a) byte-pattern contract.
    (void)provider;

    root_ = nbt::Compound{};

    root_.put_int   ("DataVersion",   4325);
    root_.put_int   ("xPos",          chunk_x_);
    root_.put_int   ("yPos",          -4);
    root_.put_int   ("zPos",          chunk_z_);
    root_.put_string("Status",        "minecraft:full");
    root_.put_long  ("LastUpdate",    0);
    root_.put_long  ("InhabitedTime", 0);
    root_.put_byte  ("isLightOn",     0);

    // Empty TAG_Lists. Element type TAG_End is the canonical encoding for
    // an empty list (proposal §5 — "type TAG_End (empty list)"); the
    // underlying writer emits the element-type byte unconditionally and
    // no items, which twoolie/NBT and anvil-parser both parse cleanly.
    root_.put_list("block_entities", nbt::List(nbt::TagType::End));
    root_.put_list("fluid_ticks",    nbt::List(nbt::TagType::End));
    root_.put_list("block_ticks",    nbt::List(nbt::TagType::End));
    root_.put_list("PostProcessing", nbt::List(nbt::TagType::End));

    root_.put_compound("structures", nbt::Compound{});

    {
        nbt::Compound heightmaps;
        // 256 × 9-bit non-straddling → 37 longs (proposal §1.5 cross-check 1).
        // All-zero arrays mean "no qualifying block in any column", which is
        // the correct heightmap value for an all-air chunk.
        constexpr std::size_t kHeightmapLongs = 37;
        heightmaps.put_long_array("MOTION_BLOCKING",
                                  std::vector<std::int64_t>(kHeightmapLongs, 0));
        heightmaps.put_long_array("MOTION_BLOCKING_NO_LEAVES",
                                  std::vector<std::int64_t>(kHeightmapLongs, 0));
        heightmaps.put_long_array("OCEAN_FLOOR",
                                  std::vector<std::int64_t>(kHeightmapLongs, 0));
        heightmaps.put_long_array("WORLD_SURFACE",
                                  std::vector<std::int64_t>(kHeightmapLongs, 0));
        root_.put_compound("Heightmaps", std::move(heightmaps));
    }

    {
        nbt::List sections(nbt::TagType::Compound);
        for (int i = 0; i < SECTIONS_PER_CHUNK; ++i) {
            sections.push_compound(make_all_air_section(i, biome_id_));
        }
        root_.put_list("sections", std::move(sections));
    }

    built_ = true;
}

std::vector<std::uint8_t> ChunkBuilder::to_bytes() {
    if (!built_) {
        throw chunk_error("ChunkBuilder::to_bytes called before build()");
    }
    return nbt::to_bytes("", nbt::Tag(std::move(root_)));
}

}  // namespace objtomc::chunk
