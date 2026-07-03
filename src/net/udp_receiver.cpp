// PitchFrame — Stage 2, pitchframe_net driver
// Receives the UDP feed, applies the (optional, test-only) PacketLossInjector,
// hands packets to SequenceTracker for gap detection/reordering, and drives
// in-order messages into the unchanged Stage 1 pipeline via dispatch_message().
// Gap fills are fetched synchronously over a persistent TCP connection to
// RetransmissionServer (documented v1 simplification — see README Known
// Limitations for the shared-thread / secondary-kernel-buffer-loss tradeoff).
#include "pitchframe/book/book_map.h"
#include "pitchframe/common/types.h"
#include "pitchframe/net/loss_injector.h"
#include "pitchframe/net/message_dispatch.h"
#include "pitchframe/net/retrans_protocol.h"
#include "pitchframe/net/sequence_tracker.h"
#include "pitchframe/parser/itch_parser.h"
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
    uint16_t    udp_port     = 21002;
    uint16_t    retrans_port = 21003;
    const char* retrans_host = "127.0.0.1";
    LossPolicy  loss_policy  = LossPolicy::None;
    double      loss_pct     = 0.0;
    uint64_t    loss_every_n = 0;
    uint64_t    burst_start  = 0;
    uint64_t    burst_len    = 0;
    uint64_t    seed         = 42;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--udp-port") && i + 1 < argc)          a.udp_port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--retrans-port") && i + 1 < argc) a.retrans_port = static_cast<uint16_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--retrans-host") && i + 1 < argc) a.retrans_host = argv[++i];
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)         a.seed = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--loss-policy") && i + 1 < argc) {
            const char* p = argv[++i];
            if (!strcmp(p, "random"))       a.loss_policy = LossPolicy::RandomPct;
            else if (!strcmp(p, "every-n")) a.loss_policy = LossPolicy::EveryNth;
            else if (!strcmp(p, "burst"))   a.loss_policy = LossPolicy::Burst;
        }
        else if (!strcmp(argv[i], "--loss-pct") && i + 1 < argc)     a.loss_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--loss-every-n") && i + 1 < argc) a.loss_every_n = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--burst-start") && i + 1 < argc)  a.burst_start = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--burst-len") && i + 1 < argc)    a.burst_len = strtoull(argv[++i], nullptr, 10);
    }
    return a;
}

// --- DeliverFn: pushes an in-order message into the Stage 1 pipeline. -----
struct DeliverCtx {
    ITCHParser*    parser;
    BookMap*       books;
    SymbolSnapshot snapshots[k_max_locate]; // captured at the closing 'M' event
    uint64_t       total = 0;
    bool           done  = false; // set once the end-of-stream sentinel (len==0) is delivered contiguously
};

void deliver(void* ctx_v, uint64_t /*seq*/, const uint8_t* body, uint16_t len) {
    auto* ctx = static_cast<DeliverCtx*>(ctx_v);
    if (len == 0) { ctx->done = true; return; } // sentinel reached the front of the queue
    maybe_capture_close_snapshot(body, len, *ctx->books, ctx->snapshots);
    dispatch_message(body, len, *ctx->parser, *ctx->books);
    ++ctx->total;
}

// --- RetransmitFn: synchronous TCP fetch of [start_seq, end_seq]. ---------
struct RetransmitCtx {
    int              tcp_sock;
    SequenceTracker* tracker;
};

