// apex_route: compares routing strategies on a real NASDAQ day.
//
// Usage: apex_route <file.NASDAQ_ITCH50[.gz]> <SYMBOL> [shares_per_order=1000]
//
// Parent orders are created on two schedules between 09:45 and 15:45:
//   timer : every 30 seconds, alternating buy and sell
//   move  : right after the best bid or ask (across venues) changes, at most once per
//           30 seconds. A buy if the ask moved, otherwise a sell. These are the
//           moments when slow venues are most likely to have stale quotes.
// For every parent order, all strategies decide at the same moment on the same books,
// so the comparison is paired (same market, same time, same size).
// Each parent's limit is 10 bps (0.1%) away from the best opposite price at arrival.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "apex/analytics.hpp"
#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"
#include "apex/market_simulator.hpp"
#include "apex/order_manager.hpp"
#include "apex/router.hpp"

using namespace apex;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000ull;
constexpr std::uint64_t kMinute = 60 * kSecond;
constexpr std::uint64_t kHour = 60 * kMinute;
constexpr std::uint64_t kStart = 9 * kHour + 45 * kMinute;
constexpr std::uint64_t kEnd = 15 * kHour + 45 * kMinute;
constexpr std::uint64_t kGap = 30 * kSecond;
constexpr double kLimitBps = 10.0;

enum Schedule { kTimer = 0, kMove = 1 };
const char* kScheduleNames[] = {"timer (every 30 s)", "move (right after a quote change)"};

struct ParentInfo {
    Schedule schedule;
    Side side;
    Quantity qty;
    double mid;
    std::uint64_t time;
};

// A strategy = a router + how many waves it may send.
struct Strategy {
    std::string name;
    std::unique_ptr<Router> router;
    int max_waves;
};

// Best bid and best ask across all venues.
struct Nbbo {
    std::optional<Price> bid, ask;
};

