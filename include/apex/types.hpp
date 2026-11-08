#pragma once
// Basic types shared by every Apex component.

#include <cstdint>

namespace apex {

// Price in ITCH units: 1/10000 of a dollar. $12.3400 is stored as 123400.
// Integers avoid floating-point rounding errors when comparing prices.
using Price = std::int64_t;

// Number of shares.
using Quantity = std::uint32_t;

// Exchange-assigned order reference number (unique per trading day in ITCH).
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

// One row of the book: total shares resting at a single price.
struct Level {
    Price price = 0;
    Quantity quantity = 0;     // sum of shares of all orders at this price
    std::uint32_t order_count = 0;

    bool operator==(const Level&) const = default;
};

}  // namespace apex
