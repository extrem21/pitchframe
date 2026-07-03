#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pitchframe {

// Wire framing shared by the UDP feed and the TCP retransmission channel:
//   UDP packet / TCP response record:  [8-byte seq BE][2-byte len BE][body]
//   TCP retransmission request:        [8-byte start_seq BE][8-byte end_seq BE] (inclusive)
//
// A zero-length body (len == 0) is never a real ITCH message (min message
// length is far larger); it is reserved as a sentinel:
//   - on the main feed: "replay ended" (seq = last_real_seq + 1)
//   - on a retransmission response: "server has nothing more for this range"
static constexpr size_t k_seq_header_size      = 8 + 2; // seq + len prefix, precedes body
static constexpr size_t k_retrans_request_size = 8 + 8; // start_seq + end_seq

inline void pack_seq_header(uint8_t* out, uint64_t seq, uint16_t len) {
    uint64_t seq_be = __builtin_bswap64(seq);
    uint16_t len_be = __builtin_bswap16(len);
    memcpy(out,     &seq_be, 8);
    memcpy(out + 8, &len_be, 2);
}

inline void unpack_seq_header(const uint8_t* in, uint64_t& seq, uint16_t& len) {
    uint64_t seq_be;
    uint16_t len_be;
    memcpy(&seq_be, in,     8);
    memcpy(&len_be, in + 8, 2);
    seq = __builtin_bswap64(seq_be);
    len = __builtin_bswap16(len_be);
}

inline void pack_retrans_request(uint8_t* out, uint64_t start_seq, uint64_t end_seq) {
    uint64_t s_be = __builtin_bswap64(start_seq);
    uint64_t e_be = __builtin_bswap64(end_seq);
    memcpy(out,     &s_be, 8);
    memcpy(out + 8, &e_be, 8);
}

inline void unpack_retrans_request(const uint8_t* in, uint64_t& start_seq, uint64_t& end_seq) {
    uint64_t s_be, e_be;
    memcpy(&s_be, in,     8);
    memcpy(&e_be, in + 8, 8);
    start_seq = __builtin_bswap64(s_be);
    end_seq   = __builtin_bswap64(e_be);
}

} // namespace pitchframe
