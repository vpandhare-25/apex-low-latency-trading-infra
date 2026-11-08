// Unit tests for the baseline OrderBook.
#include <gtest/gtest.h>

#include "apex/order_book.hpp"

using namespace apex;

namespace {
// $10.00 in ITCH price units (1/10000 dollar).
constexpr Price kTenDollars = 100000;
}  // namespace

TEST(OrderBook, EmptyBookHasNoBestPrices) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(OrderBook, BestBidIsHighestAndBestAskIsLowest) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars + 100, 50);   // $10.01, better bid
    book.add(3, Side::Sell, kTenDollars + 300, 70);  // $10.03
    book.add(4, Side::Sell, kTenDollars + 200, 20);  // $10.02, better ask

    EXPECT_EQ(book.best_bid(), (Level{kTenDollars + 100, 50, 1}));
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars + 200, 20, 1}));
}

TEST(OrderBook, OrdersAtSamePriceAreAggregated) {
    OrderBook book;
    book.add(1, Side::Sell, kTenDollars, 100);
    book.add(2, Side::Sell, kTenDollars, 250);
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 350, 2}));
    EXPECT_EQ(book.level_count(Side::Sell), 1u);
}

TEST(OrderBook, DuplicateIdAndZeroQuantityAreRejected) {
    OrderBook book;
    EXPECT_TRUE(book.add(1, Side::Buy, kTenDollars, 100));
    EXPECT_FALSE(book.add(1, Side::Buy, kTenDollars, 100));
    EXPECT_FALSE(book.add(2, Side::Buy, kTenDollars, 0));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, PartialExecuteReducesLevel) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.execute(1, 30));
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars, 70, 1}));
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, FullExecuteRemovesOrderAndEmptyLevel) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.execute(1, 100));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.level_count(Side::Buy), 0u);
}

TEST(OrderBook, ExecuteMoreThanRemainingIsRejected) {
    OrderBook book;
    book.add(1, Side::Sell, kTenDollars, 10);
    EXPECT_FALSE(book.execute(1, 11));
    EXPECT_FALSE(book.execute(99, 1));  // unknown id
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 10, 1}));
}

TEST(OrderBook, CancelKeepsOtherOrdersAtLevel) {
    OrderBook book;
    book.add(1, Side::Sell, kTenDollars, 100);
    book.add(2, Side::Sell, kTenDollars, 50);
    EXPECT_TRUE(book.cancel(1, 100));  // order 1 fully cancelled
    EXPECT_EQ(book.best_ask(), (Level{kTenDollars, 50, 1}));
}

TEST(OrderBook, RemoveDeletesWholeOrder) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars - 100, 40);
    EXPECT_TRUE(book.remove(1));
    EXPECT_FALSE(book.remove(1));  // already gone
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars - 100, 40, 1}));
}

TEST(OrderBook, ReplaceMovesOrderAndKeepsSide) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    EXPECT_TRUE(book.replace(1, 2, kTenDollars + 500, 60));
    EXPECT_FALSE(book.remove(1));  // old id no longer exists
    EXPECT_EQ(book.best_bid(), (Level{kTenDollars + 500, 60, 1}));
    EXPECT_EQ(book.level_count(Side::Buy), 1u);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(OrderBook, ReplaceRejectsUnknownOrTakenIds) {
    OrderBook book;
    book.add(1, Side::Buy, kTenDollars, 100);
    book.add(2, Side::Buy, kTenDollars, 100);
    EXPECT_FALSE(book.replace(99, 3, kTenDollars, 10));  // unknown old id
    EXPECT_FALSE(book.replace(1, 2, kTenDollars, 10));   // new id taken
    EXPECT_EQ(book.order_count(), 2u);
}

TEST(OrderBook, DepthReturnsBestLevelsFirst) {
    OrderBook book;
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
