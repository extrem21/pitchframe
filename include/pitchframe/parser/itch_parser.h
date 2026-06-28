// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#pragma once
#include "pitchframe/common/types.h"
#include <cstdint>

namespace pitchframe {

// Maps stock_locate (1-based, 1..8191) → null-terminated trimmed symbol string.
// Zero-initialised at construction; entry is empty until R message is seen.
struct SymbolTable {
    char entries[k_max_locate][9] = {};
    int  count = 0;

    void        insert(uint16_t locate, const uint8_t* sym8_bytes);
    const char* lookup(uint16_t locate) const;
};

// Handles ITCH message types decoded in Stage 1.
// Parser state that persists across messages (the symbol table) lives here.
class ITCHParser {
public:
    // Populate symbol table from R (Stock Directory) message body.
    void handle_r(const uint8_t* body, uint16_t msg_len);

    // Print market-session boundary event from S (System Event) message body.
    void handle_s(const uint8_t* body, uint16_t msg_len, uint64_t msg_num);

    const SymbolTable& symbol_table() const { return symbols_; }

private:
    SymbolTable symbols_;
};

} // namespace pitchframe
