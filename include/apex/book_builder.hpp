#pragma once
// Rebuilds the order book for one stock from a stream of ITCH messages.
//
// The ITCH feed contains every stock on NASDAQ. BookBuilder:
//   1. watches 'R' stock directory messages to learn the locate number of our ticker,
//   2. ignores messages for every other stock,
//   3. applies add / execute / cancel / delete / replace to its OrderBook,
//   4. keeps correctness counters so we can tell if the rebuilt book is sane.

#include <cstdint>
#include <optional>
#include <string>

#include "apex/itch.hpp"
#include "apex/order_book.hpp"

namespace apex {

class BookBuilder {
public:
    struct Stats {
        std::uint64_t adds = 0;
        std::uint64_t executes = 0;
        std::uint64_t cancels = 0;
        std::uint64_t deletes = 0;
        std::uint64_t replaces = 0;
        // Book operations that failed (unknown order id, too many shares, ...).
        // Should be 0 for a correct parser and book.
        std::uint64_t rejected = 0;
        // Updates after which best bid >= best ask ("locked" or "crossed" book).
        std::uint64_t crossed_updates = 0;
    };

    explicit BookBuilder(std::string symbol) : symbol_(std::move(symbol)) {}

    // ---- Handler methods called by itch::parse_message ----
    void on_stock_directory(const itch::StockDirectory& m);
    void on_add(const itch::AddOrder& m);
    void on_execute(const itch::OrderExecuted& m);
    void on_cancel(const itch::OrderCancel& m);
    void on_delete(const itch::OrderDelete& m);
    void on_replace(const itch::OrderReplace& m);

    const OrderBook& book() const { return book_; }
    const Stats& stats() const { return stats_; }
    std::optional<std::uint16_t> locate() const { return locate_; }

private:
    bool is_ours(std::uint16_t locate) const { return locate_ && *locate_ == locate; }

    // Records whether the operation worked and whether the book is now crossed.
    void after_update(bool ok);

    std::string symbol_;
    std::optional<std::uint16_t> locate_;  // unknown until we see our 'R' message
    OrderBook book_;
    Stats stats_;
};

}  // namespace apex
