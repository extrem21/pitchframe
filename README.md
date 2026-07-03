# PitchFrame — NASDAQ ITCH 5.0 Feed Handler

A low-latency market data processing engine in C++17 modeled after the feed handler component
found inside real HFT firms. Receives raw binary ITCH 5.0 messages, decodes them, and maintains
a per-symbol limit order book in real time. Correctness is verified against NASDAQ's own
in-stream data — not synthetic test cases.

Portfolio project targeting low latency systems adjacent SWE roles. Every design decision is defensible under
technical questioning; simplifications are documented honestly.

---

## Status

| Stage | Description | State |
|-------|-------------|-------|
| 1 | ITCH 5.0 parser + order book reconstruction | **Complete** |
| 2 | Gap detection + retransmission recovery | In progress |
| 3 | Lock-free Disruptor ring buffer | Not started |

---

## Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# Tests
ctest --output-on-failure

# Benchmarks (book-op microbenchmarks, no file needed)
./itch_bench

# Full replay throughput benchmark (requires replay file)
ITCH_REPLAY_FILE=/path/to/12302019.NASDAQ_ITCH50 ./itch_bench --benchmark_filter=BM_FullReplay

# Full replay with validation output
./pitchframe /path/to/12302019.NASDAQ_ITCH50
```

```bash
# Stage 2 — replay the file over localhost UDP with a gap-recovery path.
# Start the retransmission server and receiver first, then the replayer.
./retrans_server /path/to/12302019.NASDAQ_ITCH50 --port 21003
./pitchframe_net --udp-port 21002 --retrans-port 21003 \
    --loss-policy random --loss-pct 1.0 --seed 42
./udp_replayer /path/to/12302019.NASDAQ_ITCH50 --port 21002 --delay-us 0
```

`--loss-policy` accepts `random` (`--loss-pct`, `--seed`), `every-n`
(`--loss-every-n`), or `burst` (`--burst-start`, `--burst-len`); omit it for a
lossless run. The end-of-stream sentinel is exempt from all loss policies, so
`pitchframe_net` always terminates once the replay finishes, whether or not
gaps occurred in between.

---

## Architecture (Stage 1)

Stage 1 reads from a file, not a network socket. The goal of Stage 1 is parser and order-book
correctness against the NASDAQ oracle; transport complexity (UDP multicast, gap detection,
retransmission) is Stage 2.

```
NASDAQ ITCH replay file (.bin)
        │
        ▼
  FileReader        — 1 MB ring buffer; memmove on refill; amortises fread() syscall cost
        │
        ▼
  ITCHParser        — zero-copy binary decode; bswap intrinsics; no intermediate structs
        │
        ▼
  MessageRouter     — switch dispatch by message type; locate code → BookMap index
        │
        ▼
  OrderBook         — per-symbol; unordered_map<ref, Order> + price-level array (see below)
        │
        ▼
  Validator         — NOII closing-cross reference price vs book best bid/ask
```

---

## Architecture (Stage 2)

Stage 1 assumed perfect delivery. The real NASDAQ feed is UDP multicast, which guarantees neither
delivery nor ordering: a dropped packet is a missed message, and a missed message corrupts book
state — a missed Add Order understates liquidity, a missed Delete leaves a ghost order resting.
Stage 2 adds the machinery to detect sequence-number gaps, buffer out-of-order messages, and
recover missing ones over a reliable side channel, so the reconstructed book is correct even over
a lossy transport.

**This simulates the live feed; it does not connect to it.** NASDAQ's real multicast feed requires
paid institutional access. Stage 2 replays the same historical file over localhost UDP sockets and
uses a seeded loss injector to create the packet-drop conditions that make recovery necessary. The
gap-detection, reordering, and retransmission logic is identical to what a real feed would exercise
— only the packet source differs.

```
NASDAQ ITCH replay file (same file as Stage 1)
        │
        ▼
  UDPReplayer         — synthesizes sequence numbers, sends each message as a UDP packet on
        │               localhost: [8-byte seq BE][2-byte len BE][body]. Separate process/thread.
        ▼
  PacketLossInjector  — receive-path filter; drops packets per policy (every-Nth, random-pct,
        │               burst-of-N) with a seeded RNG. Test-only, behind a CLI flag.
        ▼
  UDPReceiver         — recvfrom() loop; strips the sequence number; hands (seq, body, len) on.
        │
        ▼
  SequenceTracker     — tracks last-contiguous sequence; on a gap, buffers and requests the
        │               missing range over TCP; on fill, drains the buffer in order.
        ▼
  ReorderBuffer       — bounded (default 65536), keyed by sequence number; overflow → full resync.
        │
        ▼
  [Stage 1 pipeline unchanged] ITCHParser → MessageRouter → BookMap → OrderBook

  SequenceTracker ←── TCP ──→ RetransmissionServer  (separate process; reads the same file;
                                serves any sequence range from a pre-built seq→offset index)
