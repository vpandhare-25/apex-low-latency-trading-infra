#include "apex/venue.hpp"

#include <algorithm>

namespace apex {

std::vector<VenueConfig> default_venues() {
    return {
        //  name  share  fee ($/share x 10000)  latency
        {"A", 0.50, 30, 50'000},     // $0.0030/share, 50 us
        {"B", 0.20, 10, 20'000},     // $0.0010/share, 20 us
        {"C", 0.30, 20, 1'000'000},  // $0.0020/share, 1 ms
    };
}

TakeResult take_liquidity(const OrderBook& book, Side our_side, Quantity qty, Price limit) {
    // A buy takes from the asks (cheapest first); a sell takes from the bids (highest first).
    const Side resting_side = (our_side == Side::Buy) ? Side::Sell : Side::Buy;

    TakeResult result;
    book.for_each_level(resting_side, [&](const Level& level) {
        const bool too_expensive = (our_side == Side::Buy) ? level.price > limit : level.price < limit;
        if (too_expensive) return false;

        const Quantity take = std::min(level.quantity, qty - result.filled);
        result.filled += take;
        result.notional += level.price * static_cast<std::int64_t>(take);
        return result.filled < qty;  // keep walking only while shares are still needed
    });
    return result;
}

}  // namespace apex
