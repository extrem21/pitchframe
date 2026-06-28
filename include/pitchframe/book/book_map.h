#pragma once
#include "pitchframe/book/order_book.h"
#include "pitchframe/common/types.h"

namespace pitchframe {

// Maps stock_locate codes to per-symbol OrderBook instances.
//
// Lookup: O(1) array index — no hash, no branch on the common path.
//
// Books are heap-allocated lazily on the first add-order for each locate.
// This is a deliberate simplification: a production system would pre-allocate
// all books during the R-message (Stock Directory) pass at startup so the
// order-processing hot path is completely allocation-free. The lazy path here
// bounds total allocations to ≤ k_max_locate (8192) over the whole session.
class BookMap {
public:
    BookMap()  = default;
    ~BookMap() { for (auto* b : books_) delete b; }

    BookMap(const BookMap&)            = delete;
    BookMap& operator=(const BookMap&) = delete;

    OrderBook* get(uint16_t locate) const {
        return (locate < k_max_locate) ? books_[locate] : nullptr;
    }

    OrderBook* get_or_create(uint16_t locate) {
        if (locate >= k_max_locate) return nullptr;
        if (!books_[locate]) books_[locate] = new OrderBook();
        return books_[locate];
    }

private:
    OrderBook* books_[k_max_locate] = {};
};

} // namespace pitchframe
