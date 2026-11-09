// Tests for the SPSC queue, the Framer, and the multi-threaded pipeline.
#include <gtest/gtest.h>
#include <zlib.h>

#include <filesystem>
#include <random>
#include <thread>
#include <vector>

#include "apex/pipeline.hpp"
#include "apex/spsc_queue.hpp"

using namespace apex;
using Bytes = std::vector<std::uint8_t>;

// ---- SpscQueue ----

TEST(SpscQueue, FifoOrderAndCapacity) {
    SpscQueue<int, 4> q;
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(99));  // full
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));  // empty
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<int, 8> q;
    int v = 0;
    for (int i = 0; i < 1000; ++i) {
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_pop(v));
        ASSERT_EQ(v, i);
    }
}

// A producer and a consumer thread run flat out through a small queue. Every value must
// arrive exactly once and in order; a memory-ordering bug shows up as a gap or repeat.
TEST(SpscQueue, TwoThreadsKeepOrder) {
    constexpr std::uint64_t kCount = 5'000'000;
    SpscQueue<std::uint64_t, 1024> q;
    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kCount; ++i) q.push(i);
    });
    std::uint64_t expected = 1, bad = 0;
    while (expected <= kCount) {
        if (q.pop() != expected) ++bad;
        ++expected;
    }
    producer.join();
    EXPECT_EQ(bad, 0u);
}

// ---- Framer ----

namespace {

void put(Bytes& b, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Bytes frame(const Bytes& msg) {
    Bytes b;
    put(b, msg.size(), 2);
    b.insert(b.end(), msg.begin(), msg.end());
    return b;
}

}  // namespace

TEST(Framer, ReassemblesFramesSplitAcrossAnyChunkSizes) {
    std::mt19937_64 rng(5);
    std::vector<Bytes> messages;
    Bytes stream;
    for (int i = 0; i < 2000; ++i) {
        Bytes m(rng() % 60);  // includes empty messages
        for (auto& byte : m) byte = static_cast<std::uint8_t>(rng());
        messages.push_back(m);
        const Bytes f = frame(m);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    for (std::size_t max_chunk : {1u, 2u, 3u, 7u, 64u, 100000u}) {
        Framer framer;
        std::vector<Bytes> got;
        for (std::size_t pos = 0; pos < stream.size();) {
            const std::size_t n = std::min<std::size_t>(1 + rng() % max_chunk, stream.size() - pos);
            framer.feed(stream.data() + pos, n, [&](const std::uint8_t* d, std::size_t len) {
                got.emplace_back(d, d + len);
            });
            pos += n;
        }
        EXPECT_EQ(got, messages) << "max chunk " << max_chunk;
        EXPECT_FALSE(framer.has_partial());
    }
}

TEST(Framer, ReportsUnfinishedFrame) {
    Framer framer;
    const Bytes f = frame(Bytes(10, 1));
    int frames = 0;
    framer.feed(f.data(), f.size() - 1, [&](const std::uint8_t*, std::size_t) { ++frames; });
    EXPECT_EQ(frames, 0);
    EXPECT_TRUE(framer.has_partial());
}

// ---- Pipeline ----

namespace {

Bytes header(char type, std::uint16_t locate) {
    Bytes b{static_cast<std::uint8_t>(type)};
    put(b, locate, 2);
    put(b, 0, 2);
    put(b, 0, 6);
    return b;
}

// Writes a gzip ITCH file with random but valid activity for 5 stocks, plus some
// message types the pipeline ignores. Returns the path.
std::string write_random_market(std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::pair<std::uint16_t, std::uint64_t>> live;  // (locate, id)
    std::uint64_t next_id = 1;
    Bytes stream;
    auto emit = [&](const Bytes& m) {
        const Bytes f = frame(m);
        stream.insert(stream.end(), f.begin(), f.end());
    };
    for (int step = 0; step < 30000; ++step) {
        const int action = live.empty() ? 0 : static_cast<int>(rng() % 6);
        if (action <= 1) {
            const std::uint16_t locate = 1 + rng() % 5;
            Bytes m = header('A', locate);
            put(m, next_id, 8);
            const bool buy = rng() % 2;
            m.push_back(buy ? 'B' : 'S');
            put(m, 2 + rng() % 500, 4);  // at least 2 shares (see the cancel below)
            for (int i = 0; i < 8; ++i) m.push_back(' ');
            put(m, buy ? 10000 - rng() % 200 : 10001 + rng() % 200, 4);
            emit(m);
            live.push_back({locate, next_id++});
        } else if (action == 5) {
            Bytes m = header('S', 0);  // system event: ignored by the pipeline
            m.push_back('O');
            emit(m);
        } else {
            const std::size_t pick = rng() % live.size();
            const auto [locate, id] = live[pick];
            Bytes m;
            if (action == 2) {
                m = header('D', locate);
                put(m, id, 8);
                live.erase(live.begin() + static_cast<long>(pick));
            } else if (action == 3) {
                m = header('U', locate);
                put(m, id, 8);
                put(m, next_id, 8);
                put(m, 100, 4);  // replacement keeps at least 2 shares too
                put(m, 9900 + rng() % 50, 4);
                live[pick].second = next_id++;
            } else {
                // Cancel 1 share (every order has at least 2), then delete the rest.
                m = header('X', locate);
                put(m, id, 8);
                put(m, 1, 4);
                emit(m);
                m = header('D', locate);
                put(m, id, 8);
                live.erase(live.begin() + static_cast<long>(pick));
            }
            emit(m);
        }
    }
    const auto path = (std::filesystem::temp_directory_path() / ("apex_pipeline_" + std::to_string(seed) + ".gz")).string();
    gzFile f = gzopen(path.c_str(), "wb");
    gzwrite(f, stream.data(), static_cast<unsigned>(stream.size()));
    gzclose(f);
    return path;
}

}  // namespace

TEST(Pipeline, EveryThreadLayoutGivesIdenticalResults) {
    const std::string path = write_random_market(11);
    const PipelineConfig layouts[] = {
        {false, 0, 256 * 1024, 1}, {true, 0, 256 * 1024, 1}, {false, 1, 256 * 1024, 1},
        {true, 1, 256 * 1024, 1},  {true, 2, 256 * 1024, 1}, {true, 3, 256 * 1024, 1},
        {true, 2, 37, 1},  // tiny chunks: almost every frame is split between chunks
    };
    const PipelineResult reference = run_pipeline(path, layouts[0]);
    ASSERT_TRUE(reference.ok);
    EXPECT_GT(reference.book_events, 30000u);
    EXPECT_EQ(reference.books, 5u);
    EXPECT_EQ(reference.rejected, 0u);

    for (const PipelineConfig& c : layouts) {
        const PipelineResult r = run_pipeline(path, c);
        SCOPED_TRACE(::testing::Message() << "decompress thread " << c.separate_decompress << ", book threads "
                                          << c.book_threads << ", chunk " << c.chunk_size);
        EXPECT_TRUE(r.ok);
        EXPECT_EQ(r.messages, reference.messages);
        EXPECT_EQ(r.book_events, reference.book_events);
        EXPECT_EQ(r.rejected, reference.rejected);
        EXPECT_EQ(r.books, reference.books);
        EXPECT_EQ(r.checksum, reference.checksum);
        if (c.book_threads > 0) EXPECT_EQ(r.handoff_ns.count(), r.book_events);  // sampled every event
    }
    std::filesystem::remove(path);
}

TEST(Pipeline, MissingFileIsNotOk) {
    EXPECT_FALSE(run_pipeline("/no/such/file.gz", {}).ok);
}
