#include "apex/analytics.hpp"

#include <algorithm>
#include <cmath>

namespace apex {

void ExecutionStats::add_parent(Side side, Quantity requested, double arrival_mid, const std::vector<Fill>& fills) {
    ++parents_;
    requested_ += requested;
    for (const Fill& f : fills) {
        ++child_orders_;
        if (f.filled == 0) continue;
        filled_ += f.filled;
        fees_ += f.fees;
        const double at_mid = arrival_mid * f.filled;
        const double all_in = (side == Side::Buy) ? double(f.notional + f.fees) : double(f.notional - f.fees);
        cost_ += (side == Side::Buy) ? all_in - at_mid : at_mid - all_in;
        mid_value_ += at_mid;
    }
}

std::uint64_t LatencyStats::percentile(double p) const {
    if (samples_.empty()) return 0;
    std::vector<std::uint64_t> sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    // Nearest rank: the smallest sample with at least p% of samples at or below it.
    // The tiny epsilon stops floating-point error (99.9/100*1000 = 999.0000000000001)
    // from pushing the rank up by one.
    const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * double(sorted.size()) - 1e-9));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

}  // namespace apex
