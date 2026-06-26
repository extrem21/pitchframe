// PitchFrame — Stage 1, Milestone 2
// Goal: build a locate→symbol table from R (Stock Directory) messages,
//       detect market-session boundaries from S (System Event) messages,
//       walk the full stream, and print a type-count summary.
//
// HOT PATH — no allocation, no exceptions, no virtual dispatch.
//
// Framing spec (MoldUDP64 / ITCH-family binary file format):
//   [2 bytes big-endian length N][N bytes message body]
//   Body byte 0 is the message type character.
//   N includes the type byte — so payload after type is (N-1) bytes.
//
// Targets ITCH 5.0 (12302019.NASDAQ_ITCH50).
//   S message: 12 bytes — type(1)+locate(2)+tracking(2)+timestamp(6)+event(1)
//   R message: 39 bytes — type(1)+locate(2)+tracking(2)+timestamp(6)+symbol(8)+...
//   Symbol is at body[11] in both; event code is at body[11] in S.
//
// Compile: g++ -std=c++17 -O2 -Wall -Wextra -o pitchframe main.cpp
// Run:     ./pitchframe 12302019.NASDAQ_ITCH50

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr size_t   k_buf_size    = 1 * 1024 * 1024;
static constexpr size_t   k_max_msg_len = 512;
static constexpr uint16_t k_max_locate  = 8192;  // ITCH spec: locate codes 1..8191

// Field offsets within the message body (byte 0 = message type).
// These target the 4-byte-timestamp variant in the current file.
// For ITCH 5.0 (6-byte timestamp) add 2 to each offset below.
static constexpr size_t k_locate_offset = 1;  // uint16 BE
static constexpr size_t k_sym_offset_r  = 11; // 8-byte ASCII symbol in R: 1(type)+2(locate)+2(tracking)+6(ts)
static constexpr size_t k_event_offset  = 11; // 1-byte event code in S: same layout
static constexpr size_t k_r_min_len     = 19; // need up to end of symbol field
static constexpr size_t k_s_min_len     = 12; // need up to event code

// ---------------------------------------------------------------------------
// Symbol table
//
// Maps stock_locate (1-based uint16) → null-terminated symbol string.
// Pre-allocated at program startup; O(1) lookup, zero allocation at runtime.
// In the current file, all locates are 0, so the table stays mostly empty —
// this will work correctly once we switch to a proper ITCH 5.0 file.
// ---------------------------------------------------------------------------

static char g_symbols[k_max_locate][9];  // zero-init = "not yet seen"
static int  g_symbol_count = 0;

// ---------------------------------------------------------------------------
// FileReader
//
// Maintains a 1 MB buffer. When the caller needs more bytes than remain it:
//   1. memmoves the leftover tail to the front (avoids losing a partially-seen
//      message that straddles a chunk boundary)
//   2. fread()s more data from the file into the space after the leftover
//
// Why chunked reads?
//   A per-message fread() would be two syscalls per message. Across 27M
//   messages that's ~54M kernel mode-transitions. Chunked reading amortises
//   that to one syscall per ~50k messages at 1 MB chunks.
// ---------------------------------------------------------------------------

struct FileReader {
    FILE*   file_ = nullptr;
    uint8_t buf_[k_buf_size];
    size_t  pos_  = 0;
    size_t  len_  = 0;
    bool    eof_  = false;

    bool open(const char* path) {
        file_ = fopen(path, "rb");
        return file_ != nullptr;
    }

    void close() {
        if (file_) { fclose(file_); file_ = nullptr; }
    }

    size_t available() const { return len_ - pos_; }

    bool ensure(size_t needed) {
        if (available() >= needed) return true;
        if (eof_) return false;

        size_t leftover = available();
        if (leftover > 0 && pos_ > 0)
            memmove(buf_, buf_ + pos_, leftover);
        pos_ = 0;
        len_ = leftover;

        size_t space = k_buf_size - len_;
        size_t got   = fread(buf_ + len_, 1, space, file_);
        len_ += got;
        if (got < space) eof_ = true;

        return available() >= needed;
    }

