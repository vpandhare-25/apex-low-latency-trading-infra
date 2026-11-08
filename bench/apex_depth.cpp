// apex_depth: how deep does the real book get, and how far from the best price are
// new price levels created?
//
// Usage: apex_depth [path/to/SYMBOL.itch]
//
// Why it matters: FlatOrderBook/FastOrderBook keep levels in a sorted vector with the
// best price at the back. Creating a level `k` places away from the best price makes
// vector::insert shift those k better-priced levels. Usually k is tiny, but a real book
// can be thousands of levels deep, and those rare large shifts make up the p99 tail
// that apex_latency measures.

#include <cstdio>
#include <string>

#include "apex/order_book.hpp"
#include "bench_data.hpp"

using namespace apex;
using namespace apex::bench;

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "data/AAPL.itch";
    const MarketData data = load_market_data(path);
    OrderBook book;
    std::size_t max_levels[2] = {0, 0};
    long buckets[4] = {0, 0, 0, 0};  // levels shifted: <10, 10-99, 100-999, >=1000
    long new_levels = 0;

    for (const BookEvent& e : data.events) {
        if (e.type == 'A') {
            // Count the levels strictly better than the new price (= levels a vector shifts).
            std::size_t better = 0;
            book.for_each_level(e.side, [&](const Level& l) {
                const bool is_better = e.side == Side::Buy ? l.price > e.price : l.price < e.price;
                if (!is_better) return false;
                ++better;
                return true;
            });
            const std::size_t before = book.level_count(e.side);
            apply(book, e);
            if (book.level_count(e.side) > before) {
                ++new_levels;
                ++buckets[better < 10 ? 0 : better < 100 ? 1 : better < 1000 ? 2 : 3];
            }
        } else {
            apply(book, e);
        }
        for (Side s : {Side::Buy, Side::Sell}) {
            const std::size_t n = book.level_count(s);
            std::size_t& m = max_levels[s == Side::Buy ? 0 : 1];
            if (n > m) m = n;
        }
    }

    std::printf("%s: %zu book events\n", path.c_str(), data.events.size());
    std::printf("deepest book seen: %zu bid levels, %zu ask levels\n", max_levels[0], max_levels[1]);
    std::printf("adds that created a new level: %ld (%.1f%% of events)\n", new_levels,
                100.0 * new_levels / data.events.size());
    const char* names[] = {"< 10", "10-99", "100-999", ">= 1000"};
    for (int b = 0; b < 4; ++b) {
        std::printf("  better levels to shift %-8s %8ld  (%.1f%%)\n", names[b], buckets[b],
                    new_levels ? 100.0 * buckets[b] / new_levels : 0.0);
    }
    return 0;
}
