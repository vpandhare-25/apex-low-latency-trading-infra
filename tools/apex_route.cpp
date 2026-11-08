// apex_route: compares the three routers on a real NASDAQ day.
//
// Usage: apex_route <file.NASDAQ_ITCH50[.gz]> <SYMBOL> [shares_per_order=1000]
//
// Parent orders are created on two schedules between 09:45 and 15:45:
//   timer : every 30 seconds, alternating buy and sell
//   move  : right after the best bid or ask (across venues) changes, at most once per
//           30 seconds. A buy if the ask moved, otherwise a sell. These are the
//           moments when slow venues are most likely to have stale quotes.
// For every parent order, all three routers decide at the same moment on the same
// books, so the comparison is paired (same market, same time, same size).
// Each parent's limit is 10 bps (0.1%) away from the best opposite price at arrival.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "apex/analytics.hpp"
#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"
#include "apex/market_simulator.hpp"
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

    std::vector<std::unique_ptr<Router>> routers;
    routers.push_back(std::make_unique<SingleVenueRouter>());
    routers.push_back(std::make_unique<SplitRouter>(false));
    routers.push_back(std::make_unique<SplitRouter>(true));
    const std::size_t n_routers = routers.size();
    std::vector<LatencyStats> decision_ns(n_routers);

    std::vector<ParentInfo> parents;

    // Creates one parent order and lets every router route it.
    auto send_parent = [&](Schedule schedule, Side side, std::uint64_t now) {
        const Nbbo n = nbbo(sim);
        if (!n.bid || !n.ask) return;  // need both sides to know the mid
        const Price far_side = side == Side::Buy ? *n.ask : *n.bid;
        const auto collar = static_cast<Price>(double(far_side) * kLimitBps / 10000.0);
        const ParentOrder parent{side, order_size, side == Side::Buy ? far_side + collar : far_side - collar, now};

        MarketView view{{}, &venues};
        for (std::size_t v = 0; v < sim.venue_count(); ++v) view.books.push_back(&sim.book(v));

        const std::uint64_t parent_index = parents.size();
        parents.push_back({schedule, side, order_size, (double(*n.bid) + double(*n.ask)) / 2.0});
        // Rotate which router goes first, so no router always pays for a cold cache.
        for (std::size_t k = 0; k < n_routers; ++k) {
            const std::size_t r = (k + parent_index) % n_routers;
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<ChildOrder> children = routers[r]->route(parent, view);
            const auto t1 = std::chrono::steady_clock::now();
            decision_ns[r].add(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
            for (ChildOrder& c : children) {
                c.tag = parent_index * n_routers + r;  // lets us find this parent+router later
                sim.submit(c);
            }
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

    // ---- Group fills by (parent, router) and build the report card ----
    std::unordered_map<std::uint64_t, std::vector<Fill>> fills_by_tag;
    for (const Fill& f : sim.fills()) fills_by_tag[f.tag].push_back(f);

    ExecutionStats stats[2][3];
    for (std::uint64_t p = 0; p < parents.size(); ++p) {
        for (std::size_t r = 0; r < n_routers; ++r) {
            const auto it = fills_by_tag.find(p * n_routers + r);
            const std::vector<Fill> empty;
            stats[parents[p].schedule][r].add_parent(parents[p].side, parents[p].qty, parents[p].mid,
                                                     it == fills_by_tag.end() ? empty : it->second);
        }
    }

    std::printf("%s, %u shares per parent order, limit %.0f bps, rejected book updates: %llu\n",
                argv[2], order_size, kLimitBps, (unsigned long long)sim.rejected());
    for (int s = 0; s < 2; ++s) {
        std::printf("\nschedule: %s, %llu parent orders\n", kScheduleNames[s],
                    (unsigned long long)stats[s][0].parents());
        std::printf("  router         fill rate  children/order  cost vs mid   total cost  of which fees\n");
        for (std::size_t r = 0; r < n_routers; ++r) {
            const ExecutionStats& st = stats[s][r];
            std::printf("  %-13s  %8.2f%%  %14.2f  %7.3f bps  %10.2f $  %11.2f $\n", routers[r]->name().c_str(),
                        st.fill_rate() * 100, st.parents() ? double(st.child_orders()) / st.parents() : 0.0,
                        st.cost_bps(), st.cost_dollars(), st.fees_dollars());
        }
        const ExecutionStats& base = stats[s][0];
        const ExecutionStats& smart = stats[s][2];
        std::printf("  split+fees vs single-venue: %.3f bps cheaper per filled share, fill rate %+.2f points\n",
                    base.cost_bps() - smart.cost_bps(), (smart.fill_rate() - base.fill_rate()) * 100);
    }

    // Where were shares missed? Requested vs filled per venue, for each router.
    std::uint64_t req[2][3][8] = {}, got[2][3][8] = {};
    for (const Fill& f : sim.fills()) {
        const std::uint64_t p = f.tag / n_routers;
        const std::size_t r = f.tag % n_routers;
        req[parents[p].schedule][r][f.venue] += f.requested;
        got[parents[p].schedule][r][f.venue] += f.filled;
    }
    std::printf("\nshares filled / sent, per venue:\n");
    for (int s = 0; s < 2; ++s) {
        std::printf("  %s\n", kScheduleNames[s]);
        for (std::size_t r = 0; r < n_routers; ++r) {
            std::printf("    %-13s", routers[r]->name().c_str());
            for (std::size_t v = 0; v < sim.venue_count(); ++v) {
                const std::uint64_t sent = req[s][r][v];
                std::printf("  %s: %5.1f%% of %8llu", venues[v].name.c_str(),
                            sent ? 100.0 * got[s][r][v] / sent : 0.0, (unsigned long long)sent);
            }
            std::printf("\n");
        }
    }

    std::printf("\nrouting decision time (in-process, one call to route(), steady_clock):\n");
    std::printf("  router         samples     p50      p99    p99.9      max\n");
    for (std::size_t r = 0; r < n_routers; ++r) {
        const LatencyStats& l = decision_ns[r];
        std::printf("  %-13s  %7zu  %6llu ns %6llu ns %6llu ns %6llu ns\n", routers[r]->name().c_str(), l.count(),
                    (unsigned long long)l.percentile(50), (unsigned long long)l.percentile(99),
                    (unsigned long long)l.percentile(99.9), (unsigned long long)l.percentile(100));
    }
    return 0;
}
