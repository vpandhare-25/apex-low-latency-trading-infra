#pragma once
// OrderManager: works a parent order in "waves" and re-routes what did not fill.
//
// Wave 1: the router splits the parent across venues and we send the child orders.
// We then wait for every child's fill report. A report comes back one venue latency
// after the child executed (the message has to travel back to us), so a slow venue
// both executes late *and* reports late.
// If shares are still missing and waves remain, we ask the router again, now with the
// remaining quantity and the *current* books. A quote that vanished is no longer in the
// book, so the router sends the rest somewhere that still has liquidity. The parent's
// limit price never changes.
//
// With max_waves = 1 this is exactly the old behaviour (send once, accept what fills).

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "apex/analytics.hpp"
#include "apex/market_simulator.hpp"
#include "apex/router.hpp"

namespace apex {

class OrderManager {
public:
    OrderManager(const Router& router, int max_waves) : router_(router), max_waves_(max_waves) {}

    // Starts working a parent order. Every child order (from any wave) gets `tag`.
    void start(std::uint64_t tag, const ParentOrder& parent, const MarketView& view, MarketSimulator& sim);

    // Call after the simulator has processed messages up to time `now`. Reads new fill
    // reports and starts the next wave for every parent whose reports have all arrived.
    void poll(std::uint64_t now, const MarketView& view, MarketSimulator& sim);

    // Time taken by router.route() for each first-wave decision.
    const LatencyStats& first_wave_decision_ns() const { return first_wave_ns_; }
    // Number of waves sent, over all parents (for "average waves per parent").
    std::uint64_t waves_sent() const { return waves_sent_; }
    // Parents still being worked.
    std::size_t working() const { return working_.size(); }

private:
    struct Working {
        ParentOrder parent;       // original parent (side and limit are reused)
        Quantity remaining;       // shares not filled yet
        int wave;                 // waves sent so far
        int outstanding;          // child orders in this wave without a fill report
        std::uint64_t report_due; // when the last report of this wave reaches us
    };

    // Routes `qty` shares for the parent at time `now`. Returns false if nothing was sent.
    bool send_wave(std::uint64_t tag, Working& w, std::uint64_t now, const MarketView& view,
                   MarketSimulator& sim, bool first);

    const Router& router_;
    int max_waves_;
    std::unordered_map<std::uint64_t, Working> working_;
    std::size_t fills_seen_ = 0;  // how far into sim.fills() we have read
    LatencyStats first_wave_ns_;
    std::uint64_t waves_sent_ = 0;
};

}  // namespace apex
