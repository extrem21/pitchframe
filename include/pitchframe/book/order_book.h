// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#pragma once
#include "pitchframe/book/order.h"
#include "pitchframe/book/price_level.h"
#include <cstdint>
#include <map>
#include <unordered_map>

namespace pitchframe {

// Per-symbol limit order book.
//
// Order lookup:
//   std::unordered_map<order_ref, Order>  — O(1) lookup by ID.
//   Cancel / delete / replace all arrive by order ID, so this is required.
//
// Price levels (Option A from design doc):
//   Fixed array [bids_/asks_] centered at base_price_ (set on first add).
//   Covers [base_price_ - k_half_band, base_price_ + k_half_band) ticks.
//   ±2048 ticks = ±$0.2048 — handles normal intra-day range for most stocks.
//   Prices outside the window go to std::map overflow (one per side).
//   Overflow handles stock gaps and halt/reopen dislocations; ops are O(log n)
//   but rare, so amortised cost is fine. Raise this tradeoff in interviews
//   before the interviewer asks.
//
// best_bid / best_ask scan the array O(k_array_size) and check the overflow map.
// This is acceptable for the correctness milestone and will be replaced with
// tracked-index O(1) maintenance before the benchmarking milestone.
class OrderBook {
public:
    static constexpr int k_half_band  = 2048;
    static constexpr int k_array_size = k_half_band * 2;

    OrderBook() { orders_.reserve(1024); }

    // Returns false if the order reference is unknown (cancel / delete / replace on
    // a ref we never saw). Signals a gap or corrupt stream, not a crash.
    bool add    (uint64_t ref, char side, uint32_t price, uint32_t shares, uint16_t locate);
    bool cancel (uint64_t ref, uint32_t cancelled_shares);
    bool remove (uint64_t ref);
    bool replace(uint64_t orig_ref, uint64_t new_ref, uint32_t new_shares, uint32_t new_price);
    // Handles both E and C: reduces resting order shares; removes order if fully filled.
    bool execute(uint64_t ref, uint32_t executed_shares);

    // Returns 0 when the relevant side is empty.
    uint32_t best_bid() const;
    uint32_t best_ask() const;

    int      active_orders()  const { return static_cast<int>(orders_.size()); }
    uint64_t shares_traded()  const { return shares_traded_; }

private:
    std::unordered_map<uint64_t, Order> orders_;

    uint32_t base_price_ = 0;
    bool     base_set_   = false;

    PriceLevel bids_[k_array_size] = {};
    PriceLevel asks_[k_array_size] = {};

    std::map<uint32_t, PriceLevel> bid_overflow_;
    std::map<uint32_t, PriceLevel> ask_overflow_;

    uint64_t shares_traded_ = 0;

    bool        in_band         (uint32_t price) const;
    int         price_to_idx    (uint32_t price) const;
    PriceLevel& level           (char side, uint32_t price);
    void        remove_from_level(const Order& o);
};

} // namespace pitchframe
