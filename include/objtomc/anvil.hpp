#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "objtomc/nbt.hpp"

namespace objtomc::anvil {

constexpr std::size_t  SECTOR_SIZE        = 4096;
constexpr std::size_t  CHUNKS_PER_REGION  = 1024;
constexpr std::uint8_t MAX_CHUNK_SECTORS  = 255;

enum class CompressionScheme : std::uint8_t {
    Zlib = 2,
};

class anvil_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RegionWriter {
public:
    RegionWriter(std::int32_t region_x, std::int32_t region_z) noexcept;

    // Add one chunk's uncompressed NBT byte buffer to the region.
    //
    // (chunk_x, chunk_z) are world-space chunk coordinates. They must
    // satisfy (chunk_x >> 5) == region_x && (chunk_z >> 5) == region_z,
    // else anvil_error is thrown.
    //
    // The same (chunk_x, chunk_z) cannot be added twice; second call
    // throws anvil_error.
    //
    // uncompressed_nbt is taken by-value (move-friendly path); must be
    // non-empty.
    //
    // timestamp_epoch_s is the chunk's last-modification time written
    // into the timestamp table. nullopt (the default) means "fill with
    // std::time(nullptr) at first write() time".
    //
    // Throws anvil_error("writer is sealed after first write()") if
    // called after write() has run. The writer is single-use-then-frozen
    // for accumulating new chunks; re-emitting the same layout via
    // additional write() calls is allowed (and produces byte-identical
    // output) but no chunks can be added after the seal.
    void add_chunk(std::int32_t chunk_x,
                   std::int32_t chunk_z,
                   std::vector<std::uint8_t> uncompressed_nbt,
                   std::optional<std::int32_t> timestamp_epoch_s = std::nullopt);

    std::size_t chunk_count() const noexcept;

    std::int32_t region_x() const noexcept;
    std::int32_t region_z() const noexcept;

    // Serialize the complete region to `out`. On the FIRST call, any
    // chunks added with nullopt timestamps have their timestamps
    // resolved to std::time(nullptr) and the resolved values are
    // frozen onto the writer; the writer also transitions to a "sealed"
    // state where add_chunk() will throw anvil_error. Subsequent
    // write() calls are allowed and produce byte-identical output to
    // the first call (no fresh time(NULL), no re-shuffling, no
    // re-allocation of sectors). See research/anvil_api_proposal.md
    // §3.6 for the rationale.
    template <nbt::Writer W>
    void write(W& out);

    // Convenience: serialize into a fresh std::vector<std::uint8_t> and
    // return it. Equivalent to constructing an nbt::VectorWriter over a
    // local vector and calling write().
    std::vector<std::uint8_t> to_bytes();

private:
    struct PendingChunk {
        std::int32_t                chunk_x;
        std::int32_t                chunk_z;
        std::vector<std::uint8_t>   uncompressed_nbt;
        std::optional<std::int32_t> timestamp_epoch_s;
    };

    std::int32_t               region_x_;
    std::int32_t               region_z_;
    std::vector<PendingChunk>  pending_;
    bool                       sealed_ = false;
};

extern template void RegionWriter::write<nbt::VectorWriter>(nbt::VectorWriter&);
extern template void RegionWriter::write<nbt::OstreamWriter>(nbt::OstreamWriter&);

}  // namespace objtomc::anvil
