#include "pitchframe/book/book_map.h"
#include "pitchframe/net/loss_injector.h"
#include "pitchframe/net/message_dispatch.h"
#include "pitchframe/net/reorder_buffer.h"
#include "pitchframe/net/retrans_protocol.h"
#include "pitchframe/net/sequence_tracker.h"
#include "pitchframe/parser/file_reader.h"
#include "pitchframe/parser/itch_parser.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

#ifndef PITCHFRAME_BUILD_DIR
#error "PITCHFRAME_BUILD_DIR must be defined by CMake (see CMakeLists.txt)"
#endif

using namespace pitchframe;

// ---------------------------------------------------------------------------
// ReorderBuffer
// ---------------------------------------------------------------------------

TEST(ReorderBufferTest, PutThenTakeRoundTrips) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1, 2, 3, 4};
    ASSERT_TRUE(buf.put(5, msg, sizeof(msg)));

    const uint8_t* body = nullptr;
    uint16_t       len  = 0;
    ASSERT_TRUE(buf.take(5, body, len));
    EXPECT_EQ(len, sizeof(msg));
    EXPECT_EQ(memcmp(body, msg, sizeof(msg)), 0);
}

TEST(ReorderBufferTest, TakeUnknownSeqFails) {
    ReorderBuffer buf(16);
    const uint8_t* body = nullptr;
    uint16_t       len  = 0;
    EXPECT_FALSE(buf.take(99, body, len));
}

TEST(ReorderBufferTest, TakeDrainsSlot) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1};
    buf.put(5, msg, 1);

    const uint8_t* body = nullptr;
    uint16_t       len  = 0;
    ASSERT_TRUE(buf.take(5, body, len));
    EXPECT_FALSE(buf.take(5, body, len)); // second take on same seq: already drained
}

TEST(ReorderBufferTest, ContainsReflectsPutAndTake) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1};
    EXPECT_FALSE(buf.contains(5));
    buf.put(5, msg, 1);
    EXPECT_TRUE(buf.contains(5));

    const uint8_t* body = nullptr;
    uint16_t       len  = 0;
    buf.take(5, body, len);
    EXPECT_FALSE(buf.contains(5));
}

// Aliasing: seq and seq+capacity map to the same ring slot index. The slot
// must disambiguate by stored seq, not just a valid flag — see item 2 of the
// Stage 2 plan addendum (same bug class as Stage 1's closing_ref[8205] stomp).
TEST(ReorderBufferTest, AliasedSeqPutRejectedWhileOriginalUndrained) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1};
    ASSERT_TRUE(buf.put(5, msg, 1));
    // 5 + capacity(16) = 21 aliases to the same slot as 5.
    EXPECT_FALSE(buf.put(21, msg, 1));
    // Original entry must still be intact and correctly attributed to seq 5.
    EXPECT_TRUE(buf.contains(5));
    EXPECT_FALSE(buf.contains(21));
}

TEST(ReorderBufferTest, AliasedSeqContainsNoFalsePositive) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1};
    buf.put(5, msg, 1);
    EXPECT_FALSE(buf.contains(21)); // must not false-hit off the aliased index
}

TEST(ReorderBufferTest, AliasedSeqPutSucceedsAfterOriginalDrained) {
    ReorderBuffer buf(16);
    const uint8_t msg[] = {1};
    buf.put(5, msg, 1);
    const uint8_t* body = nullptr;
    uint16_t       len  = 0;
    ASSERT_TRUE(buf.take(5, body, len)); // drain 5 first
    EXPECT_TRUE(buf.put(21, msg, 1));    // now the slot is free for the alias
    EXPECT_TRUE(buf.contains(21));
    EXPECT_FALSE(buf.contains(5));
}

TEST(ReorderBufferTest, SmallCapacityOverflowTriggersRejection) {
    // Small capacity exercises the overflow -> full-resync trigger cheaply,
    // without needing 65536 real drops (CLAUDE.md's default bound).
    ReorderBuffer buf(4);
    const uint8_t msg[] = {1};
    for (uint64_t seq = 1; seq <= 4; ++seq) ASSERT_TRUE(buf.put(seq, msg, 1));
    // Slot for seq 5 aliases slot for seq 1 (5 % 4 == 1 % 4), which is still live.
    EXPECT_FALSE(buf.put(5, msg, 1));
}

