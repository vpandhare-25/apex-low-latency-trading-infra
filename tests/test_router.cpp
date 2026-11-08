// Tests for the routers and execution analytics.
#include <gtest/gtest.h>

#include "apex/analytics.hpp"
#include "apex/router.hpp"

using namespace apex;

namespace {

// Three venues with different fees. Latency does not matter for routing decisions.
const std::vector<VenueConfig> kVenues = {
    {"A", 0.5, 30, 0},  // expensive fee
    {"B", 0.2, 0, 0},   // free
    {"C", 0.3, 10, 0},
};

struct Books {
    OrderBook a, b, c;
    MarketView view() const { return {{&a, &b, &c}, &kVenues}; }
};

ParentOrder buy(Quantity qty, Price limit) { return {Side::Buy, qty, limit, 0}; }
ParentOrder sell(Quantity qty, Price limit) { return {Side::Sell, qty, limit, 0}; }

}  // namespace

TEST(SingleVenueRouter, SendsEverythingToBestPricedVenue) {
    Books books;
    books.a.add(1, Side::Sell, 1000, 50);
    books.b.add(2, Side::Sell, 1001, 500);
    const auto children = SingleVenueRouter().route(buy(300, 1100), books.view());
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0].venue, 0u);  // A has the best price, even though it is small
    EXPECT_EQ(children[0].qty, 300u);
    EXPECT_EQ(children[0].limit, 1100);
}

TEST(SingleVenueRouter, SendsNothingIfBestPriceIsOutsideLimit) {
    Books books;
    books.a.add(1, Side::Sell, 1000, 50);
    EXPECT_TRUE(SingleVenueRouter().route(buy(10, 999), books.view()).empty());
    EXPECT_TRUE(SingleVenueRouter().route(buy(10, 2000), Books{}.view()).empty());
}

TEST(SplitRouter, TakesCheapestSharesAcrossVenues) {
    Books books;
    books.a.add(1, Side::Sell, 1000, 50);   // cheapest
    books.a.add(2, Side::Sell, 1003, 100);
    books.b.add(3, Side::Sell, 1001, 30);
    books.c.add(4, Side::Sell, 1002, 40);

    const auto children = SplitRouter(/*include_fees=*/false).route(buy(100, 1100), books.view());
    // 50 @ 1000 (A) + 30 @ 1001 (B) + 20 @ 1002 (C) = 100
    ASSERT_EQ(children.size(), 3u);
    EXPECT_EQ(children[0].venue, 0u); EXPECT_EQ(children[0].qty, 50u); EXPECT_EQ(children[0].limit, 1000);
    EXPECT_EQ(children[1].venue, 1u); EXPECT_EQ(children[1].qty, 30u); EXPECT_EQ(children[1].limit, 1001);
    EXPECT_EQ(children[2].venue, 2u); EXPECT_EQ(children[2].qty, 20u); EXPECT_EQ(children[2].limit, 1002);
}

TEST(SplitRouter, FeesCanMakeAWorsePriceTheBetterDeal) {
    Books books;
    books.a.add(1, Side::Sell, 1000, 100);  // 1000 + fee 30 = 1030 all-in
    books.b.add(2, Side::Sell, 1020, 100);  // 1020 + fee 0  = 1020 all-in (cheaper!)

    const auto blind = SplitRouter(false).route(buy(100, 1100), books.view());
    ASSERT_EQ(blind.size(), 1u);
    EXPECT_EQ(blind[0].venue, 0u);

    const auto smart = SplitRouter(true).route(buy(100, 1100), books.view());
    ASSERT_EQ(smart.size(), 1u);
    EXPECT_EQ(smart[0].venue, 1u);
}

TEST(SplitRouter, SellSideTakesHighestBidsAndRespectsLimit) {
    Books books;
    books.a.add(1, Side::Buy, 1000, 40);
    books.b.add(2, Side::Buy, 999, 40);
    books.c.add(3, Side::Buy, 990, 40);  // below the sell limit
    const auto children = SplitRouter(false).route(sell(200, 995), books.view());
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0].qty + children[1].qty, 80u);  // only 80 shares at 995 or better
    EXPECT_EQ(children[1].limit, 999);
}

TEST(SplitRouter, UsesWorstChosenPriceAsChildLimit) {
    Books books;
    books.a.add(1, Side::Sell, 1000, 10);
    books.a.add(2, Side::Sell, 1001, 10);
    books.a.add(3, Side::Sell, 1002, 10);
    const auto children = SplitRouter(false).route(buy(15, 1100), books.view());
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0].qty, 15u);
    EXPECT_EQ(children[0].limit, 1001);  // does not need to reach 1002
}

// ---- Analytics ----

TEST(ExecutionStats, BuyCostIncludesFeesAgainstMid) {
    ExecutionStats stats;
    Fill f;
    f.requested = 100;
    f.filled = 100;
    f.notional = 100 * 10010;  // paid $1.0010 per share
    f.fees = 100 * 10;         // plus $0.0010 per share
    stats.add_parent(Side::Buy, 100, /*mid=*/10000.0, {f});
    // cost = (1001000 + 1000) - 1000000 = 2000 units = $0.20, which is 20 bps of $100
    EXPECT_NEAR(stats.cost_dollars(), 0.20, 1e-9);
    EXPECT_NEAR(stats.cost_bps(), 20.0, 1e-9);
    EXPECT_DOUBLE_EQ(stats.fill_rate(), 1.0);
}

TEST(ExecutionStats, SellCostAndPartialFill) {
    ExecutionStats stats;
    Fill f;
    f.requested = 100;
    f.filled = 50;
    f.notional = 50 * 9990;  // received $0.9990
    f.fees = 50 * 10;
    stats.add_parent(Side::Sell, 100, 10000.0, {f});
    // cost = 500000 - (499500 - 500) = 1000 units; 1000 / 500000 = 20 bps
    EXPECT_NEAR(stats.cost_bps(), 20.0, 1e-9);
    EXPECT_DOUBLE_EQ(stats.fill_rate(), 0.5);
    EXPECT_EQ(stats.child_orders(), 1u);
}

TEST(LatencyStats, NearestRankPercentiles) {
    LatencyStats lat;
    for (std::uint64_t i = 1; i <= 1000; ++i) lat.add(i);
    EXPECT_EQ(lat.percentile(50), 500u);
    EXPECT_EQ(lat.percentile(99), 990u);
    EXPECT_EQ(lat.percentile(99.9), 999u);
    EXPECT_EQ(lat.percentile(100), 1000u);
    EXPECT_EQ(LatencyStats().percentile(50), 0u);
}
