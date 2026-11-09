# Measured results

These CSV files hold the numbers reported in the main README, copied from the output of
the Apex tools. Every number comes from a real run; none are estimated.

Dataset: NASDAQ TotalView-ITCH 5.0, 2019-12-30 (`12302019.NASDAQ_ITCH50.gz`).
Machine: Apple M1 (4 performance + 4 efficiency cores, 8 GB), macOS 26.5, Apple clang 17,
Release build. Runs were made in November 2026 with the 1-minute load average about 4 to 7.

| File | Produced by | Notes |
|---|---|---|
| `book_latency_aapl.csv` | `apex_latency data/AAPL.itch 3` | ns per update; median of 3 rounds per percentile |
| `book_throughput_aapl.csv` | `apex_bench data/AAPL.itch --benchmark_repetitions=10` | median ms to replay the whole AAPL day |
| `router.csv` | `apex_route data/12302019.NASDAQ_ITCH50.gz AAPL` (and `MSFT`) | 1,000-share parents |
| `pipeline.csv` | `apex_pipeline data/12302019.NASDAQ_ITCH50.gz 3` | seconds for all 8,892 stocks, 3 rounds |

`scripts/plot_results.py` turns these files into the charts in `results/charts/`.