Nbbo nbbo(const MarketSimulator& sim) {
    Nbbo n;
    for (std::size_t v = 0; v < sim.venue_count(); ++v) {
        if (auto b = sim.book(v).best_bid(); b && (!n.bid || b->price > *n.bid)) n.bid = b->price;
        if (auto a = sim.book(v).best_ask(); a && (!n.ask || a->price < *n.ask)) n.ask = a->price;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: %s <itch_file> <SYMBOL> [shares_per_order]\n", argv[0]);
        return 2;
    }
    const Quantity order_size = argc == 4 ? static_cast<Quantity>(std::atoi(argv[3])) : 1000;
    itch::ItchFileReader reader(argv[1]);
    if (!reader.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    const std::vector<VenueConfig> venues = default_venues();
    MarketSimulator sim(argv[2], venues);
    MarketView view{{}, &venues};
    for (std::size_t v = 0; v < sim.venue_count(); ++v) view.books.push_back(&sim.book(v));

    std::vector<Strategy> strategies;
    strategies.push_back({"single-venue", std::make_unique<SingleVenueRouter>(), 1});
    strategies.push_back({"split", std::make_unique<SplitRouter>(false), 1});
    strategies.push_back({"split+fees", std::make_unique<SplitRouter>(true), 1});
    strategies.push_back({"split+fees+reroute", std::make_unique<SplitRouter>(true), 3});
    const std::size_t n_strat = strategies.size();

    std::vector<OrderManager> managers;
    for (const Strategy& s : strategies) managers.emplace_back(*s.router, s.max_waves);

    std::vector<ParentInfo> parents;

    // Creates one parent order and lets every strategy start working it.
    auto send_parent = [&](Schedule schedule, Side side, std::uint64_t now) {
        const Nbbo n = nbbo(sim);
        if (!n.bid || !n.ask) return;  // need both sides to know the mid
        const Price far_side = side == Side::Buy ? *n.ask : *n.bid;
        const auto collar = static_cast<Price>(double(far_side) * kLimitBps / 10000.0);
        const ParentOrder parent{side, order_size, side == Side::Buy ? far_side + collar : far_side - collar, now};

        const std::uint64_t parent_index = parents.size();
        parents.push_back({schedule, side, order_size, (double(*n.bid) + double(*n.ask)) / 2.0, now});
        // Rotate which strategy goes first, so no router always pays for a cold cache.
        for (std::size_t k = 0; k < n_strat; ++k) {
            const std::size_t s = (k + parent_index) % n_strat;
            managers[s].start(parent_index * n_strat + s, parent, view, sim);  // tag -> parent+strategy
        }
    };

    std::uint64_t next_timer = kStart;
    bool timer_buys = true;
    std::uint64_t last_move_parent = 0;
    Nbbo last_quote;

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    while (reader.next(data, len)) {
        if (len < 11) continue;
        const std::uint64_t ts = itch::read_u48(data + 5);

        // Timer schedule: decide using the books as of just before this message.
        while (next_timer <= kEnd && ts >= next_timer) {
            send_parent(kTimer, timer_buys ? Side::Buy : Side::Sell, next_timer);
            timer_buys = !timer_buys;
            next_timer += kGap;
        }

        itch::parse_message(data, len, sim);

        // Fill reports that have come back by now may trigger the next wave.
        for (OrderManager& m : managers) m.poll(ts, view, sim);

        // Move schedule: only messages for our stock can change our quotes.
        if (!sim.locate() || itch::read_u16(data + 1) != *sim.locate()) continue;
        const Nbbo q = nbbo(sim);
        const bool ask_moved = q.ask != last_quote.ask;
        const bool bid_moved = q.bid != last_quote.bid;
        last_quote = q;
        if ((ask_moved || bid_moved) && ts >= kStart && ts <= kEnd && ts >= last_move_parent + kGap) {
            send_parent(kMove, ask_moved ? Side::Buy : Side::Sell, ts);
            last_move_parent = ts;
        }
    }
    sim.finish();
    if (!sim.locate()) {
        std::printf("symbol %s not found\n", argv[2]);
        return 1;
    }

    // ---- Group fills by (parent, strategy) and build the report card ----
    std::unordered_map<std::uint64_t, std::vector<Fill>> fills_by_tag;
    for (const Fill& f : sim.fills()) fills_by_tag[f.tag].push_back(f);

    std::vector<ExecutionStats> stats[2] = {std::vector<ExecutionStats>(n_strat),
                                            std::vector<ExecutionStats>(n_strat)};
    // Simulated time from the decision to the last child's execution.
    std::vector<LatencyStats> finish_ns[2] = {std::vector<LatencyStats>(n_strat),
                                              std::vector<LatencyStats>(n_strat)};
    const std::vector<Fill> no_fills;
    for (std::uint64_t p = 0; p < parents.size(); ++p) {
        const ParentInfo& info = parents[p];
        for (std::size_t s = 0; s < n_strat; ++s) {
            const auto it = fills_by_tag.find(p * n_strat + s);
            const std::vector<Fill>& fills = it == fills_by_tag.end() ? no_fills : it->second;
            stats[info.schedule][s].add_parent(info.side, info.qty, info.mid, fills);
            std::uint64_t last = 0;
            for (const Fill& f : fills) last = std::max(last, f.arrival_time);
            if (last > 0) finish_ns[info.schedule][s].add(last - info.time);
        }
    }

    std::printf("%s, %u shares per parent order, limit %.0f bps, rejected book updates: %llu\n",
                argv[2], order_size, kLimitBps, (unsigned long long)sim.rejected());
    for (int sc = 0; sc < 2; ++sc) {
        std::printf("\nschedule: %s, %llu parent orders\n", kScheduleNames[sc],
                    (unsigned long long)stats[sc][0].parents());
        std::printf("  strategy             fill rate  children/order  cost vs mid   total cost  of which fees"
                    "   time to finish p50 / p99\n");
        for (std::size_t s = 0; s < n_strat; ++s) {
            const ExecutionStats& st = stats[sc][s];
            std::printf("  %-19s  %8.2f%%  %14.2f  %7.3f bps  %10.2f $  %11.2f $   %8.1f / %8.1f us\n",
                        strategies[s].name.c_str(), st.fill_rate() * 100,
                        st.parents() ? double(st.child_orders()) / st.parents() : 0.0, st.cost_bps(),
                        st.cost_dollars(), st.fees_dollars(), finish_ns[sc][s].percentile(50) / 1000.0,
                        finish_ns[sc][s].percentile(99) / 1000.0);
        }
        const ExecutionStats& base = stats[sc][0];
        for (std::size_t s = 2; s < n_strat; ++s) {
            const ExecutionStats& st = stats[sc][s];
            std::printf("  %s vs single-venue: %.3f bps cheaper per filled share, fill rate %+.2f points\n",
                        strategies[s].name.c_str(), base.cost_bps() - st.cost_bps(),
                        (st.fill_rate() - base.fill_rate()) * 100);
        }
    }

    // Where were shares missed? Shares filled / sent per venue, for each strategy.
    std::vector<std::vector<std::uint64_t>> req[2], got[2];
    for (int sc = 0; sc < 2; ++sc) {
        req[sc].assign(n_strat, std::vector<std::uint64_t>(sim.venue_count(), 0));
        got[sc].assign(n_strat, std::vector<std::uint64_t>(sim.venue_count(), 0));
    }
    for (const Fill& f : sim.fills()) {
        const ParentInfo& info = parents[f.tag / n_strat];
        req[info.schedule][f.tag % n_strat][f.venue] += f.requested;
        got[info.schedule][f.tag % n_strat][f.venue] += f.filled;
    }
    std::printf("\nshares filled / sent, per venue:\n");
    for (int sc = 0; sc < 2; ++sc) {
        std::printf("  %s\n", kScheduleNames[sc]);
        for (std::size_t s = 0; s < n_strat; ++s) {
            std::printf("    %-19s", strategies[s].name.c_str());
            for (std::size_t v = 0; v < sim.venue_count(); ++v) {
                const std::uint64_t sent = req[sc][s][v];
                std::printf("  %s: %5.1f%% of %8llu", venues[v].name.c_str(),
                            sent ? 100.0 * got[sc][s][v] / sent : 0.0, (unsigned long long)sent);
            }
            std::printf("\n");
        }
    }

    std::printf("\nfirst-wave routing decision time (in-process, one call to route(), steady_clock):\n");
    std::printf("  strategy             samples     p50      p99    p99.9    waves/parent\n");
    for (std::size_t s = 0; s < n_strat; ++s) {
        const LatencyStats& l = managers[s].first_wave_decision_ns();
        std::printf("  %-19s  %7zu  %6llu ns %6llu ns %6llu ns   %.3f\n", strategies[s].name.c_str(), l.count(),
                    (unsigned long long)l.percentile(50), (unsigned long long)l.percentile(99),
                    (unsigned long long)l.percentile(99.9),
                    parents.empty() ? 0.0 : double(managers[s].waves_sent()) / double(parents.size()));
    }
    return 0;
}
