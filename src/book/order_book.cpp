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
    return ((side == 'B') ? bid_overflow_ : ask_overflow_)[price];
}

// Returns true if the level's total_shares hit zero after removal.
bool OrderBook::remove_from_level(const Order& o) {
    if (in_band(o.price)) {
        int        idx = price_to_idx(o.price);
        PriceLevel& lv = (o.side == 'B') ? bids_[idx] : asks_[idx];
        lv.total_shares -= std::min(o.shares, lv.total_shares);
        if (lv.order_count > 0) lv.order_count--;
        return lv.total_shares == 0;
    } else {
        auto& ov = (o.side == 'B') ? bid_overflow_ : ask_overflow_;
        auto  it = ov.find(o.price);
        if (it != ov.end()) {
            it->second.total_shares -= std::min(o.shares, it->second.total_shares);
            if (it->second.order_count > 0) it->second.order_count--;
            return it->second.total_shares == 0;
        }
        return false;
    }
}

void OrderBook::rescan_best_bid() {
    best_bid_ = 0;
    if (base_set_) {
        for (int i = k_array_size - 1; i >= 0; --i) {
            if (bids_[i].total_shares > 0) {
                int32_t p = static_cast<int32_t>(base_price_) + (i - k_half_band);
                if (p > 0) { best_bid_ = static_cast<uint32_t>(p); return; }
            }
        }
    }
    for (auto it = bid_overflow_.rbegin(); it != bid_overflow_.rend(); ++it) {
        if (it->second.total_shares > 0) { best_bid_ = it->first; return; }
    }
}

void OrderBook::rescan_best_ask() {
    uint32_t result = std::numeric_limits<uint32_t>::max();
    if (base_set_) {
        for (int i = 0; i < k_array_size; ++i) {
            if (asks_[i].total_shares > 0) {
                int32_t p = static_cast<int32_t>(base_price_) + (i - k_half_band);
                if (p > 0) { result = static_cast<uint32_t>(p); break; }
            }
        }
    }
    for (auto it = ask_overflow_.begin(); it != ask_overflow_.end(); ++it) {
        if (it->second.total_shares > 0) { result = std::min(result, it->first); break; }
    }
    best_ask_ = (result == std::numeric_limits<uint32_t>::max()) ? 0 : result;
}

bool OrderBook::add(uint64_t ref, char side, uint32_t price, uint32_t shares, uint16_t locate) {
    if (!base_set_) { base_price_ = price; base_set_ = true; }

    orders_.emplace(ref, Order{ref, price, shares, locate, side});

    PriceLevel& lv = level(side, price);
    lv.total_shares += shares;
    lv.order_count++;

    if (side == 'B') {
        if (price > best_bid_) best_bid_ = price;
    } else {
        if (best_ask_ == 0 || price < best_ask_) best_ask_ = price;
    }
    return true;
}

bool OrderBook::cancel(uint64_t ref, uint32_t cancelled_shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;

    Order& o = it->second;
    uint32_t actual = std::min(cancelled_shares, o.shares);
    PriceLevel& lv = level(o.side, o.price);
    lv.total_shares -= actual;
    o.shares -= actual;

    if (lv.total_shares == 0) {
        if      (o.side == 'B' && o.price == best_bid_) rescan_best_bid();
        else if (o.side == 'S' && o.price == best_ask_) rescan_best_ask();
    }
    return true;
}

bool OrderBook::remove(uint64_t ref) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return false;
    const Order o = it->second;
    bool drained = remove_from_level(o);
    orders_.erase(it);
    if (drained) {
        if      (o.side == 'B' && o.price == best_bid_) rescan_best_bid();
        else if (o.side == 'S' && o.price == best_ask_) rescan_best_ask();
    }
    return true;
}

bool OrderBook::replace(uint64_t orig_ref, uint64_t new_ref,
                        uint32_t new_shares, uint32_t new_price) {
    auto it = orders_.find(orig_ref);
    if (it == orders_.end()) return false;
    Order orig = it->second;
    bool drained = remove_from_level(orig);
    orders_.erase(it);
    if (drained) {
        if      (orig.side == 'B' && orig.price == best_bid_) rescan_best_bid();
        else if (orig.side == 'S' && orig.price == best_ask_) rescan_best_ask();
    }
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
        char     side    = o.side;
        uint32_t price   = o.price;
        bool     drained = (lv.total_shares == 0);
        orders_.erase(it);
        if (drained) {
            if      (side == 'B' && price == best_bid_) rescan_best_bid();
            else if (side == 'S' && price == best_ask_) rescan_best_ask();
        }
    }
    return true;
}

uint32_t OrderBook::best_bid() const { return best_bid_; }
uint32_t OrderBook::best_ask() const { return best_ask_; }

} // namespace pitchframe
