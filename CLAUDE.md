# PitchFrame — NASDAQ ITCH Feed Handler

## What This Project Is

A low-latency market data processing engine in C++17 modeled after the feed handler component
found inside real HFT firms. A feed handler is the first component in the HFT pipeline — it
receives raw binary packets from a stock exchange, decodes them, and maintains an internal copy
of the exchange's order book in real time.

The exchange used is NASDAQ. NASDAQ publicly publishes the ITCH 5.0 binary protocol spec and
historical replay files of real trading days. Correctness is verified against NASDAQ's own
published end-of-day summary statistics — not synthetic test data.

This is a portfolio project targeting FAANG and adjacent product-based company SWE fresher
roles. Every design decision must be defensible under rigorous technical questioning from a
senior engineer. When in doubt, prioritise correctness over cleverness.

**Honesty policy for this project.** This is a learning project built by one new grad, not a
production system. Where something is a deliberate simplification versus what a real HFT firm
does, say so — in the README and in interviews. "I simplified X because Y, and here's what a
production version would do instead" is a stronger answer than pretending the simplification
isn't there. Interviewers trust visible scope limits more than suspicious polish.

---

## Current Stage

**STAGE 1: ITCH Feed Handler + Order Book Reconstruction**

Do not move to Stage 2 until all Stage 1 done conditions are met and benchmarks are recorded.

---

## Project Stages Overview

- **Stage 1** — ITCH 5.0 binary parser + order book reconstruction on real NASDAQ replay data
- **Stage 2** — Gap detection, reorder buffer, TCP retransmission recovery engine
- **Stage 3** — Lock-free Disruptor ring buffer for inter-thread messaging

---

## Build Commands

```bash
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja

# Run tests
ctest --output-on-failure

# Run benchmarks
./bench/itch_bench

# Run with NASDAQ replay file
./pitchframe <path-to-itch-replay-file>
```

---

## Architecture (Stage 1)

Stage 1 reads from a **file**, not a network socket. This is deliberate: the goal of Stage 1 is
to nail parser and order-book correctness against the NASDAQ oracle, with zero transport
complexity in the way. The real exchange feed is UDP multicast, and the gap/recovery problems
that come with UDP are the entire subject of Stage 2. Do not claim any UDP/TCP transport
behaviour in Stage 1 — there is none yet.

```
NASDAQ ITCH replay file (.bin)
        |
        v
  FileReader (reads raw bytes, handles 2-byte length framing, feeds buffer)
        |
        v
  ITCHParser (zero-copy binary decode, big-endian to host byte order)
        |
        v
  MessageRouter (dispatches by message type; routes by stock locate code)
        |
        v
  OrderBook (per-symbol, HashMap + price-level array)
        |
        v
  Validator (end-of-day state vs NASDAQ published stats)
```

---

## Key Design Decisions — Know These Cold

**Zero dynamic allocation in the hot path.**
No malloc, no new, no std::vector resizing during parsing or book update. All memory is
pre-allocated at startup. Reason: heap allocation has **unpredictable tail latency**. A malloc
that hits the allocator's fast path is tens of nanoseconds, but the slow path may call
mmap/sbrk — a syscall (mode transition into the kernel) — and the first touch of freshly
mapped memory takes a page fault. In HFT the enemy is not mean latency, it's the **tail**: the
occasional 10µs spike when you needed 100ns. Pre-allocation removes that variance entirely.
(Note: the cost is a possible syscall + page fault, NOT a context switch — be precise about the
mechanism if asked.) Enforce with a custom allocation counter in debug builds; assert it stays
zero during replay.

**Zero-copy parsing.**
The parser operates directly on the buffer containing raw packet bytes. No intermediate copy of
fields into a separate struct before processing. Reason: the bytes are already in L1 from the
read, so copying them is **redundant work that doubles the cache footprint** of the same data —
you touch extra cache lines and pay a store-then-reload round trip for no benefit. (Be careful:
do NOT say "a copy is a cache miss" — a copy of data already in cache is not a miss, it's just
wasted cycles and L1 bandwidth. The honest argument is redundancy and footprint, not misses.)

