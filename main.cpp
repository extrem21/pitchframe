// PitchFrame — Stage 1, Milestone 1
// Goal: walk the 2-byte length-framed ITCH message stream from a replay file
//       and print message type + length for the first 20 messages.
//
// HOT PATH — no allocation, no exceptions, no virtual dispatch.
//
// Framing spec (ITCH 5.0 over MoldUDP64 file format):
//   [2 bytes big-endian length N][N bytes message body]
//   Body byte 0 is the message type character.
//   N includes the type byte — so payload after type is (N-1) bytes.
//
// Compile: g++ -std=c++17 -O2 -Wall -Wextra -o pitchframe main.cpp
// Run:     ./pitchframe data/S061226-v50.txt

#include <cstdint>
#include <cstdio>
#include <cstring>   // memmove

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr int    k_max_print     = 20;
static constexpr size_t k_buf_size      = 1 * 1024 * 1024;  // 1 MB read buffer
static constexpr size_t k_max_msg_len   = 512;               // no ITCH msg is this big;
                                                              // sanity-check the length field

// ---------------------------------------------------------------------------
// FileReader
//
// Maintains a 1 MB buffer. Exposes a cursor (data_ + pos_) into buffered bytes.
// When the caller needs more bytes than remain, it calls refill():
//   1. memmove the leftover tail to the front of the buffer (avoid discarding
//      a partially-seen message that straddles a chunk boundary).
//   2. fread() more bytes from the file into the space after the leftover.
//
// Why chunked reads?
//   A per-message fread() would be two syscalls per message (read 2 bytes,
//   read N bytes). Across hundreds of millions of messages in a full trading
//   day file that's hundreds of millions of mode transitions into the kernel.
//   Chunked reading amortises that to one syscall per ~50k messages at 1MB
//   chunks. The tradeoff is the boundary-spanning logic below — worth it.
// ---------------------------------------------------------------------------

struct FileReader {
    FILE*    file_  = nullptr;
    uint8_t  buf_[k_buf_size];
    size_t   pos_   = 0;   // current read cursor within buf_
    size_t   len_   = 0;   // number of valid bytes currently in buf_
    bool     eof_   = false;

    bool open(const char* path) {
        file_ = fopen(path, "rb");
        return file_ != nullptr;
    }

    void close() {
        if (file_) { fclose(file_); file_ = nullptr; }
    }

    // How many bytes are available to read right now without a refill.
    size_t available() const { return len_ - pos_; }

    // Ensure at least `needed` bytes are available.
    // Returns false if EOF was hit before we could satisfy the request.
    bool ensure(size_t needed) {
        if (available() >= needed) return true;
        if (eof_) return false;

        // Move unread tail to the front of the buffer.
        size_t leftover = available();
        if (leftover > 0 && pos_ > 0) {
            // memmove handles overlapping regions correctly — unlike memcpy.
            memmove(buf_, buf_ + pos_, leftover);
        }
        pos_ = 0;
        len_ = leftover;

        // Fill the rest of the buffer from the file.
        size_t space = k_buf_size - len_;
        size_t got   = fread(buf_ + len_, 1, space, file_);
        len_ += got;

        if (got < space) {
            // Either real EOF or a read error. Either way, no more data coming.
            eof_ = true;
        }

        return available() >= needed;
    }

    // Read a big-endian uint16 from current position and advance cursor.
    // Caller must have called ensure(2) first.
    uint16_t read_u16_be() {
        uint16_t raw;
        // Why not just cast buf_+pos_ to uint16_t*?
        // Because that's undefined behaviour in C++ (strict aliasing + alignment).
        // memcpy into a local and bswap is the correct, UB-free way.
        memcpy(&raw, buf_ + pos_, 2);
        pos_ += 2;
        return __builtin_bswap16(raw);
        // Why __builtin_bswap16 over ntohs?
        // ntohs may or may not inline depending on platform/headers. The builtin
        // guarantees a single BSWAP instruction with no function-call ambiguity.
        // (On many modern systems ntohs also lowers to BSWAP — the honest claim is
        // "I used the intrinsic to guarantee it," not "ntohs is always slow.")
    }

    // Return pointer to current position and advance cursor by `n`.
    // Caller must have called ensure(n) first.
    const uint8_t* consume(size_t n) {
        const uint8_t* p = buf_ + pos_;
        pos_ += n;
        return p;
    }
};

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <itch-replay-file>\n", argv[0]);
        return 1;
    }

    FileReader reader;
    if (!reader.open(argv[1])) {
        fprintf(stderr, "error: cannot open file: %s\n", argv[1]);
        return 1;
    }

    int count = 0;

    while (count < k_max_print) {
        // --- Read the 2-byte length prefix ---
        if (!reader.ensure(2)) {
            // Legitimate EOF between messages — we're done.
            fprintf(stderr, "EOF after %d messages\n", count);
            break;
        }
        uint16_t msg_len = reader.read_u16_be();

        // Sanity check. A corrupt file or wrong format will produce garbage
        // lengths. Catching this early gives a useful error rather than a
        // nonsensical hang or crash.
        if (msg_len == 0 || msg_len > k_max_msg_len) {
            fprintf(stderr,
                "error: implausible message length %u at message %d — "
                "wrong file format or corrupt data\n",
                msg_len, count);
            return 1;
        }

        // --- Ensure the full message body is buffered ---
        if (!reader.ensure(msg_len)) {
            fprintf(stderr,
                "error: unexpected EOF mid-message at message %d "
                "(expected %u bytes, got %zu)\n",
                count, msg_len, reader.available());
            return 1;
        }

        // --- Extract message type (first byte of body) ---
        // consume() returns a pointer directly into the read buffer.
        // We're not copying the message body anywhere — this is zero-copy:
        // the bytes are already in L1 from the fread, and we read them in
        // place. (Be careful about what "zero-copy" actually means: it means
        // no redundant copy, not that no copy ever occurred. The fread from
        // disk into the buffer is a necessary copy.)
        const uint8_t* msg = reader.consume(msg_len);
        char msg_type = static_cast<char>(msg[0]);

        printf("type: %c  len: %u\n", msg_type, msg_len);
        ++count;
    }

    reader.close();
    return 0;
}