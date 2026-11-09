#pragma once
// Market data pipeline: keeps an order book for EVERY stock in a full-day ITCH file,
// using 1 to 4+ threads connected by lock-free SPSC queues.
//
// The work has three stages:
//   1. decompress  gzip bytes -> raw bytes, in 256 KB chunks
//   2. parse       split chunks into ITCH frames, decode, emit book events
//   3. books       apply each event to its stock's HybridOrderBook
//
// PipelineConfig chooses which stages get their own thread:
//
//   separate_decompress  book_threads   threads   layout
//   false                0              1         [decompress+parse+books]
//   true                 0              2         [decompress] -> [parse+books]
//   false                1              2         [decompress+parse] -> [books]
//   true                 1              3         [decompress] -> [parse] -> [books]
//   true                 2              4         [decompress] -> [parse] -> [books x2]
//
// With several book threads, each stock always goes to the same thread
// (locate % book_threads), so a stock's events stay in order and no book is shared.
// Every layout runs the same code, so their results must be identical; a checksum of
// every stock's best bid/ask after every update proves it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "apex/analytics.hpp"
#include "apex/types.hpp"

namespace apex {

struct PipelineConfig {
    bool separate_decompress = false;
    int book_threads = 0;
    std::size_t chunk_size = 256 * 1024;
    // When books have their own thread(s), every Nth event carries a timestamp so we can
    // measure how long events wait in the queue (0 = never).
    std::uint32_t latency_sample_every = 1024;
};

struct PipelineResult {
    bool ok = false;               // file opened and did not end mid-message
    std::uint64_t messages = 0;    // ITCH messages in the file
    std::uint64_t book_events = 0; // add/execute/cancel/delete/replace messages
    std::uint64_t rejected = 0;    // book updates that failed (should be 0)
    std::uint64_t books = 0;       // stocks that got a book
    std::uint64_t checksum = 0;    // depends on every stock's top of book after every update
    double seconds = 0;
    // Time from "parser pushed the event" to "book thread finished applying it", in ns.
    LatencyStats handoff_ns;
    // Seconds each thread spent waiting on a queue (empty input or full output).
    // The thread that almost never waits is the bottleneck.
    struct ThreadIdle {
        std::string name;
        double idle_seconds;
    };
    std::vector<ThreadIdle> idle;
};

PipelineResult run_pipeline(const std::string& path, const PipelineConfig& config);

// Splits a byte stream that arrives in arbitrary chunks back into ITCH frames
// (2-byte big-endian length + message). A frame may be split across chunks.
class Framer {
public:
    template <typename OnFrame>
    void feed(const std::uint8_t* data, std::size_t size, OnFrame&& on_frame);

    // True if the stream ended in the middle of a frame.
    bool has_partial() const { return !carry_.empty(); }

private:
    std::vector<std::uint8_t> carry_;  // start of a frame that did not fit in the last chunk
};

template <typename OnFrame>
void Framer::feed(const std::uint8_t* data, std::size_t size, OnFrame&& on_frame) {
    std::size_t pos = 0;

    // 1. Finish a frame left over from the previous chunk.
    if (!carry_.empty()) {
        while (carry_.size() < 2 && pos < size) carry_.push_back(data[pos++]);
        if (carry_.size() < 2) return;  // still don't know the length
        const std::size_t need = 2 + ((std::size_t{carry_[0]} << 8) | carry_[1]);
        const std::size_t take = std::min(need - carry_.size(), size - pos);
        carry_.insert(carry_.end(), data + pos, data + pos + take);
        pos += take;
        if (carry_.size() < need) return;  // the frame continues in the next chunk
        on_frame(carry_.data() + 2, need - 2);
        carry_.clear();
    }

    // 2. Whole frames inside this chunk: no copying, just pointers into the chunk.
    while (size - pos >= 2) {
        const std::size_t len = (std::size_t{data[pos]} << 8) | data[pos + 1];
        if (size - pos < 2 + len) break;
        on_frame(data + pos + 2, len);
        pos += 2 + len;
    }

    // 3. Keep the start of a frame that runs past the end of this chunk.
    carry_.assign(data + pos, data + size);
}

}  // namespace apex