// ---------------------------------------------------------------------------
// PacketLossInjector
// ---------------------------------------------------------------------------

TEST(PacketLossInjectorTest, NoneDropsNothing) {
    PacketLossInjector inj;
    for (uint64_t seq = 1; seq <= 100; ++seq) EXPECT_FALSE(inj.should_drop(seq));
}

TEST(PacketLossInjectorTest, EveryNthDropsExactMultiples) {
    PacketLossInjector inj;
    inj.configure_every_nth(10);
    for (uint64_t seq = 1; seq <= 30; ++seq)
        EXPECT_EQ(inj.should_drop(seq), (seq % 10 == 0)) << "seq=" << seq;
}

TEST(PacketLossInjectorTest, BurstDropsOnlyConfiguredRange) {
    PacketLossInjector inj;
    inj.configure_burst(100, 10); // drops [100, 109]
    EXPECT_FALSE(inj.should_drop(99));
    for (uint64_t seq = 100; seq < 110; ++seq) EXPECT_TRUE(inj.should_drop(seq));
    EXPECT_FALSE(inj.should_drop(110));
}

TEST(PacketLossInjectorTest, RandomPctSameSeedIsReproducible) {
    PacketLossInjector a, b;
    a.configure_random_pct(0.3, 42);
    b.configure_random_pct(0.3, 42);
    for (uint64_t seq = 1; seq <= 1000; ++seq)
        ASSERT_EQ(a.should_drop(seq), b.should_drop(seq)) << "seq=" << seq;
}

TEST(PacketLossInjectorTest, RandomPctRoughlyMatchesRate) {
    PacketLossInjector inj;
    inj.configure_random_pct(0.05, 42);
    int dropped = 0;
    for (uint64_t seq = 1; seq <= 100000; ++seq)
        if (inj.should_drop(seq)) ++dropped;
    double rate = static_cast<double>(dropped) / 100000.0;
    EXPECT_NEAR(rate, 0.05, 0.01);
}

// ---------------------------------------------------------------------------
// SequenceTracker — fake callbacks, no sockets. Exercises the gap-detection
// policy in isolation (CLAUDE.md's on_packet rules) and, separately, the
// short/failed-retransmission-response -> full-resync path (Stage 2 plan
// addendum item 4: the client must not block forever on an incomplete range).
// ---------------------------------------------------------------------------

namespace {

struct DeliveredMsg {
    uint64_t             seq;
    std::vector<uint8_t> body;
};

struct TrackerTestCtx {
    std::vector<DeliveredMsg>                 delivered;
    std::vector<std::pair<uint64_t, uint64_t>> retransmit_calls;
    SequenceTracker*                          tracker = nullptr;

    struct ScriptedResponse {
        bool                       success;
        std::vector<DeliveredMsg>  messages; // fed via on_retransmitted before returning
    };
    std::vector<ScriptedResponse> script;
    size_t                        script_idx = 0;
};

void test_deliver(void* ctx_v, uint64_t seq, const uint8_t* body, uint16_t len) {
    auto* ctx = static_cast<TrackerTestCtx*>(ctx_v);
    ctx->delivered.push_back({seq, std::vector<uint8_t>(body, body + len)});
}

bool test_retransmit(void* ctx_v, uint64_t start_seq, uint64_t end_seq) {
    auto* ctx = static_cast<TrackerTestCtx*>(ctx_v);
    ctx->retransmit_calls.push_back({start_seq, end_seq});
    if (ctx->script_idx >= ctx->script.size()) return false;
    auto& resp = ctx->script[ctx->script_idx++];
    for (auto& m : resp.messages)
        ctx->tracker->on_retransmitted(m.seq, m.body.data(), static_cast<uint16_t>(m.body.size()));
    return resp.success;
}

const uint8_t kByte[1] = {0xAB};

} // namespace

