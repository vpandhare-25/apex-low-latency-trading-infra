# Apex — Low-Latency Multi-Venue Trading Engine

C++20 engine that rebuilds order books from real NASDAQ ITCH 5.0 data and routes orders
across three venues to get the best total price. The venues are **simulated** by
splitting one real NASDAQ feed: each real order lives on exactly one venue, and each
venue has its own (assumed) fee and latency. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Status: step 4 of 7 (with 4b). The order book, ITCH 5.0 parser, venue simulator, routers with
re-routing, and execution analytics are done and checked against a full real NASDAQ trading day.

## Build and test
```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
```
Requires CMake >= 3.20 and a C++20 compiler. GoogleTest downloads automatically.

Results will be added only from real benchmark runs, with hardware and dataset listed.

## Replay real NASDAQ data
NASDAQ publishes free full-day ITCH 5.0 sample files at
https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ (each is several GB). Put one in `data/` and run:
```bash
./build/apex_replay data/12302019.NASDAQ_ITCH50.gz AAPL 12:00   # time is optional: prints the book then
```
The `.gz` file is read directly, so there's no need to unzip it.

## Correctness check on real data (step 2)

Dataset: NASDAQ TotalView-ITCH 5.0, full trading day 2019-12-30
(`12302019.NASDAQ_ITCH50.gz`, 3.52 GB compressed, gzip CRC verified).
Machine: Apple M1, macOS, Apple clang 17, Release build, single thread.

| | AAPL | MSFT | SPY |
|---|---|---|---|
| Add orders | 698,744 | 576,026 | 991,829 |
| Executions | 60,543 | 35,315 | 42,635 |
| Partial cancels | 1,467 | 840 | 19,861 |
| Deletes | 654,457 | 548,858 | 960,499 |
| Replaces | 96,968 | 54,873 | 117,317 |
| **Rejected updates** (unknown order / too many shares) | **0** | **0** | **0** |
| **Crossed or locked book** after an update | **0** | **0** | **0** |
| Orders left at close (day orders expire, so 0 is correct) | 0 | 0 | 0 |

The file has 268,744,780 messages, and none had a bad length. AAPL's book at 12:00 had a
best bid of $290.45 and a best ask of $290.49.

A full replay took 38–45 s (about 6–7 million messages/s), but this is **not** a parser
benchmark: just decompressing the file (`gzip -t`) takes about 26 s on the same machine, so zlib
is most of the time. Parser and book speed will be measured on their own in step 5.

## Three simulated venues (step 3)

```bash
./build/apex_venues data/12302019.NASDAQ_ITCH50.gz AAPL
```

Same dataset and machine as above. The fees and latencies are assumptions:

| Venue | Share of real orders | Fee / share | Latency |
|---|---|---|---|
| A | 50% | $0.0030 | 50 µs |
| B | 20% | $0.0010 | 20 µs |
| C | 30% | $0.0020 | 1 ms |

The AAPL split came out as 349,556 / 139,831 / 209,357 orders, with 0 rejected updates.
At 12:00 the venues' best asks were $290.49 (A), $290.60 (B) and $290.50 (C), and the
best of the three matched the single real NASDAQ book.

**Quote probe:** every minute from 09:45 to 15:45 (361 times), we sent each venue a buy
for exactly its best ask size at exactly that price.

| | A (50 µs) | B (20 µs) | C (1 ms) |
|---|---|---|---|
| AAPL fully filled | 360/361 | 361/361 | 358/361 |
| MSFT fully filled | 361/361 | 361/361 | 358/361 |

At random moments, a quote seen 1 ms earlier is almost always still there, so latency
cost very little in this test. Latency hurts most when the market is moving, which these
evenly spaced probes mostly miss. The router experiments in step 4 should also measure
probes sent right after price changes.

## Router comparison (step 4)

```bash
./build/apex_route data/12302019.NASDAQ_ITCH50.gz AAPL      # optional 3rd arg: shares per order
```

Parent orders are 1,000 shares, with the limit 10 bps past the best opposite price. Between
09:45 and 15:45 they're sent on two schedules: every 30 s (alternating buy and sell), and
right after the best quote changes (at most once per 30 s). All three routers decide on the
same books at the same moment, so the comparison is paired. Cost is the all-in cost (price
plus fees) against the arrival mid, per filled share. Same dataset and machine as above.

**AAPL**

