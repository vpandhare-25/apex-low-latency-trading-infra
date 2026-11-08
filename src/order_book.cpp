#include "apex/order_book.hpp"

#include <algorithm>

namespace apex {

namespace {

// Copies up to n levels from a sorted map into a vector (best price first).
template <typename Map>
std::vector<Level> top_levels(const Map& levels, std::size_t n) {
    std::vector<Level> out;
    out.reserve(std::min(n, levels.size()));
    for (const auto& [price, level] : levels) {
        if (out.size() == n) break;
        out.push_back(level);
    }
    return out;
}

}  // namespace

bool OrderBook::add(OrderId id, Side side, Price price, Quantity qty) {
    if (qty == 0) return false;
    // try_emplace does nothing if the id is already present.
    auto [it, inserted] = orders_.try_emplace(id, Order{side, price, qty});
    if (!inserted) return false;
    add_to_level(side, price, qty);
    return true;
}

bool OrderBook::execute(OrderId id, Quantity qty) { return reduce(id, qty); }

bool OrderBook::cancel(OrderId id, Quantity qty) { return reduce(id, qty); }

bool OrderBook::reduce(OrderId id, Quantity qty) {
    auto it = orders_.find(id);
    if (it == orders_.end() || qty == 0 || qty > it->second.qty) return false;

    Order& order = it->second;
    order.qty -= qty;
    const bool order_gone = (order.qty == 0);
    remove_from_level(order.side, order.price, qty, order_gone);
    if (order_gone) orders_.erase(it);
    return true;
}

bool OrderBook::remove(OrderId id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;
    const Order& order = it->second;
    remove_from_level(order.side, order.price, order.qty, /*order_gone=*/true);
    orders_.erase(it);
    return true;
}

bool OrderBook::replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty) {
    auto it = orders_.find(old_id);
    if (it == orders_.end()) return false;
    // new_id must be free (unless it is the same id being re-priced).
    if (new_id != old_id && orders_.count(new_id) != 0) return false;
    if (new_qty == 0) return false;

    const Side side = it->second.side;  // ITCH replace keeps the original side
    remove(old_id);
    return add(new_id, side, new_price, new_qty);
}

std::optional<Level> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->second;
}

std::optional<Level> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->second;
}

std::vector<Level> OrderBook::depth(Side side, std::size_t n) const {
    return side == Side::Buy ? top_levels(bids_, n) : top_levels(asks_, n);
}

std::size_t OrderBook::level_count(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

void OrderBook::add_to_level(Side side, Price price, Quantity qty) {
    // operator[] creates an empty Level the first time we see this price.
    Level& level = (side == Side::Buy) ? bids_[price] : asks_[price];
    level.price = price;
    level.quantity += qty;
    level.order_count += 1;
}

void OrderBook::remove_from_level(Side side, Price price, Quantity qty, bool order_gone) {
    // A generic lambda lets the same code work for both map types.
    auto update = [&](auto& levels) {
        auto it = levels.find(price);
        if (it == levels.end()) return;  // cannot happen if orders_ is consistent
        it->second.quantity -= qty;
        if (order_gone) it->second.order_count -= 1;
        if (it->second.order_count == 0) levels.erase(it);
    };
    if (side == Side::Buy) update(bids_); else update(asks_);
}

}  // namespace apex
