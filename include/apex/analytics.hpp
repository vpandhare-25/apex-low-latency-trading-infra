#pragma once
// Execution analytics: the "report card" for a router.
//
// Cost is measured against the arrival mid price: the midpoint of the best bid and best
// ask (across all venues) when the router made its decision. Paying above the mid when
// buying, or receiving below it when selling, is a cost. Fees are included, so this is
// the "all-in" cost a real trader cares about.
//
//   buy cost  = (money paid + fees)        - mid * shares
//   sell cost = mid * shares - (money received - fees)
//   cost in basis points (bps) = cost / (mid * shares) * 10000     (1 bps = 0.01%)

#include <cstdint>
#include <vector>

#include "apex/venue.hpp"

namespace apex {

class ExecutionStats {
public:
    // Records one parent order. `fills` are the fills of all its child orders.
    void add_parent(Side side, Quantity requested, double arrival_mid, const std::vector<Fill>& fills);

    std::uint64_t parents() const { return parents_; }
    std::uint64_t requested() const { return requested_; }
    std::uint64_t filled() const { return filled_; }
    std::uint64_t child_orders() const { return child_orders_; }
    double fill_rate() const { return requested_ ? double(filled_) / double(requested_) : 0.0; }

    // Totals in dollars.
    double cost_dollars() const { return cost_ / 10000.0; }
    double fees_dollars() const { return double(fees_) / 10000.0; }
    // Average cost per filled share in bps of the mid (share-weighted).
    double cost_bps() const { return mid_value_ > 0 ? cost_ / mid_value_ * 10000.0 : 0.0; }

private:
    std::uint64_t parents_ = 0, requested_ = 0, filled_ = 0, child_orders_ = 0;
    double cost_ = 0;       // ITCH units
    std::int64_t fees_ = 0; // ITCH units
    double mid_value_ = 0;  // sum of mid * filled shares, ITCH units
};

// Collects timing samples (nanoseconds) and reports percentiles.
class LatencyStats {
public:
    void add(std::uint64_t ns) { samples_.push_back(ns); }
    void merge(const LatencyStats& other) { samples_.insert(samples_.end(), other.samples_.begin(), other.samples_.end()); }
    std::size_t count() const { return samples_.size(); }
    // p in [0, 100], e.g. 50, 99, 99.9. Uses the "nearest rank" method on sorted samples.
    std::uint64_t percentile(double p) const;

private:
    std::vector<std::uint64_t> samples_;
};

}  // namespace apex
