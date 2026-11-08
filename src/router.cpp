#include "apex/router.hpp"

#include <algorithm>

namespace apex {

namespace {

// For our order on `side`, the venue's resting orders are on the other side.
Side resting_side(Side our_side) { return our_side == Side::Buy ? Side::Sell : Side::Buy; }

// Is price `a` better than `b` for us? Buyers want lower prices, sellers higher.
bool better(Side our_side, Price a, Price b) { return our_side == Side::Buy ? a < b : a > b; }

// Is this price acceptable given our limit?
bool within_limit(Side our_side, Price price, Price limit) {
    return our_side == Side::Buy ? price <= limit : price >= limit;
}

}  // namespace

std::vector<ChildOrder> SingleVenueRouter::route(const ParentOrder& parent, const MarketView& view) const {
    const Side other = resting_side(parent.side);
    std::size_t best_venue = 0;
    std::optional<Price> best_price;
    for (std::size_t v = 0; v < view.books.size(); ++v) {
        const auto top = (other == Side::Sell) ? view.books[v]->best_ask() : view.books[v]->best_bid();
        if (top && (!best_price || better(parent.side, top->price, *best_price))) {
            best_price = top->price;
            best_venue = v;
        }
    }
    if (!best_price || !within_limit(parent.side, *best_price, parent.limit)) return {};
    return {ChildOrder{0, best_venue, parent.side, parent.qty, parent.limit, parent.time}};
}

std::vector<ChildOrder> SplitRouter::route(const ParentOrder& parent, const MarketView& view) const {
    // One candidate per (venue, price level).
    struct Candidate {
        Price ranking_price;  // price used for sorting (includes fee if include_fees_)
        Price price;          // real price on the venue
        Quantity qty;
        std::size_t venue;
    };
    std::vector<Candidate> candidates;

    const Side other = resting_side(parent.side);
    for (std::size_t v = 0; v < view.books.size(); ++v) {
        // A fee makes buying more expensive and selling less profitable.
        const Price fee = include_fees_ ? (*view.venues)[v].fee_per_share : 0;
        const Price fee_effect = parent.side == Side::Buy ? fee : -fee;
        // Levels come best first, so stop at the first one past our limit, or once this
        // venue alone could fill the whole order (deeper levels can never be chosen).
        Quantity seen = 0;
        view.books[v]->for_each_level(other, [&](const Level& level) {
            if (!within_limit(parent.side, level.price, parent.limit)) return false;
            candidates.push_back({level.price + fee_effect, level.price, level.quantity, v});
            seen += level.quantity;
            return seen < parent.qty;
        });
    }

    // Best first. Ties go to the lower venue index so results are deterministic.
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
        if (a.ranking_price != b.ranking_price) return better(parent.side, a.ranking_price, b.ranking_price);
        return a.venue < b.venue;
    });

    // Take shares from the top until the parent is covered.
    std::vector<Quantity> qty_per_venue(view.books.size(), 0);
    std::vector<Price> worst_price_per_venue(view.books.size(), 0);
    Quantity remaining = parent.qty;
    for (const Candidate& c : candidates) {
        if (remaining == 0) break;
        const Quantity take = std::min(c.qty, remaining);
        qty_per_venue[c.venue] += take;
        worst_price_per_venue[c.venue] = c.price;  // later candidates on a venue are worse
        remaining -= take;
    }

    std::vector<ChildOrder> children;
    for (std::size_t v = 0; v < view.books.size(); ++v) {
        if (qty_per_venue[v] > 0) {
            children.push_back({0, v, parent.side, qty_per_venue[v], worst_price_per_venue[v], parent.time});
        }
    }
    return children;
}

}  // namespace apex