```

**Component summary.** *UDPReplayer* walks the file and multicasts each message with a synthesized
MoldUDP64-style sequence number, since the historical file strips NASDAQ's real session framing.
*PacketLossInjector* is test infrastructure only — a seeded filter in the receive path so drop
sequences are reproducible and never compiled into a production build. *SequenceTracker* is the
core: it holds the last fully-contiguous sequence, processes in-order arrivals immediately, buffers
anything ahead of a gap, and requests the missing range. *ReorderBuffer* is bounded so sustained
loss can't leak memory without limit; on overflow it triggers a full resync. *RetransmissionServer*
answers gap requests over TCP from a startup-built sequence→byte-offset index; it handles one
client at a time (documented simplification — production would fan out to many).

**Why TCP for retransmission but UDP for the feed.** The main feed is UDP multicast because NASDAQ
broadcasts to thousands of subscribers at wire speed with zero per-subscriber overhead; a TCP
fan-out at that scale is infeasible and TCP head-of-line blocking would let one slow receiver stall
everyone. Retransmission requests are low-frequency, latency-tolerant, and need guaranteed delivery
— exactly TCP's sweet spot. Doing retransmission over UDP would mean reimplementing reliability by
hand, i.e. reinventing TCP. If a request or its response is lost, TCP recovers it for us.

---

## Order Book Data Structure

### Order lookup: `std::unordered_map<uint64_t, Order>`

Keyed by order reference number. Cancel, delete, replace, and execute messages all arrive by
order ID — without this map, finding an order to cancel would require scanning the price levels.
O(1) average-case for all four operations.

**Known limitation:** `std::unordered_map` heap-allocates one linked-list node per order
insertion. On the 30 Dec 2019 replay this produces 121 million allocations during regular
trading hours. The fix — an open-addressing flat hash map backed by a pre-allocated arena —
eliminates this entirely. Fix is documented and deferred post-Stage 2.

### Price levels: fixed array + overflow map (Option A)

```
bids_[4096]   — PriceLevel[i] where i = price - base_price + 2048
asks_[4096]   — same indexing, ask side

