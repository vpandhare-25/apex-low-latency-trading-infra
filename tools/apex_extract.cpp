// apex_extract: copies one stock's messages out of a full-day ITCH file.
//
// Usage: apex_extract <full_day.NASDAQ_ITCH50.gz> <SYMBOL> <out.itch>
//
// The output uses the same framing (2-byte length + message) and holds the stock's
// 'R' directory message plus every book message for that stock, unchanged. All Apex
// tools read it like the full file, but it loads in about a second instead of
// 40 seconds, and the benchmarks can keep the whole thing in memory.

#include <cstdint>
#include <cstdio>
#include <optional>

#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"

using namespace apex;

namespace {

// Remembers our stock's locate number once its directory message goes past.
struct LocateFinder {
    std::string_view symbol;
    std::optional<std::uint16_t> locate;
    void on_stock_directory(const itch::StockDirectory& m) { if (m.symbol() == symbol) locate = m.locate; }
    void on_add(const itch::AddOrder&) {}
    void on_execute(const itch::OrderExecuted&) {}
    void on_cancel(const itch::OrderCancel&) {}
    void on_delete(const itch::OrderDelete&) {}
    void on_replace(const itch::OrderReplace&) {}
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <itch_file> <SYMBOL> <out.itch>\n", argv[0]);
        return 2;
    }
    itch::ItchFileReader reader(argv[1]);
    std::FILE* out = std::fopen(argv[3], "wb");
    if (!reader.is_open() || out == nullptr) {
        std::fprintf(stderr, "cannot open input or output file\n");
        return 1;
    }

    LocateFinder finder{argv[2], std::nullopt};
    std::uint64_t written = 0;
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    while (reader.next(data, len)) {
        if (len < 11) continue;
        const char type = static_cast<char>(data[0]);
        const bool was_known = finder.locate.has_value();
        if (type == 'R') itch::parse_message(data, len, finder);

        // Keep our 'R' message and every book message (A F E C X D U) for our locate.
        const bool ours = finder.locate && itch::read_u16(data + 1) == *finder.locate;
        const bool book_type = type == 'A' || type == 'F' || type == 'E' || type == 'C' ||
                               type == 'X' || type == 'D' || type == 'U';
        if (ours && ((type == 'R' && !was_known) || book_type)) {
            const std::uint8_t prefix[2] = {static_cast<std::uint8_t>(len >> 8), static_cast<std::uint8_t>(len)};
            std::fwrite(prefix, 1, 2, out);
            std::fwrite(data, 1, len, out);
            ++written;
        }
    }
    std::fclose(out);
    if (!finder.locate) {
        std::fprintf(stderr, "symbol %s not found\n", argv[2]);
        return 1;
    }
    std::printf("wrote %llu messages for %s to %s\n", (unsigned long long)written, argv[2], argv[3]);
    return 0;
}
