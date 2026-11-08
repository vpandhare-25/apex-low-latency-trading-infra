// apex_replay: replays a NASDAQ ITCH 5.0 file and rebuilds one stock's order book.
//
// Usage: apex_replay <file.NASDAQ_ITCH50[.gz]> <SYMBOL>
//
// Prints how many messages were read, the correctness counters from BookBuilder,
// the final top of book, and how long the replay took on this machine.

#include <chrono>
#include <cstdint>
#include <cstdio>
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
    BookBuilder builder(argv[2]);

    std::uint64_t total = 0, handled = 0, bad_length = 0;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;

    const auto start = std::chrono::steady_clock::now();
    while (reader.next(data, len)) {
        ++total;
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
    std::printf("resting orders:   %zu\n", builder.book().order_count());

    std::printf("final book (top 5):\n");
    const auto asks = builder.book().depth(Side::Sell, 5);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) print_level("ask", *it);
    for (const auto& level : builder.book().depth(Side::Buy, 5)) print_level("bid", level);

    std::printf("replay time:      %.2f s  (%.1f million messages/s, read+decompress+parse+book)\n",
                seconds, total / seconds / 1e6);
    return 0;
}
