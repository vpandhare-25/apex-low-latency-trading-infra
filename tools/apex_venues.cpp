// apex_venues: splits one stock's real NASDAQ orders across three simulated venues,
// shows their books, and measures how latency affects fills.
//
// Usage: apex_venues <file.NASDAQ_ITCH50[.gz]> <SYMBOL>
//
// The "quote probe" experiment: every minute from 09:45 to 15:45, look at each venue's
// best ask and immediately send a buy for exactly that many shares at exactly that
// price. The order reaches the venue after the venue's latency. If other traders took
// or cancelled those shares in the meantime, we get fewer. Slow venues should show
// lower fill rates; how much lower is what this measures.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"
#include "apex/market_simulator.hpp"
#include "apex/venue.hpp"

using namespace apex;

namespace {

constexpr std::uint64_t kSecond = 1'000'000'000ull;
constexpr std::uint64_t kMinute = 60 * kSecond;
constexpr std::uint64_t kHour = 60 * kMinute;

void print_price(Price p) {
    std::printf("%lld.%04lld", static_cast<long long>(p / 10000), static_cast<long long>(p % 10000));
}

void print_venue_books(const MarketSimulator& sim) {
    for (std::size_t v = 0; v < sim.venue_count(); ++v) {
        const auto bid = sim.book(v).best_bid();
        const auto ask = sim.book(v).best_ask();
        std::printf("  venue %s:  bid ", sim.venue(v).name.c_str());
        if (bid) { print_price(bid->price); std::printf(" x %-6u", bid->quantity); } else std::printf("  (none)      ");
        std::printf("  ask ");
        if (ask) { print_price(ask->price); std::printf(" x %u", ask->quantity); } else std::printf("  (none)");
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <itch_file> <SYMBOL>\n", argv[0]);
        return 2;
    }
    itch::ItchFileReader reader(argv[1]);
    if (!reader.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }

    MarketSimulator sim(argv[2], default_venues());
    const std::uint64_t first_probe = 9 * kHour + 45 * kMinute;
    const std::uint64_t last_probe = 15 * kHour + 45 * kMinute;
    const std::uint64_t snapshot_time = 12 * kHour;
    std::uint64_t next_probe = first_probe;
    bool snapshot_done = false;
    std::uint64_t tag = 0;

    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    while (reader.next(data, len)) {
        if (len >= 11) {
            const std::uint64_t ts = itch::read_u48(data + 5);
            if (!snapshot_done && ts >= snapshot_time) {
                snapshot_done = true;
                std::printf("venue books at 12:00 (%s):\n", argv[2]);
                print_venue_books(sim);
            }
            // Books are up to date as of the previous message, so this is what a
            // router would see at next_probe.
            while (next_probe <= last_probe && ts >= next_probe) {
                for (std::size_t v = 0; v < sim.venue_count(); ++v) {
                    if (const auto ask = sim.book(v).best_ask()) {
                        sim.submit({tag++, v, Side::Buy, ask->quantity, ask->price, next_probe});
                    }
                }
                next_probe += kMinute;
            }
        }
        itch::parse_message(data, len, sim);
    }
    sim.finish();

    if (!sim.locate()) {
        std::printf("symbol %s not found\n", argv[2]);
        return 1;
    }
    std::printf("\nrejected book updates: %llu\n", (unsigned long long)sim.rejected());

    // ---- Summarise the probes per venue ----
    struct Totals { std::uint64_t probes = 0, full = 0, requested = 0, filled = 0; };
    std::vector<Totals> totals(sim.venue_count());
    for (const Fill& f : sim.fills()) {
        Totals& t = totals[f.venue];
        ++t.probes;
        t.requested += f.requested;
        t.filled += f.filled;
        if (f.filled == f.requested) ++t.full;
    }

    std::printf("\nquote probes, 09:45-15:45 every minute (buy the whole best ask):\n");
    std::printf("  venue  orders  latency   fee/share  real orders  fully filled  shares filled\n");
    for (std::size_t v = 0; v < sim.venue_count(); ++v) {
        const VenueConfig& cfg = sim.venue(v);
        const Totals& t = totals[v];
        std::printf("  %-5s  %6.0f%%  %6llu us  $0.%04lld    %9llu    %5.1f%% (%llu/%llu)  %5.1f%%\n",
                    cfg.name.c_str(), cfg.order_share * 100, (unsigned long long)(cfg.latency_ns / 1000),
                    (long long)cfg.fee_per_share, (unsigned long long)sim.orders_assigned()[v],
                    t.probes ? 100.0 * t.full / t.probes : 0.0, (unsigned long long)t.full,
                    (unsigned long long)t.probes, t.requested ? 100.0 * t.filled / t.requested : 0.0);
    }
    return 0;
}
