#include "apex/order_manager.hpp"

#include <algorithm>
#include <chrono>

namespace apex {

void OrderManager::start(std::uint64_t tag, const ParentOrder& parent, const MarketView& view,
                         MarketSimulator& sim) {
    Working w{parent, parent.qty, 0, 0, 0};
    if (send_wave(tag, w, parent.time, view, sim, /*first=*/true)) working_.emplace(tag, w);
}

bool OrderManager::send_wave(std::uint64_t tag, Working& w, std::uint64_t now, const MarketView& view,
                             MarketSimulator& sim, bool first) {
    ParentOrder request = w.parent;
    request.qty = w.remaining;
    request.time = now;

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ChildOrder> children = router_.route(request, view);
    const auto t1 = std::chrono::steady_clock::now();
    if (first) {
        first_wave_ns_.add(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    if (children.empty()) return false;

    for (ChildOrder& c : children) {
        c.tag = tag;
        sim.submit(c);
    }
    ++w.wave;
    ++waves_sent_;
    w.outstanding = static_cast<int>(children.size());
    w.report_due = 0;
    return true;
}

void OrderManager::poll(std::uint64_t now, const MarketView& view, MarketSimulator& sim) {
    if (working_.empty()) {
        fills_seen_ = sim.fills().size();  // nothing of ours can be in there
        return;
    }

    // 1. Read fill reports that appeared since the last poll.
    const auto& fills = sim.fills();
    for (; fills_seen_ < fills.size(); ++fills_seen_) {
        const Fill& f = fills[fills_seen_];
        auto it = working_.find(f.tag);
        if (it == working_.end()) continue;  // another manager's order
        Working& w = it->second;
        w.remaining -= f.filled;
        --w.outstanding;
        // The report travels back over the same link, so it takes the venue latency again.
        w.report_due = std::max(w.report_due, f.arrival_time + sim.venue(f.venue).latency_ns);
    }

    // 2. Parents whose whole wave has reported: finish them or send the next wave.
    for (auto it = working_.begin(); it != working_.end();) {
        Working& w = it->second;
        const bool wave_done = (w.outstanding == 0 && w.report_due <= now);
        if (!wave_done) { ++it; continue; }

        const bool more = w.remaining > 0 && w.wave < max_waves_ &&
                          send_wave(it->first, w, now, view, sim, /*first=*/false);
        if (more) ++it; else it = working_.erase(it);
    }
}

}  // namespace apex
