// Tests for OrderManager (waves of child orders and re-routing).
#include <gtest/gtest.h>

#include "apex/order_manager.hpp"

using namespace apex;

namespace {

constexpr std::uint16_t kLocate = 1;

// Two venues: "slow" holds the cheapest shares, "fast" is a little more expensive.
// Order shares 0.0/1.0 do not matter here: we add orders to each venue by id below.
struct TwoVenueMarket {
    std::vector<VenueConfig> venues{{"slow", 0.5, 0, 1000}, {"fast", 0.5, 0, 10}};
    MarketSimulator sim{"TEST", venues};
    SplitRouter router{false};

    TwoVenueMarket() {
        itch::StockDirectory dir{kLocate, 0, {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' '}};
        sim.on_stock_directory(dir);
    }
    MarketView view() const { return {{&sim.book(0), &sim.book(1)}, &venues}; }

    // Adds a sell order and returns the venue it landed on (the split is a hash).
    std::size_t add_sell(std::uint64_t ts, OrderId id, Quantity qty, Price price) {
        sim.on_add({kLocate, ts, id, Side::Sell, qty, price});
        return sim.book(0).contains(id) ? 0 : 1;
    }
    // Moves the clock forward with a message for another stock.
    void tick(std::uint64_t ts) { sim.on_delete({/*locate=*/99, ts, 0}); }
};

// Finds an id that the hash puts on the wanted venue, starting from `from`.
OrderId id_on_venue(TwoVenueMarket& s, std::size_t venue, OrderId from) {
    for (OrderId id = from;; ++id) {
        TwoVenueMarket probe;  // fresh simulator with the same seed and venues = same hash
        if (probe.add_sell(0, id, 1, 1) == venue) return id;
    }
}

}  // namespace

TEST(OrderManager, ReroutesSharesLostOnSlowVenue) {
    TwoVenueMarket s;
    const OrderId cheap = id_on_venue(s, 0, 1);       // on the slow venue
    const OrderId dear = id_on_venue(s, 1, cheap + 1); // on the fast venue
    ASSERT_EQ(s.add_sell(1, cheap, 100, 500), 0u);
    ASSERT_EQ(s.add_sell(2, dear, 100, 501), 1u);

    OrderManager manager(s.router, /*max_waves=*/2);
    manager.start(/*tag=*/5, ParentOrder{Side::Buy, 100, 510, 10}, s.view(), s.sim);
    // Wave 1 goes to the slow venue (cheapest). It arrives at 10 + 1000 = 1010.

    s.sim.on_execute({kLocate, 50, cheap, 100, false, 0});  // someone else takes it at t=50
    s.tick(1010);  // our child arrives: nothing left, fills 0 shares
    manager.poll(1010, s.view(), s.sim);
    EXPECT_EQ(manager.working(), 1u);  // report still travelling back (due at 2010)

    s.tick(2010);
    manager.poll(2010, s.view(), s.sim);  // report in: re-route 100 shares to "fast"
    s.tick(2020);                          // wave 2 arrives at 2010 + 10
    s.tick(2030);                          // its report arrives at 2030
    manager.poll(2030, s.view(), s.sim);

    ASSERT_EQ(s.sim.fills().size(), 2u);
    EXPECT_EQ(s.sim.fills()[0].venue, 0u);
    EXPECT_EQ(s.sim.fills()[0].filled, 0u);
    EXPECT_EQ(s.sim.fills()[1].venue, 1u);
    EXPECT_EQ(s.sim.fills()[1].filled, 100u);
    EXPECT_EQ(s.sim.fills()[1].tag, 5u);
    EXPECT_EQ(manager.waves_sent(), 2u);
    EXPECT_EQ(manager.working(), 0u);
}

TEST(OrderManager, OneWaveMeansNoReroute) {
    TwoVenueMarket s;
    const OrderId cheap = id_on_venue(s, 0, 1);
    const OrderId dear = id_on_venue(s, 1, cheap + 1);
    s.add_sell(1, cheap, 100, 500);
    s.add_sell(2, dear, 100, 501);

    OrderManager manager(s.router, /*max_waves=*/1);
    manager.start(1, ParentOrder{Side::Buy, 100, 510, 10}, s.view(), s.sim);
    s.sim.on_execute({kLocate, 50, cheap, 100, false, 0});
    s.tick(5000);
    manager.poll(5000, s.view(), s.sim);
    s.tick(6000);
    manager.poll(6000, s.view(), s.sim);

    ASSERT_EQ(s.sim.fills().size(), 1u);
    EXPECT_EQ(s.sim.fills()[0].filled, 0u);
    EXPECT_EQ(manager.waves_sent(), 1u);
    EXPECT_EQ(manager.working(), 0u);
}

TEST(OrderManager, FullyFilledParentNeedsOneWave) {
    TwoVenueMarket s;
    const OrderId id = id_on_venue(s, 1, 1);
    s.add_sell(1, id, 100, 500);
    OrderManager manager(s.router, /*max_waves=*/3);
    manager.start(1, ParentOrder{Side::Buy, 100, 510, 10}, s.view(), s.sim);
    s.tick(100);
    manager.poll(100, s.view(), s.sim);
    EXPECT_EQ(s.sim.fills().size(), 1u);
    EXPECT_EQ(s.sim.fills()[0].filled, 100u);
    EXPECT_EQ(manager.waves_sent(), 1u);
    EXPECT_EQ(manager.working(), 0u);
}

TEST(OrderManager, NothingRoutableMeansNothingWorking) {
    TwoVenueMarket s;  // empty books
    OrderManager manager(s.router, 2);
    manager.start(1, ParentOrder{Side::Buy, 100, 510, 10}, s.view(), s.sim);
    EXPECT_EQ(manager.working(), 0u);
    EXPECT_EQ(manager.waves_sent(), 0u);
    EXPECT_EQ(manager.first_wave_decision_ns().count(), 1u);  // the decision was still timed
}
