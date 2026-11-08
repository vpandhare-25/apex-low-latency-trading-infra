// Unit tests for both order books. Every test runs on OrderBook (std::map levels)
// and FlatOrderBook (sorted vector levels): they must behave identically.
#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "apex/flat_order_book.hpp"
#include "apex/order_book.hpp"

using namespace apex;

namespace {
// $10.00 in ITCH price units (1/10000 dollar).
constexpr Price kTenDollars = 100000;
}  // namespace

template <typename Book>
class BookTest : public ::testing::Test {};
using BookTypes = ::testing::Types<OrderBook, FlatOrderBook, FastOrderBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

TYPED_TEST(BookTest, EmptyBookHasNoBestPrices) {
    TypeParam book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TYPED_TEST(BookTest, BestBidIsHighestAndBestAskIsLowest) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars + 100, 50);   // $10.01, better bid
    book.add(3, Side::Sell, kTenDollars + 300, 70);  // $10.03
    book.add(4, Side::Sell, kTenDollars + 200, 20);  // $10.02, better ask

    EXPECT_EQ(book.best_bid(), (Level{kTenDollars + 100, 50, 1}));
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars + 200, 20, 1}));
}

TYPED_TEST(BookTest, OrdersAtSamePriceAreAggregated) {
    TypeParam book;
    book.add(1, Side::Sell, kTenDollars, 100);
    book.add(2, Side::Sell, kTenDollars, 250);
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 350, 2}));
    EXPECT_EQ(book.level_count(Side::Sell), 1u);
}

TYPED_TEST(BookTest, DuplicateIdAndZeroQuantityAreRejected) {
    TypeParam book;
    EXPECT_TRUE(book.add(1, Side::Buy, kTenDollars, 100));
    EXPECT_FALSE(book.add(1, Side::Buy, kTenDollars, 100));
    EXPECT_FALSE(book.add(2, Side::Buy, kTenDollars, 0));
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(BookTest, PartialExecuteReducesLevel) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.execute(1, 30));
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars, 70, 1}));
    EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(BookTest, FullExecuteRemovesOrderAndEmptyLevel) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.execute(1, 100));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Buy), 0u);
}

TYPED_TEST(BookTest, ExecuteMoreThanRemainingIsRejected) {
    TypeParam book;
    book.add(1, Side::Sell, kTenDollars, 10);
    EXPECT_FALSE(book.execute(1, 11));
    EXPECT_FALSE(book.execute(99, 1));  // unknown id
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 10, 1}));
}

TYPED_TEST(BookTest, CancelKeepsOtherOrdersAtLevel) {
    TypeParam book;
    book.add(1, Side::Sell, kTenDollars, 100);
    book.add(2, Side::Sell, kTenDollars, 50);
    EXPECT_TRUE(book.cancel(1, 100));  // order 1 fully cancelled
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 50, 1}));
}

TYPED_TEST(BookTest, RemoveDeletesWholeOrder) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars - 100, 40);
    EXPECT_TRUE(book.remove(1));
    EXPECT_FALSE(book.remove(1));  // already gone
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars - 100, 40, 1}));
}

TYPED_TEST(BookTest, ReplaceMovesOrderAndKeepsSide) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.replace(1, 2, kTenDollars + 500, 60));
    EXPECT_FALSE(book.remove(1));  // old id no longer exists
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars + 500, 60, 1}));
    EXPECT_EQ(book.level_count(Side::Buy), 1u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TYPED_TEST(BookTest, ReplaceRejectsUnknownOrTakenIds) {
    TypeParam book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars, 100);
    EXPECT_FALSE(book.replace(99, 3, kTenDollars, 10));  // unknown old id
    EXPECT_FALSE(book.replace(1, 2, kTenDollars, 10));   // new id taken
    EXPECT_EQ(book.order_count(), 2u);
}

TYPED_TEST(BookTest, DepthReturnsBestLevelsFirst) {
    TypeParam book;
    book.add(1, Side::Sell, kTenDollars + 300, 10);
    book.add(2, Side::Sell, kTenDollars + 100, 20);
    book.add(3, Side::Sell, kTenDollars + 200, 30);

    auto top2 = book.depth(Side::Sell, 2);
    ASSERT_EQ(top2.size(), 2u);
    EXPECT_EQ(top2[0].price, kTenDollars + 100);
    EXPECT_EQ(top2[1].price, kTenDollars + 200);

    EXPECT_EQ(book.depth(Side::Sell, 10).size(), 3u);  // asks for more than exists
    EXPECT_TRUE(book.depth(Side::Buy, 5).empty());
}

// Differential test: apply the same random operations to both books and compare the
// full book after every step. Prices cluster near the middle, like a real book.
template <typename Book>
class DifferentialTest : public ::testing::Test {};
using FlatTypes = ::testing::Types<FlatOrderBook, FastOrderBook>;
TYPED_TEST_SUITE(DifferentialTest, FlatTypes);

TYPED_TEST(DifferentialTest, MatchesOrderBookOnRandomOperations) {
    OrderBook reference;
    TypeParam flat;
    std::mt19937_64 rng(2024);
    std::vector<OrderId> live;
    OrderId next_id = 1;

    for (int step = 0; step < 50000; ++step) {
        const int action = live.empty() ? 0 : static_cast<int>(rng() % 5);
        bool ok_ref = false, ok_flat = false;
        if (action <= 1) {  // add (twice as likely, so the book grows)
            const Side side = (rng() % 2) ? Side::Buy : Side::Sell;
            const Price offset = Price(rng() % 30) * Price(rng() % 30);  // mostly small
            const Price price = side == Side::Buy ? 10000 - offset : 10001 + offset;
            const Quantity qty = 1 + Quantity(rng() % 300);
            ok_ref = reference.add(next_id, side, price, qty);
            ok_flat = flat.add(next_id, side, price, qty);
            live.push_back(next_id++);
        } else {
            const std::size_t pick = rng() % live.size();
            const OrderId id = live[pick];
            if (action == 2) {
                ok_ref = reference.remove(id);
                ok_flat = flat.remove(id);
            } else if (action == 3) {
                const Quantity qty = 1 + Quantity(rng() % 100);  // may be too many: both must reject
                ok_ref = reference.execute(id, qty);
                ok_flat = flat.execute(id, qty);
            } else {
                const Price price = 9990 + Price(rng() % 20);
                ok_ref = reference.replace(id, next_id, price, 50);
                ok_flat = flat.replace(id, next_id, price, 50);
                live[pick] = next_id++;
            }
            if (!reference.contains(live[pick])) live.erase(live.begin() + static_cast<long>(pick));
        }
        ASSERT_EQ(ok_ref, ok_flat) << "step " << step;
        ASSERT_EQ(reference.order_count(), flat.order_count()) << "step " << step;
        for (Side side : {Side::Buy, Side::Sell}) {
            ASSERT_EQ(reference.depth(side, 100000), flat.depth(side, 100000)) << "step " << step;
        }
    }
}
