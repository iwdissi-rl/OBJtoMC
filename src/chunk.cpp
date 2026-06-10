#include "objtomc/chunk.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "objtomc/nbt.hpp"

namespace objtomc::chunk {

namespace {

// ---------------------------------------------------------------------------
// One immutable air state, returned by reference from BlockState::air().
// ---------------------------------------------------------------------------
const BlockState& air_singleton() noexcept {
    static const BlockState kAir{"minecraft:air", {}};
    return kAir;
}

// ---------------------------------------------------------------------------
// Hash for BlockState. Hashes `name`, then for each (key, value) in sorted
// order (std::map iterates sorted) combines std::hash<std::string> of both.
// Per research/chunk_schema_proposal.md §3.1.
//
// hash_combine() is the boost-pattern mixer with the 64-bit golden-ratio
// constant; std::size_t is 64-bit on the project's x86_64 / Linux target.
// ---------------------------------------------------------------------------
inline void hash_combine(std::size_t& seed, std::size_t v) noexcept {
    constexpr std::size_t kMagic =
        static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
    seed ^= v + kMagic + (seed << 6) + (seed >> 2);
}

struct BlockStateHash {
    std::size_t operator()(const BlockState& bs) const noexcept {
        const std::hash<std::string> h;
        std::size_t seed = h(bs.name);
        for (const auto& [k, v] : bs.properties) {
            hash_combine(seed, h(k));
            hash_combine(seed, h(v));
        }
        return seed;
    }
};

// ---------------------------------------------------------------------------
// bits_per_block per proposal §3.4:
//
//   bits_per_block = max(4, bit_width(palette_size - 1))
//
// std::bit_width(0) == 0; for palette_size <= 1 the max clause returns 4.
// That value is never actually serialised when palette_size == 1 (data field
// is omitted), but the function is well-defined for any input.
// ---------------------------------------------------------------------------
inline unsigned bits_per_block(std::size_t palette_size) noexcept {
    const std::size_t max_index = palette_size > 0 ? palette_size - 1u : 0u;
    const unsigned width = static_cast<unsigned>(
        std::bit_width(static_cast<std::uint64_t>(max_index)));
    return std::max(4u, width);
}

// ---------------------------------------------------------------------------
// LSB-first non-straddling pack per proposal §3.4 (pseudocode reproduced
// verbatim, modulo the C++ syntax). Bits at the high end of each long that
// don't fit a full entry are left zero — entries never straddle a 64-bit
// boundary.
//
// The result reinterprets each uint64_t pack into int64_t via static_cast.
// Under C++20's mandatory two's-complement representation
// (§[basic.fundamental]/3) this is well-defined for any source bit pattern
// — proposal §3.4 "Signed vs unsigned in NBT" paragraph; OPEN_QUESTIONS.md
// 2026-06-09 explicitly retired the implementation-defined-behaviour
// concern this conversion used to carry.
// ---------------------------------------------------------------------------
inline std::vector<std::int64_t> pack_lsb_first_nonstraddling(
        std::span<const std::uint32_t> values, unsigned bits) {
    const unsigned   entries_per_long = 64u / bits;
    const std::size_t longs_needed =
        (values.size() + entries_per_long - 1u) / entries_per_long;
    const std::uint64_t mask = (bits == 64u)
                             ? ~std::uint64_t{0}
                             : ((std::uint64_t{1} << bits) - 1u);

    std::vector<std::int64_t> out(longs_needed, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t   long_idx = i / entries_per_long;
        const unsigned      slot     = static_cast<unsigned>(i % entries_per_long);
        const std::uint64_t word     = static_cast<std::uint64_t>(values[i]) & mask;
        std::uint64_t cur = static_cast<std::uint64_t>(out[long_idx]);
        cur |= (word << (slot * bits));
        out[long_idx] = static_cast<std::int64_t>(cur);
    }
    return out;
}

// ---------------------------------------------------------------------------
// BlockState → palette-entry compound. Always emits Name; emits Properties
// only when the property map is non-empty. Properties iterate in sorted
// (std::map) order per proposal §3.1.
// ---------------------------------------------------------------------------
nbt::Compound block_state_to_nbt(const BlockState& bs) {
    nbt::Compound c;
    c.put_string("Name", bs.name);
    if (!bs.properties.empty()) {
        nbt::Compound props;
        for (const auto& [k, v] : bs.properties) {
            props.put_string(k, v);
        }
        c.put_compound("Properties", std::move(props));
    }
    return c;
}

// ---------------------------------------------------------------------------
// Build the per-section compound. Handles both the single-entry-palette
// case (no `data` field, matching proposal §3.4) and the multi-entry case
// (palette + packed `data`).
//
// biomes stays at 15a's single-biome-per-chunk shape (palette = [biome_id],
// no data); proper per-cell biome paletting lands in 15c.
// ---------------------------------------------------------------------------
nbt::Compound make_section_compound(
        int section_index,
        const std::string& biome_id,
        const std::vector<BlockState>& palette_order,
        const std::array<std::uint32_t, 4096>& block_indices) {
    nbt::Compound section;
    section.put_byte("Y", static_cast<std::int8_t>(section_index - 4));

    {
        nbt::Compound block_states;
        nbt::List palette_list(nbt::TagType::Compound);
        for (const auto& bs : palette_order) {
            palette_list.push_compound(block_state_to_nbt(bs));
        }
        block_states.put_list("palette", std::move(palette_list));

        if (palette_order.size() > 1) {
            const unsigned bits = bits_per_block(palette_order.size());
            std::vector<std::int64_t> packed = pack_lsb_first_nonstraddling(
                std::span<const std::uint32_t>{block_indices.data(),
                                               block_indices.size()},
                bits);
            block_states.put_long_array("data", std::move(packed));
        }
        section.put_compound("block_states", std::move(block_states));
    }

    {
        nbt::Compound biomes;
        nbt::List biome_palette(nbt::TagType::String);
        biome_palette.push_string(biome_id);
        biomes.put_list("palette", std::move(biome_palette));
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
    // an empty list (proposal §5 — "type TAG_End (empty list)").
    root_.put_list("block_entities", nbt::List(nbt::TagType::End));
    root_.put_list("fluid_ticks",    nbt::List(nbt::TagType::End));
    root_.put_list("block_ticks",    nbt::List(nbt::TagType::End));
    root_.put_list("PostProcessing", nbt::List(nbt::TagType::End));

    root_.put_compound("structures", nbt::Compound{});

    {
        nbt::Compound heightmaps;
        // 15b still emits all-zero heightmaps; the per-column predicate scan
        // lands in 15c. The shape (4 keys × 37 longs each) is locked from 15a
        // — see proposal §1.5 cross-checks 1 & 2.
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

    // Section-major Y-Z-X iteration per proposal §3.2. The provider is
    // called exactly once per (lx, world_y, lz) for the full
    // 16 × 384 × 16 grid (98 304 calls per chunk). Within each section we
    // build a deduplicated palette + a 4096-entry index array, then either
    // emit the single-palette form (no `data`) or pack the indices via
    // pack_lsb_first_nonstraddling.
    nbt::List sections(nbt::TagType::Compound);
    for (int section_index = 0; section_index < SECTIONS_PER_CHUNK; ++section_index) {
        const int section_y = section_index - 4;

        std::unordered_map<BlockState, std::uint16_t, BlockStateHash> palette_index;
        std::vector<BlockState> palette_order;
        std::array<std::uint32_t, 4096> block_indices{};

        for (int ly = 0; ly < SECTION_SIZE; ++ly) {
            const int world_y = section_y * SECTION_SIZE + ly;
            for (int lz = 0; lz < SECTION_SIZE; ++lz) {
                for (int lx = 0; lx < SECTION_SIZE; ++lx) {
                    BlockState state = provider(lx, world_y, lz);
                    if (state.name.empty()) {
                        throw chunk_error(
                            "provider returned BlockState with empty name");
                    }

                    std::uint16_t idx;
                    if (auto it = palette_index.find(state);
                            it != palette_index.end()) {
                        idx = it->second;
                    } else {
                        if (palette_order.size() >= 65535u) {
                            throw chunk_error(
                                "palette overflow: section has too many "
                                "distinct block states (> 65535)");
                        }
                        idx = static_cast<std::uint16_t>(palette_order.size());
                        palette_order.push_back(state);
                        palette_index.emplace(std::move(state), idx);
                    }

                    const std::size_t bi =
                        static_cast<std::size_t>(ly) * 256u +
                        static_cast<std::size_t>(lz) * 16u +
                        static_cast<std::size_t>(lx);
                    block_indices[bi] = idx;
                }
            }
        }

        sections.push_compound(make_section_compound(
            section_index, biome_id_, palette_order, block_indices));
    }
    root_.put_list("sections", std::move(sections));

    built_ = true;
}

std::vector<std::uint8_t> ChunkBuilder::to_bytes() {
    if (!built_) {
        throw chunk_error("ChunkBuilder::to_bytes called before build()");
    }
    return nbt::to_bytes("", nbt::Tag(std::move(root_)));
}

}  // namespace objtomc::chunk