bid_overflow_ / ask_overflow_   — std::map<uint32_t, PriceLevel> for out-of-band prices
```

The fixed array is indexed by `(price - base_price + half_band)` where `base_price` is set on
the first order seen for that symbol. This gives O(1) price-level access — one arithmetic
computation, one array load, no pointer chasing.

`best_bid` and `best_ask` are maintained as tracked fields updated on every add. When the best
price level drains to zero shares, a rescan fires (O(array size) = O(4096)), but this is rare
enough in normal trading to be negligible amortised. (Honest caveat: this makes best-price access
O(1) amortised, not O(1) worst case.)

**Why not `std::map` for price levels?** Empirical evidence from the A/B benchmarks (warm 27K
order book, AAPL-scale):

| Operation | Array (current) | `std::map` price levels | Ratio |
|-----------|-----------------|------------------------|-------|
| Cancel    | 4.4 ns          | 9.2 ns                 | 2.1×  |
| Add       | 44.8 ns         | 43.2 ns                | ~1.0× |

Cancel isolates the price-level access cost because it makes no heap allocation. The 4.8 ns
gap is the cost of O(log 100) tree traversal + pointer chasing through scattered heap nodes
versus a direct array index. Add shows no meaningful delta because `unordered_map::emplace`
(~40 ns malloc) dominates both sides equally.

### Out-of-range price handling

A price-indexed array centered on the first observed price has an obvious failure mode: what
happens when a valid order arrives at a price outside the band? Stocks gap; a halt-and-reopen
can move price 10%+. Silent drops produce wrong EOD totals; an out-of-bounds index crashes.

**Decision: Option A — std::map overflow fallback.**

The fixed array covers ±2,048 ticks = ±$0.2048 around the initial price. Orders outside this
range go to `bid_overflow_` / `ask_overflow_` (one `std::map` per side). Overflow operations
are O(log n) and cache-unfriendly, but price dislocations are rare enough that the amortised
cost is acceptable. The book is always correct — no orders are dropped regardless of price.

This tradeoff is worth raising unprompted in interviews. "What happens when an order arrives
outside your array range?" should be answered before the interviewer asks it.

---

## Correctness Verification

Validated via NOII (Net Order Imbalance Indicator) closing-cross messages embedded in the
replay stream. Each closing NOII carries NASDAQ's own reference price for that symbol's closing
auction. Comparing this against the order book's best bid/ask at the moment of the message
gives a per-symbol in-stream oracle.

**30 Dec 2019 replay results:**
- Symbols with valid closing NOII: 2,328 of 8,191
- Median price deviation: **0.000%**
- Max deviation: 29.7% (TKKSW — warrant, thinly traded, expected outlier)

A 0.000% median means the book's best bid or ask exactly matches NASDAQ's reference price at
the time of the cross for the majority of symbols. This is the project's strongest correctness
claim — it's a verifiable fact, not a hope.

An end-of-day summary file was considered as an alternative oracle and abandoned: NASDAQ does not
publish a standalone EOD statistics file for free public download. The NOII reference price, which
is embedded in the replay stream itself, is the oracle used — and checking the book mid-session at
the cross is a stronger signal than a final-state comparison would be.

---

## Stage 2 Correctness Proof

Stage 2's correctness argument is exact equality of book state across a lossy path, not a
qualitative "it recovers."

```
Run A (baseline):  replay file → Stage 1 pipeline (no UDP, no loss)
                   → capture final per-symbol book state (shares_traded, active_orders,
                     best_bid, best_ask) at the 'M' System Event.

Run B (Stage 2):   same replay file → UDPReplayer → PacketLossInjector (1% random, seed 42)
                   → SequenceTracker → RetransmissionClient fills gaps → Stage 1 pipeline
                   → capture the same per-symbol book state at the 'M' System Event.

