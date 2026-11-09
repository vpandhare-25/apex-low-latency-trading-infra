// Google Benchmark suite for Apex. All input is real NASDAQ data held in memory.
//
// Usage: apex_bench [path/to/SYMBOL.itch] [Google Benchmark flags...]
//        (default data file: data/AAPL.itch, made with apex_extract)
//
// What is measured:
//   BM_ParseItch          decode every message (no book), messages/s
//   BM_OrderMapOnly       only the order-id hash map work a book does (no price levels)
//   BM_BookReplay<Book>   apply the whole day's book events to an empty book
//   BM_BookReplayTop<Book> same, and read best bid + best ask after every event
//                          (what a router or strategy does on every update)
//   BM_Route<...>         one routing decision on the three venue books at 12:00

#include <benchmark/benchmark.h>

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <type_traits>

#include "apex/flat_order_book.hpp"
#include "apex/market_simulator.hpp"
#include "apex/order_book.hpp"
#include "apex/router.hpp"
#include "bench_data.hpp"

using namespace apex;
using namespace apex::bench;

namespace {

MarketData* g_data = nullptr;
std::string g_symbol;

// ---- Parsing ----

// Handler that touches every decoded field so the compiler cannot skip the work.
struct SumHandler {
    std::uint64_t sum = 0;
    void on_stock_directory(const itch::StockDirectory& m) { sum += m.locate; }
    void on_add(const itch::AddOrder& m) { sum += m.id + m.shares + static_cast<std::uint64_t>(m.price); }
    void on_execute(const itch::OrderExecuted& m) { sum += m.id + m.shares; }
    void on_cancel(const itch::OrderCancel& m) { sum += m.id + m.shares; }
    void on_delete(const itch::OrderDelete& m) { sum += m.id; }
    void on_replace(const itch::OrderReplace& m) { sum += m.old_id + m.new_id + m.shares; }
};

void BM_ParseItch(benchmark::State& state) {
    const MarketData& d = *g_data;
    for (auto _ : state) {
        SumHandler h;
        for (std::size_t i = 0; i < d.offset.size(); ++i) {
            itch::parse_message(d.bytes.data() + d.offset[i], d.length[i], h);
        }
        benchmark::DoNotOptimize(h.sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * d.offset.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * d.bytes.size()));
}

// ---- Order books ----

template <typename Book>
void BM_BookReplay(benchmark::State& state) {
    const auto& events = g_data->events;
    std::uint64_t rejected = 0;
    for (auto _ : state) {
        Book book;
        for (const BookEvent& e : events) rejected += !apply(book, e);
        benchmark::DoNotOptimize(book.order_count());
    }
    if (rejected != 0) state.SkipWithError("book rejected an event");
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * events.size()));
}

template <typename Book>
void BM_BookReplayTop(benchmark::State& state) {
    const auto& events = g_data->events;
    for (auto _ : state) {
        Book book;
        Price spread_sum = 0;
        for (const BookEvent& e : events) {
            apply(book, e);
            const auto bid = book.best_bid();
            const auto ask = book.best_ask();
            if (bid && ask) spread_sum += ask->price - bid->price;
        }
        benchmark::DoNotOptimize(spread_sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * events.size()));
}

// Only the "order id -> order" hash map work a book does, with no price levels.
// If this is most of BM_BookReplay's time, the hash map (not the levels) is the bottleneck.
void BM_OrderMapOnly(benchmark::State& state) {
    struct Order { Side side; Price price; Quantity qty; };
    const auto& events = g_data->events;
    for (auto _ : state) {
        std::unordered_map<OrderId, Order> orders;
        for (const BookEvent& e : events) {
            switch (e.type) {
                case 'A': orders.try_emplace(e.id, Order{e.side, e.price, e.qty}); break;
                case 'E':
                case 'X': {
                    auto it = orders.find(e.id);
                    if (it != orders.end() && (it->second.qty -= e.qty) == 0) orders.erase(it);
                    break;
                }
                case 'D': orders.erase(e.id); break;
                case 'U': {
                    auto it = orders.find(e.id);
                    const Side side = it->second.side;
                    orders.erase(it);
                    orders.try_emplace(e.new_id, Order{side, e.price, e.qty});
                    break;
                }
            }
        }
        benchmark::DoNotOptimize(orders.size());
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * events.size()));
}