TEST(SequenceTrackerTest, InOrderDeliveryAdvancesContiguous) {
    TrackerTestCtx ctx;
    SequenceTracker tracker(test_deliver, &ctx, test_retransmit, &ctx);
    ctx.tracker = &tracker;

    tracker.on_packet(1, kByte, 1);
    tracker.on_packet(2, kByte, 1);
    tracker.on_packet(3, kByte, 1);

    ASSERT_EQ(ctx.delivered.size(), 3u);
    EXPECT_EQ(ctx.delivered[0].seq, 1u);
    EXPECT_EQ(ctx.delivered[1].seq, 2u);
    EXPECT_EQ(ctx.delivered[2].seq, 3u);
    EXPECT_TRUE(ctx.retransmit_calls.empty());
    EXPECT_EQ(tracker.last_contiguous_seq(), 3u);
}

TEST(SequenceTrackerTest, DuplicateAndOldSeqDiscarded) {
    TrackerTestCtx ctx;
    SequenceTracker tracker(test_deliver, &ctx, test_retransmit, &ctx);
    ctx.tracker = &tracker;

    tracker.on_packet(1, kByte, 1);
    tracker.on_packet(1, kByte, 1); // duplicate
    tracker.on_packet(0, kByte, 1); // stale/invalid, <= last_contiguous_seq_

    EXPECT_EQ(ctx.delivered.size(), 1u);
    EXPECT_TRUE(ctx.retransmit_calls.empty());
    EXPECT_EQ(tracker.last_contiguous_seq(), 1u);
}

TEST(SequenceTrackerTest, GapTriggersRetransmitWithCorrectRangeAndDrains) {
    TrackerTestCtx ctx;
    SequenceTracker tracker(test_deliver, &ctx, test_retransmit, &ctx);
    ctx.tracker = &tracker;

    ctx.script.push_back({true, {{2, {kByte, kByte + 1}}, {3, {kByte, kByte + 1}}, {4, {kByte, kByte + 1}}}});

    tracker.on_packet(1, kByte, 1); // contiguous
    tracker.on_packet(5, kByte, 1); // gap: buffers 5, requests [2,4]

    ASSERT_EQ(ctx.retransmit_calls.size(), 1u);
    EXPECT_EQ(ctx.retransmit_calls[0].first, 2u);
    EXPECT_EQ(ctx.retransmit_calls[0].second, 4u);

    ASSERT_EQ(ctx.delivered.size(), 5u);
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(ctx.delivered[i].seq, i + 1);
    EXPECT_EQ(tracker.last_contiguous_seq(), 5u);
}

// Item 4 of the Stage 2 plan addendum: a short/failed retransmission response
// must not block the client forever. It must fall back to full resync.
TEST(SequenceTrackerTest, ShortRetransmitResponseTriggersFullResync) {
    TrackerTestCtx ctx;
    SequenceTracker tracker(test_deliver, &ctx, test_retransmit, &ctx);
    ctx.tracker = &tracker;

    // First attempt for [2,4] comes back short (server couldn't supply it).
    ctx.script.push_back({false, {}});
    // full_resync()'s follow-up attempt for [2, highest_seq_seen=5] succeeds.
    ctx.script.push_back({true, {{2, {kByte, kByte + 1}}, {3, {kByte, kByte + 1}},
                                  {4, {kByte, kByte + 1}}, {5, {kByte, kByte + 1}}}});

    tracker.on_packet(1, kByte, 1);
    tracker.on_packet(5, kByte, 1); // gap; first fetch fails -> full_resync retries wider range

    ASSERT_EQ(ctx.retransmit_calls.size(), 2u);
    EXPECT_EQ(ctx.retransmit_calls[0], (std::pair<uint64_t, uint64_t>{2, 4}));
    EXPECT_EQ(ctx.retransmit_calls[1], (std::pair<uint64_t, uint64_t>{2, 5}));

    EXPECT_EQ(tracker.last_contiguous_seq(), 5u); // fully recovered, not hung
}

