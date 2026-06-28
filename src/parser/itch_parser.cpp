// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#include "pitchframe/parser/itch_parser.h"
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace pitchframe {

void SymbolTable::insert(uint16_t locate, const uint8_t* sym8_bytes) {
    if (locate == 0 || locate >= k_max_locate) return;
    char sym[9];
    memcpy(sym, sym8_bytes, 8);
    sym[8] = '\0';
    for (int i = 7; i >= 0 && sym[i] == ' '; --i) sym[i] = '\0';
    memcpy(entries[locate], sym, 9);
    ++count;
}

const char* SymbolTable::lookup(uint16_t locate) const {
    if (locate == 0 || locate >= k_max_locate) return nullptr;
    return entries[locate][0] ? entries[locate] : nullptr;
}

void ITCHParser::handle_r(const uint8_t* body, uint16_t msg_len) {
    if (msg_len < k_r_min_len) return;
    uint16_t locate;
    memcpy(&locate, body + k_locate_offset, 2);
    locate = __builtin_bswap16(locate);
    symbols_.insert(locate, body + k_sym_offset_r);
}

void ITCHParser::handle_s(const uint8_t* body, uint16_t msg_len, uint64_t msg_num) {
    if (msg_len < k_s_min_len) return;
    char event = static_cast<char>(body[k_event_offset]);
    const char* label = "unknown";
    switch (event) {
        case 'O': label = "Start of Messages";     break;
        case 'S': label = "Start of System Hours"; break;
        case 'Q': label = "Start of Market Hours"; break;
        case 'M': label = "End of Market Hours";   break;
        case 'E': label = "End of System Hours";   break;
        case 'C': label = "End of Messages";       break;
    }
    printf("[msg #%" PRIu64 "] System Event: %c \xe2\x80\x94 %s\n", msg_num, event, label);
}

} // namespace pitchframe
