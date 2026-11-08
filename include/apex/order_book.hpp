#pragma once
// Baseline limit order book for one stock on one venue.
//
// It is "aggregated by price": for every price we only keep the total shares and
// the number of orders. That is all a router needs ("how much can I buy at $10.01?")
// and it matches what NASDAQ ITCH messages let us rebuild.
//
// Operations map 1:1 to ITCH messages:
//   add      <- 'A' / 'F'  Add Order
//   execute  <- 'E' / 'C'  Order Executed
//   cancel   <- 'X'        Order Cancel (partial)
//   remove   <- 'D'        Order Delete
//   replace  <- 'U'        Order Replace

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "apex/types.hpp"

namespace apex {

class OrderBook {
public:
    // Adds a new resting order. Returns false if the id already exists or qty is 0.
    bool add(OrderId id, Side side, Price price, Quantity qty);

    // Removes `qty` shares from an order because they traded.
    // If the order reaches 0 shares it is removed. Returns false if id is unknown
    // or qty is larger than the order's remaining shares.
    bool execute(OrderId id, Quantity qty);

    // Removes `qty` shares because the owner cancelled part of the order.
    // Same rules as execute (they differ only in meaning, which analytics will use).
    bool cancel(OrderId id, Quantity qty);

    // Removes the whole order. Returns false if id is unknown.
    bool remove(OrderId id);

    // Replaces an order with a new id, price and size, keeping the same side.
    // Returns false if old_id is unknown or new_id already exists.
    bool replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty);

    // Highest buy price level / lowest sell price level, if any.
    std::optional<Level> best_bid() const;
    std::optional<Level> best_ask() const;

    // Up to `n` best levels on one side, best first. Used by the router.
    std::vector<Level> depth(Side side, std::size_t n) const;

    std::size_t order_count() const { return orders_.size(); }
    std::size_t level_count(Side side) const;

private:
    // What we remember about each live order.
    struct Order {
        Side side;
        Price price;
        Quantity qty;
    };

    // Shared logic for execute() and cancel().
    bool reduce(OrderId id, Quantity qty);

    // Adds (+) or removes (-) shares at a price level, erasing empty levels.
    void add_to_level(Side side, Price price, Quantity qty);
    void remove_from_level(Side side, Price price, Quantity qty, bool order_gone);

    // id -> order: ITCH execute/cancel/delete only carry the id, so lookup must be O(1).
    std::unordered_map<OrderId, Order> orders_;

    // Bids sorted high -> low and asks low -> high, so begin() is always the best price.
    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level, std::less<Price>> asks_;
};

}  // namespace apex