TEST(SequenceTrackerTest, PermanentlyFailedResyncGivesUpWithoutHanging) {
    TrackerTestCtx ctx;
    SequenceTracker tracker(test_deliver, &ctx, test_retransmit, &ctx);
    ctx.tracker = &tracker;
    // No scripted responses at all -> every retransmit_fn_ call returns false.

    tracker.on_packet(1, kByte, 1);
    tracker.on_packet(5, kByte, 1); // gap fetch fails, full_resync's retry also fails

    // v1 gives up after one resync attempt rather than looping forever.
    EXPECT_EQ(ctx.retransmit_calls.size(), 2u);
    EXPECT_EQ(tracker.last_contiguous_seq(), 1u); // stuck at 1, but did not hang
}

// ---------------------------------------------------------------------------
// Run A/B correctness proof (CLAUDE.md's Stage 2 Correctness Proof section).
//
// Run A: replay file -> dispatch_message directly, no network at all.
// Run B: udp_replayer -> PacketLossInjector (inside pitchframe_net) ->
//        SequenceTracker -> retrans_server -> dispatch_message, driven
//        through the actual compiled binaries as subprocesses talking over
//        real localhost sockets. Deliberately not re-using library code for
//        Run B — exercising the real binaries a user would run from the CLI
//        is a stronger proof than reusing internals, and it's what the plan
//        already validated manually.
//
// Assert: per-symbol {shares_traded, active_orders, best_bid, best_ask},
// captured at the closing 'M' System Event, are exactly equal.
// ---------------------------------------------------------------------------

