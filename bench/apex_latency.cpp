// apex_latency: per-message latency percentiles for each order book, on real data.
//
// Usage: apex_latency [path/to/SYMBOL.itch] [rounds=3]
//
// Google Benchmark reports averages. Trading systems care about the slow tail
// (p99, p99.9), so this times every single book update with steady_clock.
//
// Honest limits of this measurement:
//   * On Apple Silicon, steady_clock ticks at 24 MHz, so one tick is 41.7 ns. Updates
//     faster than that read as 0 or 42 ns. The tool prints the measured tick and the cost
//     of reading the clock, so you know how much of each number is the timer itself.
//   * Each round replays the whole day into a fresh book. Tail samples include page
//     faults and hash-table growth, which a long-running system mostly avoids by
//     reserving memory up front.
//
// It also checks correctness on real data: after every update, all books must report
// the same best bid and best ask.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "apex/analytics.hpp"
#include "apex/flat_order_book.hpp"
#include "apex/order_book.hpp"
#include "bench_data.hpp"

using namespace apex;
using namespace apex::bench;
using Clock = std::chrono::steady_clock;

namespace {

std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

template <typename Book>
LatencyStats time_every_update(const std::vector<BookEvent>& events) {
    LatencyStats stats;
    Book book;
    for (const BookEvent& e : events) {
        const auto t0 = Clock::now();
        apply(book, e);
        const auto t1 = Clock::now();
        stats.add(ns_between(t0, t1));
    }
    return stats;
}

void print_row(const char* name, const LatencyStats& s) {
    std::printf("  %-14s  %6llu  %6llu  %6llu  %7llu  %8llu  %9llu\n", name,
                (unsigned long long)s.percentile(50), (unsigned long long)s.percentile(90),
                (unsigned long long)s.percentile(99), (unsigned long long)s.percentile(99.9),
                (unsigned long long)s.percentile(99.99), (unsigned long long)s.percentile(100));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/AAPL.itch";
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 3;
    const MarketData data = load_market_data(path);
    if (data.events.empty()) {
        std::fprintf(stderr, "no book events in %s\n", path.c_str());
        return 1;
    }
    std::printf("data: %s, %zu book events\n", path.c_str(), data.events.size());

    // ---- 1. Correctness: all books must agree after every real update ----
    {
        OrderBook a;
        FlatOrderBook b;
        FastOrderBook c;
        HybridOrderBookN<16> d;  // small N: levels cross the vector/tree boundary often
        std::uint64_t rejected = 0, mismatches = 0, depth_mismatches = 0;
        std::size_t noon_levels = 0;
        const std::uint64_t noon = 12ull * 3600 * 1'000'000'000ull;
        bool noon_checked = false;
        for (const BookEvent& e : data.events) {
            // Once, at 12:00, compare EVERY level of every book (the closing book is empty,
            // so a full comparison at the end would prove nothing).
            if (!noon_checked && e.timestamp >= noon) {
                noon_checked = true;
                for (Side s : {Side::Buy, Side::Sell}) {
                    const auto ref = a.depth(s, 1u << 20);
                    noon_levels += ref.size();
                    if (ref != b.depth(s, 1u << 20) || ref != c.depth(s, 1u << 20) || ref != d.depth(s, 1u << 20)) {
                        ++depth_mismatches;
                    }
                }
            }
            const bool ra = apply(a, e), rb = apply(b, e), rc = apply(c, e), rd = apply(d, e);
            rejected += !ra;
            if (ra != rb || ra != rc || ra != rd || a.best_bid() != b.best_bid() || a.best_bid() != c.best_bid() ||
                a.best_bid() != d.best_bid() || a.best_ask() != b.best_ask() || a.best_ask() != c.best_ask() ||
                a.best_ask() != d.best_ask()) {
                ++mismatches;
            }
        }
        std::printf("cross-check (4 books): %llu rejected updates, %llu top-of-book mismatches; "
                    "full depth at 12:00 (%zu levels): %s\n",
                    (unsigned long long)rejected, (unsigned long long)mismatches, noon_levels,
                    depth_mismatches == 0 ? "identical" : "DIFFERENT");
        if (mismatches != 0 || depth_mismatches != 0) return 1;
    }

    // ---- 2. How precise is the clock, and what does reading it cost? ----
    {
        LatencyStats overhead;
        std::uint64_t tick = ~0ull;
        for (int i = 0; i < 1'000'000; ++i) {
            const auto t0 = Clock::now();
            const auto t1 = Clock::now();
            const std::uint64_t d = ns_between(t0, t1);
            overhead.add(d);
            if (d > 0 && d < tick) tick = d;
        }
        std::printf("clock: smallest non-zero step %llu ns; two back-to-back reads: p50 %llu ns, p99 %llu ns\n",
                    (unsigned long long)tick, (unsigned long long)overhead.percentile(50),
                    (unsigned long long)overhead.percentile(99));
    }

    // ---- 3. Per-update latency, several rounds, books interleaved ----
    std::printf("\nper-update latency in ns (each round replays the whole day into a fresh book):\n");
    for (int r = 1; r <= rounds; ++r) {
        std::printf(" round %d\n  book               p50     p90     p99    p99.9   p99.99        max\n", r);
        print_row("OrderBook", time_every_update<OrderBook>(data.events));
        print_row("FlatOrderBook", time_every_update<FlatOrderBook>(data.events));
        print_row("FastOrderBook", time_every_update<FastOrderBook>(data.events));
        print_row("Hybrid N=16", time_every_update<HybridOrderBookN<16>>(data.events));
        print_row("Hybrid N=64", time_every_update<HybridOrderBookN<64>>(data.events));
        print_row("Hybrid N=256", time_every_update<HybridOrderBookN<256>>(data.events));
    }
    return 0;
}
