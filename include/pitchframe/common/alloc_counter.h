#pragma once
// Heap allocation counter for hot-path verification.
// Only active when compiled with -DPITCHFRAME_COUNT_ALLOCS.
// Overrides global operator new/delete — use only in the pitchframe binary,
// never in the test or bench targets (GTest allocates freely).
#ifdef PITCHFRAME_COUNT_ALLOCS
#include <cstddef>

namespace pitchframe {
    void  alloc_counter_reset();
    long  alloc_counter_get();
    void  alloc_counter_set_active(bool);
}
#endif // PITCHFRAME_COUNT_ALLOCS