namespace {

// --- Small hand-built ITCH fixture -----------------------------------------

void put_u16be(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void put_u32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void put_u64be(std::vector<uint8_t>& v, uint64_t x) {
    for (int shift = 56; shift >= 0; shift -= 8) v.push_back(static_cast<uint8_t>(x >> shift));
}

std::vector<uint8_t> msg_header(char type, uint16_t locate) {
    std::vector<uint8_t> b(11, 0);
    b[0] = static_cast<uint8_t>(type);
    b[1] = static_cast<uint8_t>(locate >> 8);
    b[2] = static_cast<uint8_t>(locate);
    return b; // [3..10] tracking + timestamp left zero, unused by the decoders
}

void append_msg(std::vector<uint8_t>& out, std::vector<uint8_t> body) {
    put_u16be(out, static_cast<uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void add_s(std::vector<uint8_t>& out, char event) {
    auto b = msg_header('S', 0);
    b.push_back(static_cast<uint8_t>(event));
    while (b.size() < 12) b.push_back(0);
    append_msg(out, b);
}

void add_r(std::vector<uint8_t>& out, uint16_t locate, const char* symbol) {
    auto b = msg_header('R', locate);
    for (int i = 0; i < 8; ++i) b.push_back(symbol[i] ? static_cast<uint8_t>(symbol[i]) : ' ');
    while (b.size() < 19) b.push_back(0);
    append_msg(out, b);
}

void add_a(std::vector<uint8_t>& out, uint16_t locate, uint64_t ref, char side,
           uint32_t shares, uint32_t price) {
    auto b = msg_header('A', locate);
    put_u64be(b, ref);
    b.push_back(static_cast<uint8_t>(side));
    put_u32be(b, shares);
    for (int i = 0; i < 8; ++i) b.push_back(0); // stock symbol field, unused by decoder
    put_u32be(b, price);
    append_msg(out, b);
}

void add_x(std::vector<uint8_t>& out, uint16_t locate, uint64_t ref, uint32_t cancelled) {
    auto b = msg_header('X', locate);
    put_u64be(b, ref);
    put_u32be(b, cancelled);
    append_msg(out, b);
}

void add_d(std::vector<uint8_t>& out, uint16_t locate, uint64_t ref) {
    auto b = msg_header('D', locate);
    put_u64be(b, ref);
    append_msg(out, b);
}

void add_u(std::vector<uint8_t>& out, uint16_t locate, uint64_t orig_ref, uint64_t new_ref,
           uint32_t new_shares, uint32_t new_price) {
    auto b = msg_header('U', locate);
    put_u64be(b, orig_ref);
    put_u64be(b, new_ref);
    put_u32be(b, new_shares);
    put_u32be(b, new_price);
    append_msg(out, b);
}

void add_e(std::vector<uint8_t>& out, uint16_t locate, uint64_t ref, uint32_t executed) {
    auto b = msg_header('E', locate);
    put_u64be(b, ref);
    put_u32be(b, executed);
    put_u64be(b, 0); // match_number, unused by the decoder
    append_msg(out, b);
}

void add_c(std::vector<uint8_t>& out, uint16_t locate, uint64_t ref, uint32_t executed, uint32_t exec_price) {
    auto b = msg_header('C', locate);
    put_u64be(b, ref);
    put_u32be(b, executed);
    put_u64be(b, 0);  // match_number, unused
    b.push_back(0);   // printable flag, unused
    put_u32be(b, exec_price);
    append_msg(out, b);
}

// A 20-message stream spanning every book-mutating type (A, F is untested
// separately but shares decode_add_order with A; X, D, U, E, C) across two
// symbols. Content is arbitrary but fixed — Run A/B equality only requires
// both legs see the identical stream.
std::vector<uint8_t> build_fixture() {
    std::vector<uint8_t> out;
    add_s(out, 'O');
    add_r(out, 1, "AAPL");
    add_r(out, 2, "MSFT");
    add_a(out, 1, 100, 'B', 100, 500000);
    add_a(out, 1, 101, 'S', 200, 500100);
    add_a(out, 2, 200, 'B', 500, 300000);
    add_x(out, 1, 100, 40);
    add_e(out, 2, 200, 100);
    add_d(out, 1, 101);
    add_u(out, 2, 200, 201, 300, 300050);
    add_a(out, 1, 102, 'B', 50, 499900);
    add_a(out, 2, 202, 'S', 150, 300200);
    add_c(out, 2, 201, 50, 300060);
    add_x(out, 2, 202, 20);
    add_d(out, 2, 201);
    add_a(out, 1, 103, 'S', 75, 500200);
    add_e(out, 1, 102, 20);
    add_a(out, 2, 203, 'B', 100, 299900);
    add_d(out, 1, 103);
    add_s(out, 'M');
    return out;
}

std::string write_fixture_file() {
    std::string path = std::string(PITCHFRAME_BUILD_DIR) + "/net_test_fixture.bin";
    std::vector<uint8_t> data = build_fixture();
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return path;
}

// --- Run A: file -> dispatch_message, no network. ---------------------------

void run_a(const std::string& fixture_path, SymbolSnapshot* snapshots) {
    FileReader reader;
    ASSERT_TRUE(reader.open(fixture_path.c_str()));
    ITCHParser parser;
    BookMap    books;

    while (reader.ensure(2)) {
        uint16_t msg_len = reader.read_u16_be();
        if (!reader.ensure(msg_len)) break;
        const uint8_t* body = reader.consume(msg_len);
        maybe_capture_close_snapshot(body, msg_len, books, snapshots);
        dispatch_message(body, msg_len, parser, books);
    }
    reader.close();
}

// --- Run B: real binaries as subprocesses over real localhost sockets. -----

pid_t spawn_process(const std::vector<std::string>& args, const char* stdout_path = nullptr) {
    pid_t pid = fork();
    if (pid == 0) {
        if (stdout_path) {
            int fd = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127); // exec failed
    }
    return pid;
}

// True if the process exited within timeout_ms; false if it had to be killed
// (the caller treats that as a test failure, not a hang).
bool wait_for_exit(pid_t pid, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return true;
        usleep(10000);
        waited += 10;
    }
    kill(pid, SIGKILL);
    int status;
    waitpid(pid, &status, 0);
    return false;
}

void parse_snapshot_file(const std::string& path, SymbolSnapshot* snapshots) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned           locate;
        unsigned long long shares;
        int                active;
        unsigned           bid, ask;
        if (sscanf(line, "SNAPSHOT %u %llu %d %u %u", &locate, &shares, &active, &bid, &ask) == 5
            && locate < k_max_locate) {
            snapshots[locate].shares_traded = shares;
            snapshots[locate].active_orders = active;
            snapshots[locate].best_bid      = bid;
            snapshots[locate].best_ask      = ask;
            snapshots[locate].captured      = true;
        }
    }
    fclose(f);
}

struct LossScenario {
    std::string loss_policy;   // "" = none, else "random" | "every-n" | "burst"
    double      loss_pct     = 0.0;
    uint64_t    loss_every_n = 0;
    uint64_t    burst_start  = 0;
    uint64_t    burst_len    = 0;
    uint64_t    seed         = 42;
};

void run_b(const std::string& fixture_path, uint16_t udp_port, uint16_t retrans_port,
           const LossScenario& scenario, SymbolSnapshot* snapshots) {
    std::string build_dir = PITCHFRAME_BUILD_DIR;
    std::string net_stdout = build_dir + "/net_test_stdout_" + std::to_string(udp_port) + ".log";

    pid_t retrans_pid = spawn_process({build_dir + "/retrans_server", fixture_path,
                                        "--port", std::to_string(retrans_port)});
    usleep(150000); // let retrans_server bind + listen

    std::vector<std::string> net_args = {build_dir + "/pitchframe_net",
                                          "--udp-port", std::to_string(udp_port),
                                          "--retrans-port", std::to_string(retrans_port),
                                          "--retrans-host", "127.0.0.1"};
    if (!scenario.loss_policy.empty()) {
        net_args.push_back("--loss-policy");
        net_args.push_back(scenario.loss_policy);
        if (scenario.loss_policy == "random") {
            net_args.push_back("--loss-pct");
            net_args.push_back(std::to_string(scenario.loss_pct));
            net_args.push_back("--seed");
            net_args.push_back(std::to_string(scenario.seed));
        } else if (scenario.loss_policy == "every-n") {
            net_args.push_back("--loss-every-n");
            net_args.push_back(std::to_string(scenario.loss_every_n));
        } else if (scenario.loss_policy == "burst") {
            net_args.push_back("--burst-start");
            net_args.push_back(std::to_string(scenario.burst_start));
            net_args.push_back("--burst-len");
            net_args.push_back(std::to_string(scenario.burst_len));
        }
    }
    pid_t net_pid = spawn_process(net_args, net_stdout.c_str());
    usleep(150000); // let pitchframe_net bind UDP + connect TCP

    // Non-zero delay: per the Stage 2 plan addendum (item 1), a zero-delay
    // send at this small fixture size is harmless, but keeping the delay
    // matches the documented mitigation for synchronous-fetch-induced
    // secondary kernel-buffer loss at scale, and keeps this test's behavior
    // representative of the documented recommendation.
    pid_t replayer_pid = spawn_process({build_dir + "/udp_replayer", fixture_path,
                                         "--port", std::to_string(udp_port),
                                         "--delay-us", "1000"});
    ASSERT_TRUE(wait_for_exit(replayer_pid, 5000)) << "udp_replayer did not exit";

    // Generous timeout: the very first subprocess exec of a freshly-built
    // binary in a test run can be slow on macOS (Gatekeeper/codesign
    // scanning a not-yet-cached binary) — observed as a one-off ~12s stall
    // on an otherwise-passing run. Real hangs are logic bugs and would still
    // time out well within this window; this margin is about avoiding a
    // false failure from OS-level first-exec cost, not masking recovery bugs.
    bool net_ok = wait_for_exit(net_pid, 15000);
    EXPECT_TRUE(net_ok) << "pitchframe_net did not exit within timeout (possible hang)";

    wait_for_exit(retrans_pid, 3000); // self-exits once pitchframe_net closes the TCP connection

    if (net_ok) parse_snapshot_file(net_stdout, snapshots);
}

void expect_snapshots_equal(const SymbolSnapshot* a, const SymbolSnapshot* b) {
    for (uint16_t i = 1; i < k_max_locate; ++i) {
        if (!a[i].captured && !b[i].captured) continue;
        SCOPED_TRACE(::testing::Message() << "locate=" << i);
        ASSERT_EQ(a[i].captured, b[i].captured);
        EXPECT_EQ(a[i].shares_traded, b[i].shares_traded);
        EXPECT_EQ(a[i].active_orders, b[i].active_orders);
        EXPECT_EQ(a[i].best_bid, b[i].best_bid);
        EXPECT_EQ(a[i].best_ask, b[i].best_ask);
    }
}

} // namespace

