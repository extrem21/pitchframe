// PitchFrame — Stage 1, Milestone 5
// Thin driver: open file, run parse loop, dispatch to order books, print summary.

// Baseline — Stage 1, Milestone 3 (pre-execution-handling)
// Hardware: Apple M2 Air (8GB RAM assumed)
// Build: clang++ -std=c++17 -O2 -fno-rtti -fno-exceptions
// Total messages: 268,744,780
// Wall time: 1m 48.77s
// Throughput: ~2.48M msg/sec
// Thread count: 1
// Notes: E/C execution messages not yet handled, crossed books expected

#include "pitchframe/common/types.h"
#include "pitchframe/parser/file_reader.h"
#include "pitchframe/parser/itch_parser.h"
#include "pitchframe/parser/itch_messages.h"
#include "pitchframe/book/book_map.h"
#include "pitchframe/common/alloc_counter.h"
#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>

using namespace pitchframe;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <itch-replay-file>\n", argv[0]);
        return 1;
    }

    // Static: 1 MB buffer lives in BSS, not on the stack.
    static FileReader reader;
    if (!reader.open(argv[1])) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return 1;
    }

    ITCHParser parser;
    BookMap    books;

    // NOII closing-cross state, indexed by locate code.
    // Spec says to get best_bid/ask "at end of replay", but EOD cancellations
    // drain all books before the replay ends. We snapshot bid/ask at the time
    // the last closing NOII arrives (before EOD cancellations) — this is the
    // point where the comparison is meaningful.
    static uint32_t closing_ref [k_max_locate];  // NASDAQ reference price
    static uint32_t closing_bid [k_max_locate];  // book best_bid at NOII time
    static uint32_t closing_ask [k_max_locate];  // book best_ask at NOII time
    for (uint16_t i = 0; i < k_max_locate; ++i)
        closing_ref[i] = closing_bid[i] = closing_ask[i] = 0;

    uint64_t type_counts[256] = {};
    uint64_t total = 0;

#ifdef PITCHFRAME_COUNT_ALLOCS
    pitchframe::alloc_counter_set_active(true);
