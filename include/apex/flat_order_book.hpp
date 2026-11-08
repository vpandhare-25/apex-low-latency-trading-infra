#pragma once
// Cache-friendly order books: same behaviour and API as OrderBook, different storage.
//
// OrderBook keeps price levels in a std::map, a red-black tree. Every level is a
// separate heap allocation, and walking or searching the tree jumps around memory,
// which the CPU cache handles badly.
//
// BasicFlatOrderBook keeps each side's levels in one contiguous, sorted std::vector:
//   bids_: ascending price, so the BEST bid (highest) is at the back
//   asks_: descending price, so the BEST ask (lowest) is at the back
// Most activity happens near the best price, and with the best price at the *back*,
// inserting or erasing a level there moves only a few elements. Finding a level is a
// binary search over contiguous memory.
//
// The "order id -> order" map is a template parameter (see order_map.hpp):
//   FlatOrderBook = flat levels + std::unordered_map  (changes only the levels)
//   FastOrderBook = flat levels + FlatOrderMap        (changes levels and the hash map)
// so benchmarks can show what each change is worth on its own.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <vector>

#include "apex/order_map.hpp"
#include "apex/types.hpp"

namespace apex {

template <template <typename> class OrderMap>
class BasicFlatOrderBook {
public:
    bool add(OrderId id, Side side, Price price, Quantity qty) {
        if (qty == 0) return false;
        if (!orders_.insert(id, Order{side, price, qty})) return false;
        add_to_level(side, price, qty);
        return true;
    }

    bool execute(OrderId id, Quantity qty) { return reduce(id, qty); }
    bool cancel(OrderId id, Quantity qty) { return reduce(id, qty); }

    bool remove(OrderId id) {
        Order* order = orders_.find(id);
        if (order == nullptr) return false;
        remove_from_level(order->side, order->price, order->qty, /*order_gone=*/true);
        orders_.erase(id);
        return true;
    }

    bool replace(OrderId old_id, OrderId new_id, Price new_price, Quantity new_qty) {
        Order* order = orders_.find(old_id);
        if (order == nullptr) return false;
        if (new_id != old_id && orders_.find(new_id) != nullptr) return false;
        if (new_qty == 0) return false;
        const Side side = order->side;  // ITCH replace keeps the original side
        remove(old_id);
        return add(new_id, side, new_price, new_qty);
    }

    std::optional<Level> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.back();
    }
    std::optional<Level> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.back();
    }

    std::vector<Level> depth(Side side, std::size_t n) const {
        std::vector<Level> out;
        for_each_level(side, [&](const Level& l) {
            if (out.size() == n) return false;
            out.push_back(l);
            return true;
        });
        return out;
    }

    // Best price first, stop when visit returns false (same contract as OrderBook).
    template <typename Visit>
    void for_each_level(Side side, Visit&& visit) const {
        const auto& levels = (side == Side::Buy) ? bids_ : asks_;
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
            if (!visit(*it)) return;
        }
    }

    bool contains(OrderId id) const { return orders_.find(id) != nullptr; }
    std::size_t order_count() const { return orders_.size(); }
    std::size_t level_count(Side side) const { return side == Side::Buy ? bids_.size() : asks_.size(); }

private:
    struct Order {
        Side side;
        Price price;
        Quantity qty;
    };

    bool reduce(OrderId id, Quantity qty) {
        Order* order = orders_.find(id);
        if (order == nullptr || qty == 0 || qty > order->qty) return false;
        order->qty -= qty;
        const bool order_gone = (order->qty == 0);
        remove_from_level(order->side, order->price, qty, order_gone);
        if (order_gone) orders_.erase(id);
        return true;
    }

    // Where `price` is (or would go) in a side's sorted vector.
    static typename std::vector<Level>::iterator find_slot(std::vector<Level>& levels, Side side, Price price) {
        if (side == Side::Buy) {
            return std::lower_bound(levels.begin(), levels.end(), price,
                                    [](const Level& l, Price p) { return l.price < p; });
        }
        return std::lower_bound(levels.begin(), levels.end(), price,
                                [](const Level& l, Price p) { return l.price > p; });
    }

    void add_to_level(Side side, Price price, Quantity qty) {
        auto& levels = (side == Side::Buy) ? bids_ : asks_;
        auto it = find_slot(levels, side, price);
        if (it == levels.end() || it->price != price) {
            it = levels.insert(it, Level{price, 0, 0});  // shifts only the better-priced levels
        }
        it->quantity += qty;
        it->order_count += 1;
    }

    void remove_from_level(Side side, Price price, Quantity qty, bool order_gone) {
        auto& levels = (side == Side::Buy) ? bids_ : asks_;
        auto it = find_slot(levels, side, price);
        if (it == levels.end() || it->price != price) return;  // cannot happen if orders_ is consistent
        it->quantity -= qty;
        if (order_gone) it->order_count -= 1;
        if (it->order_count == 0) levels.erase(it);
    }

    OrderMap<Order> orders_;
    std::vector<Level> bids_;  // ascending price, best at back
    std::vector<Level> asks_;  // descending price, best at back
};

using FlatOrderBook = BasicFlatOrderBook<StdOrderMap>;
using FastOrderBook = BasicFlatOrderBook<FlatOrderMap>;

}  // namespace apex
