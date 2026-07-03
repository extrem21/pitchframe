#include "pitchframe/net/sequence_tracker.h"

namespace pitchframe {

SequenceTracker::SequenceTracker(DeliverFn deliver, void* deliver_ctx,
                                  RetransmitFn retransmit, void* retransmit_ctx,
                                  size_t reorder_capacity)
    : deliver_fn_(deliver), deliver_ctx_(deliver_ctx),
      retransmit_fn_(retransmit), retransmit_ctx_(retransmit_ctx),
      buffer_(reorder_capacity) {}

void SequenceTracker::deliver_and_drain(uint64_t seq, const uint8_t* body, uint16_t len) {
    deliver_fn_(deliver_ctx_, seq, body, len);
    last_contiguous_seq_ = seq;

    const uint8_t* next_body;
    uint16_t       next_len;
    while (buffer_.take(last_contiguous_seq_ + 1, next_body, next_len)) {
        deliver_fn_(deliver_ctx_, last_contiguous_seq_ + 1, next_body, next_len);
        ++last_contiguous_seq_;
    }
}

void SequenceTracker::on_packet(uint64_t seq, const uint8_t* body, uint16_t len) {
    if (seq > highest_seq_seen_) highest_seq_seen_ = seq;

    if (seq == last_contiguous_seq_ + 1) {
        deliver_and_drain(seq, body, len);
        return;
    }
    if (seq <= last_contiguous_seq_) {
        return; // duplicate or already-seen out-of-order arrival
    }

    // seq > last_contiguous_seq_ + 1: gap.
    if (!buffer_.put(seq, body, len)) {
        // ReorderBuffer overflow — log + full resync, never accumulate
        // unbounded state (CLAUDE.md's documented overflow policy).
        full_resync();
        return;
    }

    uint64_t gap_start = last_contiguous_seq_ + 1;
    uint64_t gap_end   = seq - 1;
    if (!retransmit_fn_(retransmit_ctx_, gap_start, gap_end)) {
        full_resync();
    }
}

void SequenceTracker::on_retransmitted(uint64_t seq, const uint8_t* body, uint16_t len) {
    if (seq > highest_seq_seen_) highest_seq_seen_ = seq;

    if (seq == last_contiguous_seq_ + 1) {
        deliver_and_drain(seq, body, len);
    } else if (seq > last_contiguous_seq_) {
        // Not expected for a well-formed response (records arrive in
        // ascending seq order), but handled safely rather than assumed away.
        buffer_.put(seq, body, len);
    }
    // seq <= last_contiguous_seq_: duplicate, discard.
}

void SequenceTracker::full_resync() {
    buffer_.clear();
    uint64_t start = last_contiguous_seq_ + 1;
    uint64_t end   = highest_seq_seen_;
    if (start > end) return;

    // Best-effort single attempt. If this also comes back short, v1 gives up
    // here rather than looping — a production version would retry with
    // backoff; deferred, same spirit as Stage 1's documented simplifications.
    retransmit_fn_(retransmit_ctx_, start, end);
}

} // namespace pitchframe
