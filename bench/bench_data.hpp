#pragma once
// Loads a single-stock ITCH file (made by apex_extract) into memory for benchmarks,
// so the measurements never include disk reads or decompression.

#include <cstdint>
#include <string>
#include <vector>

#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"
#include "apex/types.hpp"

namespace apex::bench {

// One decoded book message, ready to apply to any order book type.
struct BookEvent {
    char type;  // 'A' add, 'E' execute, 'X' cancel, 'D' delete, 'U' replace
    Side side;
    Quantity qty;
    Price price;
    OrderId id;
    OrderId new_id;
    std::uint64_t timestamp;
};

struct MarketData {
    std::vector<std::uint8_t> bytes;   // every message body, back to back
    std::vector<std::uint32_t> offset; // where message i starts in bytes
    std::vector<std::uint16_t> length; // length of message i
    std::vector<BookEvent> events;     // decoded book messages
};

// Turns ITCH messages into BookEvents.
struct EventCollector {
    std::vector<BookEvent>* out;
    void on_stock_directory(const itch::StockDirectory&) {}
    void on_add(const itch::AddOrder& m) { out->push_back({'A', m.side, m.shares, m.price, m.id, 0, m.timestamp}); }
    void on_execute(const itch::OrderExecuted& m) { out->push_back({'E', Side::Buy, m.shares, 0, m.id, 0, m.timestamp}); }
    void on_cancel(const itch::OrderCancel& m) { out->push_back({'X', Side::Buy, m.shares, 0, m.id, 0, m.timestamp}); }
    void on_delete(const itch::OrderDelete& m) { out->push_back({'D', Side::Buy, 0, 0, m.id, 0, m.timestamp}); }
    void on_replace(const itch::OrderReplace& m) {
        out->push_back({'U', Side::Buy, m.shares, m.price, m.old_id, m.new_id, m.timestamp});
    }
};

inline MarketData load_market_data(const std::string& path) {
    MarketData data;
    itch::ItchFileReader reader(path);
    const std::uint8_t* msg = nullptr;
    std::size_t len = 0;
    EventCollector collector{&data.events};
    while (reader.next(msg, len)) {
        data.offset.push_back(static_cast<std::uint32_t>(data.bytes.size()));
        data.length.push_back(static_cast<std::uint16_t>(len));
        data.bytes.insert(data.bytes.end(), msg, msg + len);
        itch::parse_message(msg, len, collector);
    }
    return data;
}

// Applies one event to a book. Works for OrderBook and FlatOrderBook (same API).
template <typename Book>
inline bool apply(Book& book, const BookEvent& e) {
    switch (e.type) {
        case 'A': return book.add(e.id, e.side, e.price, e.qty);
        case 'E': return book.execute(e.id, e.qty);
        case 'X': return book.cancel(e.id, e.qty);
        case 'D': return book.remove(e.id);
        case 'U': return book.replace(e.id, e.new_id, e.price, e.qty);
        default: return false;
    }
}

}  // namespace apex::bench
