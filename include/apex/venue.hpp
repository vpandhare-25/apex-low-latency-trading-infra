#pragma once
// Venues (exchanges), child orders, and fills.
//
// How the three venues are simulated
// ----------------------------------
// We only have real data from one exchange (NASDAQ). To get three venues that really
// differ, we *split* the real NASDAQ orders between them: every real order is placed
// on exactly one venue, chosen by a deterministic hash of its order id. So:
//   * each venue's book is a real slice of NASDAQ liquidity (real prices, real sizes,
//     real cancels and real trades taking liquidity away),
//   * the three books added together equal the real NASDAQ book (nothing counted twice),
//   * the venues' best prices differ naturally, because each holds different orders.
// On top of that, each venue has its own fee and its own latency (how long our order
// takes to arrive). Fee and latency values are illustrative assumptions, not the real
// fees of any exchange.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "apex/order_book.hpp"
#include "apex/types.hpp"

namespace apex {

struct VenueConfig {
    std::string name;
    double order_share;       // fraction of real orders that rest on this venue (sums to 1)
    Price fee_per_share;      // fee we pay to take liquidity, in ITCH units (30 = $0.0030)
    std::uint64_t latency_ns; // time from sending an order until the venue executes it
};

// Three venues with different trade-offs (assumed values, see the comment above):
//   A: most liquidity, standard fee, normal latency
//   B: little liquidity, cheap, very fast
//   C: medium liquidity, medium fee, slow ("far away", quotes may be gone on arrival)
std::vector<VenueConfig> default_venues();

// An order we send to one venue. It is "marketable": it takes resting liquidity at
// prices up to `limit` (buy) or down to `limit` (sell). Unfilled shares are cancelled
// (immediate-or-cancel), which is how routers usually probe venues.
struct ChildOrder {
    std::uint64_t tag;        // our own id, copied into the Fill so we can match them
    std::size_t venue;        // index into the venue list
    Side side;
    Quantity qty;
    Price limit;
    std::uint64_t send_time;  // ITCH time (ns since midnight) when we sent it
};

// What happened to a ChildOrder.
struct Fill {
    std::uint64_t tag = 0;
    std::size_t venue = 0;
    Side side = Side::Buy;
    Quantity requested = 0;
    Quantity filled = 0;
    std::int64_t notional = 0;  // sum of price * shares, in ITCH units
    std::int64_t fees = 0;      // fee_per_share * filled, in ITCH units
    std::uint64_t send_time = 0;
    std::uint64_t arrival_time = 0;
};

// Result of walking the book: how many shares we could take and their total cost.
struct TakeResult {
    Quantity filled = 0;
    std::int64_t notional = 0;
};

// Walks the opposite side of `book` best price first and takes up to `qty` shares at
// prices no worse than `limit`. Does not change the book (see MarketSimulator notes).
TakeResult take_liquidity(const OrderBook& book, Side our_side, Quantity qty, Price limit);

}  // namespace apex