| Schedule | Router | Fill rate | Cost vs mid | Total cost |
|---|---|---|---|---|
| every 30 s (721 orders) | single-venue | 99.58% | 1.943 bps | $40,493 |
| | split | 99.92% | 1.173 bps | $24,526 |
| | split+fees | 99.90% | 1.169 bps | $24,431 |
| after quote change (700) | single-venue | 99.27% | 2.061 bps | $41,561 |
| | split | 96.64% | 1.201 bps | $23,588 |
| | split+fees | 96.52% | 1.195 bps | $23,438 |

**MSFT**

| Schedule | Router | Fill rate | Cost vs mid | Total cost |
|---|---|---|---|---|
| every 30 s (721 orders) | single-venue | 100.00% | 1.354 bps | $15,401 |
| | split | 99.96% | 0.872 bps | $9,911 |
| | split+fees | 99.97% | 0.853 bps | $9,695 |
| after quote change (649) | single-venue | 100.00% | 1.489 bps | $15,243 |
| | split | 95.94% | 0.991 bps | $9,734 |
| | split+fees | 94.97% | 0.971 bps | $9,436 |

What the numbers say:
- **Splitting across venues is where the savings come from.** It's 0.5 to 0.9 bps cheaper
  per filled share than the single-venue baseline, because it takes the best levels on every
  venue instead of walking deep into one venue's book.
- **Adding fees to the ranking helps only a little** here (0.004 to 0.02 bps). It does move
  shares to cheaper venues, and total fees fall about 5 to 13%.
- **The trade-off is fills right after quote changes.** The AAPL breakdown shows the missed
  shares are on the slow venue: C filled 90.6 to 91.9% of what was sent after quote changes,
  versus 99.8% on the 30 s schedule. Within its 1 ms latency, the quote the router saw is often
  gone. The single-venue router doesn't suffer as much because it sends its whole order with
  the 10 bps limit and keeps buying deeper. Step 4b below fixes this.

**Routing decision time** (AAPL run on its own, 1,421 decisions per router, one `route()`
call timed with `steady_clock`, in-process with no network):

| Router | p50 | p99 | p99.9 |
|---|---|---|---|
| single-venue | 334 ns | 1.3 µs | 15.4 µs |
| split | 5.3 µs | 21.6 µs | 135 µs |
| split+fees | 3.3 µs | 19.9 µs | 79.6 µs |

With about 1,400 samples, p99.9 rests on one or two data points, and the run shares a
thread with decompression, so the tail numbers are rough. When two replays ran at the same
time, the p99 values were up to 10 to 20 times higher. Step 5 measures latency on its own
with Google Benchmark.

## Re-routing unfilled shares (step 4b)

A fourth strategy, `split+fees+reroute`, uses the same fee-aware router but works each order
in **waves** (up to 3). After every child's fill report comes back, which takes one more
venue latency, any missing shares are routed again using the current books. Same run
settings as step 4, paired on the same parent orders:

| Schedule | Strategy | AAPL fill | AAPL cost | MSFT fill | MSFT cost |
|---|---|---|---|---|---|
| every 30 s | single-venue | 99.58% | 1.943 bps | 100.00% | 1.354 bps |
| | split+fees | 99.90% | 1.169 bps | 99.97% | 0.853 bps |
| | **split+fees+reroute** | **100.00%** | **1.168 bps** | **100.00%** | **0.853 bps** |
| after quote change | single-venue | 99.27% | 2.061 bps | 100.00% | 1.489 bps |
| | split+fees | 96.52% | 1.195 bps | 94.97% | 0.971 bps |
| | **split+fees+reroute** | **100.00%** | **1.190 bps** | **100.00%** | **0.965 bps** |

- **The fill-rate gap is closed** on both schedules and both stocks, and the cost per share
  stays 0.50 to 0.87 bps below the single-venue baseline.
- **Total cost rises a little** compared with `split+fees` (AAPL after quote changes:
  $23,438 to $24,172) only because more shares get bought. Per share, it's slightly cheaper.
- **Most orders need one wave:** 1.13 waves per parent on average for AAPL and 1.14 for MSFT,
  counting both schedules.
- **The trade-off is time.** In simulated time from decision to last execution, after quote
  changes:

  | | p50 | p99 |
  |---|---|---|
  | single-venue | 50 µs | 1.0 ms |
  | split+fees | 1.0 ms | 1.0 ms |
  | split+fees+reroute | 1.0 ms | 3.3 ms (AAPL), 3.5 ms (MSFT) |

  The split routers nearly always use slow venue C, so their median is its 1 ms latency. A
  second wave waits for C's report (another 1 ms) before it can be sent.

First-wave decision times in this run had p50 values of 1.6 to 4.0 µs for the split
strategies. These identical routers differ by about ±1 µs between strategies and between
runs, which shows how noisy in-process timing is. Step 5 benchmarks this properly.
