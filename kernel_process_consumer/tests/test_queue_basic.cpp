// 6a. 佇列基本行為：空、滿、環繞
#include "spsc_ring.h"
#include "test_util.h"

#include <cstdint>
#include <stdexcept>

namespace {

bool ThrowsInvalidCapacity(size_t capacity)
{
    try {
        SpscRing<uint64_t> ring(capacity);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    // 容量必須是非 0 的 2 的冪次
    CHECK(ThrowsInvalidCapacity(0));
    CHECK(ThrowsInvalidCapacity(3));
    CHECK(ThrowsInvalidCapacity(6));
    CHECK(!ThrowsInvalidCapacity(1));
    CHECK(!ThrowsInvalidCapacity(4));

    SpscRing<uint64_t> ring(4);
    CHECK_EQ(ring.Capacity(), 4);

    // 空
    uint64_t out = 0;
    bool readCalled = false;
    CHECK(!ring.TryPop([&](uint64_t& v) { readCalled = true; out = v; }));
    CHECK(!readCalled);
    CHECK_EQ(ring.SizeApprox(), 0);

    // 填滿
    for (uint64_t i = 0; i < 4; ++i) {
        CHECK(ring.TryPush([&](uint64_t& slot) { slot = 100 + i; }));
        CHECK_EQ(ring.SizeApprox(), i + 1);
    }

    // 滿：不可呼叫 fill，也不可覆寫
    bool fillCalled = false;
    CHECK(!ring.TryPush([&](uint64_t& slot) { fillCalled = true; slot = 999; }));
    CHECK(!fillCalled);
    CHECK_EQ(ring.SizeApprox(), 4);

    // FIFO 取出
    for (uint64_t i = 0; i < 4; ++i) {
        CHECK(ring.TryPop([&](uint64_t& v) { out = v; }));
        CHECK_EQ(out, 100 + i);
    }
    CHECK(!ring.TryPop([&](uint64_t& v) { out = v; }));
    CHECK_EQ(ring.SizeApprox(), 0);

    // 環繞：每輪放入 1~4 個再全部取出，索引跨越容量很多次
    uint64_t nextPush = 0;
    uint64_t nextPop = 0;
    for (int round = 0; round < 1000; ++round) {
        const int k = 1 + round % 4;
        for (int j = 0; j < k; ++j)
            CHECK(ring.TryPush([&](uint64_t& slot) { slot = nextPush++; }));
        for (int j = 0; j < k; ++j) {
            CHECK(ring.TryPop([&](uint64_t& v) { out = v; }));
            CHECK_EQ(out, nextPop++);
        }
    }
    CHECK_EQ(ring.SizeApprox(), 0);

    // 環繞且佇列內一直有殘留：放 3 取 2，再放 2 取 3
    for (int round = 0; round < 1000; ++round) {
        for (int j = 0; j < 3; ++j)
            CHECK(ring.TryPush([&](uint64_t& slot) { slot = nextPush++; }));
        for (int j = 0; j < 2; ++j) {
            CHECK(ring.TryPop([&](uint64_t& v) { out = v; }));
            CHECK_EQ(out, nextPop++);
        }
        CHECK_EQ(ring.SizeApprox(), 1);
        for (int j = 0; j < 2; ++j)
            CHECK(ring.TryPush([&](uint64_t& slot) { slot = nextPush++; }));
        for (int j = 0; j < 3; ++j) {
            CHECK(ring.TryPop([&](uint64_t& v) { out = v; }));
            CHECK_EQ(out, nextPop++);
        }
        CHECK_EQ(ring.SizeApprox(), 0);
    }
    CHECK_EQ(nextPush, nextPop);

    std::printf("test_queue_basic: PASS (pushed/popped %llu values, capacity 4)\n",
                static_cast<unsigned long long>(nextPop));
    return 0;
}
