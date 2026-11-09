# Detailed results

Step-by-step measurements behind the main [README](../README.md), in the order they were
built. Every number comes from a real run of the tools named in each section. Dataset:
NASDAQ TotalView-ITCH 5.0, 2019-12-30. Machine: Apple M1, 8 GB, macOS 26.5, Apple clang 17,
Release build. The raw numbers used for the charts are in [`results/`](../results/).

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

## Benchmarks (step 5)

```bash
./build/apex_extract data/12302019.NASDAQ_ITCH50.gz AAPL data/AAPL.itch   # once, about 40 s
./build/apex_bench   data/AAPL.itch --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
./build/apex_latency data/AAPL.itch 3
./build/apex_depth   data/AAPL.itch
```

Input: all 1,512,179 AAPL book events from 2019-12-30, held in memory, so no disk reads or
decompression are included. Machine: Apple M1 (8 cores, 8 GB), macOS 26.5, Apple clang 17,
Release (`-O3`), single thread. The 1-minute load average was about 4 to 5 during the runs,
mostly a macOS Bluetooth service using one core. Values are medians of 10 repetitions, with
the coefficient of variation (CV) under 3% except where noted.

**Throughput (Google Benchmark)**

| Benchmark | Time for the whole day | Per event | Rate |
|---|---|---|---|
| Parse ITCH (decode only) | 13.9 ms | 9.2 ns | 108.6 M msg/s (2.87 GiB/s) |
| `OrderBook` replay (`std::map`) | 212 ms (CV 9%) | 140 ns | 7.13 M/s |
| `FlatOrderBook` replay | 213 ms | 141 ns | 7.12 M/s |
| `FastOrderBook` replay | **136 ms** | **90 ns** | **11.16 M/s** |
| `FastOrderBook` + read best bid/ask after every event | 138 ms | 91 ns | 10.96 M/s |
| Hash-map work alone (`std::unordered_map`, no levels) | 102 ms | 67 ns | 14.8 M/s |

- **The hash map was the bottleneck, not the price levels.** Swapping only the levels to a
  sorted vector (`FlatOrderBook`) changed nothing. The `unordered_map` work alone takes
  102 ms of the 212 ms replay. Replacing it with an open-addressing table
  (`FastOrderBook`) makes replay **1.56x faster** than the `std::map` baseline.
- **Parsing is cheap** at 9 ns per message. The 40 s full-file replay in step 2 was almost
  all decompression.

**Per-update latency** (`apex_latency`, every update timed with `steady_clock`, 3 rounds
that agreed within one clock tick; values in ns):

| Book | p50 | p90 | p99 | p99.9 | p99.99 |
|---|---|---|---|---|---|
| `OrderBook` | 125 | 208 | **334** | **500** | 1,084–1,292 |
| `FlatOrderBook` | 84 | 208 | 792 | 1,000–1,042 | 1,500–1,792 |
| `FastOrderBook` | **42** | **125** | 708–709 | 958–959 | 1,375–1,458 |

The clock on this machine ticks every 41 ns, and reading it twice costs about one tick, so
the p50 values are only accurate to about ±41 ns. 42 ns means one tick.

- **The flat books win the typical case but lose the tail.** `FastOrderBook` is 3x faster at
  p50 and about 2x slower at p99. `apex_depth` explains why: the AAPL book reached
  **4,227 bid levels** (MSFT: 2,969). In a sorted vector with the best price at the back,
  creating a level shifts every better-priced level. That's under 10 for 84% of new AAPL
  levels, but 100 or more for 5.8% of them, and 1,000 or more for 2.5% (MSFT: 12.6% and
  6.2%). Those rare large memory moves are the p99. The tree pays O(log n) every time,
  so its tail is flatter.
- **Correctness on real data:** across all 4.86 M AAPL, MSFT and SPY updates, the three books
  report the same best bid and ask after every update, with 0 rejected updates.

**Routing decision** (`BM_Route`, venue books as of 12:00, buy order, median of 10):

| Order size | single-venue | split | split+fees |
|---|---|---|---|
| 100 shares | 52 ns | 290 ns | 291 ns |
| 1,000 shares | 52 ns | 586 ns | 598 ns |
| 10,000 shares | 52 ns | 1,047 ns | 1,074 ns |

The split routers cost more for bigger orders because they read more levels. The
in-process timings in step 4 (about 2 to 5 µs) were inflated by timer overhead, a cold cache
and the decompression running in the same thread, which is why benchmarks run on their own.

## Hybrid order book (step 5b)

The flat books were fast in the typical case, but deep inserts made their p99 slow.
`HybridOrderBook` keeps only the **best 16 levels** of each side in a sorted vector, puts
deeper levels in a `std::map`, and uses the flat hash map for orders. So a vector shift is
never more than 16 levels.

Same machine, data and method as step 5 (load average about 4.5). Per-update latency in ns
on AAPL, ranges across 3 rounds:

| Book | p50 | p90 | p99 | p99.9 | p99.99 |
|---|---|---|---|---|---|
| `OrderBook` (`std::map`) | 125 | 208–209 | 375 | 500–542 | 1,042–1,166 |
| `FastOrderBook` | 42 | 125 | 708 | 958 | 1,334–1,459 |
| Hybrid N=16 | **42** | **125** | **250** | **416–417** | **833–834** |
| Hybrid N=64 | 42 | 166–167 | 250–292 | 416–583 | 791–15,166 |
| Hybrid N=256 | 42 | 208–209 | 292–375 | 500–625 | 834–10,333 |

MSFT and SPY (1 round each), ns:

| | MSFT p50 / p99 / p99.9 | SPY p50 / p99 / p99.9 |
|---|---|---|
| `OrderBook` | 84 / 333 / 500 | 83 / 250 / 333 |
| `FastOrderBook` | 42 / 417 / 750 | 42 / 250 / 334 |
| Hybrid N=16 | **42 / 250 / 416** | **42 / 166 / 333** |

Throughput (Google Benchmark, AAPL whole day, median of 10):

| Book | Replay | Replay + best bid/ask each event |
|---|---|---|
| `OrderBook` | 208 ms (7.28 M/s) | 214 ms |
| `FastOrderBook` | 136 ms (11.18 M/s) | 139 ms |
| Hybrid N=16 | **108 ms (14.08 M/s)** | **110 ms** |
| Hybrid N=64 | 110 ms (CV 11%) | — |
| Hybrid N=256 | 133 ms | — |

- **N=16 is best or tied at every percentile on all three stocks**, and **1.93x the
  throughput** of the `std::map` book (1.26x `FastOrderBook`). The rare deep shifts were
  costing throughput as well as tail latency.
- **A smaller hot size is better here.** Larger N makes p90 grow (125 → 166 → 208 ns),
  because each shift can move more levels. The 10–15 µs p99.99 values for N=64 and 256
  appeared in only one round each, so they're likely system noise, not the data structure.
- **Correctness:** across all AAPL, MSFT and SPY updates, every book reports the same best
  bid and ask after every update. At 12:00, all 4,720 AAPL levels (MSFT 3,531, SPY 732) are
  identical in all four books.

## Multithreaded pipeline (step 6)

```bash
./build/apex_pipeline data/12302019.NASDAQ_ITCH50.gz 3     # 3 rounds of every layout
```

This builds the order book of **every stock** (8,892 of them) from the full 3.5 GB `.gz`
file: 268.7 M messages and 263.2 M book updates. The three stages are gunzip, parse and
books, connected by lock-free single-producer/single-consumer queues. The layouts below run
the same code with 1 to 5 threads. With several book threads, each stock always goes to the
same thread (`locate % N`).

Same M1 machine (4 performance + 4 efficiency cores; macOS decides which core runs each
thread). The load average was 4.75 to 6.9 during the run. Times are the median of 3 rounds,
with the range in brackets:

| Threads | Layout | Time | Speedup |
|---|---|---|---|
| 1 | [gunzip+parse+books] | 67.9 s (67.8–77.7) | 1.00x |
| 2 | [gunzip] → [parse+books] | 55.7 s (53.1–59.3) | 1.22x |
| 2 | [gunzip+parse] → [books] | 59.9 s (54.6–60.7) | 1.13x |
| 3 | [gunzip] → [parse] → [books] | 71.5 s (65.0–72.6) | 0.95x |
| 3 | [gunzip+parse] → [books ×2] | 44.2 s (36.3–56.4) | 1.54x |
| 4 | [gunzip] → [parse] → [books ×2] | 49.4 s (42.9–55.1) | 1.37x |
| 5 | [gunzip] → [parse] → [books ×3] | **39.3 s (34.7–41.1)** | **1.73x** |

Every run of every layout had **0 rejected updates, 8,892 books, and the same checksum** of
every stock's best bid and ask after every update.

**Where the time goes.** Each thread records how long it waited on its queues (round 1):

| Layout | Waiting time per thread |
|---|---|
| [gunzip] → [parse+books] | gunzip 34.3 s, parse+books **0.1 s** |
| [gunzip+parse] → [books] | gunzip+parse 29.4 s, books **0.1 s** |
| [gunzip] → [parse] → [books ×3] | gunzip 4.4 s, parse 17.0 s, books 5.7 / 2.3 / 0.5 s |

- **The bottleneck is building the books.** Whenever books share one thread, that thread
  never waits, and it takes about 55 to 60 s by itself. In the AAPL-only benchmark an update
  took about 70 ns. Here it's about 210 ns, because 8,892 books don't fit in the CPU cache,
  so most updates touch memory that isn't cached.
- **A pipeline is only as fast as its slowest stage.** Moving gunzip or parse to their own
  threads overlaps their work with the books, which helps at most about 1.2x. Splitting
  parse from gunzip while the books stay on one thread made things slightly slower
  (0.95x): the books thread is still the limit, and the extra thread only adds hand-off
  work.
- **Splitting the bottleneck stage is what pays off.** Sharding the books across 3 threads
  gives the best result: 1.73x median, 1.96x best. With 3 book threads, the books are no
  longer the only limit (gunzip waits only 4.4 s), and the shards are uneven: shard 3
  waits 0.5 s versus 5.7 s for shard 1, because `locate % 3` doesn't balance busy stocks.
- **Why it isn't 3 to 5x:** the M1 has only 4 performance cores, so a 5-thread run has at
  least one thread on a slow core, and every thread competes for the same memory bandwidth.
  Run-to-run variation is up to about ±15%, which is why every layout ran 3 times. (One
  earlier single run of [gunzip] → [parse+books] took 90.6 s and didn't repeat.)

**Queue hand-off latency** (parser push to book applied, sampled 1 in 1,024 events): p50 is
0.9 to 18 ms. That isn't per-message processing time. The replay pushes data as fast as it
can, so the bottleneck stage's queue (65,536 events) stays nearly full, and each event waits
behind about 65 K others. That's Little's law: wait = queue length × service time. A live
feed arrives far slower than this replay, and the queues would be mostly empty. With
3 book threads the queues drain faster, and p50 falls to 0.9–2.2 ms.