    uint16_t read_u16_be() {
        uint16_t raw;
        // memcpy avoids undefined behaviour from strict-aliasing / alignment.
        // __builtin_bswap16 guarantees a single BSWAP instruction; ntohs may
        // or may not inline depending on platform headers.
        memcpy(&raw, buf_ + pos_, 2);
        pos_ += 2;
        return __builtin_bswap16(raw);
    }

    // Zero-copy: returns a pointer into the buffer, no intermediate copy.
    const uint8_t* consume(size_t n) {
        const uint8_t* p = buf_ + pos_;
        pos_ += n;
        return p;
    }
};

// ---------------------------------------------------------------------------
// handle_r: extract locate + symbol, populate g_symbols[]
// ---------------------------------------------------------------------------

static void handle_r(const uint8_t* body, uint16_t msg_len) {
    if (msg_len < k_r_min_len) return;

    uint16_t locate;
    memcpy(&locate, body + k_locate_offset, 2);
    locate = __builtin_bswap16(locate);

    // Locate 0 is invalid per ITCH spec; guard against out-of-range.
    if (locate == 0 || locate >= k_max_locate) return;

    // 8-byte ASCII symbol, right-padded with spaces — trim and null-terminate.
    char sym[9];
    memcpy(sym, body + k_sym_offset_r, 8);
    sym[8] = '\0';
    for (int i = 7; i >= 0 && sym[i] == ' '; --i) sym[i] = '\0';

    memcpy(g_symbols[locate], sym, 9);
    ++g_symbol_count;
}

// ---------------------------------------------------------------------------
// handle_s: print market-session boundary events
// ---------------------------------------------------------------------------

static void handle_s(const uint8_t* body, uint16_t msg_len, uint64_t msg_num) {
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
    printf("[msg #%" PRIu64 "] System Event: %c — %s\n", msg_num, event, label);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <itch-replay-file>\n", argv[0]);
        return 1;
    }

    // static: 1 MB buffer lives in BSS, not on the stack.
    static FileReader reader;
    if (!reader.open(argv[1])) {
        fprintf(stderr, "error: cannot open '%s'\n", argv[1]);
        return 1;
    }

    uint64_t type_counts[256] = {};
    uint64_t total = 0;

    while (true) {
        if (!reader.ensure(2)) break;  // clean EOF between messages
        uint16_t msg_len = reader.read_u16_be();

        if (msg_len == 0 || msg_len > k_max_msg_len) {
            fprintf(stderr,
                "error: implausible length %u at message %" PRIu64 "\n",
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
            case 'R': handle_r(body, msg_len); break;
            case 'S': handle_s(body, msg_len, total); break;
            default:  break;
        }
    }

    reader.close();

    // --- Type-count summary ---
    printf("\n=== Message counts ===\n");
    printf("Total: %" PRIu64 "\n", total);
    for (int c = 0; c < 256; ++c) {
        if (type_counts[c])
            printf("  '%c' (0x%02x) : %" PRIu64 "\n", c, c, type_counts[c]);
    }

    // --- Symbol table sample ---
    printf("\n=== Symbol table (locate→symbol, first 10 non-empty) ===\n");
    printf("Total R messages with valid locate: %d\n", g_symbol_count);
    int shown = 0;
    for (uint16_t i = 1; i < k_max_locate && shown < 10; ++i) {
        if (g_symbols[i][0]) {
            printf("  locate %4u → %s\n", i, g_symbols[i]);
            ++shown;
        }
    }
    if (g_symbol_count == 0) {
        printf("  (no entries — this file carries locate=0 for all messages;\n"
               "   symbol table will populate correctly with an ITCH 5.0 file)\n");
    }

    return 0;
}
