#pragma once
// Lock-free single-producer / single-consumer (SPSC) ring buffer.
//
// One thread pushes, one other thread pops. No mutex: the two threads only share two
// counters, `tail_` (written by the producer) and `head_` (written by the consumer).
//
//   * Each counter only ever increases. The slot is counter % Capacity (Capacity is a
//     power of two, so that is a cheap bit-mask). The queue is empty when head == tail
//     and full when tail - head == Capacity.
//   * Memory ordering: the producer writes the slot, then stores tail with `release`.
//     The consumer loads tail with `acquire` before reading the slot. That pair
//     guarantees the consumer sees the finished slot, not a half-written one. The same
//     pairing on head tells the producer a slot is free to reuse.
//   * Each thread keeps a private copy of the *other* thread's counter and re-reads the
//     shared one only when its copy says "full" (or "empty"). Reading a counter the
//     other core keeps writing is expensive, so this makes most operations touch only
//     memory the thread owns.
//   * head_ and tail_ sit on separate cache lines, so the two cores do not fight over
//     one line ("false sharing"). Apple M-series cores use 128-byte lines.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace apex {

inline constexpr std::size_t kCacheLine = 128;

// Tells the CPU we are spinning (saves power, helps the other hyper-thread / core).
inline void cpu_relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    SpscQueue() : slots_(std::make_unique<T[]>(Capacity)) {}
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Producer only. Returns false if the queue is full.
    bool try_push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == Capacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == Capacity) return false;
        }
        slots_[tail & kMask] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. Returns false if the queue is empty.
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false;
        }
        out = slots_[head & kMask];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Blocking versions: spin briefly, then let the OS run something else.
    void push(const T& value) {
        for (unsigned spins = 0; !try_push(value); ++spins) wait(spins);
    }
    T pop() {
        T out;
        for (unsigned spins = 0; !try_pop(out); ++spins) wait(spins);
        return out;
    }

    // Same as push/pop, but add the time spent waiting (queue full / empty) to `waited_ns`.
    // The clock is read only when the fast path fails, so a busy queue costs nothing extra.
    void push_timed(const T& value, std::uint64_t& waited_ns) {
        if (try_push(value)) return;
        const auto t0 = std::chrono::steady_clock::now();
        push(value);
        waited_ns += elapsed_ns(t0);
    }
    T pop_timed(std::uint64_t& waited_ns) {
        T out;
        if (try_pop(out)) return out;
        const auto t0 = std::chrono::steady_clock::now();
        out = pop();
        waited_ns += elapsed_ns(t0);
        return out;
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    static std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point t0) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }

    static void wait(unsigned spins) {
        if (spins < 256) cpu_relax();
        else std::this_thread::yield();
    }

    // Consumer-owned line: its counter and its copy of the producer's counter.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_ = 0;
    // Producer-owned line.
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_ = 0;
    // Read by both threads but never written after construction: its own line, so the
    // counter writes above never invalidate it.
    alignas(kCacheLine) std::unique_ptr<T[]> slots_;
};

}  // namespace apex
