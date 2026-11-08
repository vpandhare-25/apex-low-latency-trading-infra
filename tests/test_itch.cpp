// Tests for the ITCH decoder, file reader and BookBuilder, using hand-built messages
// laid out exactly as in the ITCH 5.0 spec.
#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "apex/book_builder.hpp"
#include "apex/itch.hpp"
#include "apex/itch_file_reader.hpp"

using namespace apex;
using Bytes = std::vector<std::uint8_t>;

namespace {

// ---- Tiny big-endian encoder for building test messages ----
void put(Bytes& b, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

// The 11-byte header: type, locate, tracking number (0), timestamp.
Bytes header(char type, std::uint16_t locate, std::uint64_t ts = 123456789) {
    Bytes b{static_cast<std::uint8_t>(type)};
    put(b, locate, 2);
    put(b, 0, 2);
    put(b, ts, 6);
    return b;
}

Bytes stock_directory(std::uint16_t locate, const std::string& symbol) {
    Bytes b = header('R', locate);
    std::string padded = symbol;
    padded.resize(8, ' ');
    for (char c : padded) b.push_back(static_cast<std::uint8_t>(c));
    b.resize(itch::kStockDirectorySize, 0);  // remaining fields not used by us
    return b;
}

Bytes add_order(std::uint16_t locate, std::uint64_t id, char side, std::uint32_t shares,
                const std::string& symbol, std::uint32_t price) {
    Bytes b = header('A', locate);
    put(b, id, 8);
    b.push_back(static_cast<std::uint8_t>(side));
    put(b, shares, 4);
    std::string padded = symbol;
    padded.resize(8, ' ');
    for (char c : padded) b.push_back(static_cast<std::uint8_t>(c));
    put(b, price, 4);
    return b;
}

Bytes executed(std::uint16_t locate, std::uint64_t id, std::uint32_t shares) {
    Bytes b = header('E', locate);
    put(b, id, 8);
    put(b, shares, 4);
    put(b, 999, 8);  // match number
    return b;
}

Bytes cancel(std::uint16_t locate, std::uint64_t id, std::uint32_t shares) {
    Bytes b = header('X', locate);
    put(b, id, 8);
    put(b, shares, 4);
    return b;
}

Bytes del(std::uint16_t locate, std::uint64_t id) {
    Bytes b = header('D', locate);
    put(b, id, 8);
    return b;
}

Bytes replace(std::uint16_t locate, std::uint64_t old_id, std::uint64_t new_id,
              std::uint32_t shares, std::uint32_t price) {
    Bytes b = header('U', locate);
    put(b, old_id, 8);
    put(b, new_id, 8);
    put(b, shares, 4);
    put(b, price, 4);
    return b;
}

// Handler that just remembers the last message of each kind.
struct Recorder {
    itch::StockDirectory dir{};
    itch::AddOrder add{};
    itch::OrderExecuted exec{};
    itch::OrderCancel cxl{};
    itch::OrderDelete dlt{};
    itch::OrderReplace rep{};
    void on_stock_directory(const itch::StockDirectory& m) { dir = m; }
    void on_add(const itch::AddOrder& m) { add = m; }
    void on_execute(const itch::OrderExecuted& m) { exec = m; }
    void on_cancel(const itch::OrderCancel& m) { cxl = m; }
    void on_delete(const itch::OrderDelete& m) { dlt = m; }
    void on_replace(const itch::OrderReplace& m) { rep = m; }
};

itch::ParseResult parse(const Bytes& b, Recorder& r) { return itch::parse_message(b.data(), b.size(), r); }

// Writes messages with 2-byte length prefixes, either plain or gzip-compressed.
std::string write_itch_file(const std::vector<Bytes>& messages, bool gzip) {
    const auto path = (std::filesystem::temp_directory_path() /
                       (gzip ? "apex_test_itch.gz" : "apex_test_itch.bin")).string();
    Bytes framed;
    for (const auto& m : messages) {
        put(framed, m.size(), 2);
        framed.insert(framed.end(), m.begin(), m.end());
    }
    gzFile f = gzopen(path.c_str(), gzip ? "wb" : "wbT");  // "T" = write without compression
    gzwrite(f, framed.data(), static_cast<unsigned>(framed.size()));
    gzclose(f);
    return path;
}

}  // namespace

// ---- Decoder ----

TEST(ItchDecoder, BigEndianHelpers) {
    const std::uint8_t bytes[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(itch::read_u16(bytes), 0x0102u);
    EXPECT_EQ(itch::read_u32(bytes), 0x01020304u);
    EXPECT_EQ(itch::read_u48(bytes), 0x010203040506ull);
    EXPECT_EQ(itch::read_u64(bytes), 0x0102030405060708ull);
}

TEST(ItchDecoder, MessageSizesMatchSpec) {
    EXPECT_EQ(stock_directory(1, "AAPL").size(), itch::kStockDirectorySize);
    EXPECT_EQ(add_order(1, 1, 'B', 1, "AAPL", 1).size(), itch::kAddOrderSize);
    EXPECT_EQ(executed(1, 1, 1).size(), itch::kOrderExecutedSize);
    EXPECT_EQ(cancel(1, 1, 1).size(), itch::kOrderCancelSize);
    EXPECT_EQ(del(1, 1).size(), itch::kOrderDeleteSize);
    EXPECT_EQ(replace(1, 1, 2, 1, 1).size(), itch::kOrderReplaceSize);
}

TEST(ItchDecoder, StockDirectoryTrimsSymbol) {
    Recorder r;
    ASSERT_EQ(parse(stock_directory(42, "AAPL"), r), itch::ParseResult::Handled);
    EXPECT_EQ(r.dir.locate, 42);
    EXPECT_EQ(r.dir.timestamp, 123456789u);
    EXPECT_EQ(r.dir.symbol(), "AAPL");
}

TEST(ItchDecoder, AddOrderFields) {
    Recorder r;
    ASSERT_EQ(parse(add_order(7, 0xABCDEF0123ull, 'S', 300, "MSFT", 4123400), r),
              itch::ParseResult::Handled);
    EXPECT_EQ(r.add.locate, 7);
    EXPECT_EQ(r.add.id, 0xABCDEF0123ull);
    EXPECT_EQ(r.add.side, Side::Sell);
    EXPECT_EQ(r.add.shares, 300u);
    EXPECT_EQ(r.add.price, 4123400);  // $412.3400
}

TEST(ItchDecoder, ExecuteCancelDeleteReplaceFields) {
    Recorder r;
    parse(executed(1, 11, 25), r);
    EXPECT_EQ(r.exec.id, 11u);
    EXPECT_EQ(r.exec.shares, 25u);
    EXPECT_FALSE(r.exec.has_price);

    parse(cancel(1, 12, 5), r);
    EXPECT_EQ(r.cxl.id, 12u);
    EXPECT_EQ(r.cxl.shares, 5u);

    parse(del(1, 13), r);
    EXPECT_EQ(r.dlt.id, 13u);

    parse(replace(1, 14, 15, 80, 100100), r);
    EXPECT_EQ(r.rep.old_id, 14u);
    EXPECT_EQ(r.rep.new_id, 15u);
    EXPECT_EQ(r.rep.shares, 80u);
    EXPECT_EQ(r.rep.price, 100100);
}

TEST(ItchDecoder, ShortMessageIsRejectedAndUnknownTypeIgnored) {
    Recorder r;
    Bytes short_add = add_order(1, 1, 'B', 1, "AAPL", 1);
    short_add.pop_back();
    EXPECT_EQ(parse(short_add, r), itch::ParseResult::BadLength);

    Bytes system_event = header('S', 0);
    system_event.push_back('O');
    EXPECT_EQ(parse(system_event, r), itch::ParseResult::Ignored);
    EXPECT_EQ(parse(Bytes{}, r), itch::ParseResult::BadLength);
}

// ---- File reader ----

class ItchFileReaderTest : public ::testing::TestWithParam<bool> {};

TEST_P(ItchFileReaderTest, ReadsEveryFrame) {
    const std::vector<Bytes> messages = {stock_directory(1, "AAPL"), add_order(1, 1, 'B', 10, "AAPL", 5),
                                         del(1, 1)};
    const std::string path = write_itch_file(messages, /*gzip=*/GetParam());

    itch::ItchFileReader reader(path);
    ASSERT_TRUE(reader.is_open());
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    for (const auto& expected : messages) {
        ASSERT_TRUE(reader.next(data, len));
        EXPECT_EQ(Bytes(data, data + len), expected);
    }
    EXPECT_FALSE(reader.next(data, len));
    EXPECT_FALSE(reader.truncated());
    std::filesystem::remove(path);
}

INSTANTIATE_TEST_SUITE_P(PlainAndGzip, ItchFileReaderTest, ::testing::Values(false, true));

TEST(ItchFileReader, MissingFileIsNotOpen) {
    itch::ItchFileReader reader("/definitely/not/here.itch");
    EXPECT_FALSE(reader.is_open());
}

TEST(ItchFileReader, DetectsCutOffFile) {
    const std::string path = write_itch_file({del(1, 1)}, /*gzip=*/false);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 3);  // chop the end
    itch::ItchFileReader reader(path);
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    EXPECT_FALSE(reader.next(data, len));
    EXPECT_TRUE(reader.truncated());
    std::filesystem::remove(path);
}

// ---- BookBuilder ----

TEST(BookBuilder, RebuildsOnlyItsOwnStock) {
    BookBuilder builder("AAPL");
    const std::vector<Bytes> messages = {
        stock_directory(1, "AAPL"),
        stock_directory(2, "MSFT"),
        add_order(1, 100, 'B', 500, "AAPL", 1500000),  // AAPL bid $150.0000 x 500
        add_order(1, 101, 'S', 200, "AAPL", 1501000),  // AAPL ask $150.1000 x 200
        add_order(2, 200, 'B', 999, "MSFT", 4000000),  // MSFT: must be ignored
        executed(1, 100, 100),                         // bid down to 400
        cancel(1, 101, 50),                            // ask down to 150
        replace(1, 100, 102, 300, 1500500),            // bid moves to $150.0500 x 300
        del(2, 200),                                   // MSFT: ignored
    };
    for (const auto& m : messages) itch::parse_message(m.data(), m.size(), builder);

    ASSERT_TRUE(builder.locate().has_value());
    EXPECT_EQ(*builder.locate(), 1);
    EXPECT_EQ(builder.book().best_bid(), (Level{1500500, 300, 1}));
    EXPECT_EQ(builder.book().best_ask(), (Level{1501000, 150, 1}));
    EXPECT_EQ(builder.book().order_count(), 2u);

    const auto& s = builder.stats();
    EXPECT_EQ(s.adds, 2u);
    EXPECT_EQ(s.executes, 1u);
    EXPECT_EQ(s.cancels, 1u);
    EXPECT_EQ(s.replaces, 1u);
    EXPECT_EQ(s.deletes, 0u);
    EXPECT_EQ(s.rejected, 0u);
    EXPECT_EQ(s.crossed_updates, 0u);
}

TEST(BookBuilder, CountsRejectedAndCrossedUpdates) {
    BookBuilder builder("AAPL");
    const std::vector<Bytes> messages = {
        stock_directory(1, "AAPL"),
        del(1, 555),                           // unknown order -> rejected
        add_order(1, 1, 'S', 10, "AAPL", 100),
        add_order(1, 2, 'B', 10, "AAPL", 100),  // bid == ask -> locked
    };
    for (const auto& m : messages) itch::parse_message(m.data(), m.size(), builder);
    EXPECT_EQ(builder.stats().rejected, 1u);
    EXPECT_EQ(builder.stats().crossed_updates, 1u);
}
