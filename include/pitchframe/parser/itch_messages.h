// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#pragma once
#include <cstdint>
#include <cstring>

namespace pitchframe {

// Decoded forms of ITCH 5.0 order-book messages.
// All multi-byte fields are converted to host byte order on decode.
// body[0] is the message type byte; all offsets are relative to body[0].

struct MsgAddOrder {
    uint64_t order_ref;
    uint32_t price;    // fixed-point, 4 implied decimal places ($0.0001/unit)
    uint32_t shares;
    uint16_t locate;
    char     side;     // 'B' buy / 'S' sell
};

struct MsgOrderCancel {
    uint64_t order_ref;
    uint32_t cancelled_shares;
    uint16_t locate;
};

struct MsgOrderDelete {
    uint64_t order_ref;
    uint16_t locate;
};

struct MsgOrderReplace {
    uint64_t orig_ref;
    uint64_t new_ref;
    uint32_t new_shares;
    uint32_t new_price;
    uint16_t locate;
};

// ---------------------------------------------------------------------------
// Inline free functions — stateless bytes-to-struct transforms.
// Return false if msg_len is too short for the required fields.
// ---------------------------------------------------------------------------

// Handles both A (36 bytes) and F (40 bytes, extra MPID at body[36-39] ignored).
inline bool decode_add_order(const uint8_t* body, uint16_t msg_len, MsgAddOrder& out) {
    if (msg_len < 36) return false;
    memcpy(&out.locate,    body +  1, 2); out.locate    = __builtin_bswap16(out.locate);
    memcpy(&out.order_ref, body + 11, 8); out.order_ref = __builtin_bswap64(out.order_ref);
    out.side = static_cast<char>(body[19]);
    memcpy(&out.shares,    body + 20, 4); out.shares    = __builtin_bswap32(out.shares);
    memcpy(&out.price,     body + 32, 4); out.price     = __builtin_bswap32(out.price);
    return true;
}

inline bool decode_order_cancel(const uint8_t* body, uint16_t msg_len, MsgOrderCancel& out) {
    if (msg_len < 23) return false;
    memcpy(&out.locate,           body +  1, 2); out.locate           = __builtin_bswap16(out.locate);
    memcpy(&out.order_ref,        body + 11, 8); out.order_ref        = __builtin_bswap64(out.order_ref);
    memcpy(&out.cancelled_shares, body + 19, 4); out.cancelled_shares = __builtin_bswap32(out.cancelled_shares);
    return true;
}

inline bool decode_order_delete(const uint8_t* body, uint16_t msg_len, MsgOrderDelete& out) {
    if (msg_len < 19) return false;
    memcpy(&out.locate,    body +  1, 2); out.locate    = __builtin_bswap16(out.locate);
    memcpy(&out.order_ref, body + 11, 8); out.order_ref = __builtin_bswap64(out.order_ref);
    return true;
}

inline bool decode_order_replace(const uint8_t* body, uint16_t msg_len, MsgOrderReplace& out) {
    if (msg_len < 35) return false;
    memcpy(&out.locate,     body +  1, 2); out.locate     = __builtin_bswap16(out.locate);
    memcpy(&out.orig_ref,   body + 11, 8); out.orig_ref   = __builtin_bswap64(out.orig_ref);
    memcpy(&out.new_ref,    body + 19, 8); out.new_ref    = __builtin_bswap64(out.new_ref);
    memcpy(&out.new_shares, body + 27, 4); out.new_shares = __builtin_bswap32(out.new_shares);
    memcpy(&out.new_price,  body + 31, 4); out.new_price  = __builtin_bswap32(out.new_price);
    return true;
}

} // namespace pitchframe
