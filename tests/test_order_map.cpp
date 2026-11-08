// Tests for the order-id hash maps, mainly FlatOrderMap's probing and deletion.
#include <gtest/gtest.h>

#include <random>
#include <unordered_map>

#include "apex/order_map.hpp"

using namespace apex;

TEST(FlatOrderMap, InsertFindErase) {
    FlatOrderMap<int> map(16);
    EXPECT_TRUE(map.insert(5, 50));
    EXPECT_FALSE(map.insert(5, 99));  // duplicate key
    ASSERT_NE(map.find(5), nullptr);
    EXPECT_EQ(*map.find(5), 50);
    EXPECT_EQ(map.find(6), nullptr);
    EXPECT_TRUE(map.erase(5));
    EXPECT_FALSE(map.erase(5));
    EXPECT_EQ(map.size(), 0u);
}

TEST(FlatOrderMap, RejectsReservedEmptyKey) {
    FlatOrderMap<int> map(16);
    EXPECT_FALSE(map.insert(FlatOrderMap<int>::kEmpty, 1));
}

TEST(FlatOrderMap, GrowsBeyondInitialCapacity) {
    FlatOrderMap<int> map(16);
    for (OrderId k = 1; k <= 10000; ++k) ASSERT_TRUE(map.insert(k, static_cast<int>(k)));
    EXPECT_EQ(map.size(), 10000u);
    for (OrderId k = 1; k <= 10000; ++k) ASSERT_EQ(*map.find(k), static_cast<int>(k));
}

// Random inserts and erases, checked against std::unordered_map after every step.
// A small table forces long probe runs, wrap-around and many backward shifts.
TEST(FlatOrderMap, MatchesStdMapOnRandomOperations) {
    FlatOrderMap<std::uint64_t> map(16);
    std::unordered_map<OrderId, std::uint64_t> ref;
    std::mt19937_64 rng(99);
    for (int step = 0; step < 200000; ++step) {
        const OrderId key = rng() % 2000;  // small key space = many repeats and collisions
        if (rng() % 3 == 0) {
            ASSERT_EQ(map.erase(key), ref.erase(key) == 1) << step;
        } else {
            const std::uint64_t value = rng();
            ASSERT_EQ(map.insert(key, value), ref.try_emplace(key, value).second) << step;
        }
        ASSERT_EQ(map.size(), ref.size());
        const OrderId probe = rng() % 2000;
        const auto it = ref.find(probe);
        const std::uint64_t* found = map.find(probe);
        ASSERT_EQ(found != nullptr, it != ref.end()) << step;
        if (found) ASSERT_EQ(*found, it->second) << step;
    }
    for (const auto& [k, v] : ref) ASSERT_EQ(*map.find(k), v);
}
