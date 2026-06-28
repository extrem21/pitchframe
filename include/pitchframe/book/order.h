#pragma once
#include <cstdint>

namespace pitchframe {

struct Order {
    uint64_t ref    = 0;
    uint32_t price  = 0;  // fixed-point, 4 implied decimal places
    uint32_t shares = 0;
    uint16_t locate = 0;
    char     side   = 0;  // 'B' or 'S'
};

} // namespace pitchframe
