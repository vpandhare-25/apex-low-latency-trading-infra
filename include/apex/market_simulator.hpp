#pragma once
// Replays real ITCH data into several simulated venues and executes our child orders.
//
// It is an ITCH handler (like BookBuilder), so it plugs straight into parse_message().
// For one stock it:
//   * assigns every real order to one venue (deterministic hash, see venue.hpp),
//   * keeps one OrderBook per venue up to date with the real adds/executes/cancels,
//   * holds our child orders "in flight" for the venue's latency, then fills them
//     against that venue's book *as it is when the order arrives*. If real traders
//     took the liquidity in the meantime, we get less, just like a real slow venue.
//
// Simplification: our fills do not remove shares from the venue books. Real data keeps
// arriving for those orders, so changing the books would make later real messages
// disagree with them. This is fine as long as our orders are small and spaced out,
// which the tools and the router benchmarks are designed to respect.

#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "apex/itch.hpp"
#include "apex/order_book.hpp"
#include "apex/venue.hpp"

namespace apex {

class MarketSimulator {
public:
    MarketSimulator(std::string symbol, std::vector<VenueConfig> venues, std::uint64_t seed = 42);

    // ---- ITCH handler methods (called by itch::parse_message) ----
    void on_stock_directory(const itch::StockDirectory& m);
    void on_add(const itch::AddOrder& m);
    void on_execute(const itch::OrderExecuted& m);
    void on_cancel(const itch::OrderCancel& m);
    void on_delete(const itch::OrderDelete& m);
    void on_replace(const itch::OrderReplace& m);

    // Sends one of our orders. It executes at send_time + venue latency.
    void submit(const ChildOrder& order);

    // Executes any orders still in flight (call once at the end of the replay).
    void finish();

    // Fills in the order they happened.
    const std::vector<Fill>& fills() const { return fills_; }

    std::size_t venue_count() const { return venues_.size(); }
    const VenueConfig& venue(std::size_t i) const { return venues_[i]; }
    const OrderBook& book(std::size_t i) const { return books_[i]; }

    // Latest ITCH timestamp seen (ns since midnight).
    std::uint64_t now() const { return now_; }

    std::optional<std::uint16_t> locate() const { return locate_; }
    // Book updates that failed. Should stay 0, like BookBuilder.
    std::uint64_t rejected() const { return rejected_; }
    // Number of real orders placed on each venue.
    const std::vector<std::uint64_t>& orders_assigned() const { return orders_assigned_; }

private:
    struct InFlight {
        std::uint64_t arrival_time;
        std::uint64_t sequence;  // breaks ties so results are always in the same order
        ChildOrder order;
        bool operator>(const InFlight& other) const {
            return arrival_time != other.arrival_time ? arrival_time > other.arrival_time
                                                      : sequence > other.sequence;
        }
    };

    // Moves the clock forward, executing every order that arrives by time `t`.
    void advance_to(std::uint64_t t);
    void execute(const InFlight& in_flight);

    // Picks the venue for a real order id. Same id + same seed = same venue, always.
    std::size_t pick_venue(OrderId id) const;

    bool is_ours(std::uint16_t locate) const { return locate_ && *locate_ == locate; }
    void count(bool ok) { if (!ok) ++rejected_; }

    std::string symbol_;
    std::vector<VenueConfig> venues_;
    std::vector<double> cumulative_share_;  // e.g. {0.5, 0.7, 1.0} for picking venues
    std::uint64_t seed_;

    std::vector<OrderBook> books_;
    std::unordered_map<OrderId, std::uint8_t> venue_of_order_;  // which book holds each real order

    // Min-heap: the order that arrives first is on top.
    std::priority_queue<InFlight, std::vector<InFlight>, std::greater<InFlight>> in_flight_;
    std::uint64_t next_sequence_ = 0;
    std::vector<Fill> fills_;

    std::optional<std::uint16_t> locate_;
    std::uint64_t now_ = 0;
    std::uint64_t rejected_ = 0;
    std::vector<std::uint64_t> orders_assigned_;
};

}  // namespace apex
