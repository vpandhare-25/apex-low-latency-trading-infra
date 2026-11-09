#pragma once
// Hash maps from order id to order data, with one small common API so an order book
// can use either:
//   V*   find(key)          nullptr if missing
//   bool insert(key, value) false if the key already exists
//   bool erase(key)         false if missing
//   size()
//
// StdOrderMap   wraps std::unordered_map. Every entry is a separate heap node, so a
//               lookup follows a pointer to a random place in memory (a cache miss).
// FlatOrderMap  open addressing with linear probing: all entries live in one array.
//               A lookup hashes to a slot and checks it and its neighbours, which are
//               usually in the same cache line. No allocation per order.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "apex/types.hpp"

namespace apex {

template <typename V>
class StdOrderMap {
public:
    // The size hint is ignored, so std::unordered_map keeps its default growth behaviour
    // (and the step 5 benchmarks stay comparable).
    explicit StdOrderMap(std::size_t /*initial_capacity*/ = 0) {}

    V* find(OrderId key) {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    const V* find(OrderId key) const {
        auto it = map_.find(key);
        return it == map_.end() ? nullptr : &it->second;
    }
    bool insert(OrderId key, const V& value) { return map_.try_emplace(key, value).second; }
    bool erase(OrderId key) { return map_.erase(key) != 0; }
    std::size_t size() const { return map_.size(); }

private:
    std::unordered_map<OrderId, V> map_;
};

template <typename V>
class FlatOrderMap {
public:
    // Reserved key meaning "empty slot". ITCH order reference numbers never use it.
    static constexpr OrderId kEmpty = ~OrderId{0};

    explicit FlatOrderMap(std::size_t initial_capacity = 1 << 16) {
        std::size_t cap = 16;
        while (cap < initial_capacity) cap *= 2;  // capacity is a power of two
        slots_.assign(cap, Slot{kEmpty, V{}});
        mask_ = cap - 1;
    }

    V* find(OrderId key) {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return &slots_[i].value;
            if (slots_[i].key == kEmpty) return nullptr;
        }
    }
    const V* find(OrderId key) const { return const_cast<FlatOrderMap*>(this)->find(key); }

    bool insert(OrderId key, const V& value) {
        if (key == kEmpty) return false;
        if ((size_ + 1) * 2 > slots_.size()) grow();  // keep the table at most half full
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            if (slots_[i].key == key) return false;
            if (slots_[i].key == kEmpty) {
                slots_[i] = Slot{key, value};
                ++size_;
                return true;
            }
        }
    }

    // "Backward shift" deletion: after emptying a slot, move later entries of the same
    // probe run back into the gap, so lookups never stop early at a false "empty".
    bool erase(OrderId key) {
        std::size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
        std::size_t gap = i;
        for (std::size_t j = (i + 1) & mask_; slots_[j].key != kEmpty; j = (j + 1) & mask_) {
            // Entry j may move into the gap only if its home slot is not between gap and j
            // (in circular order); otherwise moving it would put it before its home.
            const std::size_t h = home(slots_[j].key);
            const bool home_in_range = (gap <= j) ? (gap < h && h <= j) : (gap < h || h <= j);
            if (!home_in_range) {
                slots_[gap] = slots_[j];
                gap = j;
            }
        }
        slots_[gap].key = kEmpty;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }

private:
    struct Slot {
        OrderId key;
        V value;
    };

    // Fibonacci hashing: multiply by 2^64 / golden ratio and keep the top bits. It spreads
    // nearby ids (ITCH ids often count up) across the whole table.
    std::size_t home(OrderId key) const {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift()) & mask_;
    }
    int shift() const { return 64 - __builtin_ctzll(slots_.size()); }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{kEmpty, V{}});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& s : old) {
            if (s.key != kEmpty) insert(s.key, s.value);
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace apex
