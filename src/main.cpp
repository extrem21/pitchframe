// PitchFrame — Stage 1, Milestone 3
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

    uint64_t type_counts[256] = {};
    uint64_t total = 0;

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
            case 'S': parser.handle_s(body, msg_len, total); break;

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
            default: break;
        }
    }

    reader.close();

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

    // --- Order book sample ---
    printf("\n=== Order book sample (first 5 active symbols) ===\n");
    int shown_books = 0;
    for (uint16_t i = 1; i < k_max_locate && shown_books < 5; ++i) {
        const OrderBook* book = books.get(i);
        if (!book || book->active_orders() == 0) continue;
        const char* sym = st.lookup(i);
        // Prices are fixed-point with 4 implied decimal places.
        printf("  %-8s  orders=%6d  bid=$%.4f  ask=$%.4f\n",
               sym ? sym : "?",
               book->active_orders(),
               book->best_bid() / 10000.0,
               book->best_ask() / 10000.0);
        ++shown_books;
    }

    return 0;
}