#endif

    while (true) {
        if (!reader.ensure(2)) break;
        uint16_t msg_len = reader.read_u16_be();

        if (msg_len == 0 || msg_len > k_max_msg_len) {
            fprintf(stderr, "error: implausible length %u at message %" PRIu64 "\n",
                    msg_len, total);
            return 1;
        }

        if (!reader.ensure(msg_len)) {
            fprintf(stderr,
                    "error: truncated body at message %" PRIu64
                    " (need %u, have %zu)\n",
                    total, msg_len, reader.available());
            return 1;
        }

        const uint8_t* body = reader.consume(msg_len);
        const char     type = static_cast<char>(body[0]);

        type_counts[static_cast<uint8_t>(type)]++;
        ++total;

        switch (type) {
            case 'R': parser.handle_r(body, msg_len); break;
            case 'S':
                parser.handle_s(body, msg_len, total);
#ifdef PITCHFRAME_COUNT_ALLOCS
                // Reset at 'Q' (Start of Market Hours) so the counter measures
                // only regular-hours hot-path allocation, not startup/pre-market.
                if (msg_len >= 12 && static_cast<char>(body[11]) == 'Q') {
                    printf("[alloc-counter] Pre-market allocs: %ld\n",
                           pitchframe::alloc_counter_get());
                    pitchframe::alloc_counter_reset();
                }
                if (msg_len >= 12 && static_cast<char>(body[11]) == 'M') {
                    printf("[alloc-counter] Regular-hours allocs (Q→M): %ld\n",
                           pitchframe::alloc_counter_get());
                    pitchframe::alloc_counter_reset();
                }
#endif
                break;

            case 'A':
            case 'F': {
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
            case 'I': {
                MsgNOII msg;
                // Locate codes > k_max_locate appear in the NOII stream. Without the
                // bounds check, closing_ref[oob_locate] writes past the array into the
                // adjacent closing_bid[] in BSS, corrupting unrelated symbols' stored bids.
                if (decode_noii(body, msg_len, msg) && msg.cross_type == 'C'
                        && msg.locate > 0 && msg.locate < k_max_locate) {
                    closing_ref[msg.locate] = msg.reference_price;
                    if (const OrderBook* book = books.get(msg.locate)) {
                        closing_bid[msg.locate] = book->best_bid();
                        closing_ask[msg.locate] = book->best_ask();
                    }
                }
                break;
            }
            default: break;
        }
    }

    reader.close();

#ifdef PITCHFRAME_COUNT_ALLOCS
    pitchframe::alloc_counter_set_active(false);
    printf("[alloc-counter] Post-market allocs (M→end): %ld\n\n",
           pitchframe::alloc_counter_get());
#endif

    // --- Message type census ---
    printf("\n=== Message counts ===\n");
    printf("Total: %" PRIu64 "\n", total);
    for (int c = 0; c < 256; ++c) {
        if (type_counts[c])
            printf("  '%c' (0x%02x) : %" PRIu64 "\n", c, c, type_counts[c]);
    }

    // --- Symbol table sample ---
    const SymbolTable& st = parser.symbol_table();
    printf("\n=== Symbol table (locate\xe2\x86\x92symbol, first 10) ===\n");
    printf("Total R messages with valid locate: %d\n", st.count);
    int shown = 0;
    for (uint16_t i = 1; i < k_max_locate && shown < 10; ++i) {
        if (st.entries[i][0]) {
            printf("  locate %4u \xe2\x86\x92 %s\n", i, st.entries[i]);
            ++shown;
        }
    }

    // --- Shares-traded totals (top 10 by volume) ---
    struct VolEntry { uint16_t locate; uint64_t vol; };
    VolEntry top[10] = {};
    uint64_t grand_total_shares = 0;

    for (uint16_t i = 1; i < k_max_locate; ++i) {
        const OrderBook* book = books.get(i);
        if (!book) continue;
        uint64_t v = book->shares_traded();
        grand_total_shares += v;
        if (v > top[9].vol) {
            top[9] = {i, v};
            for (int j = 8; j >= 0 && top[j].vol < top[j+1].vol; --j)
                std::swap(top[j], top[j+1]);
        }
    }

    printf("\n=== Shares traded \xe2\x80\x94 top 10 by volume ===\n");
    printf("Grand total shares traded: %" PRIu64 "\n", grand_total_shares);
    for (auto& e : top) {
        if (!e.vol) break;
        const char* sym = st.lookup(e.locate);
        printf("  %-8s  %" PRIu64 "\n", sym ? sym : "?", e.vol);
    }

    // --- NOII Closing Cross Validation ---
    // Collect deviations for all symbols with a valid closing NOII for median/max.
    static double all_devs[k_max_locate];
    int           dev_count      = 0;
    int           noii_total     = 0;

    printf("\n=== NOII Closing Cross Validation (top 10 by volume) ===\n");
    printf("%-8s  %-15s  %-24s  %-10s  %s\n",
           "Symbol", "NASDAQ RefPrice", "Book Bid/Ask", "Diff ($)", "Diff (%)");

    for (auto& e : top) {
        if (!e.vol) break;
        const char* sym = st.lookup(e.locate);
        uint32_t ref = closing_ref[e.locate];
        if (!ref) {
            printf("  %-8s  (no closing NOII)\n", sym ? sym : "?");
            continue;
        }
        uint32_t bid = closing_bid[e.locate];
        uint32_t ask = closing_ask[e.locate];
        if (!bid && !ask) {
            printf("  %-8s  $%8.4f  (incomplete book at NOII time)\n",
                   sym ? sym : "?", ref / 10000.0);
            continue;
        }
        double diff;
        if (bid && ask)
            diff = std::min(std::fabs((double)ref - bid), std::fabs((double)ref - ask));
        else if (bid)
            diff = std::fabs((double)ref - bid);
        else
            diff = std::fabs((double)ref - ask);

        double pct = 100.0 * diff / ref;
        printf("  %-8s  $%8.4f  $%8.4f / $%-8.4f  $%7.4f  %.4f%%\n",
               sym ? sym : "?",
               ref / 10000.0,
               bid / 10000.0,
               ask / 10000.0,
               diff / 10000.0,
               pct);
    }

    // Full-set median/max across all symbols with valid closing NOII.
    for (uint16_t i = 1; i < k_max_locate; ++i) {
        uint32_t ref = closing_ref[i];
        if (!ref) continue;
        ++noii_total;
        uint32_t bid = closing_bid[i];
        uint32_t ask = closing_ask[i];
        if (!bid && !ask) continue;
        double diff;
        if (bid && ask)
            diff = std::min(std::fabs((double)ref - bid), std::fabs((double)ref - ask));
        else if (bid)
            diff = std::fabs((double)ref - bid);
        else
            diff = std::fabs((double)ref - ask);
        all_devs[dev_count++] = 100.0 * diff / ref;
    }

    printf("\nSymbols with valid closing NOII: %d / %d\n", noii_total, st.count);

    if (dev_count > 0) {
        std::sort(all_devs, all_devs + dev_count);
        double median = (dev_count % 2 == 0)
            ? (all_devs[dev_count/2 - 1] + all_devs[dev_count/2]) / 2.0
            : all_devs[dev_count/2];
        double max_dev = all_devs[dev_count - 1];
        // Find the symbol with max deviation for attribution.
        uint16_t max_loc = 0;
        double   max_val = -1.0;
        for (uint16_t i = 1; i < k_max_locate; ++i) {
            uint32_t ref = closing_ref[i];
            if (!ref) continue;
            uint32_t bid = closing_bid[i], ask = closing_ask[i];
            if (!bid && !ask) continue;
            double diff;
            if (bid && ask)
                diff = std::min(std::fabs((double)ref - bid), std::fabs((double)ref - ask));
            else
                diff = std::fabs((double)ref - (bid ? bid : ask));
            double pct = 100.0 * diff / ref;
            if (pct > max_val) { max_val = pct; max_loc = i; }
        }
        const char* max_sym = st.lookup(max_loc);
        printf("Median price deviation: %.4f%%\n", median);
        printf("Max price deviation:    %.4f%% (symbol: %s)\n",
               max_dev, max_sym ? max_sym : "?");
    }

    return 0;
}
