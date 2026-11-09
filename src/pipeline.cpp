#include "apex/pipeline.hpp"

#include <pthread.h>
#include <zlib.h>

#include <chrono>
#include <memory>
#include <thread>

#include "apex/flat_order_book.hpp"
#include "apex/itch.hpp"
#include "apex/spsc_queue.hpp"

namespace apex {

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t now_ns() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Ask macOS to run this thread on a performance core if it can (Apple M1 has 4 fast
// and 4 efficiency cores; macOS does not allow pinning a thread to a specific core).
void prefer_fast_core() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// ---- Events passed from the parser to the book threads ----------------------------

struct Event {
    char type;  // 'A' 'E' 'X' 'D' 'U', or 0 = end of stream
    Side side;
    std::uint16_t locate;
    Quantity qty;
    Price price;
    OrderId id;
    OrderId new_id;
    std::uint64_t stamp_ns;  // 0 unless this event is a latency sample
};

// ITCH handler that turns book messages into Events and hands them to `emit`.
template <typename Emit>
struct EventMaker {
    Emit& emit;
    void on_stock_directory(const itch::StockDirectory&) {}
    void on_add(const itch::AddOrder& m) { emit(Event{'A', m.side, m.locate, m.shares, m.price, m.id, 0, 0}); }
    void on_execute(const itch::OrderExecuted& m) { emit(Event{'E', Side::Buy, m.locate, m.shares, 0, m.id, 0, 0}); }
    void on_cancel(const itch::OrderCancel& m) { emit(Event{'X', Side::Buy, m.locate, m.shares, 0, m.id, 0, 0}); }
    void on_delete(const itch::OrderDelete& m) { emit(Event{'D', Side::Buy, m.locate, 0, 0, m.id, 0, 0}); }
    void on_replace(const itch::OrderReplace& m) {
        emit(Event{'U', Side::Buy, m.locate, m.shares, m.price, m.old_id, m.new_id, 0});
    }
};

// ---- Stage 3: the books -------------------------------------------------------------

// The books one thread owns. Each thread has its own BookShard, so no memory is shared.
// alignas: shards sit next to each other in a vector, and their counters change on
// every event. Without it, two threads would keep writing the same cache line.
class alignas(kCacheLine) BookShard {
public:
    BookShard() : books_(65536), hash_(65536, 0) {}

    void apply(const Event& e) {
        std::unique_ptr<HybridOrderBook>& slot = books_[e.locate];
        if (!slot) slot = std::make_unique<HybridOrderBook>(/*initial_order_capacity=*/64);
        HybridOrderBook& book = *slot;

        bool ok = false;
        switch (e.type) {
            case 'A': ok = book.add(e.id, e.side, e.price, e.qty); break;
            case 'E': ok = book.execute(e.id, e.qty); break;
            case 'X': ok = book.cancel(e.id, e.qty); break;
            case 'D': ok = book.remove(e.id); break;
            case 'U': ok = book.replace(e.id, e.new_id, e.price, e.qty); break;
        }
        ++events;
        if (!ok) ++rejected;

        // Fold this stock's new top of book into its own running hash. The per-stock
        // hashes are summed at the end, so the total does not depend on which thread
        // handled which stock, but it does depend on every update of every stock.
        const auto bid = book.best_bid();
        const auto ask = book.best_ask();
        std::uint64_t x = hash_[e.locate] ^ (bid ? static_cast<std::uint64_t>(bid->price) * 31 + bid->quantity : 1);
        x = (x * 0x9E3779B97F4A7C15ull) ^ (ask ? static_cast<std::uint64_t>(ask->price) * 37 + ask->quantity : 2);
        hash_[e.locate] = x ^ (x >> 29);
    }

    void add_to(PipelineResult& r) const {
        r.book_events += events;
        r.rejected += rejected;
        for (std::size_t l = 0; l < books_.size(); ++l) {
            if (books_[l]) ++r.books;
            r.checksum += hash_[l];
        }
    }

    std::uint64_t events = 0;
    std::uint64_t rejected = 0;
    LatencyStats handoff_ns;  // queue wait + apply time for sampled events
    std::uint64_t waited_ns = 0;  // time this book thread waited for events

private:
    std::vector<std::unique_ptr<HybridOrderBook>> books_;  // indexed by stock locate
    std::vector<std::uint64_t> hash_;
};

// ---- Stage 1: decompression ---------------------------------------------------------

struct Chunk {
    std::uint8_t* data;  // nullptr marks the end of the file
    std::size_t size;
};
constexpr std::size_t kChunks = 16;  // buffers circulating between stages 1 and 2

}  // namespace

PipelineResult run_pipeline(const std::string& path, const PipelineConfig& config) {
    PipelineResult result;
    gzFile file = gzopen(path.c_str(), "rb");
    if (file == nullptr) return result;
    gzbuffer(file, 1 << 20);
    prefer_fast_core();

    // Chunk buffers: full ones go 1 -> 2 in `full`, empty ones come back in `free_list`.
    std::vector<std::vector<std::uint8_t>> buffers(kChunks, std::vector<std::uint8_t>(config.chunk_size));
    SpscQueue<Chunk, 32> full;
    SpscQueue<std::uint8_t*, 32> free_list;
    for (auto& b : buffers) free_list.push(b.data());

    const int n_book_threads = config.book_threads;
    std::vector<std::unique_ptr<SpscQueue<Event, 1 << 16>>> event_queues;
    std::vector<BookShard> shards(n_book_threads > 0 ? n_book_threads : 1);
    for (int i = 0; i < n_book_threads; ++i) event_queues.push_back(std::make_unique<SpscQueue<Event, 1 << 16>>());

    std::uint64_t messages = 0;
    std::uint32_t until_sample = config.latency_sample_every;
    std::uint64_t main_waited_ns = 0;        // main thread (parse, maybe decompress and books)
    std::uint64_t decompress_waited_ns = 0;  // decompress thread, if it has its own
    bool read_error = false;

    // Stage 2 output: either apply directly (books in this thread) or queue to a book thread.
    auto emit = [&](Event e) {
        if (n_book_threads == 0) {
            shards[0].apply(e);
            return;
        }
        if (config.latency_sample_every != 0 && --until_sample == 0) {
            until_sample = config.latency_sample_every;
            e.stamp_ns = now_ns();
        }
        event_queues[e.locate % n_book_threads]->push_timed(e, main_waited_ns);
    };
    EventMaker<decltype(emit)> maker{emit};
    Framer framer;
    auto parse_chunk = [&](const std::uint8_t* data, std::size_t size) {
        framer.feed(data, size, [&](const std::uint8_t* msg, std::size_t len) {
            ++messages;
            itch::parse_message(msg, len, maker);
        });
    };

    // Reads one chunk; returns its size (0 at end of file or on error).
    auto read_chunk = [&](std::uint8_t* buffer) -> std::size_t {
        const int n = gzread(file, buffer, static_cast<unsigned>(config.chunk_size));
        if (n < 0) read_error = true;
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    };

    const auto start = Clock::now();

    // Stage 3 threads.
    std::vector<std::thread> book_threads;
    for (int i = 0; i < n_book_threads; ++i) {
        book_threads.emplace_back([&, i] {
            prefer_fast_core();
            SpscQueue<Event, 1 << 16>& q = *event_queues[i];
            for (;;) {
                const Event e = q.pop_timed(shards[i].waited_ns);
                if (e.type == 0) break;
                shards[i].apply(e);
                if (e.stamp_ns != 0) shards[i].handoff_ns.add(now_ns() - e.stamp_ns);
            }
        });
    }

    // Stage 1 thread (or not).
    std::thread decompress_thread;
    if (config.separate_decompress) {
        decompress_thread = std::thread([&] {
            prefer_fast_core();
            for (;;) {
                std::uint8_t* buffer = free_list.pop_timed(decompress_waited_ns);
                const std::size_t n = read_chunk(buffer);
                if (n == 0) {
                    full.push_timed(Chunk{nullptr, 0}, decompress_waited_ns);
                    return;
                }
                full.push_timed(Chunk{buffer, n}, decompress_waited_ns);
            }
        });
        for (;;) {  // stage 2 runs on this thread
            const Chunk c = full.pop_timed(main_waited_ns);
            if (c.data == nullptr) break;
            parse_chunk(c.data, c.size);
            free_list.push(c.data);  // give the buffer back to the decompressor
        }
        decompress_thread.join();
    } else {
        std::uint8_t* buffer = buffers[0].data();
        for (std::size_t n; (n = read_chunk(buffer)) > 0;) parse_chunk(buffer, n);
    }

    // Tell book threads to finish, then wait for them.
    for (auto& q : event_queues) q->push(Event{0, Side::Buy, 0, 0, 0, 0, 0, 0});
    for (auto& t : book_threads) t.join();
    result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    gzclose(file);

    result.ok = !read_error && !framer.has_partial();
    result.messages = messages;
    for (const BookShard& s : shards) s.add_to(result);
    for (const BookShard& s : shards) result.handoff_ns.merge(s.handoff_ns);

    // Name each thread by the stages it runs, and report how long it sat waiting.
    if (config.separate_decompress) result.idle.push_back({"gunzip", decompress_waited_ns / 1e9});
    std::string main_name = config.separate_decompress ? "parse" : "gunzip+parse";
    if (n_book_threads == 0) main_name += "+books";
    result.idle.push_back({main_name, main_waited_ns / 1e9});
    for (int i = 0; i < n_book_threads; ++i) {
        result.idle.push_back({"books" + std::to_string(i + 1), shards[i].waited_ns / 1e9});
    }
    return result;
}

}  // namespace apex
