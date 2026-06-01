#include "objtomc/anvil.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

bool check_empty_region(std::int32_t region_x, std::int32_t region_z) {
    objtomc::anvil::RegionWriter region(region_x, region_z);

    if (region.chunk_count() != 0u) {
        std::printf("FAIL: chunk_count() = %zu for fresh writer, expected 0\n",
                    region.chunk_count());
        return false;
    }
    if (region.region_x() != region_x || region.region_z() != region_z) {
        std::printf("FAIL: region_x/z accessors do not echo constructor args\n");
        return false;
    }

    const std::vector<std::uint8_t> bytes = region.to_bytes();

    if (bytes.size() != 2u * objtomc::anvil::SECTOR_SIZE) {
        std::printf("FAIL: empty region (%d, %d) produced %zu bytes, "
                    "expected %zu\n",
                    region_x, region_z, bytes.size(),
                    2u * objtomc::anvil::SECTOR_SIZE);
        return false;
    }

    const auto first_nonzero =
        std::find_if(bytes.begin(), bytes.end(),
                     [](std::uint8_t b){ return b != 0u; });
    if (first_nonzero != bytes.end()) {
        const auto off =
            static_cast<std::size_t>(first_nonzero - bytes.begin());
        std::printf("FAIL: empty region (%d, %d) has non-zero byte at "
                    "offset %zu (value 0x%02X)\n",
                    region_x, region_z, off,
                    static_cast<unsigned>(*first_nonzero));
        return false;
    }

    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= check_empty_region(0, 0);
    ok &= check_empty_region(-3, -7);

    if (!ok) {
        return 1;
    }

    std::printf("PASS: anvil_empty_region: RegionWriter(0,0) and "
                "RegionWriter(-3,-7) both produce 8192 bytes of zeros.\n");
    return 0;
}
