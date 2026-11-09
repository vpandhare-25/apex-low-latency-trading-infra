#pragma once
// Cache-friendly order books: same behaviour and API as OrderBook, different storage.
//
// A book here = an "order id -> order" map + one "level store" per side. Both are template
// parameters, so we can swap one piece at a time and measure what each change is worth:
//
//   FlatOrderBook   = VectorLevels + std::unordered_map   (changes only the levels)
//   FastOrderBook   = VectorLevels + FlatOrderMap         (also changes the hash map)
//   HybridOrderBook = HybridLevels + FlatOrderMap         (fixes VectorLevels' slow tail)
//
// Level stores (one per side, kBid = true for bids):
//
//   VectorLevels  every level in one sorted std::vector with the BEST price at the BACK.
//                 Near the best price, insert/erase moves only a few elements and the top
//                 of the book is in one or two cache lines. But real books are thousands
//                 of levels deep, and creating a level k places from the best shifts those
//                 k levels: rare, but it makes the p99 slow (see README, step 5).
//
//   HybridLevels  the best N levels in a VectorLevels ("hot"), everything worse in a
//                 std::map ("cold"). A shift now moves at most N elements, and deep levels
//                 cost O(log n) in the tree, like OrderBook. Invariants:
//                   * every hot price is better than every cold price
//                   * if cold is not empty, hot holds exactly N levels

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "apex/order_map.hpp"
#include "apex/types.hpp"

namespace apex {

// Is price a better than price b on this side? (Higher for bids, lower for asks.)
template <bool kBid>
constexpr bool better(Price a, Price b) {
    return kBid ? a > b : a < b;
}

template <bool kBid>
class VectorLevels {
public:
    // Adds shares at a price, creating the level if needed.
    void add(Price price, Quantity qty) {
        auto it = find(price);
        if (it == levels_.end() || it->price != price) {
            it = levels_.insert(it, Level{price, 0, 0});  // shifts the better-priced levels
        }
        it->quantity += qty;
        it->order_count += 1;
    }

    // Removes shares. Returns true if that emptied and erased the level.
    bool remove(Price price, Quantity qty, bool order_gone) {
        auto it = find(price);
        if (it == levels_.end() || it->price != price) return false;  // cannot happen if consistent
        it->quantity -= qty;
        if (order_gone) it->order_count -= 1;
        if (it->order_count != 0) return false;
        levels_.erase(it);
        return true;
    }

    bool has(Price price) const {
        auto it = const_cast<VectorLevels*>(this)->find(price);
        return it != levels_.end() && it->price == price;
    }

    const Level* best() const { return levels_.empty() ? nullptr : &levels_.back(); }
    std::size_t size() const { return levels_.size(); }

    // Best first, until visit returns false. Returns false if visit stopped early.
    template <typename Visit>
    bool for_each(Visit&& visit) const {
        for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
            if (!visit(*it)) return false;
        }
        return true;
    }

    // ---- Used by HybridLevels to move levels at the worst end ----
    const Level& worst() const { return levels_.front(); }
    Level pop_worst() {
        Level l = levels_.front();
        levels_.erase(levels_.begin());
        return l;
    }
    void push_worst(const Level& l) { levels_.insert(levels_.begin(), l); }

private:
    // Sorted worst -> best, so lower_bound with "is worse than" finds the slot.
    typename std::vector<Level>::iterator find(Price price) {
        return std::lower_bound(levels_.begin(), levels_.end(), price,
                                [](const Level& l, Price p) { return better<kBid>(p, l.price); });
    }

    std::vector<Level> levels_;
};

template <bool kBid, std::size_t N>
class HybridLevels {
    static_assert(N >= 1, "need at least one hot level");

public:
    void add(Price price, Quantity qty) {
        if (belongs_in_hot(price)) {
            hot_.add(price, qty);
            if (hot_.size() > N) {  // hot is too big: its worst level becomes cold
                const Level l = hot_.pop_worst();
                cold_.emplace(l.price, l);
            }
        } else {
            Level& l = cold_[price];
            l.price = price;
            l.quantity += qty;
            l.order_count += 1;
        }
    }

    bool remove(Price price, Quantity qty, bool order_gone) {
        if (hot_.has(price)) {
            const bool erased = hot_.remove(price, qty, order_gone);
            if (erased && !cold_.empty()) {  // refill hot with the best cold level
                hot_.push_worst(cold_.begin()->second);
                cold_.erase(cold_.begin());
            }
            return erased;
        }
        auto it = cold_.find(price);
        if (it == cold_.end()) return false;
        it->second.quantity -= qty;
        if (order_gone) it->second.order_count -= 1;
        if (it->second.order_count != 0) return false;
        cold_.erase(it);
        return true;
    }

    const Level* best() const { return hot_.best(); }  // hot is never empty while cold has levels
    std::size_t size() const { return hot_.size() + cold_.size(); }

    template <typename Visit>
    bool for_each(Visit&& visit) const {
        if (!hot_.for_each(visit)) return false;
        for (const auto& entry : cold_) {
            if (!visit(entry.second)) return false;
        }
        return true;
    }

private:
    bool belongs_in_hot(Price price) const {
        if (hot_.size() == 0) return true;
        if (!better<kBid>(hot_.worst().price, price)) return true;  // at least as good as worst hot
        return cold_.empty() && hot_.size() < N;                     // room left, nothing colder
    }

    // std::map ordered best first: greater<> for bids, less<> for asks.
    using Compare = std::conditional_t<kBid, std::greater<Price>, std::less<Price>>;

    VectorLevels<kBid> hot_;
    std::map<Price, Level, Compare> cold_;
};

// Lets HybridLevels<_, N> be passed where a "template <bool> class" is expected.
template <std::size_t N>
struct HybridOf {
    template <bool kBid>
    using type = HybridLevels<kBid, N>;
};

template <template <typename> class OrderMap, template <bool> class Levels>
class BasicFlatOrderBook {
public:
    bool add(OrderId id, Side side, Price price, Quantity qty) {
        if (qty == 0) return false;
        if (!orders_.insert(id, Order{side, price, qty})) return false;
        if (side == Side::Buy) bids_.add(price, qty); else asks_.add(price, qty);
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
        const Level* l = bids_.best();
        return l ? std::optional<Level>(*l) : std::nullopt;
    }
    std::optional<Level> best_ask() const {
        const Level* l = asks_.best();
        return l ? std::optional<Level>(*l) : std::nullopt;
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
        if (side == Side::Buy) bids_.for_each(visit); else asks_.for_each(visit);
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

    void remove_from_level(Side side, Price price, Quantity qty, bool order_gone) {
        if (side == Side::Buy) bids_.remove(price, qty, order_gone); else asks_.remove(price, qty, order_gone);
    }

    OrderMap<Order> orders_;
    Levels<true> bids_;
    Levels<false> asks_;
};

using FlatOrderBook = BasicFlatOrderBook<StdOrderMap, VectorLevels>;
using FastOrderBook = BasicFlatOrderBook<FlatOrderMap, VectorLevels>;
template <std::size_t N>
using HybridOrderBookN = BasicFlatOrderBook<FlatOrderMap, HybridOf<N>::template type>;
using HybridOrderBook = HybridOrderBookN<16>;  // best tail on real AAPL data, see README

}  // namespace apex
