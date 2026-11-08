// apex_replay: replays a NASDAQ ITCH 5.0 file and rebuilds one stock's order book.
//
// Usage: apex_replay <file.NASDAQ_ITCH50[.gz]> <SYMBOL> [HH:MM]
//
// Prints how many messages were read, the correctness counters from BookBuilder,
// and how long the replay took on this machine. If a time is given, it also prints
// the top of the book at that moment (the end-of-day book is empty, because NASDAQ
// removes all unfilled day orders at the close).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "apex/book_builder.hpp"
#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"

using namespace apex;

namespace {

// Prints a price in ITCH units (1/10000 dollar) as dollars, e.g. 1234500 -> 123.4500.
void print_level(const char* label, const Level& level) {
    std::printf("  %-4s %8lld.%04lld  x %8u shares  (%u orders)\n", label,
                static_cast<long long>(level.price / 10000), static_cast<long long>(level.price % 10000),
                level.quantity, level.order_count);
}

void print_book(const OrderBook& book) {
    const auto asks = book.depth(Side::Sell, 5);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) print_level("ask", *it);  // highest ask on top
    for (const auto& level : book.depth(Side::Buy, 5)) print_level("bid", level);
}

// "HH:MM" -> nanoseconds since midnight (the unit of ITCH timestamps).
std::uint64_t parse_time(const char* text) {
    int hours = 0, minutes = 0;
    if (std::sscanf(text, "%d:%d", &hours, &minutes) != 2) {
        std::fprintf(stderr, "time must look like 12:00\n");
        std::exit(2);
    }
    return (std::uint64_t(hours) * 3600 + std::uint64_t(minutes) * 60) * 1'000'000'000ull;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: %s <itch_file> <SYMBOL> [HH:MM]\n", argv[0]);
        return 2;
    }
    const bool want_snapshot = (argc == 4);
    const std::uint64_t snapshot_time = want_snapshot ? parse_time(argv[3]) : 0;
    bool snapshot_done = false;

    itch::ItchFileReader reader(argv[1]);
    if (!reader.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    BookBuilder builder(argv[2]);

    std::uint64_t total = 0, handled = 0, bad_length = 0;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;

    const auto start = std::chrono::steady_clock::now();
    while (reader.next(data, len)) {
        ++total;
        // Every ITCH message has its timestamp at byte 5. Print the book once, just
        // before the first message at or after the requested time.
        if (want_snapshot && !snapshot_done && len >= 11 && itch::read_u48(data + 5) >= snapshot_time) {
            snapshot_done = true;
            std::printf("book at %s:\n", argv[3]);
            print_book(builder.book());
        }
        switch (itch::parse_message(data, len, builder)) {
            case itch::ParseResult::Handled: ++handled; break;
            case itch::ParseResult::BadLength: ++bad_length; break;
            case itch::ParseResult::Ignored: break;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const auto& s = builder.stats();
    std::printf("file messages:    %llu (book/directory: %llu, bad length: %llu)%s\n",
                (unsigned long long)total, (unsigned long long)handled, (unsigned long long)bad_length,
                reader.truncated() ? "  [file truncated]" : "");
    if (!builder.locate()) {
        std::printf("symbol %s not found in stock directory\n", argv[2]);
        return 1;
    }
    std::printf("%s (locate %u): adds %llu, executes %llu, cancels %llu, deletes %llu, replaces %llu\n",
                argv[2], *builder.locate(), (unsigned long long)s.adds, (unsigned long long)s.executes,
                (unsigned long long)s.cancels, (unsigned long long)s.deletes, (unsigned long long)s.replaces);
    std::printf("rejected updates: %llu\n", (unsigned long long)s.rejected);
    std::printf("crossed/locked:   %llu updates\n", (unsigned long long)s.crossed_updates);

    std::printf("resting at close: %zu orders (expected 0: day orders expire)\n",
                builder.book().order_count());

    std::printf("replay time:      %.2f s  (%.1f million messages/s, read+decompress+parse+book)\n",
                seconds, total / seconds / 1e6);
    return 0;
}
