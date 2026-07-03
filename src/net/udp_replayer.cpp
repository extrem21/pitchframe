// PitchFrame — Stage 2, UDPReplayer
// Reads an ITCH replay file and resends each message as a UDP packet with a
// synthesized, monotonic sequence number: [8B seq BE][2B len BE][body]. The
// file has no sequence numbers of its own (NASDAQ's real MoldUDP64 session
// framing is stripped from the historical file) — message 1 -> seq 1, and so
// on. After the last real message, sends a zero-length sentinel packet
// (seq = N+1) so the receiver knows the replay ended; real UDP has no EOF.
#include "pitchframe/common/types.h"
#include "pitchframe/net/retrans_protocol.h"
#include "pitchframe/parser/file_reader.h"
#include <arpa/inet.h>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace pitchframe;

namespace {

struct Args {
    const char* file     = nullptr;
    uint16_t    port     = 21002;
    const char* host     = "127.0.0.1";
    uint64_t    delay_us = 0;
};

Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <itch-replay-file> [--port N] [--host H] [--delay-us N]\n", argv[0]);
        exit(1);
    }
    a.file = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)          a.port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)     a.host = argv[++i];
        else if (!strcmp(argv[i], "--delay-us") && i + 1 < argc) a.delay_us = strtoull(argv[++i], nullptr, 10);
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    static FileReader reader;
    if (!reader.open(args.file)) {
        fprintf(stderr, "error: cannot open '%s'\n", args.file);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(args.port);
    if (inet_pton(AF_INET, args.host, &dest.sin_addr) != 1) {
        fprintf(stderr, "error: bad host '%s'\n", args.host);
        return 1;
    }

    static uint8_t packet[k_seq_header_size + k_max_msg_len];
    uint64_t seq = 0;

    while (true) {
        if (!reader.ensure(2)) break;
        uint16_t msg_len = reader.read_u16_be();
        if (msg_len == 0 || msg_len > k_max_msg_len) {
            fprintf(stderr, "error: implausible length %u at seq %" PRIu64 "\n", msg_len, seq + 1);
            return 1;
        }
        if (!reader.ensure(msg_len)) {
            fprintf(stderr, "error: truncated body at seq %" PRIu64 "\n", seq + 1);
            return 1;
        }
        const uint8_t* body = reader.consume(msg_len);
        ++seq;

        pack_seq_header(packet, seq, msg_len);
        memcpy(packet + k_seq_header_size, body, msg_len);
        sendto(sock, packet, k_seq_header_size + msg_len, 0,
               reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

        if (args.delay_us) usleep(static_cast<useconds_t>(args.delay_us));
    }
    reader.close();

    // End-of-stream sentinel: seq = N+1, len = 0, no body. This is
    // replay-mode-only framing — a real exchange feed never ends.
    pack_seq_header(packet, seq + 1, 0);
    sendto(sock, packet, k_seq_header_size, 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    printf("udp_replayer: sent %" PRIu64 " messages + sentinel to %s:%u\n", seq, args.host, args.port);
    close(sock);
    return 0;
}