TEST(RunABCorrectnessTest, NoLoss) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    run_b(fixture, 25010, 25011, {}, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

TEST(RunABCorrectnessTest, LightLoss_0_1pct) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    LossScenario s;
    s.loss_policy = "random";
    s.loss_pct    = 0.001;
    s.seed        = 42;
    run_b(fixture, 25020, 25021, s, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

TEST(RunABCorrectnessTest, Loss1pct_Seed42) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    LossScenario s;
    s.loss_policy = "random";
    s.loss_pct    = 0.01;
    s.seed        = 42;
    run_b(fixture, 25030, 25031, s, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

TEST(RunABCorrectnessTest, HeavyLoss5pct) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    LossScenario s;
    s.loss_policy = "random";
    s.loss_pct    = 0.05;
    s.seed        = 42;
    run_b(fixture, 25040, 25041, s, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

TEST(RunABCorrectnessTest, BurstOf10ConsecutiveDrops) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    LossScenario s;
    s.loss_policy  = "burst";
    s.burst_start  = 5;
    s.burst_len    = 10;
    run_b(fixture, 25050, 25051, s, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

// Burst covering the very start of the stream: last_contiguous_seq_ is still
// 0 when the gap is first detected — a distinct edge case from a mid-stream burst.
TEST(RunABCorrectnessTest, BurstAtMarketOpen) {
    std::string fixture = write_fixture_file();
    std::vector<SymbolSnapshot> snap_a(k_max_locate), snap_b(k_max_locate);
    run_a(fixture, snap_a.data());
    LossScenario s;
    s.loss_policy  = "burst";
    s.burst_start  = 1;
    s.burst_len    = 3;
    run_b(fixture, 25060, 25061, s, snap_b.data());
    expect_snapshots_equal(snap_a.data(), snap_b.data());
}

// Stage 2 plan addendum item 4, server side: a request extending past the
// file's message count must end in a terminator record, not a hang — the
// client-side handling of that terminator is covered by
// SequenceTrackerTest.ShortRetransmitResponseTriggersFullResync above.
TEST(RetransmissionServerTest, OutOfRangeRequestReturnsTerminator) {
    std::string fixture = write_fixture_file(); // 20 real messages, seq 1..20
    std::string build_dir = PITCHFRAME_BUILD_DIR;
    pid_t server_pid = spawn_process({build_dir + "/retrans_server", fixture, "--port", "25070"});
    usleep(150000);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(25070);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    ASSERT_EQ(connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

    uint8_t req[k_retrans_request_size];
    pack_retrans_request(req, 15, 30); // extends 10 past the fixture's 20 messages
    ASSERT_EQ(send(sock, req, sizeof(req), 0), static_cast<ssize_t>(sizeof(req)));

    bool saw_terminator = false;
    for (int i = 0; i < 20 && !saw_terminator; ++i) {
        uint8_t hdr[k_seq_header_size];
        size_t  got = 0;
        while (got < sizeof(hdr)) {
            ssize_t n = recv(sock, hdr + got, sizeof(hdr) - got, 0);
            ASSERT_GT(n, 0);
            got += static_cast<size_t>(n);
        }
        uint64_t seq;
        uint16_t len;
        unpack_seq_header(hdr, seq, len);
        if (len == 0) { saw_terminator = true; break; }

        std::vector<uint8_t> body(len);
        size_t bgot = 0;
        while (bgot < body.size()) {
            ssize_t n = recv(sock, body.data() + bgot, body.size() - bgot, 0);
            ASSERT_GT(n, 0);
            bgot += static_cast<size_t>(n);
        }
    }
    EXPECT_TRUE(saw_terminator);

    close(sock);
    wait_for_exit(server_pid, 2000);
}
