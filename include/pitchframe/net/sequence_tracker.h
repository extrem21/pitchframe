#pragma once
#include "pitchframe/net/reorder_buffer.h"
#include <cstddef>
#include <cstdint>

namespace pitchframe {

// Deliver an in-order message downstream (Stage 1 pipeline).
using DeliverFn = void (*)(void* ctx, uint64_t seq, const uint8_t* body, uint16_t len);

// Synchronously fetch [start_seq, end_seq] (inclusive) over the retransmission
// channel, calling SequenceTracker::on_retransmitted() for each message
// received. Returns true iff the full requested range was delivered; false
// (short/zero-length response, connection failure) tells the tracker the
// server could not supply the range, so it falls back to full resync rather
// than blocking forever.
using RetransmitFn = bool (*)(void* ctx, uint64_t start_seq, uint64_t end_seq);

// Tracks last_contiguous_seq_ — the highest seq such that every number from 1
// through it has been delivered downstream, in order. Both callbacks are
// plain function pointers + void* ctx (no std::function, no heap, matching
// the project's existing style — see BookMap/FileReader) so this class is
// fully unit-testable without opening a single real socket.
//
// Gap policy (CLAUDE.md):
//   seq == last+1        -> deliver immediately, advance, drain any now-
//                            contiguous buffered entries.
//   seq >  last+1         -> gap: buffer this message, synchronously request
//                            [last+1, seq-1] over the retransmission channel.
//   seq <= last           -> duplicate / already-seen; discard.
//
// The retransmission fetch is synchronous (documented v1 simplification —
// see README Known Limitations): this makes gap-request dedup trivial by
// construction, since only one fetch can ever be outstanding at a time.
class SequenceTracker {
public:
    SequenceTracker(DeliverFn deliver, void* deliver_ctx,
                     RetransmitFn retransmit, void* retransmit_ctx,
                     size_t reorder_capacity = 65536);

    // From the live UDP path (post loss-injector).
    void on_packet(uint64_t seq, const uint8_t* body, uint16_t len);

    // From the synchronous retransmission fetch, once per message received.
    void on_retransmitted(uint64_t seq, const uint8_t* body, uint16_t len);

    uint64_t last_contiguous_seq() const { return last_contiguous_seq_; }
    uint64_t highest_seq_seen()    const { return highest_seq_seen_; }

private:
    void deliver_and_drain(uint64_t seq, const uint8_t* body, uint16_t len);
    // ReorderBuffer overflow or a short/failed retransmission response both
    // land here: clear the buffer and make one best-effort synchronous fetch
    // for everything outstanding, [last_contiguous_seq_+1, highest_seq_seen_].
    void full_resync();

    DeliverFn    deliver_fn_;
    void*        deliver_ctx_;
    RetransmitFn retransmit_fn_;
    void*        retransmit_ctx_;

    ReorderBuffer buffer_;
    uint64_t      last_contiguous_seq_ = 0;
    uint64_t      highest_seq_seen_    = 0;
};

} // namespace pitchframe
