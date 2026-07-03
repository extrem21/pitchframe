#pragma once
#include "pitchframe/book/book_map.h"
#include "pitchframe/parser/itch_messages.h"
#include "pitchframe/parser/itch_parser.h"
#include <cstdint>

namespace pitchframe {

// Mirrors main.cpp's per-message switch for the book-mutating types (A, F, X,
// D, U, E, C) plus R (symbol table). Extracted fresh rather than factored out
// of main.cpp — CLAUDE.md says not to restructure existing Stage 1 files.
// pitchframe_net and the Run A/B correctness test both call this same
// function, which is what makes "the Stage 1 pipeline is unchanged downstream
// of the sequence tracker" a checked fact rather than an assertion.
// inline: this header is included by multiple translation units.
inline void dispatch_message(const uint8_t* body, uint16_t msg_len,
                              ITCHParser& parser, BookMap& books) {
    if (msg_len == 0) return;
    const char type = static_cast<char>(body[0]);

    switch (type) {
        case 'R': parser.handle_r(body, msg_len); break;
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
        default: break;
    }
}

// Per-symbol state captured at the closing 'M' System Event — the same four
// fields CLAUDE.md's Stage 2 correctness proof compares (Run A == Run B).
struct SymbolSnapshot {
    uint64_t shares_traded  = 0;
    int      active_orders  = 0;
    uint32_t best_bid       = 0;
    uint32_t best_ask       = 0;
    bool     captured       = false;
};

// Call for every message a caller sees (including 'S', which dispatch_message
// itself ignores). Captures per-locate book state into snapshots[] the moment
// the closing 'M' System Event is seen — before any post-close cancellations
// run, matching main.cpp's rationale (see its NOII-validation comment).
// Shared by pitchframe_net and the Run A/B test's baseline leg so both sides
// snapshot at the identical point in the stream, using the identical fields.
inline void maybe_capture_close_snapshot(const uint8_t* body, uint16_t len,
                                          const BookMap& books, SymbolSnapshot* snapshots) {
    if (len < 12) return;
    if (static_cast<char>(body[0]) != 'S') return;
    if (static_cast<char>(body[11]) != 'M') return;

    for (uint16_t i = 1; i < k_max_locate; ++i) {
        if (const OrderBook* b = books.get(i)) {
            snapshots[i].shares_traded = b->shares_traded();
            snapshots[i].active_orders = b->active_orders();
            snapshots[i].best_bid      = b->best_bid();
            snapshots[i].best_ask      = b->best_ask();
            snapshots[i].captured      = true;
        }
    }
}

} // namespace pitchframe
