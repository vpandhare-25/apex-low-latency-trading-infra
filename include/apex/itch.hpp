#pragma once
// Decoder for NASDAQ TotalView-ITCH 5.0 binary messages.
//
// ITCH is the feed NASDAQ uses to publish every order event. Each message is a
// fixed-size block of bytes. Byte 0 is the message type (a letter), and numbers are
// stored big-endian (most significant byte first), so we decode them by hand.
//
// Every message starts with the same 11-byte header:
//   offset 0  type            1 byte  ('A', 'E', 'D', ...)
//   offset 1  stock locate    2 bytes (small integer id for the stock, set by 'R')
//   offset 3  tracking number 2 bytes (not used here)
//   offset 5  timestamp       6 bytes (nanoseconds since midnight)
//
// We decode only the messages that change the order book, plus the stock directory
// ('R'), which tells us which locate number belongs to which ticker symbol.
// Reference: NASDAQ TotalView-ITCH 5.0 specification.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "apex/types.hpp"

namespace apex::itch {

// ---- Big-endian helpers -------------------------------------------------------

inline std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t read_u32(const std::uint8_t* p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

// 6-byte timestamp.
inline std::uint64_t read_u48(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}

inline std::uint64_t read_u64(const std::uint8_t* p) {
    return (std::uint64_t{read_u32(p)} << 32) | read_u32(p + 4);
}

// ---- Decoded message structs --------------------------------------------------

// 'R' Stock Directory: maps a locate number to a ticker symbol.
struct StockDirectory {
    std::uint16_t locate;
    std::uint64_t timestamp;
    char stock[8];  // ticker, padded with spaces on the right ("AAPL    ")

    // Ticker without the trailing spaces.
    std::string_view symbol() const {
        std::size_t n = 8;
        while (n > 0 && stock[n - 1] == ' ') --n;
        return {stock, n};
    }
};

// 'A' Add Order and 'F' Add Order with MPID attribution.
struct AddOrder {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId id;
    Side side;
    Quantity shares;
    Price price;
};

// 'E' Order Executed and 'C' Order Executed With Price.
struct OrderExecuted {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId id;
    Quantity shares;
    bool has_price;   // true for 'C' (executed at a price different from the order's)
    Price price;      // only valid when has_price
};

// 'X' Order Cancel (partial cancel).
struct OrderCancel {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId id;
    Quantity shares;
};

// 'D' Order Delete (whole order removed).
struct OrderDelete {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId id;
};

// 'U' Order Replace: old order is removed, new order added with a new id.
struct OrderReplace {
    std::uint16_t locate;
    std::uint64_t timestamp;
    OrderId old_id;
    OrderId new_id;
    Quantity shares;
    Price price;
};

// Exact message sizes from the spec (bytes, not counting the 2-byte length prefix).
inline constexpr std::size_t kStockDirectorySize = 39;
inline constexpr std::size_t kAddOrderSize = 36;
inline constexpr std::size_t kAddOrderMpidSize = 40;
inline constexpr std::size_t kOrderExecutedSize = 31;
inline constexpr std::size_t kOrderExecutedPriceSize = 36;
inline constexpr std::size_t kOrderCancelSize = 23;
inline constexpr std::size_t kOrderDeleteSize = 19;
inline constexpr std::size_t kOrderReplaceSize = 35;

enum class ParseResult {
    Handled,    // a book message or stock directory, passed to the handler
    Ignored,    // valid message type we do not need (trades, system events, ...)
    BadLength,  // message shorter than its type requires
};

// Decodes one message and calls the matching handler method:
//   on_stock_directory, on_add, on_execute, on_cancel, on_delete, on_replace.
// It is a template so the compiler can inline the handler calls (no virtual calls).
template <typename Handler>
ParseResult parse_message(const std::uint8_t* p, std::size_t len, Handler& handler) {
    if (len == 0) return ParseResult::BadLength;

    // Header fields shared by all messages (only read once we know len is large enough).
    auto locate = [&] { return read_u16(p + 1); };
    auto timestamp = [&] { return read_u48(p + 5); };

    switch (p[0]) {
        case 'R': {
            if (len < kStockDirectorySize) return ParseResult::BadLength;
            StockDirectory m{locate(), timestamp(), {}};
            for (int i = 0; i < 8; ++i) m.stock[i] = static_cast<char>(p[11 + i]);
            handler.on_stock_directory(m);
            return ParseResult::Handled;
        }
        case 'A':
        case 'F': {  // 'F' is 'A' plus 4 bytes of market-participant id at the end
            if (len < (p[0] == 'A' ? kAddOrderSize : kAddOrderMpidSize)) return ParseResult::BadLength;
            AddOrder m{locate(), timestamp(), read_u64(p + 11),
                       p[19] == 'B' ? Side::Buy : Side::Sell,
                       read_u32(p + 20),
                       static_cast<Price>(read_u32(p + 32))};
            handler.on_add(m);
            return ParseResult::Handled;
        }
        case 'E':
        case 'C': {
            const bool with_price = (p[0] == 'C');
            if (len < (with_price ? kOrderExecutedPriceSize : kOrderExecutedSize)) return ParseResult::BadLength;
            OrderExecuted m{locate(), timestamp(), read_u64(p + 11), read_u32(p + 19), with_price,
                            with_price ? static_cast<Price>(read_u32(p + 32)) : 0};
            handler.on_execute(m);
            return ParseResult::Handled;
        }
        case 'X': {
            if (len < kOrderCancelSize) return ParseResult::BadLength;
            handler.on_cancel(OrderCancel{locate(), timestamp(), read_u64(p + 11), read_u32(p + 19)});
            return ParseResult::Handled;
        }
        case 'D': {
            if (len < kOrderDeleteSize) return ParseResult::BadLength;
            handler.on_delete(OrderDelete{locate(), timestamp(), read_u64(p + 11)});
            return ParseResult::Handled;
        }
        case 'U': {
            if (len < kOrderReplaceSize) return ParseResult::BadLength;
            handler.on_replace(OrderReplace{locate(), timestamp(), read_u64(p + 11), read_u64(p + 19),
                                            read_u32(p + 27), static_cast<Price>(read_u32(p + 31))});
            return ParseResult::Handled;
        }
        default:
            return ParseResult::Ignored;
    }
}

}  // namespace apex::itch
