#pragma once
#include "pitchframe/common/types.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace pitchframe {

// Bounded ring buffer keyed by sequence number: slot = seq % capacity.
//
// Each slot stores its own seq (not just a valid flag) and every read
// verifies it. Two sequence numbers exactly `capacity` apart alias to the
// same slot index — a valid-flag-only check would let a wrapped write
// silently clobber a live, undrained entry, or let a stale slot read back as
// a false-positive hit for the wrong seq. Same bug class as Stage 1's
// closing_ref[8205] BSS stomp (aliased array slots), second venue — see
// CLAUDE.md's Correctness Verification section.
//
// Backing storage is a std::vector<Slot> sized once at construction (not a
// per-message hot-path allocation — one-time startup cost, same spirit as
// BookMap's bounded lazy heap allocation in Stage 1). Default capacity
// matches CLAUDE.md's documented bound of 65536; tests use a small capacity
// to exercise the overflow path cheaply.
class ReorderBuffer {
public:
    explicit ReorderBuffer(size_t capacity = 65536)
        : capacity_(capacity), slots_(capacity) {}

    // False means the target slot already holds a different, undrained seq
    // — this is the overflow condition; the caller must not treat it as a
    // silent success, per CLAUDE.md's "log + trigger full resync" policy.
    bool put(uint64_t seq, const uint8_t* body, uint16_t len) {
        Slot& s = slots_[seq % capacity_];
        if (s.valid && s.seq != seq) return false;
        s.valid = true;
        s.seq   = seq;
        s.len   = len;
        memcpy(s.body, body, len);
        return true;
    }

    // Hit only when slot.valid && slot.seq == seq — an index match with a
    // seq mismatch (aliasing) is a miss, never a hit. Drains (clears) the
    // slot on a hit; the returned pointer is valid until the next put() to
    // the same slot, matching zero-copy usage in SequenceTracker.
    bool take(uint64_t seq, const uint8_t*& body, uint16_t& len) {
        Slot& s = slots_[seq % capacity_];
        if (!s.valid || s.seq != seq) return false;
        body    = s.body;
        len     = s.len;
        s.valid = false;
        return true;
    }

    bool contains(uint64_t seq) const {
        const Slot& s = slots_[seq % capacity_];
        return s.valid && s.seq == seq;
    }

    void clear() {
        for (auto& s : slots_) s.valid = false;
    }

    size_t capacity() const { return capacity_; }

private:
    struct Slot {
        bool     valid = false;
        uint64_t seq   = 0;
        uint16_t len   = 0;
        uint8_t  body[k_max_msg_len];
    };

    size_t            capacity_;
    std::vector<Slot> slots_;
};

} // namespace pitchframe