bool recv_all(int sock, void* buf, size_t len) {
    uint8_t* p   = static_cast<uint8_t*>(buf);
    size_t   got = 0;
    while (got < len) {
        ssize_t n = recv(sock, p + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_all(int sock, const void* buf, size_t len) {
    const uint8_t* p    = static_cast<const uint8_t*>(buf);
    size_t         sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool tcp_retransmit_fetch(void* ctx_v, uint64_t start_seq, uint64_t end_seq) {
    auto* ctx = static_cast<RetransmitCtx*>(ctx_v);

    uint8_t req[k_retrans_request_size];
    pack_retrans_request(req, start_seq, end_seq);
    if (!send_all(ctx->tcp_sock, req, sizeof(req))) return false;

    uint64_t expected = end_seq - start_seq + 1;
    uint64_t received = 0;
    static uint8_t body[k_max_msg_len];

    while (received < expected) {
        uint8_t hdr[k_seq_header_size];
        if (!recv_all(ctx->tcp_sock, hdr, sizeof(hdr))) return false;

        uint64_t seq;
        uint16_t len;
        unpack_seq_header(hdr, seq, len);
        if (len == 0) return false; // server terminator: cannot supply the full range

        if (!recv_all(ctx->tcp_sock, body, len)) return false;
        ctx->tracker->on_retransmitted(seq, body, len);
        ++received;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    // --- UDP receive socket ---
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) { perror("socket"); return 1; }

    // See README Known Limitations: ingestion and synchronous gap recovery
    // share this thread, so a generous buffer absorbs bursts while blocked
    // on a retransmission fetch, rather than the kernel silently dropping
    // packets nobody asked it to lose.
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int       effective     = 0;
    socklen_t effective_len = sizeof(effective);
    getsockopt(udp_sock, SOL_SOCKET, SO_RCVBUF, &effective, &effective_len);
    printf("pitchframe_net: SO_RCVBUF requested=%d effective=%d\n", rcvbuf, effective);

    sockaddr_in udp_addr{};
    udp_addr.sin_family      = AF_INET;
    udp_addr.sin_addr.s_addr = INADDR_ANY;
    udp_addr.sin_port        = htons(args.udp_port);
    if (bind(udp_sock, reinterpret_cast<sockaddr*>(&udp_addr), sizeof(udp_addr)) < 0) {
        perror("bind");
        return 1;
    }

    // --- TCP connection to RetransmissionServer (persistent, single-client) ---
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) { perror("socket"); return 1; }

    sockaddr_in tcp_addr{};
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port   = htons(args.retrans_port);
    if (inet_pton(AF_INET, args.retrans_host, &tcp_addr.sin_addr) != 1) {
        fprintf(stderr, "error: bad retrans-host '%s'\n", args.retrans_host);
        return 1;
    }
    if (connect(tcp_sock, reinterpret_cast<sockaddr*>(&tcp_addr), sizeof(tcp_addr)) < 0) {
        perror("connect (retrans_server)");
        return 1;
    }

    // --- Loss injector (test-only; off by default) ---
    PacketLossInjector injector;
    switch (args.loss_policy) {
        case LossPolicy::RandomPct: injector.configure_random_pct(args.loss_pct, args.seed); break;
        case LossPolicy::EveryNth:  injector.configure_every_nth(args.loss_every_n); break;
        case LossPolicy::Burst:     injector.configure_burst(args.burst_start, args.burst_len); break;
        case LossPolicy::None:      injector.configure_none(); break;
    }

    // --- Pipeline wiring ---
    ITCHParser parser;
    BookMap    books;
    DeliverCtx deliver_ctx;
    deliver_ctx.parser = &parser;
    deliver_ctx.books  = &books;

    RetransmitCtx retransmit_ctx{tcp_sock, nullptr};
    SequenceTracker tracker(deliver, &deliver_ctx, tcp_retransmit_fetch, &retransmit_ctx);
    retransmit_ctx.tracker = &tracker;

    static uint8_t packet[k_seq_header_size + k_max_msg_len];

    while (!deliver_ctx.done) {
        ssize_t n = recvfrom(udp_sock, packet, sizeof(packet), 0, nullptr, nullptr);
        if (n < static_cast<ssize_t>(k_seq_header_size)) continue; // short/garbage packet

        uint64_t seq;
        uint16_t len;
        unpack_seq_header(packet, seq, len);

        // The end-of-stream sentinel (len == 0) is never subject to the loss
        // policy — a dropped sentinel hangs the receiver, and since drops
        // are seed-dependent that would show up as an intermittent hang.
        // It still only ends the session once it arrives contiguously (see
        // DeliverCtx::done, set inside deliver()), not the moment it's seen.
        if (len != 0 && injector.should_drop(seq)) continue;
        if (len != 0 && static_cast<ssize_t>(k_seq_header_size + len) > n) continue; // truncated

        tracker.on_packet(seq, packet + k_seq_header_size, len);
    }

    close(udp_sock);
    close(tcp_sock);

    uint64_t grand_total_shares = 0;
    for (uint16_t i = 1; i < k_max_locate; ++i) {
        if (const OrderBook* book = books.get(i)) grand_total_shares += book->shares_traded();
    }

    printf("\n=== pitchframe_net summary ===\n");
    printf("Messages processed: %" PRIu64 "\n", deliver_ctx.total);
    printf("Last contiguous seq: %" PRIu64 "\n", tracker.last_contiguous_seq());
    printf("Grand total shares traded: %" PRIu64 "\n", grand_total_shares);

    // Machine-parseable per-symbol state captured at the closing 'M' event —
    // consumed by the Run A/B correctness test (tests/net_test.cpp).
    for (uint16_t i = 1; i < k_max_locate; ++i) {
        const SymbolSnapshot& s = deliver_ctx.snapshots[i];
        if (!s.captured) continue;
        printf("SNAPSHOT %u %" PRIu64 " %d %u %u\n",
               i, s.shares_traded, s.active_orders, s.best_bid, s.best_ask);
    }
    return 0;
}