**BSWAP over ntohl.**
ITCH fields are big-endian; x86 is little-endian. Use __builtin_bswap16/32/64 for byte swapping,
not ntohl/ntohs. Reason: these compile to a single BSWAP instruction. ntohl may go through a
function call and may not inline depending on the platform/headers. (Honest caveat: on many
modern systems ntohl is itself a builtin/macro that also lowers to BSWAP — so the real, safe
claim is "I used the intrinsic to *guarantee* a single instruction with no function-call
ambiguity," not "ntohl is always slow." Say it that way.)

**Prices are integers, never floats.**
ITCH prices are fixed-point: the price field is an unsigned integer in units of $0.0001 (4
implied decimal places). Keep them as integers throughout. Never use a double for price — it
introduces rounding error and is an instant red flag in an HFT screen. Bonus: integer prices in
fixed tick units are exactly what makes the price-indexed array (below) work as a direct index.

**Order book data structure: HashMap + price-level array.**
Two data structures kept in sync:
- std::unordered_map<uint64_t, Order> keyed by order reference number — O(1) cancel and modify
  by order ID. (Cancels/deletes/executions arrive by order ID, not by price, so you need this.)
- Per-side price-level array indexed by price tick, bounded to a realistic tick range around
  the mid price — O(1) insert at known price, O(1) best bid/ask access.

Why not std::map: tree nodes are scattered in memory, every operation is a pointer-chase that's
likely a cache miss. Why not flat array only: cancels arrive by order ID, not price, so you'd
have no way to find the order in O(1) without the hashmap.

**Out-of-range price handling — DECIDE THIS EXPLICITLY (this is the real hole).**
A price-indexed array bounded to a tick range around the mid has an obvious failure mode: what
happens when a valid order arrives at a price *outside* the bounded range? Stocks gap; a
halt-and-reopen can move price 10%+. If you silently drop such orders, your EOD totals will not
match the oracle (a correctness failure). If you index out of bounds, you crash. You must pick a
strategy and document it:
  - **Option A — hashmap fallback:** price-level array for the dense region near the touch, plus
    a std::map<price, level> (or hashmap) for outlier prices. Tradeoff: outlier ops are O(log n)
    / cache-unfriendly, but outliers are rare so amortised cost is fine. Most honest + robust.
  - **Option B — sparse hashmap for the whole book keyed by price:** simpler, uniform, no bounds
    problem. Tradeoff: loses the O(1) cache-friendly best-bid/ask that the array gives you.
  - **Option C — dynamically re-center/resize the window:** complex, and resizing means
    allocation, which violates the zero-alloc hot-path rule. Avoid unless you really want it.
Recommended for this project: **Option A**. Implement it, and in interviews *raise this failure
mode yourself before they ask* — proactively flagging the gap reads as senior, not junior.

**Network byte order fields.**
ITCH uses big-endian for all multi-byte fields. Convert immediately on read, not lazily. Reason:
converting lazily means carrying raw big-endian bytes through your system and risking a bug
where you forget to convert before use. Convert once, at the boundary.

---

## ITCH 5.0 Message Types to Handle

The spec is at:
https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf

- **S** — System Event (marks market open/close, use to bound replay)
- **R** — Stock Directory (maps stock locate code to symbol — build the locate→symbol lookup
  table at startup; you literally cannot identify which symbol a message belongs to without it,
  so this is required in Stage 1, not optional)
- **A** — Add Order (no MPID)
- **F** — Add Order with MPID
- **E** — Order Executed (reduces a resting order's shares — **mutates the book**)
- **C** — Order Executed with Price (also mutates the book; non-displayable price)
- **X** — Order Cancel (partial)
- **D** — Order Delete (full)
- **U** — Order Replace
- **P** — Trade (non-cross)
- **Q** — Cross Trade
- **B** — Broken Trade
- **I** — NOII (Net Order Imbalance Indicator)

Bring-up order: start with **A, D, X, U** to get the book mutating correctly, then add **E and
C** before you claim Stage 1 correctness. E and C are executions — they reduce resting order
size and contribute to shares-traded totals. If you skip them, your EOD shares-traded numbers
will not match the oracle and your "correctness" is fake. (R and S you need from day one for
symbol identity and replay bounds.)

---

## Correctness Verification

NASDAQ publishes end-of-day summary files alongside replay files. After replaying a full trading
day, your order book state for each symbol must match these:

- Total shares traded per symbol (requires E and C to be implemented — see above)
- Final bid/ask price at close
- Total number of add/cancel/delete messages processed

If these don't match, there is a bug in your parser or book logic. Do not move to benchmarking
until correctness is confirmed on at least 10 symbols.

This external oracle is the single strongest part of the project — it's what makes "my book is
correct" a verifiable claim instead of a hope. Lead with it when you present the project.

---

## Stage 1 Done Conditions

All of these must be true before moving to Stage 2:

- [ ] Parser correctly decodes all listed ITCH message types without error across a full trading
      day, including E and C
- [ ] Order book end-of-day state matches NASDAQ published stats for at least 10 symbols,
      including shares-traded totals (which depend on E/C)
- [ ] Out-of-range price strategy implemented and documented (Option A/B/C above)
- [ ] Zero dynamic allocation in the hot path — verified with allocation counter
- [ ] Benchmarks measured and recorded in README (see below — measure, don't target)
- [ ] README documents order book data structure choice with explicit tradeoff explanation,
      including the out-of-range-price decision

---

## Stage 1 Benchmarking — Measure, Don't Target

**Important framing change:** do not chase pre-set numbers. Measure what your code actually does
on your actual hardware, report it honestly, and be able to explain the methodology. A real,
modest, correctly-measured number beats an impressive number you can't defend. If an interviewer
pokes at your methodology and it holds up, that's worth more than the number itself.

Run all benchmarks on Linux for consistency. Record the exact hardware spec (CPU model, L1/L2/L3
cache sizes, RAM) alongside every number. Never put a number in the README before measuring it.

**Methodology rule for sub-microsecond timing (read this twice).** You cannot reliably time a
single ~100ns operation with a timer (clock_gettime/rdtsc) whose own overhead is ~20–40ns — your
measurement error would be 20–40%. The defensible method is: time a **batch** of N messages
(N large, e.g. 10^6) with one timer call before and after, then divide by N. Use
benchmark::DoNotOptimize() / ClobberMemory() so the optimiser can't delete the work. If asked
"how did you measure per-message latency," the correct answer is "amortised over a large batch,
because per-call timer overhead dominates at this scale" — NOT "I timed each message."

Also note: throughput (msg/sec) and amortised per-message latency are the *same measurement*
expressed two ways (10M msg/sec ⇔ 100ns/msg). Don't present them as two independent
achievements — an interviewer will notice the double-count. Report throughput as the headline;
mention per-message as its reciprocal if useful.

### 1. Message Throughput
**What:** Total ITCH messages processed per second across a full trading day replay file
(end-to-end: read → parse → book update).
**How:** Wrap the full replay loop in one std::chrono::high_resolution_clock interval. Divide
total message count by elapsed time. Exclude file I/O from the timed region if you want to
isolate compute (load the file into memory first), and say which one you measured.
**What to report:** the number you actually got, plus whether I/O was included.

### 2. Per-Message Decode Latency (amortised — see methodology rule)
**What:** Average time from start of message decode to book update complete, amortised over a
large batch. Optionally p99 if you collect a per-batch distribution, but be honest that
single-message p99 at this scale is hard to measure cleanly.
**How:** Google Benchmark over a pre-loaded buffer of ITCH messages in a tight loop, with
DoNotOptimize. Report average. Only claim p99 if you genuinely measured a distribution and can
explain how.
**Honesty note:** if you can't cleanly measure p99 at sub-µs scale, say so. "I measured average
amortised latency; clean p99 at this scale needs more careful instrumentation than I built" is a
perfectly respectable new-grad answer and far better than a p99 you can't defend.

### 3. Cache Miss Rate
**What:** L1 data cache misses and last-level cache load misses during replay.
**How (Linux, bare metal or VPS — see footgun below):**
```bash
perf stat -e L1-dcache-load-misses,LLC-load-misses ./pitchframe <replay-file>
```
**FOOTGUN — read before you plan benchmarks:** Docker Desktop on macOS runs Linux inside a VM,
and the virtualised CPU usually does **not** expose hardware PMU counters — `perf stat` for
cache events will often return `<not supported>` or zeros. You very likely need a **real Linux
box or a cheap bare-metal/Linux VPS** for the cache-miss numbers. Functional benchmarks
(throughput, latency) work fine in Docker; hardware counters frequently don't. Find this out
early so it doesn't ambush your headline benchmark. Also note event names vary across
CPUs/kernels — `L1-dcache-load-misses` vs `L1-dcache-misses` etc. — run `perf list` to see what
your machine actually exposes.
**What to report:** raw miss counts and misses-per-message (divide by total message count).
**What to compare against:** run the same replay with a std::map order book instead of your
HashMap + array. The delta is the empirical justification for your structure choice. This A/B is
worth more than the absolute numbers.

### 4. Order Book Operation Microbenchmarks
**What:** Isolated latency of add, cancel, delete, replace on the order book data structure.
**How:** Google Benchmark, separate benchmark per operation, book pre-populated with realistic
state before measuring (cold vs warm matters — say which).
**What to report:** measured per-op latency. No pre-set target — report reality.

### Benchmark Command Template
```bash
# Build in release mode — never benchmark debug builds
cmake .. -DCMAKE_BUILD_TYPE=Release

# Run Google Benchmark
./bench/itch_bench --benchmark_format=json --benchmark_out=results.json

# Cache miss measurement (Linux with real PMU access — see footgun above)
perf stat -e L1-dcache-load-misses,LLC-load-misses,cache-misses \
    --repeat 5 \
    ./pitchframe <replay-file> 2>&1 | tee perf_results.txt
```

### Resume lines
Write the resume line **after** you have the number, using your real measured figure and the
hardware it was measured on. Don't pre-write resume claims here — that's how you end up defending
a number you never actually hit.

---

## Coding Conventions

- C++17 throughout. No C++20 features until Stage 3 (coroutines not needed; concepts fine if you
  want them).
- No exceptions in the hot path. Compile with -fno-exceptions there if practical. Reason (be
  precise — the old "exceptions cost on the happy path" claim is mostly false on modern
  zero-cost/Itanium-ABI EH): the happy path is genuinely ~free, but (a) the **throwing** path is
  unbounded, unpredictable latency, which is unacceptable in a hot loop, and (b) exception
  control flow is opaque and hard to reason about for correctness. So the honest reason is
  "unbounded tail latency on throw + control-flow clarity," not "they're slow when nothing
  throws." Use return codes / std::optional-style error handling.
- No RTTI. Compile with -fno-rtti. Not needed here.
- Compile flags: -O2 -Wall -Wextra -Wshadow -fno-rtti in release. Add -fsanitize=address
  -fsanitize=undefined in debug builds.
- Header files go in include/pitchframe/. Source files go in src/. Benchmarks go in bench/. Tests
  go in tests/.
- Every file that touches the hot path gets a comment at the top: // HOT PATH — no allocation,
  no exceptions, no virtual dispatch.
- Naming: snake_case for everything. Types are PascalCase. Constants are k_prefixed snake_case.

---

## Directory Structure

```
pitchframe/
├── CLAUDE.md                   <- This file
├── CMakeLists.txt
├── README.md
├── data/                       <- NASDAQ replay files go here (gitignored)
├── include/
│   └── pitchframe/
│       ├── parser/
│       │   └── itch_parser.h
│       ├── book/
│       │   ├── order_book.h
│       │   └── price_level.h
│       ├── common/
│       │   └── types.h
│       └── validator/
│           └── validator.h
├── src/
│   ├── parser/
│   ├── book/
│   └── validator/
├── bench/
│   └── itch_bench.cpp
└── tests/
    ├── parser_test.cpp
    └── book_test.cpp
```

---

## What NOT to Do

- Do not use any existing ITCH parsing library. The parser is the point.
- Do not use RocksDB, LevelDB, or any storage engine. No persistence in Stage 1.
- Do not use Boost. Standard library only plus Google Benchmark and Google Test.
- Do not benchmark in debug builds.
- Do not put a number in the README before measuring it on your own hardware.
- Do not claim a benchmark methodology you didn't actually use (especially per-message p99).
- Do not use floats for price.
- Do not move to the next stage until all done conditions for the current stage are checked off.

---

## NASDAQ Data

Download historical ITCH replay files from:
https://emi.nasdaq.com/ITCH/  (also linked from the NASDAQ TotalView-ITCH product page; the
exact host has changed over time, so if a link 404s, search "NASDAQ ITCH historical sample
data" and verify you're on a nasdaq.com domain)

Files are in .gz format. Decompress before use. Pick any recent trading day. Full-day files are
large (often several GB uncompressed); a single-symbol or partial sample is fine for early
bring-up — you don't need a full day until correctness validation.

The file format is a raw binary stream. Each message is preceded by a 2-byte big-endian length
field, then the message type byte, then the message fields per the spec. Your file reader must
handle this framing.

---

## Interview Questions This Project Must Prepare You to Answer

Know the answers from first principles, not memorisation. Where the honest answer includes a
caveat or a simplification you made, give it — that's the strong move, not a weakness.

**Parser:**
- Why UDP multicast instead of TCP for a *real* market data feed? (And: note Stage 1 reads a
  file, so this is about the system you're modelling, not what you built yet.)
- Why is dynamic allocation in the hot path a problem — what specifically makes it slow?
  (Answer: tail-latency variance from possible syscall + page fault, not "context switch.")
- Why the BSWAP intrinsic over ntohl? (Answer: guarantee a single instruction with no
  function-call ambiguity — and note ntohl may itself lower to BSWAP on many systems.)
- What is zero-copy and why does it matter? (Answer: avoid redundant work + extra cache
  footprint — NOT "a copy is a cache miss.")

**Order book:**
- Walk me through your order book data structure. Why this over alternatives?
- Time complexity of add, cancel, delete, replace?
- Why is std::map bad here specifically? (cache-unfriendly pointer chasing)
- What happens when an order arrives at a price outside your array's range? (You should *raise*
  this yourself — see out-of-range design decision.)
- How do you handle an Order Replace / Cancel that references an order ID that doesn't exist?

**Benchmarking:**
- How did you measure per-message latency? (Answer: amortised over a large batch, because timer
  overhead dominates a single sub-µs op.)
- Isn't your throughput just the reciprocal of your per-message latency? (Yes — acknowledge it,
  don't present them as two wins.)
- What does perf stat measure and how? Why might it not work in Docker-on-Mac?
- What is a cache miss and why does it matter for performance?
- What is false sharing? (Preview for Stage 3 — read about it now.)

**Correctness:**
- How do you know your order book is correct? (External NASDAQ EOD oracle — lead with this.)
- What does your oracle actually check, and what would it miss?
