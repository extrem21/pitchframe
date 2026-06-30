# PitchFrame — Run Results Reference

Replay file: 12302019.NASDAQ_ITCH50 (NASDAQ ITCH 5.0, 30 Dec 2019)
Hardware: Apple M2 Air, 8GB RAM — L1i 192KB/core, L1d 128KB/core, L2 16MB shared, no L3
Build: clang++ -std=c++17 -O2 -fno-rtti -fno-exceptions (confirmed -O2, not CMake debug default)

---

## Message census (Milestone 2)

Total messages: 268,744,780

| Type | Count |
|------|-------|
| A (Add Order)              | 117,145,568 |
| D (Delete Order)           | 114,360,997 |
| U (Replace Order)          |  21,639,067 |
| I (NOII)                   |   4,024,315 |
| X (Cancel Order)           |   2,787,676 |
| L                          |     215,161 |
| P (Trade)                  |   1,218,602 |
| F (Add Order w/ MPID)      |   1,485,888 |
| E (Order Executed)         |   5,722,824 |
| C (Exec w/ Price)          |      99,917 |
| Q (Cross Trade)            |      17,836 |
| R (Stock Directory)        |       8,906 |
| Y                          |       9,013 |
| H                          |       8,966 |
| S (System Event)           |           6 |
| J                          |          34 |
| K                          |           3 |
| V                          |           1 |

Symbol table: 8,191 locate codes populated

---

## Throughput baseline (Milestone 3, pre-execution-handling)

Wall time:    1m 48.77s (~108.77s)
User time:    101.84s
System time:  4.52s
CPU util:     97% — workload is CPU-bound, not I/O-bound
Throughput:   ~2.48M msg/sec
Thread count: 1
Notes: E/C not yet handled; crossed books expected at this stage

---

## Shares traded (Milestone 4)

Grand total shares traded: 630,605,671

Top 10 by volume:

| Symbol | Shares Traded |
|--------|--------------|
| NIO    | 31,062,359 |
| ONTX   | 14,691,224 |
| SVRA   |  7,801,862 |
| GDX    |  7,157,510 |
| SPY    |  6,277,597 |
| AMD    |  6,189,903 |
| EEM    |  6,179,634 |
| FCEL   |  6,057,484 |
| AAPL   |  5,085,033 |
| EWZ    |  4,100,242 |

---

## Book snapshot at End of Market Hours (Milestone 4)

Taken at msg #265,920,782 ('M' — End of Market Hours). Sample of first 20 symbols:

| Symbol | Active Orders | Bid      | Ask      |
|--------|--------------|----------|----------|
| A      |          148 | $84.9000 | $84.9400 |
| AA     |          145 | $21.3300 | $21.3500 |
| AAAU   |           43 | $15.1100 | $15.7100 |
| AACG   |          144 |  $1.3800 |  $1.3900 |
| AADR   |           71 | $54.0400 | $54.5000 |
| AAL    |        4,190 | $28.2900 | $28.3000 |
| AAMC   |           56 | $10.6700 | $12.0600 |
| AAME   |          130 |  $1.8500 |  $1.9200 |
| AAN    |          104 | $56.8000 | $57.7100 |
| AAOI   |          660 | $11.6100 | $11.6300 |
| AAON   |          202 | $49.3800 | $49.4600 |
| AAP    |          129 |$159.1300 |$159.2800 |
| AAPL   |       27,106 |$291.6300 |$291.7300 |
| AAT    |           87 | $44.4600 | $46.0600 |
| AAU    |           76 |  $0.5444 |  $0.5750 |
| AAWW   |          325 | $26.5800 | $26.5900 |
| AAXJ   |          228 | $73.2200 | $73.2300 |
| AAXN   |          824 | $73.4800 | $73.5200 |
| AB     |           91 | $29.5800 | $30.8300 |
| ABB    |          141 | $24.0100 | $24.0200 |

All books empty at true end-of-replay — exchange cancels all outstanding orders after 'M'.

---

## NOII Closing Cross Validation (Milestone 5)

Symbols with valid closing NOII: 2,328 / 8,191
Median price deviation: 0.0000%
Max price deviation: 29.7405% (TKKSW — warrant, thinly traded, expected outlier)

Top 10 by volume:

| Symbol | NASDAQ Ref  | Book Bid    | Book Ask    | Diff ($) | Diff (%) |
|--------|-------------|-------------|-------------|----------|----------|
| NIO    | —           | —           | —           | no closing NOII |
| ONTX   | $0.4482     | $0.4400     | $0.4482     | $0.0000  | 0.0000%  |
| SVRA   | $4.9300     | $4.9000     | $4.9400     | $0.0100  | 0.2028%  |
| GDX    | $29.4800    | $29.4800    | $29.4900    | $0.0000  | 0.0000%  |
| SPY    | —           | —           | —           | no closing NOII |
| AMD    | $45.5100    | $45.5000    | $45.5100    | $0.0000  | 0.0000%  |
| EEM    | $44.7600    | $44.7600    | $44.7700    | $0.0000  | 0.0000%  |
| FCEL   | $1.6800     | $1.6700     | $1.6800     | $0.0000  | 0.0000%  |
| AAPL   | $291.7100   | $291.7100   | $291.7600   | $0.0000  | 0.0000%  |
| EWZ    | $47.2100    | $47.1900    | $47.2100    | $0.0000  | 0.0000%  |

**AAPL timing:** Both bid/ask snapshot and NOII reference price are captured in the same
'I' handler call — same message, same point in the replay stream (15:59:59). There is no
timing mismatch between the two.

**AAPL $59.17 bid (corrected):** The earlier run showing bid=$59.17 was a bug, not market
data. The 'I' handler wrote `closing_ref[msg.locate]` without a bounds check. Locate codes
> 8191 appear in the NOII stream but exceed k_max_locate (8192). Due to memory layout
(closing_ref[] immediately precedes closing_bid[] in BSS), `closing_ref[8205] = ref_price`
overwrote closing_bid[13] (AAPL's slot). A symbol with locate 8205 and NOII reference
price $59.17 repeatedly stomped on AAPL's stored best_bid.
Fix: gate the entire closing-cross block with `msg.locate < k_max_locate` — consistent with
how SymbolTable::insert() and BookMap::get() already reject out-of-range locates.
After the fix, AAPL correctly shows bid=$291.71 = ref price, 0.0000% deviation.

---

## Allocation counter (Milestone 6)

Build: clang++ -std=c++17 -O2 -DPITCHFRAME_COUNT_ALLOCS

| Phase                          | Heap allocations |
|-------------------------------|-----------------|
| Pre-market (loop start → Q)   |     2,973,779   |
| Regular hours (Q → M)         |   121,375,825   |
| Post-market (M → end)         |       560,965   |

Root cause: `std::unordered_map::emplace` allocates one linked-list node per
Add Order. 121M allocs ≈ 117M A messages + 4M F messages — confirms one
allocation per order insertion.
Fix: replace with open-addressing flat hash map backed by pre-allocated arena.
