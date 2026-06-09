#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "objtomc/nbt.hpp"  // for nbt::to_bytes (return type compat with anvil)

namespace objtomc::chunk {

// ---------------------------------------------------------------------------
// Format constants for MC 1.21.5 / DataVersion 4325. Values verified
// empirically against research/anvil_ground_truth.md and the Session-14
// cross-checks recorded in research/chunk_schema_proposal.md §1.5.
// ---------------------------------------------------------------------------
constexpr int SECTION_SIZE        = 16;     // block side length
constexpr int SECTIONS_PER_CHUNK  = 24;     // 24 sections × 16 blocks = 384 Y
constexpr int WORLD_BOTTOM_Y      = -64;    // lowest world Y for 1.21.5
constexpr int WORLD_TOP_Y         = 320;    // exclusive; 384 = 320 - (-64)

// ---------------------------------------------------------------------------
// BlockState: name + properties. Equality is name+properties-equal. Hashing
// for use in std::unordered_map lives in src/chunk.cpp (BlockStateHash).
// Air is reachable via the static BlockState::air() factory.
// ---------------------------------------------------------------------------
struct BlockState {
    std::string                         name;        // "minecraft:oak_stairs"
    std::map<std::string, std::string>  properties;  // alphabetised by key

    static const BlockState& air() noexcept;         // {"minecraft:air", {}}
};

inline bool operator==(const BlockState& a, const BlockState& b) noexcept {
    return a.name == b.name && a.properties == b.properties;
}
inline bool operator!=(const BlockState& a, const BlockState& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
// Errors. Programmer-misuse only. Valid input never throws.
// ---------------------------------------------------------------------------
class chunk_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------------------
// ChunkBuilder: synthesises a complete MC 1.21.5 chunk-NBT tree from a
// caller-supplied BlockProvider callable. Outputs uncompressed NBT bytes
// suitable for anvil::RegionWriter::add_chunk.
//
//   ChunkBuilder cb(chunk_x, chunk_z, "minecraft:plains");
//   cb.build([&](int x, int y, int z) -> BlockState { … });
//   std::vector<std::uint8_t> bytes = cb.to_bytes();
//   region_writer.add_chunk(chunk_x, chunk_z, std::move(bytes));
//
// Lifecycle: construction → exactly one build(...) → exactly one to_bytes().
// Calling to_bytes() before build() throws chunk_error. Calling build()
// twice throws chunk_error. (Single-use builder; no incremental edits in
// v0.1.)
// ---------------------------------------------------------------------------
class ChunkBuilder {
public:
    // chunk_x, chunk_z are world-space chunk coordinates and become the
    // chunk-NBT xPos / zPos fields. biome_id is a single biome string
    // (e.g. "minecraft:plains") applied to every section's biome palette.
    // Both must be non-empty.
    ChunkBuilder(std::int32_t chunk_x,
                 std::int32_t chunk_z,
                 std::string  biome_id);

    // Materialise the chunk's section + heightmap + biome tree by calling
    // `provider(local_x, world_y, local_z)` for every position in the
    // 16 × 384 × 16 grid:
    //   - local_x ∈ [0, SECTION_SIZE)
    //   - world_y ∈ [WORLD_BOTTOM_Y, WORLD_TOP_Y)
    //   - local_z ∈ [0, SECTION_SIZE)
    //
    // The provider is invoked in section-major order: for each section
    // index in 0..SECTIONS_PER_CHUNK, then ly, lz, lx (X fastest).
    //
    // Provider exceptions propagate unmodified. The provider is called
    // exactly once per position.
    //
    // Throws chunk_error if called twice on the same instance.
    template <typename BlockProvider>
    void build(BlockProvider&& provider);

    // Serialise the built chunk to uncompressed NBT bytes per nbt::to_bytes.
    // The result is suitable for direct hand-off to:
    //     anvil::RegionWriter::add_chunk(chunk_x, chunk_z, std::move(result));
    //
    // Throws chunk_error if called before build() has succeeded.
    std::vector<std::uint8_t> to_bytes();

    // Read-only accessors for tests and diagnostics.
    std::int32_t       chunk_x()  const noexcept;
    std::int32_t       chunk_z()  const noexcept;
    const std::string& biome_id() const noexcept;
    bool               built()    const noexcept;

private:
    // Erased entry point — the public template build() forwards into this
    // with a captured std::function. Mild type-erasure overhead at the
    // call site is acceptable: 98 304 calls per chunk × ~10 ns extra
    // per call ≈ 1 ms per chunk. The implementation prompt MAY inline the
    // erased path with a more aggressive monomorphisation; this is a
    // private detail and the public concept doesn't change.
    using ProviderFn = std::function<BlockState(int, int, int)>;
    void build_erased(ProviderFn provider);

    std::int32_t  chunk_x_;
    std::int32_t  chunk_z_;
    std::string   biome_id_;
    bool          built_ = false;

    // The built tree lives behind a single owned nbt::Compound. The
    // implementation prompt's src/chunk.cpp builds this in build_erased()
    // and serialises it in to_bytes().
    nbt::Compound root_;
};

// Template definition — provided in the header so callers can supply any
// callable without explicit instantiation. The body forwards to the
// type-erased private path.
template <typename BlockProvider>
void ChunkBuilder::build(BlockProvider&& provider) {
    build_erased(ProviderFn(std::forward<BlockProvider>(provider)));
}

}  // namespace objtomc::chunk