Assert:            Run A state == Run B state, symbol by symbol, field by field.
```

This is exact equality, not approximate matching. Either recovery is perfect or the test fails
naming the specific symbol and field that diverged. The random seed makes any failure reproducible
— same seed, same drop sequence, same failure, every time — which turns debugging a lossy
distributed-style path into replaying a deterministic script.

Loss scenarios exercised beyond the 1% baseline: 0.1% (light), 5% (heavy), a burst of 10
consecutive drops, a burst at the market-open boundary, and a burst during the high-volume open
period. Each must still produce Run A == Run B.

**Current status: proven exactly, at small-fixture scale, automated in `ctest`.** Six scenarios
(0%, 0.1%, 1% seed 42, 5%, burst-of-10, burst-at-open) run in `tests/net_test.cpp` — Run B drives
the actual `udp_replayer` / `retrans_server` / `pitchframe_net` binaries as subprocesses over real
localhost sockets, not re-implemented library calls, and every scenario asserts exact per-symbol
equality against Run A. All six pass. The full 268,744,780-message file through this same path is
a separate, not-yet-run manual step — the methodology, code, and seed are unchanged at that scale;
see Known Limitations for the one scale-dependent characteristic (synchronous-fetch throughput
under sustained loss) that full-file run would exercise but the fixture doesn't.

---

## Benchmarks

All Stage 1 benchmarks run on Apple M2 Air (8 GB RAM, L1d 128 KB/core, L2 16 MB shared, no L3),
Release build (-O2). Full results and methodology in [RESULTS.md](RESULTS.md). These are macOS
numbers and are labelled as such — Linux/x86 will differ, particularly for the allocator-bound
delete path.

Book-op benchmarks pre-populate with **27,000 resting orders** at AAPL-scale prices before
measuring. A benchmark on an empty book measures the wrong thing — hash map load and cache
pressure are completely different from real trading-hours state.

| Benchmark | CPU time | Notes |
|-----------|----------|-------|
| Parse 'A' message | 0.71 ns | 4× bswap+memcpy, all in L1 |
| Book add | 44.8 ns | malloc-dominated (`unordered_map::emplace`) |
| Book cancel | 4.4 ns | No alloc; map find + array update |
| Book execute (partial) | 4.4 ns | Same path as cancel |
| Book replace | 31.5 ns | Hot-allocator path (reuses just-freed node) |
| Book delete | 602 ns | Cold-allocator path on macOS malloc; ~5–10× lower on Linux |
| Full replay throughput | **1.81 M msg/s** | 268.7 M messages, streaming I/O included |

The 602 ns book delete is macOS malloc overhead for freeing long-lived heap nodes — the same
root cause as the 121M-alloc finding. On Linux/glibc this is ~50–100 ns. The pre-allocated
flat hash map fix eliminates it on any platform.

**Cache-miss counters** (`perf stat` L1/LLC misses) are not included: hardware PMU counters
require Linux on real hardware, and the M2/macOS setup does not expose them (Docker-on-Mac runs
Linux in a VM without PMU access). This is a stated limitation, not an omission — see RESULTS.md.

**Stage 2 throughput and recovery-path latency:** *[to be measured]* — no Stage 2 number will be
published here until it is measured on real hardware and its methodology can be defended.

---

## Known Limitations

**Hot-path heap allocation.** `std::unordered_map::emplace` allocates one node per Add Order.
121 million allocations during regular hours. This violates the zero-alloc goal and is the
primary optimisation target. Fix: open-addressing flat hash map with pre-allocated arena.
Deferred post-Stage 2.

**File-based in Stage 1; simulated UDP in Stage 2.** Stage 1 reads an ITCH replay file. Stage 2
replays that same file over localhost UDP — it does not connect to NASDAQ's live multicast feed,
which requires paid institutional access. Do not claim live-feed behaviour in interviews; claim
"I simulated the UDP feed and its loss characteristics using historical replay data," which is
what was built.

**Synthesized sequence numbers.** The historical file strips NASDAQ's MoldUDP64 session framing,
so Stage 2 synthesizes equivalent monotonic sequence numbers rather than using real ones. The
gap-detection logic is identical either way.

**Single-client retransmission server.** The Stage 2 retransmission server handles one connection
at a time. A production server would fan out to many concurrent subscribers. Documented
simplification.

**Synchronous retransmission fetch — feed ingestion and gap recovery share one thread.** When
`SequenceTracker` detects a gap, it blocks on the TCP round-trip to `RetransmissionServer` before
resuming. This makes gap-request dedup trivial by construction (only one fetch can ever be
outstanding), and it is correct — proven exactly via the Run A/B test suite. The precise cost: while
blocked, UDP packets keep arriving and the kernel receive buffer keeps filling; if it overruns
`SO_RCVBUF`, the kernel silently drops packets the loss injector never chose to drop — i.e. the
blocking itself can cause *secondary* loss on top of whatever was injected. This doesn't show up on
the small fixture the automated tests use; it can appear at full-file scale under sustained loss
(e.g. 5% random across all 268.7M messages), where every gap is a blocking round-trip. Mitigated by
a generous `SO_RCVBUF` (set and logged at startup) and a non-zero `--delay-us` on the replayer, but
not eliminated. A production version would decouple ingestion from recovery and keep draining UDP
into the reorder buffer while a fetch is outstanding (async fetch). This is also the interview
answer: it explains *why* the async design exists, not just that it would be faster.

**macOS benchmarks only.** Hardware PMU counters (`perf stat` cache miss rates) require Linux with
real hardware. The throughput and latency numbers are measured on macOS M2 — results on Linux x86
will differ.

**`best_bid`/`best_ask` rescan.** When the best price level drains to zero, an O(4096) array
scan fires. This is rare in practice (only when the last resting order at the touch is removed)
and negligible amortised, but it is not O(1) in the worst case.
