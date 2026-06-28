// HOT PATH — no allocation, no exceptions, no virtual dispatch.
#pragma once
#include "pitchframe/common/types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pitchframe {

// Maintains a 1 MB buffer. On refill: memmoves leftover tail to front, then
// fread()s more data. Amortises syscall cost to ~one per 50k messages.
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

    // memcpy avoids UB from strict-aliasing; bswap16 guarantees a single BSWAP.
    uint16_t read_u16_be() {
        uint16_t raw;
        memcpy(&raw, buf_ + pos_, 2);
        pos_ += 2;
        return __builtin_bswap16(raw);
    }

    // Zero-copy: returns pointer into buffer, no intermediate copy.
    const uint8_t* consume(size_t n) {
        const uint8_t* p = buf_ + pos_;
        pos_ += n;
        return p;
    }
};

} // namespace pitchframe
