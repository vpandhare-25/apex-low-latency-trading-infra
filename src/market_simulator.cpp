#include "apex/market_simulator.hpp"

#include <stdexcept>

namespace apex {

namespace {

// splitmix64: a tiny, well-known hash that turns similar ids into very different
// numbers, so consecutive order ids still spread evenly across venues.
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}  // namespace

MarketSimulator::MarketSimulator(std::string symbol, std::vector<VenueConfig> venues, std::uint64_t seed)
    : symbol_(std::move(symbol)), venues_(std::move(venues)), seed_(seed),
      books_(venues_.size()), orders_assigned_(venues_.size(), 0) {
    if (venues_.empty() || venues_.size() > 255) throw std::invalid_argument("need 1..255 venues");
    double total = 0;
    for (const auto& v : venues_) {
        total += v.order_share;
        cumulative_share_.push_back(total);
    }
    for (double& c : cumulative_share_) c /= total;  // normalise so the last entry is exactly 1
}

std::size_t MarketSimulator::pick_venue(OrderId id) const {
    // Top 53 bits of the hash -> a number in [0, 1), then find its venue bucket.
    const double u = static_cast<double>(splitmix64(id ^ seed_) >> 11) * 0x1.0p-53;
    for (std::size_t i = 0; i + 1 < cumulative_share_.size(); ++i) {
        if (u < cumulative_share_[i]) return i;
    }
    return cumulative_share_.size() - 1;
}

// ---- Real market data -------------------------------------------------------------
// Each handler first moves the clock (so in-flight orders that arrived earlier see
// the book as it was then), then applies the message to the venue holding the order.

void MarketSimulator::on_stock_directory(const itch::StockDirectory& m) {
    advance_to(m.timestamp);
    if (m.symbol() == symbol_) locate_ = m.locate;
}

void MarketSimulator::on_add(const itch::AddOrder& m) {
    advance_to(m.timestamp);
    if (!is_ours(m.locate)) return;
    const std::size_t v = pick_venue(m.id);
    if (!books_[v].add(m.id, m.side, m.price, m.shares)) return count(false);
    venue_of_order_[m.id] = static_cast<std::uint8_t>(v);
    ++orders_assigned_[v];
}

void MarketSimulator::on_execute(const itch::OrderExecuted& m) {
    advance_to(m.timestamp);
    if (!is_ours(m.locate)) return;
    auto it = venue_of_order_.find(m.id);
    if (it == venue_of_order_.end()) return count(false);
    OrderBook& book = books_[it->second];
    count(book.execute(m.id, m.shares));
    // When the order is fully gone from its book, forget which venue it was on.
    if (!book.contains(m.id)) venue_of_order_.erase(it);
}

void MarketSimulator::on_cancel(const itch::OrderCancel& m) {
    advance_to(m.timestamp);
    if (!is_ours(m.locate)) return;
    auto it = venue_of_order_.find(m.id);
    if (it == venue_of_order_.end()) return count(false);
    OrderBook& book = books_[it->second];
    count(book.cancel(m.id, m.shares));
    if (!book.contains(m.id)) venue_of_order_.erase(it);
}

void MarketSimulator::on_delete(const itch::OrderDelete& m) {
    advance_to(m.timestamp);
    if (!is_ours(m.locate)) return;
    auto it = venue_of_order_.find(m.id);
    if (it == venue_of_order_.end()) return count(false);
    count(books_[it->second].remove(m.id));
    venue_of_order_.erase(it);
}

void MarketSimulator::on_replace(const itch::OrderReplace& m) {
    advance_to(m.timestamp);
    if (!is_ours(m.locate)) return;
    auto it = venue_of_order_.find(m.old_id);
    if (it == venue_of_order_.end()) return count(false);
    // The replacement stays on the same venue: it is the same order, re-priced.
    const std::uint8_t v = it->second;
    venue_of_order_.erase(it);
    const bool ok = books_[v].replace(m.old_id, m.new_id, m.price, m.shares);
    count(ok);
    if (ok) venue_of_order_[m.new_id] = v;
}

// ---- Our orders -------------------------------------------------------------------

void MarketSimulator::submit(const ChildOrder& order) {
    if (order.venue >= venues_.size()) throw std::out_of_range("unknown venue index");
    const std::uint64_t arrival = order.send_time + venues_[order.venue].latency_ns;
    in_flight_.push(InFlight{arrival, next_sequence_++, order});
}

void MarketSimulator::finish() {
    while (!in_flight_.empty()) {
        execute(in_flight_.top());
        in_flight_.pop();
    }
}

void MarketSimulator::advance_to(std::uint64_t t) {
    // Orders arriving at or before t execute against the book *before* the message at t.
    while (!in_flight_.empty() && in_flight_.top().arrival_time <= t) {
        execute(in_flight_.top());
        in_flight_.pop();
    }
    if (t > now_) now_ = t;
}

void MarketSimulator::execute(const InFlight& in_flight) {
    const ChildOrder& order = in_flight.order;
    const TakeResult taken = take_liquidity(books_[order.venue], order.side, order.qty, order.limit);

    Fill fill;
    fill.tag = order.tag;
    fill.venue = order.venue;
    fill.side = order.side;
    fill.requested = order.qty;
    fill.filled = taken.filled;
    fill.notional = taken.notional;
    fill.fees = venues_[order.venue].fee_per_share * static_cast<std::int64_t>(taken.filled);
    fill.send_time = order.send_time;
    fill.arrival_time = in_flight.arrival_time;
    fills_.push_back(fill);
}

}  // namespace apex
