#pragma once
// Order routers: turn one big "parent" order into child orders for the venues.
//
// Three routers, so we can see *which* idea saves money:
//   SingleVenueRouter        baseline: send everything to the venue with the best price
//   SplitRouter(fees=false)  look at every venue's levels and take the cheapest shares
//                            first, wherever they are (splitting across venues)
//   SplitRouter(fees=true)   same, but rank shares by price *including the venue fee*
//                            (the "smart order router")
//
// All routers see the same snapshot of the venue books at decision time, and every
// child order is immediate-or-cancel (see venue.hpp).

#include <memory>
#include <string>
#include <vector>

#include "apex/order_book.hpp"
#include "apex/venue.hpp"

namespace apex {

struct ParentOrder {
    Side side;
    Quantity qty;
    Price limit;              // never pay more than this (buy) / accept less than this (sell)
    std::uint64_t time;       // decision time (ns since midnight)
};

// What a router is allowed to see: every venue's book and its fee / latency.
struct MarketView {
    std::vector<const OrderBook*> books;  // books[i] belongs to venues[i]
    const std::vector<VenueConfig>* venues;
};

class Router {
public:
    virtual ~Router() = default;
    virtual std::string name() const = 0;
    // Returns child orders (tag is left 0; the caller assigns tags).
    virtual std::vector<ChildOrder> route(const ParentOrder& parent, const MarketView& view) const = 0;
};

// Baseline: pick the venue whose best price is best (ignoring fees and size) and send
// the whole order there, up to the parent's limit.
class SingleVenueRouter : public Router {
public:
    std::string name() const override { return "single-venue"; }
    std::vector<ChildOrder> route(const ParentOrder& parent, const MarketView& view) const override;
};

// Collects every price level on every venue (within the limit), sorts them best first,
// and fills the order from the top. Each venue gets one child order whose size is the
// shares chosen there and whose limit is the worst price chosen there.
class SplitRouter : public Router {
public:
    explicit SplitRouter(bool include_fees) : include_fees_(include_fees) {}
    std::string name() const override { return include_fees_ ? "split+fees" : "split"; }
    std::vector<ChildOrder> route(const ParentOrder& parent, const MarketView& view) const override;

private:
    bool include_fees_;
};

}  // namespace apex
