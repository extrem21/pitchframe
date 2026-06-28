#pragma once
#include <cstdint>

namespace pitchframe {

struct PriceLevel {
    uint32_t total_shares = 0;
    uint32_t order_count  = 0;

    bool empty() const { return total_shares == 0; }
};

} // namespace pitchframe
