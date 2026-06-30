#ifdef PITCHFRAME_COUNT_ALLOCS
#include "pitchframe/common/alloc_counter.h"
#include <cstdlib>

static long g_count  = 0;
static bool g_active = false;

namespace pitchframe {
    void alloc_counter_reset()           { g_count = 0; }
    long alloc_counter_get()             { return g_count; }
    void alloc_counter_set_active(bool b){ g_active = b; }
}

// Global operator new override — counts every heap allocation while active.
// Uses abort() on failure; no exception thrown (compatible with -fno-exceptions).
void* operator new(std::size_t sz) {
    if (g_active) ++g_count;
    void* p = std::malloc(sz);
    if (!p) std::abort();
    return p;
}
void* operator new[](std::size_t sz) {
    if (g_active) ++g_count;
    void* p = std::malloc(sz);
    if (!p) std::abort();
    return p;
}
void operator delete(void* p)                      noexcept { std::free(p); }
void operator delete[](void* p)                    noexcept { std::free(p); }
void operator delete(void* p, std::size_t)         noexcept { std::free(p); }
void operator delete[](void* p, std::size_t)       noexcept { std::free(p); }
#endif // PITCHFRAME_COUNT_ALLOCS
