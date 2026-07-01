// HOT PATH — no allocation, no exceptions, no virtual dispatch.
//
// Benchmarks:
//   BM_ParseAddOrder       — amortised decode latency for 'A' messages (pure parser, no book)
//   BM_BookAdd             — add order on a warm 27K-order book (AAPL-scale at market close)
//   BM_BookAdd_StdMap      — same add, but price levels stored in std::map (A/B comparison)
//   BM_BookCancel          — partial cancel on warm 27K-order book
//   BM_BookCancel_StdMap   — same cancel, std::map price levels (A/B comparison)
//   BM_BookDelete          — delete on warm 27K-order book (PauseTiming replenishment)
//   BM_BookExecute         — partial execute on warm 27K-order book
//   BM_BookReplace         — replace on warm 27K-order book
//   BM_FullReplay          — end-to-end throughput, streaming (requires ITCH_REPLAY_FILE env var)
//
// Book-op benchmarks pre-populate with k_warm_orders = 27,000 resting orders at
// AAPL-scale prices before any measurement starts.  A benchmark on an empty book
// is useless: hash map load factor and cache pressure are completely different from
// the real trading-hours state.
//
// BM_BookAdd_StdMap / BM_BookCancel_StdMap use the same 27K warm state but route
// price-level access through std::map<uint32_t, PriceLevel> instead of the fixed
// array.  The delta is the empirical cache-miss cost of pointer-chasing tree nodes
// versus direct array indexing.

#include <benchmark/benchmark.h>
#include "pitchframe/book/order_book.h"
#include "pitchframe/parser/itch_messages.h"
#include "pitchframe/parser/file_reader.h"
#include "pitchframe/parser/itch_parser.h"
#include "pitchframe/book/book_map.h"
#include "pitchframe/common/types.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>

namespace {

// AAPL on 30 Dec 2019: ~27,106 resting orders, closing price ~$291.63.
// Use these constants to reproduce a realistic book state.
constexpr uint32_t k_aapl_price  = 2'916'300;  // $291.63 in ITCH fixed-point ticks
constexpr uint16_t k_locate      = 1;
constexpr int      k_warm_orders = 27'000;

// Populate a book with `count` orders spread realistically around k_aapl_price.
// Bids: 1..100 ticks below the touch.  Asks: 0..99 ticks above.
// This matches the tight cluster of a large-cap liquid stock.
// Returns the next unused ref number so callers can add more without collision.
uint64_t populate_book(pitchframe::OrderBook& book, int count, uint64_t start_ref = 0) {
    uint64_t ref = start_ref;
    for (int i = 0; i < count; ++i) {
        char     side;
        uint32_t price;
        if (i % 2 == 0) {
            side  = 'B';
            price = k_aapl_price - 1 - static_cast<uint32_t>(i % 100);
        } else {
            side  = 'S';
            price = k_aapl_price     + static_cast<uint32_t>(i % 100);
        }
        book.add(ref++, side, price, 100, k_locate);
    }
    return ref;
}

// Synthetic big-endian bytes for a minimal valid 'A' message (36 bytes).
// Price = k_aapl_price = 2,916,300 = 0x002C7FCC in big-endian.
alignas(64) static const uint8_t k_add_msg[36] = {
    'A',                                                    // [0]    type
    0x00, 0x01,                                             // [1-2]  locate = 1
    0x00, 0x00,                                             // [3-4]  tracking
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                    // [5-10] timestamp
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,        // [11-18] order_ref = 1
    'B',                                                    // [19]  side
    0x00, 0x00, 0x00, 0x64,                                 // [20-23] shares = 100
    'A',  'A',  'P',  'L',  ' ',  ' ',  ' ',  ' ',         // [24-31] stock symbol
    0x00, 0x2C, 0x7F, 0xCC,                                 // [32-35] price = $291.63
};

// ---------------------------------------------------------------------------
// A/B comparison book: identical order-lookup map, but price levels stored in
// std::map<uint32_t, PriceLevel> instead of a fixed array.
//
// std::map is a red-black tree: each node is a separate heap allocation, so
// finding a price level requires following pointer chains into non-contiguous
// memory.  With 100 distinct price levels on each side and 27K orders, every
// cancel or add that touches a price level is likely a cache miss into a cold
// tree node, versus a direct array index (single computed offset, no pointer
// chase) in the real OrderBook.
// ---------------------------------------------------------------------------
struct StdMapOrderBook {
    std::unordered_map<uint64_t, pitchframe::Order> orders_;
    std::map<uint32_t, pitchframe::PriceLevel>       bids_;
    std::map<uint32_t, pitchframe::PriceLevel>       asks_;

