# Apex — Low-Latency Multi-Venue Trading Engine

C++20 engine that rebuilds order books from real NASDAQ ITCH 5.0 data and routes orders
across three venues to get the best total price. The venues are **simulated** by
splitting one real NASDAQ feed: each real order lives on exactly one venue, and each
venue has its own (assumed) fee and latency. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Status: step 3 of 7. The order book, ITCH 5.0 parser and venue simulator are done and
checked against a full real NASDAQ trading day.

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
