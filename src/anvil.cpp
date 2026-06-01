#include "objtomc/anvil.hpp"

#include <array>
#include <cstring>
#include <ctime>
#include <limits>
#include <utility>

#include <zlib.h>

namespace objtomc::anvil {

namespace {

template <nbt::Writer W>
inline void write_u8(W& out, std::uint8_t v) {
    out.write(&v, 1);
}

template <nbt::Writer W>
inline void write_u24_be(W& out, std::uint32_t v) {
    const std::uint8_t bytes[3] = {
        static_cast<std::uint8_t>((v >> 16) & 0xFFu),
        static_cast<std::uint8_t>((v >>  8) & 0xFFu),
        static_cast<std::uint8_t>( v        & 0xFFu),
    };
    out.write(bytes, 3);
}

template <nbt::Writer W>
inline void write_u32_be(W& out, std::uint32_t v) {
    const std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>((v >> 24) & 0xFFu),
        static_cast<std::uint8_t>((v >> 16) & 0xFFu),
        static_cast<std::uint8_t>((v >>  8) & 0xFFu),
        static_cast<std::uint8_t>( v        & 0xFFu),
    };
    out.write(bytes, 4);
}

template <nbt::Writer W>
inline void write_i32_be(W& out, std::int32_t v) {
    write_u32_be(out, static_cast<std::uint32_t>(v));
}

template <nbt::Writer W>
inline void write_zero_padding(W& out, std::size_t n) {
    constexpr std::size_t kChunk = 256;
    const std::array<std::uint8_t, kChunk> zeros{};
    while (n >= kChunk) {
        out.write(zeros.data(), kChunk);
        n -= kChunk;
    }
    if (n > 0) {
        out.write(zeros.data(), n);
    }
}

std::vector<std::uint8_t>
zlib_compress(const std::vector<std::uint8_t>& input) {
    if (input.size() > static_cast<std::size_t>(
            std::numeric_limits<uLong>::max())) {
        throw anvil_error(
            "chunk uncompressed payload exceeds zlib uLong limit");
    }
    const auto in_size = static_cast<uLong>(input.size());
    const uLong bound = ::compressBound(in_size);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
    uLongf out_len = bound;
    const int rc = ::compress2(out.data(), &out_len,
                               input.data(), in_size,
                               Z_BEST_COMPRESSION);
    if (rc != Z_OK) {
        throw anvil_error("zlib compress2 failed");
    }
    out.resize(static_cast<std::size_t>(out_len));
    return out;
}

}  // namespace

RegionWriter::RegionWriter(std::int32_t region_x,
                           std::int32_t region_z) noexcept
    : region_x_(region_x), region_z_(region_z) {}

void RegionWriter::add_chunk(std::int32_t chunk_x,
                             std::int32_t chunk_z,
                             std::vector<std::uint8_t> uncompressed_nbt,
                             std::optional<std::int32_t> timestamp_epoch_s) {
    if (sealed_) {
        throw anvil_error("writer is sealed after first write()");
    }
    if ((chunk_x >> 5) != region_x_ || (chunk_z >> 5) != region_z_) {
        throw anvil_error("chunk out of region bounds");
    }
    if (uncompressed_nbt.empty()) {
        throw anvil_error("uncompressed_nbt is empty");
    }
    for (const auto& pc : pending_) {
        if (pc.chunk_x == chunk_x && pc.chunk_z == chunk_z) {
            throw anvil_error("duplicate chunk coord");
        }
    }
    pending_.push_back(PendingChunk{
        chunk_x, chunk_z,
        std::move(uncompressed_nbt),
        timestamp_epoch_s,
    });
}

std::size_t RegionWriter::chunk_count() const noexcept {
    return pending_.size();
}

std::int32_t RegionWriter::region_x() const noexcept { return region_x_; }
std::int32_t RegionWriter::region_z() const noexcept { return region_z_; }