    StdMapOrderBook() { orders_.reserve(1024); }

    bool add(uint64_t ref, char side, uint32_t price, uint32_t shares, uint16_t locate) {
        orders_.emplace(ref, pitchframe::Order{ref, price, shares, locate, side});
        pitchframe::PriceLevel& lv = (side == 'B') ? bids_[price] : asks_[price];
        lv.total_shares += shares;
        lv.order_count++;
        return true;
    }

    bool cancel(uint64_t ref, uint32_t cancelled_shares) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return false;
        pitchframe::Order& o = it->second;
        uint32_t actual = std::min(cancelled_shares, o.shares);
        auto& m   = (o.side == 'B') ? bids_ : asks_;
        auto  lit = m.find(o.price);
        if (lit != m.end()) lit->second.total_shares -= actual;
        o.shares -= actual;
        return true;
    }
};

uint64_t populate_stdmap_book(StdMapOrderBook& book, int count, uint64_t start_ref = 0) {
    uint64_t ref = start_ref;
    for (int i = 0; i < count; ++i) {
        char     side  = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(i % 100)
            : k_aapl_price     + static_cast<uint32_t>(i % 100);
        book.add(ref++, side, price, 100, k_locate);
    }
    return ref;
}

} // namespace

// ---------------------------------------------------------------------------
// 1. Parser — amortised decode latency for 'A' messages
//    Measures the pure parse cost (bswap + memcpy field extraction).
//    No book mutation, no memory side effects beyond the output struct.
//
//    k_add_msg must be copied to a non-const local so the compiler cannot
//    constant-fold the inline decode_add_order call (all inputs would otherwise
//    be compile-time constants, producing a ~0 ns measurement).
//    DoNotOptimize on both buf and out prevents the optimiser from eliminating
//    the reads and writes entirely.
// ---------------------------------------------------------------------------
static void BM_ParseAddOrder(benchmark::State& state) {
    alignas(64) uint8_t msg_buf[36];
    std::memcpy(msg_buf, k_add_msg, 36);
    pitchframe::MsgAddOrder out;
    for (auto _ : state) {
        benchmark::DoNotOptimize(msg_buf);       // prevent constant-folding of reads
        pitchframe::decode_add_order(msg_buf, 36, out);
        benchmark::DoNotOptimize(out);           // prevent elimination of writes to out
        benchmark::ClobberMemory();              // full memory barrier; no stores hoisted out
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseAddOrder);

// ---------------------------------------------------------------------------
// 2. Add order — warm 27K-order book
//    Book grows slightly during the run (new refs added); starts at AAPL-scale.
// ---------------------------------------------------------------------------
static void BM_BookAdd(benchmark::State& state) {
    pitchframe::OrderBook book;
    uint64_t next_ref = populate_book(book, k_warm_orders);

    for (auto _ : state) {
        // Alternate bid prices across a 50-tick range to avoid single-level degenerate case.
        uint32_t price = k_aapl_price - 1 - static_cast<uint32_t>(next_ref % 50);
        benchmark::DoNotOptimize(book.add(next_ref++, 'B', price, 100, k_locate));
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookAdd);

// ---------------------------------------------------------------------------
// 2b. Add order — std::map price levels (A/B vs BM_BookAdd)
//     Identical warm state and operation; only the price-level data structure
//     differs.  The delta isolates the cost of tree pointer-chasing vs array
//     indexing.  This is the empirical justification for the array choice.
// ---------------------------------------------------------------------------
static void BM_BookAdd_StdMap(benchmark::State& state) {
    StdMapOrderBook book;
    uint64_t next_ref = populate_stdmap_book(book, k_warm_orders);

    for (auto _ : state) {
        uint32_t price = k_aapl_price - 1 - static_cast<uint32_t>(next_ref % 50);
        benchmark::DoNotOptimize(book.add(next_ref++, 'B', price, 100, k_locate));
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookAdd_StdMap);

// ---------------------------------------------------------------------------
// 3. Partial cancel — warm 27K-order book
//    Orders carry 1,000,000 shares so no level ever drains during the run
//    (no rescan triggered); measures the common-case cancel path.
// ---------------------------------------------------------------------------
static void BM_BookCancel(benchmark::State& state) {
    pitchframe::OrderBook book;
    for (int i = 0; i < k_warm_orders; ++i) {
        char     side  = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(i % 100)
            : k_aapl_price     + static_cast<uint32_t>(i % 100);
        book.add(static_cast<uint64_t>(i), side, price, 1'000'000, k_locate);
    }

    uint64_t target = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.cancel(target % k_warm_orders, 100));
        ++target;
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookCancel);

// ---------------------------------------------------------------------------
// 3b. Partial cancel — std::map price levels (A/B vs BM_BookCancel)
//     Same 27K warm state, same 1M-share orders, same 100-share cancel.
//     The only difference is that the price-level update goes through
//     std::map::find (O(log n), pointer chase) instead of a direct array index.
// ---------------------------------------------------------------------------
static void BM_BookCancel_StdMap(benchmark::State& state) {
    StdMapOrderBook book;
    for (int i = 0; i < k_warm_orders; ++i) {
        char     side  = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(i % 100)
            : k_aapl_price     + static_cast<uint32_t>(i % 100);
        book.add(static_cast<uint64_t>(i), side, price, 1'000'000, k_locate);
    }

    uint64_t target = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.cancel(target % k_warm_orders, 100));
        ++target;
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookCancel_StdMap);

// ---------------------------------------------------------------------------
// 4. Delete order — warm 27K-order book
//    PauseTiming replenishment keeps book at steady-state 27K throughout.
//    The replenishment (add) is excluded from timing.
//
//    Expected to be significantly slower than BM_BookReplace even though
//    replace() is logically remove + add.  The difference is allocator state:
//    replace() always frees the node it just allocated one iteration ago (hot in
//    the per-thread free list); delete() here rotates through 27K long-lived
//    nodes whose allocator metadata is cold.  On macOS malloc this gap can be
//    10x+.  This is the same root cause as the 121M-alloc finding and shows
//    exactly why we need a pre-allocated flat hash map.
// ---------------------------------------------------------------------------
static void BM_BookDelete(benchmark::State& state) {
    pitchframe::OrderBook book;
    std::vector<uint64_t> live_refs(k_warm_orders);
    uint64_t next_ref = 0;

    for (int i = 0; i < k_warm_orders; ++i) {
        char     side  = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(i % 100)
            : k_aapl_price     + static_cast<uint32_t>(i % 100);
        book.add(next_ref, side, price, 100, k_locate);
        live_refs[i] = next_ref++;
    }

    int idx = 0;
    for (auto _ : state) {
        int slot = idx % k_warm_orders;
        benchmark::DoNotOptimize(book.remove(live_refs[slot]));

        // Replenish the deleted order so the book stays at 27K.
        state.PauseTiming();
        char     side  = (slot % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(slot % 100)
            : k_aapl_price     + static_cast<uint32_t>(slot % 100);
        book.add(next_ref, side, price, 100, k_locate);
        live_refs[slot] = next_ref++;
        state.ResumeTiming();

        ++idx;
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookDelete);

// ---------------------------------------------------------------------------
// 5. Partial execute — warm 27K-order book
//    Same large-shares trick as BM_BookCancel: orders never fully fill, so
//    no order removal or rescan is triggered.  Measures the common partial-fill path.
// ---------------------------------------------------------------------------
static void BM_BookExecute(benchmark::State& state) {
    pitchframe::OrderBook book;
    for (int i = 0; i < k_warm_orders; ++i) {
        char     side  = (i % 2 == 0) ? 'B' : 'S';
        uint32_t price = (side == 'B')
            ? k_aapl_price - 1 - static_cast<uint32_t>(i % 100)
            : k_aapl_price     + static_cast<uint32_t>(i % 100);
        book.add(static_cast<uint64_t>(i), side, price, 1'000'000, k_locate);
    }

    uint64_t target = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.execute(target % k_warm_orders, 100));
        ++target;
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookExecute);

// ---------------------------------------------------------------------------
// 6. Replace — warm 27K-order book
//    One "hot" order is repeatedly replaced with a fresh ref and a slightly
//    varied price.  The remaining 26,999 orders stay static, keeping the hash
//    map at realistic load.  Replace is remove + add so it exercises both paths.
//
//    This measures the hot-allocator path: every iteration frees the node
//    that was allocated one iteration earlier, so the allocator recycles it
//    from its per-thread free list without touching OS state.  Compare with
//    BM_BookDelete which measures the cold-allocator path on long-lived nodes.
// ---------------------------------------------------------------------------
static void BM_BookReplace(benchmark::State& state) {
    pitchframe::OrderBook book;
    uint64_t next_ref = populate_book(book, k_warm_orders);

    // Add one extra "hot" order that we will replace every iteration.
    uint64_t current_ref = next_ref++;
    book.add(current_ref, 'B', k_aapl_price - 1, 100, k_locate);

    for (auto _ : state) {
        // Vary price across a 50-tick range to exercise different array slots.
        uint32_t new_price = k_aapl_price - 1 - static_cast<uint32_t>(next_ref % 50);
        benchmark::DoNotOptimize(
            book.replace(current_ref, next_ref, 100, new_price)
        );
        current_ref = next_ref++;
    }
    benchmark::ClobberMemory();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BookReplace);

// ---------------------------------------------------------------------------
// 7. Full replay throughput — end-to-end streaming (I/O included)
//    Requires ITCH_REPLAY_FILE env var pointing to a decompressed ITCH 5.0 file.
//    Only 1 iteration (full replay takes ~100s); reports items/sec = messages/sec.
//    I/O is included in timing because the 7.7 GB file exceeds available RAM.
// ---------------------------------------------------------------------------
static void BM_FullReplay(benchmark::State& state) {
    const char* path = std::getenv("ITCH_REPLAY_FILE");
    if (!path) {
        state.SkipWithMessage("Set ITCH_REPLAY_FILE to a decompressed ITCH 5.0 file to run");
        return;
    }

    for (auto _ : state) {
        pitchframe::FileReader reader;
        if (!reader.open(path)) {
            state.SkipWithError("Cannot open ITCH_REPLAY_FILE");
            return;
        }

        pitchframe::ITCHParser parser;
        pitchframe::BookMap    books;
        uint64_t total = 0;

        while (true) {
            if (!reader.ensure(2)) break;
            uint16_t msg_len = reader.read_u16_be();
            if (msg_len == 0 || msg_len > pitchframe::k_max_msg_len) break;
            if (!reader.ensure(msg_len)) break;
            const uint8_t* body = reader.consume(msg_len);
            const char     type = static_cast<char>(body[0]);
            ++total;

            using namespace pitchframe;
            switch (type) {
                case 'R': parser.handle_r(body, msg_len); break;
                case 'A': case 'F': {
                    MsgAddOrder msg;
                    if (decode_add_order(body, msg_len, msg))
                        if (auto* book = books.get_or_create(msg.locate))
                            book->add(msg.order_ref, msg.side, msg.price, msg.shares, msg.locate);
                    break;
                }
                case 'X': {
                    MsgOrderCancel msg;
                    if (decode_order_cancel(body, msg_len, msg))
                        if (auto* book = books.get(msg.locate))
                            book->cancel(msg.order_ref, msg.cancelled_shares);
                    break;
                }
                case 'D': {
                    MsgOrderDelete msg;
                    if (decode_order_delete(body, msg_len, msg))
                        if (auto* book = books.get(msg.locate))
                            book->remove(msg.order_ref);
                    break;
                }
                case 'U': {
                    MsgOrderReplace msg;
                    if (decode_order_replace(body, msg_len, msg))
                        if (auto* book = books.get(msg.locate))
                            book->replace(msg.orig_ref, msg.new_ref, msg.new_shares, msg.new_price);
                    break;
                }
                case 'E': {
                    MsgOrderExecuted msg;
                    if (decode_order_executed(body, msg_len, msg))
                        if (auto* book = books.get(msg.locate))
                            book->execute(msg.order_ref, msg.executed_shares);
                    break;
                }
                case 'C': {
                    MsgOrderExecutedWithPrice msg;
                    if (decode_order_executed_with_price(body, msg_len, msg))
                        if (auto* book = books.get(msg.locate))
                            book->execute(msg.order_ref, msg.executed_shares);
                    break;
                }
                default: break;
            }
        }

        reader.close();
        benchmark::DoNotOptimize(total);
        state.SetItemsProcessed(static_cast<int64_t>(total));
    }
}
BENCHMARK(BM_FullReplay)->Iterations(1);

BENCHMARK_MAIN();
