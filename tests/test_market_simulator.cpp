// Tests for take_liquidity() and MarketSimulator (venue split, latency, fees).
#include <gtest/gtest.h>

#include <map>
#include <random>
#include <vector>

#include "apex/book_builder.hpp"
#include "apex/market_simulator.hpp"
#include "apex/venue.hpp"

using namespace apex;

namespace {

constexpr std::uint16_t kLocate = 1;

itch::StockDirectory directory(const char* symbol) {
    itch::StockDirectory m{kLocate, 0, {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '}};
    for (int i = 0; symbol[i] != '\0'; ++i) m.stock[i] = symbol[i];
    return m;
}
itch::AddOrder add(std::uint64_t ts, OrderId id, Side side, Quantity qty, Price price) {
    return {kLocate, ts, id, side, qty, price};
}

// One venue only, so tests about latency and fills are easy to reason about.
MarketSimulator single_venue(std::uint64_t latency_ns, Price fee) {
    MarketSimulator sim("TEST", {{"X", 1.0, fee, latency_ns}});
    sim.on_stock_directory(directory("TEST"));
    return sim;
}

}  // namespace

// ---- take_liquidity ----

TEST(TakeLiquidity, WalksLevelsUpToLimit) {
    OrderBook book;
    book.add(1, Side::Sell, 100, 50);
    book.add(2, Side::Sell, 101, 50);
    book.add(3, Side::Sell, 102, 50);  // above our limit

    const TakeResult r = take_liquidity(book, Side::Buy, 120, /*limit=*/101);
    EXPECT_EQ(r.filled, 100u);                 // 50 @ 100 + 50 @ 101, stops at 102
    EXPECT_EQ(r.notional, 50 * 100 + 50 * 101);
    EXPECT_EQ(book.best_ask()->quantity, 50u);  // book is not changed
}

TEST(TakeLiquidity, StopsWhenOrderIsFilled) {
    OrderBook book;
    book.add(1, Side::Buy, 100, 50);
    book.add(2, Side::Buy, 99, 50);
    const TakeResult r = take_liquidity(book, Side::Sell, 70, /*limit=*/90);
    EXPECT_EQ(r.filled, 70u);  // 50 @ 100 + 20 @ 99 (sell takes highest bid first)
    EXPECT_EQ(r.notional, 50 * 100 + 20 * 99);
}

TEST(TakeLiquidity, EmptyBookFillsNothing) {
    OrderBook book;
    EXPECT_EQ(take_liquidity(book, Side::Buy, 10, 1000).filled, 0u);
}

// ---- Venue split ----

// Feeds the same random order flow into a BookBuilder (one real book) and a
// three-venue MarketSimulator. Adding the three venue books together must give
// exactly the real book, and no update may be rejected.
TEST(MarketSimulator, VenueBooksAddUpToRealBook) {
    BookBuilder real("TEST");
    MarketSimulator sim("TEST", default_venues());
    real.on_stock_directory(directory("TEST"));
    sim.on_stock_directory(directory("TEST"));

    std::mt19937_64 rng(7);
    std::vector<OrderId> live;
    OrderId next_id = 1;
    std::uint64_t ts = 1;
    for (int step = 0; step < 20000; ++step, ++ts) {
        const int action = live.empty() ? 0 : static_cast<int>(rng() % 4);
        if (action == 0) {  // add
            const Side side = (rng() % 2) ? Side::Buy : Side::Sell;
            const Price price = side == Side::Buy ? 1000 - Price(rng() % 20) : 1001 + Price(rng() % 20);
            const auto m = add(ts, next_id, side, 1 + Quantity(rng() % 500), price);
            real.on_add(m);
            sim.on_add(m);
            live.push_back(next_id++);
        } else {
            const std::size_t pick = rng() % live.size();
            const OrderId id = live[pick];
            if (action == 1) {  // delete
                real.on_delete({kLocate, ts, id});
                sim.on_delete({kLocate, ts, id});
                live.erase(live.begin() + static_cast<long>(pick));
            } else if (action == 2) {  // execute 1 share (order may survive)
                real.on_execute({kLocate, ts, id, 1, false, 0});
                sim.on_execute({kLocate, ts, id, 1, false, 0});
                if (!real.book().contains(id)) live.erase(live.begin() + static_cast<long>(pick));
            } else {  // replace with a new id and price
                const Price price = 990 + Price(rng() % 5);
                real.on_replace({kLocate, ts, id, next_id, 10, price});
                sim.on_replace({kLocate, ts, id, next_id, 10, price});
                live[pick] = next_id++;
            }
        }
    }

    EXPECT_EQ(real.stats().rejected, 0u);
    EXPECT_EQ(sim.rejected(), 0u);

    for (Side side : {Side::Buy, Side::Sell}) {
        std::map<Price, Quantity> combined;
        for (std::size_t v = 0; v < sim.venue_count(); ++v) {
            for (const Level& l : sim.book(v).depth(side, 1000)) combined[l.price] += l.quantity;
        }
        std::map<Price, Quantity> expected;
        for (const Level& l : real.book().depth(side, 1000)) expected[l.price] = l.quantity;
        EXPECT_EQ(combined, expected);
    }
    std::size_t total_orders = 0;
    for (std::size_t v = 0; v < sim.venue_count(); ++v) total_orders += sim.book(v).order_count();
    EXPECT_EQ(total_orders, real.book().order_count());
}

TEST(MarketSimulator, SplitFollowsSharesAndIsDeterministic) {
    auto run = [](std::uint64_t seed) {
        MarketSimulator sim("TEST", default_venues(), seed);
        sim.on_stock_directory(directory("TEST"));
        for (OrderId id = 1; id <= 20000; ++id) sim.on_add(add(id, id, Side::Buy, 1, 100));
        return sim.orders_assigned();
    };
    const auto counts = run(42);
    // default_venues: 50% / 20% / 30%. Allow +-2 percentage points.
    EXPECT_NEAR(counts[0] / 20000.0, 0.50, 0.02);
    EXPECT_NEAR(counts[1] / 20000.0, 0.20, 0.02);
    EXPECT_NEAR(counts[2] / 20000.0, 0.30, 0.02);
    EXPECT_EQ(run(42), counts);    // same seed -> same split
    EXPECT_NE(run(7), counts);     // different seed -> different split
}

TEST(MarketSimulator, IgnoresOtherStocks) {
    MarketSimulator sim = single_venue(0, 0);
    sim.on_add({/*locate=*/99, 1, 1, Side::Sell, 100, 50});
    EXPECT_EQ(sim.book(0).order_count(), 0u);
}

// ---- Latency and fills ----

TEST(MarketSimulator, OrderSeesBookAtArrivalTime) {
    MarketSimulator sim = single_venue(/*latency_ns=*/100, /*fee=*/0);
    sim.on_add(add(10, 1, Side::Sell, 100, 500));  // 100 shares offered @ 500

    // We see the offer at t=20 and send a buy for all 100 shares; it arrives at t=120.
    sim.submit({/*tag=*/7, 0, Side::Buy, 100, 500, /*send_time=*/20});
    sim.on_execute({kLocate, 50, 1, 60, false, 0});  // t=50: someone else buys 60 first
    sim.on_add(add(130, 2, Side::Sell, 100, 500));   // t=130: after arrival, does not help us

    ASSERT_EQ(sim.fills().size(), 1u);
    const Fill& f = sim.fills()[0];
    EXPECT_EQ(f.tag, 7u);
    EXPECT_EQ(f.requested, 100u);
    EXPECT_EQ(f.filled, 40u);  // only 40 left when we arrived
    EXPECT_EQ(f.arrival_time, 120u);
}

TEST(MarketSimulator, FeesAreChargedPerFilledShare) {
    MarketSimulator sim = single_venue(0, /*fee=*/30);  // $0.0030 per share
    sim.on_add(add(1, 1, Side::Buy, 200, 1000));
    sim.submit({1, 0, Side::Sell, 150, 1000, 2});
    sim.finish();
    ASSERT_EQ(sim.fills().size(), 1u);
    EXPECT_EQ(sim.fills()[0].filled, 150u);
    EXPECT_EQ(sim.fills()[0].notional, 150 * 1000);
    EXPECT_EQ(sim.fills()[0].fees, 150 * 30);
}

TEST(MarketSimulator, OrdersExecuteInArrivalOrder) {
    // Two venues: the fast one gets an order sent later but still arrives first.
    MarketSimulator sim("TEST", {{"slow", 0.5, 0, 1000}, {"fast", 0.5, 0, 10}});
    sim.on_stock_directory(directory("TEST"));
    sim.submit({1, 0, Side::Buy, 1, 1, 0});    // arrives at 1000
    sim.submit({2, 1, Side::Buy, 1, 1, 100});  // arrives at 110
    sim.finish();
    ASSERT_EQ(sim.fills().size(), 2u);
    EXPECT_EQ(sim.fills()[0].tag, 2u);
    EXPECT_EQ(sim.fills()[1].tag, 1u);
}
