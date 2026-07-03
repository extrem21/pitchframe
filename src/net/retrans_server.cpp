// PitchFrame — Stage 2, RetransmissionServer
// Reads the same replay file as everything else, builds a seq -> file-offset
// index at startup (one fseek-driven pass — skips message bodies, doesn't
// read them), then serves retransmission requests over TCP.
//
// Wire protocol (pitchframe/net/retrans_protocol.h):
//   Request:  [8B start_seq BE][8B end_seq BE], inclusive range.
//   Response: for each seq in range that exists, [8B seq BE][2B len BE][body]
//             — literally the same per-message framing UDPReplayer uses.
//             If a requested seq doesn't exist (gap larger than the file
//             has), send a terminator record [8B seq BE][2B len=0] and stop
//             responding to that request; the client's short read tells it
//             the range couldn't be fully supplied.
//
// Single-client simplification (documented, per CLAUDE.md): one TCP
// connection for the whole session. A production server would fan out to
// many concurrent subscribers.
#include "pitchframe/common/types.h"
#include "pitchframe/net/retrans_protocol.h"
#include <arpa/inet.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace pitchframe;

namespace {

struct Args {
    const char* file = nullptr;
    uint16_t    port = 21003;
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <itch-replay-file> [--port N]\n", argv[0]);
        exit(1);
    }
    a.file = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) a.port = static_cast<uint16_t>(atoi(argv[++i]));
    }
    return a;
}

// offsets[seq] = file offset of the 2-byte length prefix for message `seq`
// (1-indexed; offsets[0] unused). Built by walking length prefixes only —
// fseek past each body rather than reading it, so this is fast even for the
// full 8.25 GB replay file.
std::vector<uint64_t> build_index(FILE* f, uint64_t& total_count) {
    std::vector<uint64_t> offsets;
    offsets.push_back(0); // index 0 unused (seq is 1-based)

    while (true) {
        long pos = ftell(f);
        uint8_t len_buf[2];
        if (fread(len_buf, 1, 2, f) != 2) break;
        uint16_t msg_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);
        if (msg_len == 0 || msg_len > k_max_msg_len) break;
        offsets.push_back(static_cast<uint64_t>(pos));
        if (fseek(f, msg_len, SEEK_CUR) != 0) break;
    }
    total_count = offsets.size() - 1;
    return offsets;
}

bool send_all(int sock, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t         sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int sock, void* buf, size_t len) {
    uint8_t* p    = static_cast<uint8_t*>(buf);
    size_t   got  = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n <= 0) return false; // 0 = peer closed, <0 = error
        got += static_cast<size_t>(n);
    }
    return true;
}

// Serves one range request from the pre-built index. Returns false if the
// client connection should be torn down (I/O error).
bool serve_request(int client, FILE* f, const std::vector<uint64_t>& offsets,
                    uint64_t total_count, uint64_t start_seq, uint64_t end_seq) {
    static uint8_t body_buf[k_max_msg_len];

    for (uint64_t seq = start_seq; seq <= end_seq; ++seq) {
        if (seq < 1 || seq > total_count) {
            uint8_t hdr[k_seq_header_size];
            pack_seq_header(hdr, seq, 0); // terminator: server has nothing more
            return send_all(client, hdr, sizeof(hdr));
        }

        if (fseek(f, static_cast<long>(offsets[seq]), SEEK_SET) != 0) return false;
        uint8_t len_buf[2];
        if (fread(len_buf, 1, 2, f) != 2) return false;
        uint16_t msg_len = static_cast<uint16_t>((len_buf[0] << 8) | len_buf[1]);
        if (fread(body_buf, 1, msg_len, f) != msg_len) return false;

        uint8_t hdr[k_seq_header_size];
        pack_seq_header(hdr, seq, msg_len);
        if (!send_all(client, hdr, sizeof(hdr))) return false;
        if (!send_all(client, body_buf, msg_len)) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    FILE* f = fopen(args.file, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", args.file);
        return 1;
    }

    uint64_t total_count = 0;
    std::vector<uint64_t> offsets = build_index(f, total_count);
    printf("retrans_server: indexed %" PRIu64 " messages from '%s'\n", total_count, args.file);

    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) { perror("socket"); return 1; }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(args.port);
    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(listen_sock, 1) < 0) { perror("listen"); return 1; }

    printf("retrans_server: listening on port %u (single client)\n", args.port);
    int client = accept(listen_sock, nullptr, nullptr);
    if (client < 0) { perror("accept"); return 1; }

    while (true) {
        uint8_t req[k_retrans_request_size];
        if (!recv_all(client, req, sizeof(req))) break; // client disconnected

        uint64_t start_seq, end_seq;
        unpack_retrans_request(req, start_seq, end_seq);
        if (!serve_request(client, f, offsets, total_count, start_seq, end_seq)) break;
    }

    close(client);
    close(listen_sock);
    fclose(f);
    return 0;
}