// ---- Routing ----

// Builds the three venue books as they were at 12:00 (built once, reused).
MarketSimulator& noon_market() {
    static std::unique_ptr<MarketSimulator> sim = [] {
        auto s = std::make_unique<MarketSimulator>(g_symbol, default_venues());
        const MarketData& d = *g_data;
        const std::uint64_t noon = 12ull * 3600 * 1'000'000'000ull;
        for (std::size_t i = 0; i < d.offset.size(); ++i) {
            const std::uint8_t* msg = d.bytes.data() + d.offset[i];
            if (d.length[i] >= 11 && itch::read_u48(msg + 5) >= noon) break;
            itch::parse_message(msg, d.length[i], *s);
        }
        return s;
    }();
    return *sim;
}

template <typename RouterT, bool kFees>
void BM_Route(benchmark::State& state) {
    MarketSimulator& sim = noon_market();
    const std::vector<VenueConfig> venues = default_venues();
    MarketView view{{&sim.book(0), &sim.book(1), &sim.book(2)}, &venues};

    Price best_ask = 0;
    for (std::size_t v = 0; v < 3; ++v) {
        if (auto a = sim.book(v).best_ask(); a && (best_ask == 0 || a->price < best_ask)) best_ask = a->price;
    }
    const ParentOrder parent{Side::Buy, static_cast<Quantity>(state.range(0)), best_ask + best_ask / 1000, 0};

    std::unique_ptr<Router> router;
    if constexpr (std::is_same_v<RouterT, SplitRouter>) router = std::make_unique<SplitRouter>(kFees);
    else router = std::make_unique<RouterT>();

    std::size_t children = 0;
    for (auto _ : state) {
        auto out = router->route(parent, view);
        children = out.size();
        benchmark::DoNotOptimize(out.data());
    }
    state.counters["children"] = static_cast<double>(children);
}

}  // namespace

BENCHMARK(BM_ParseItch)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_OrderMapOnly)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<OrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<FlatOrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<FastOrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<HybridOrderBookN<16>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<HybridOrderBookN<64>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplay<HybridOrderBookN<256>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplayTop<OrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplayTop<FlatOrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplayTop<FastOrderBook>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_BookReplayTop<HybridOrderBookN<16>>)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Route<SingleVenueRouter, false>)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK(BM_Route<SplitRouter, false>)->Arg(100)->Arg(1000)->Arg(10000);
BENCHMARK(BM_Route<SplitRouter, true>)->Arg(100)->Arg(1000)->Arg(10000);

int main(int argc, char** argv) {
    // Our own first argument (if it is not a --flag) is the data file.
    std::string path = "data/AAPL.itch";
    if (argc > 1 && argv[1][0] != '-') {
        path = argv[1];
        for (int i = 1; i + 1 < argc; ++i) argv[i] = argv[i + 1];
        --argc;
    }
    MarketData data = load_market_data(path);
    if (data.offset.empty()) {
        std::fprintf(stderr, "no messages in %s (run apex_extract first)\n", path.c_str());
        return 1;
    }
    // The symbol is in the first message (the 'R' directory entry written by apex_extract).
    {
        struct Dir {
            std::string* symbol;
            void on_stock_directory(const itch::StockDirectory& m) { *symbol = std::string(m.symbol()); }
            void on_add(const itch::AddOrder&) {}
            void on_execute(const itch::OrderExecuted&) {}
            void on_cancel(const itch::OrderCancel&) {}
            void on_delete(const itch::OrderDelete&) {}
            void on_replace(const itch::OrderReplace&) {}
        } dir{&g_symbol};
        itch::parse_message(data.bytes.data(), data.length[0], dir);
    }
    g_data = &data;
    std::printf("data: %s (%s), %zu messages, %zu book events, %.1f MB\n", path.c_str(), g_symbol.c_str(),
                data.offset.size(), data.events.size(), data.bytes.size() / 1e6);

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
