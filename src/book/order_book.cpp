// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#include "pitchframe/book/order_book.h"
#include <algorithm>
#include <limits>

namespace pitchframe {

bool OrderBook::in_band(uint32_t price) const {
    if (!base_set_) return false;
    int32_t delta = static_cast<int32_t>(price) - static_cast<int32_t>(base_price_);
    return delta >= -k_half_band && delta < k_half_band;
}

int OrderBook::price_to_idx(uint32_t price) const {
    return static_cast<int32_t>(price) - static_cast<int32_t>(base_price_) + k_half_band;
}

PriceLevel& OrderBook::level(char side, uint32_t price) {
    if (in_band(price)) {
        int idx = price_to_idx(price);
        return (side == 'B') ? bids_[idx] : asks_[idx];
    }
    // std::map::operator[] default-constructs on first access (new price level).
    // On subsequent calls the existing entry is returned — no extra allocation.
    return ((side == 'B') ? bid_overflow_ : ask_overflow_)[price];
}

void OrderBook::remove_from_level(const Order& o) {
    if (in_band(o.price)) {
        int        idx = price_to_idx(o.price);
        PriceLevel& lv = (o.side == 'B') ? bids_[idx] : asks_[idx];
        lv.total_shares -= std::min(o.shares, lv.total_shares);
        if (lv.order_count > 0) lv.order_count--;
    } else {
        auto& ov = (o.side == 'B') ? bid_overflow_ : ask_overflow_;
        auto  it = ov.find(o.price);
        if (it != ov.end()) {
            it->second.total_shares -= std::min(o.shares, it->second.total_shares);
            if (it->second.order_count > 0) it->second.order_count--;
        }
    }
}

bool OrderBook::add(uint64_t ref, char side, uint32_t price, uint32_t shares, uint16_t locate) {
    if (!base_set_) { base_price_ = price; base_set_ = true; }

    orders_.emplace(ref, Order{ref, price, shares, locate, side});

    PriceLevel& lv = level(side, price);
    lv.total_shares += shares;
    lv.order_count++;
    return true;
}

bool OrderBook::cancel(uint64_t ref, uint32_t cancelled_shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;

    Order& o = it->second;
    uint32_t actual = std::min(cancelled_shares, o.shares);
    level(o.side, o.price).total_shares -= actual;
    o.shares -= actual;
    // Per ITCH spec, X is always a partial cancel; order stays in orders_.
    return true;
}

bool OrderBook::remove(uint64_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;
    remove_from_level(it->second);
    orders_.erase(it);
    return true;
}

bool OrderBook::replace(uint64_t orig_ref, uint64_t new_ref,
                        uint32_t new_shares, uint32_t new_price) {
    auto it = orders_.find(orig_ref);
    if (it == orders_.end()) return false;
    Order orig = it->second;           // copy before erase invalidates iterator
    remove_from_level(orig);
    orders_.erase(it);
    add(new_ref, orig.side, new_price, new_shares, orig.locate);
    return true;
}

bool OrderBook::execute(uint64_t ref, uint32_t executed_shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;

    Order& o = it->second;
    uint32_t actual = std::min(executed_shares, o.shares);

    PriceLevel& lv = level(o.side, o.price);
    lv.total_shares -= actual;
    o.shares        -= actual;
    shares_traded_  += actual;

    if (o.shares == 0) {
        if (lv.order_count > 0) lv.order_count--;
        orders_.erase(it);
    }
    return true;
}

uint32_t OrderBook::best_bid() const {
    uint32_t result = 0;

    // Scan array high → low (highest index = highest price).
    // O(k_array_size) — will be replaced with a tracked index before benchmarking.
    if (base_set_) {
        for (int i = k_array_size - 1; i >= 0; --i) {
            if (bids_[i].total_shares > 0) {
                int32_t p = static_cast<int32_t>(base_price_) + (i - k_half_band);
                if (p > 0) result = static_cast<uint32_t>(p);
                break;
            }
        }
    }

    // Check overflow: rbegin() = highest bid price in overflow.
    for (auto it = bid_overflow_.rbegin(); it != bid_overflow_.rend(); ++it) {
        if (it->second.total_shares > 0) {
            result = std::max(result, it->first);
            break;
        }
    }

    return result;
}

uint32_t OrderBook::best_ask() const {
    uint32_t result = std::numeric_limits<uint32_t>::max();

    // Scan array low → high (lowest index = lowest price).
    if (base_set_) {
        for (int i = 0; i < k_array_size; ++i) {
            if (asks_[i].total_shares > 0) {
                int32_t p = static_cast<int32_t>(base_price_) + (i - k_half_band);
                if (p > 0) { result = static_cast<uint32_t>(p); break; }
            }
        }
    }

    // Check overflow: begin() = lowest ask price in overflow.
    for (auto it = ask_overflow_.begin(); it != ask_overflow_.end(); ++it) {
        if (it->second.total_shares > 0) {
            result = std::min(result, it->first);
            break;
        }
    }

    return (result == std::numeric_limits<uint32_t>::max()) ? 0 : result;
}

} // namespace pitchframe