template <nbt::Writer W>
void RegionWriter::write(W& out) {
    if (!sealed_) {
        const auto now = static_cast<std::int32_t>(std::time(nullptr));
        for (auto& pc : pending_) {
            if (!pc.timestamp_epoch_s.has_value()) {
                pc.timestamp_epoch_s = now;
            }
        }
        sealed_ = true;
    }

    // Strict-pack append: lay chunks out in ascending location-table-
    // index order. Per-slot fields are filled in two passes (compress,
    // then header emission) so we can compute every sector_offset
    // before writing the first byte.
    struct Slot {
        std::size_t                pending_index;
        std::uint32_t              entry_index;     // 0..1023
        std::vector<std::uint8_t>  compressed;
        std::uint32_t              sector_offset;
        std::uint8_t               sector_count;
    };

    std::vector<Slot> slots;
    slots.reserve(pending_.size());
    for (std::uint32_t local_z = 0; local_z < 32; ++local_z) {
        for (std::uint32_t local_x = 0; local_x < 32; ++local_x) {
            for (std::size_t i = 0; i < pending_.size(); ++i) {
                const auto& pc = pending_[i];
                const auto lx =
                    static_cast<std::uint32_t>(pc.chunk_x) & 31u;
                const auto lz =
                    static_cast<std::uint32_t>(pc.chunk_z) & 31u;
                if (lx == local_x && lz == local_z) {
                    slots.push_back(Slot{
                        i,
                        local_x + local_z * 32u,
                        {},
                        0u,
                        0u,
                    });
                }
            }
        }
    }

    std::uint32_t next_sector = 2;
    for (auto& slot : slots) {
        slot.compressed =
            zlib_compress(pending_[slot.pending_index].uncompressed_nbt);
        const std::size_t payload_bytes = 5u + slot.compressed.size();
        const std::size_t sectors =
            (payload_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
        if (sectors > static_cast<std::size_t>(MAX_CHUNK_SECTORS)) {
            throw anvil_error(
                "chunk exceeds 255 sectors after compression");
        }
        slot.sector_offset = next_sector;
        slot.sector_count  = static_cast<std::uint8_t>(sectors);
        next_sector += static_cast<std::uint32_t>(sectors);
    }

    // Location table: 1024 × (3-byte BE sector_offset + 1-byte sector_count).
    // Unpopulated entries are emitted as four zero bytes.
    std::size_t slot_cursor = 0;
    for (std::uint32_t entry_index = 0;
         entry_index < CHUNKS_PER_REGION;
         ++entry_index) {
        if (slot_cursor < slots.size() &&
            slots[slot_cursor].entry_index == entry_index) {
            write_u24_be(out, slots[slot_cursor].sector_offset);
            write_u8   (out, slots[slot_cursor].sector_count);
            ++slot_cursor;
        } else {
            write_zero_padding(out, 4u);
        }
    }

    // Timestamp table: 1024 × 4-byte BE int32 epoch seconds.
    slot_cursor = 0;
    for (std::uint32_t entry_index = 0;
         entry_index < CHUNKS_PER_REGION;
         ++entry_index) {
        if (slot_cursor < slots.size() &&
            slots[slot_cursor].entry_index == entry_index) {
            const auto& pc = pending_[slots[slot_cursor].pending_index];
            write_i32_be(out, *pc.timestamp_epoch_s);
            ++slot_cursor;
        } else {
            write_zero_padding(out, 4u);
        }
    }

    // Chunk data sectors.
    for (const auto& slot : slots) {
        const auto declared_length =
            static_cast<std::uint32_t>(slot.compressed.size() + 1u);
        write_u32_be(out, declared_length);
        write_u8   (out,
            static_cast<std::uint8_t>(CompressionScheme::Zlib));
        out.write(slot.compressed.data(), slot.compressed.size());

        const std::size_t payload_bytes = 5u + slot.compressed.size();
        const std::size_t allocated_bytes =
            static_cast<std::size_t>(slot.sector_count) * SECTOR_SIZE;
        write_zero_padding(out, allocated_bytes - payload_bytes);
    }
}

std::vector<std::uint8_t> RegionWriter::to_bytes() {
    std::vector<std::uint8_t> buf;
    nbt::VectorWriter sink(buf);
    write(sink);
    return buf;
}

template void RegionWriter::write<nbt::VectorWriter>(nbt::VectorWriter&);
template void RegionWriter::write<nbt::OstreamWriter>(nbt::OstreamWriter&);

}  // namespace objtomc::anvil
