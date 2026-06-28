#pragma once
#include <cstddef>
#include <cstdint>

namespace pitchframe {

static constexpr size_t   k_buf_size    = 1 * 1024 * 1024;
static constexpr size_t   k_max_msg_len = 512;
static constexpr uint16_t k_max_locate  = 8192;  // ITCH spec: locate codes 1..8191

// ITCH 5.0 field offsets within the message body (body[0] == message type byte).
// Layout: type(1) + locate(2) + tracking(2) + timestamp(6) + fields...
static constexpr size_t k_locate_offset = 1;   // uint16 BE
static constexpr size_t k_sym_offset_r  = 11;  // 8-byte ASCII symbol in R
static constexpr size_t k_event_offset  = 11;  // 1-byte event code in S
static constexpr size_t k_r_min_len     = 19;  // up to end of 8-byte symbol field
static constexpr size_t k_s_min_len     = 12;  // up to event code byte

} // namespace pitchframe
