// 6b. 滿載：容量 8，用正式的 callback（OnEventRecord）全速送入，確認丟棄計數正確
#include "event_callback.h"
#include "test_util.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

constexpr size_t kSmallCapacity = 8;

uint64_t ReadPayloadSeq(const EventSlot& slot)
{
    uint64_t v = 0;
    CHECK_EQ(slot.userDataLength, sizeof(v));
    std::memcpy(&v, slot.userData, sizeof(v));
    return v;
}

void SendSeq(ConsumerContext& ctx, uint64_t seq)
{
    EVENT_RECORD r = MakeFakeRecord(kKernelProcessProvider, &seq, sizeof(seq), &ctx);
    OnEventRecord(&r);
}

void SingleThreadedFull()
{
    ConsumerContext ctx(kSmallCapacity);
    EventCounters& c = ctx.counters;

    // 沒有消費者，全速送入 100 筆：只有前 8 筆能入佇列
    for (uint64_t i = 0; i < 100; ++i)
        SendSeq(ctx, i);
    CHECK_EQ(c.received.load(), 100);
    CHECK_EQ(c.enqueued.load(), kSmallCapacity);
    CHECK_EQ(c.droppedQueueFull.load(), 100 - kSmallCapacity);
    CHECK_EQ(c.droppedOversize.load(), 0);

    // 佇列內必須是最早的 8 筆，順序正確
    for (uint64_t i = 0; i < kSmallCapacity; ++i) {
        uint64_t payload = 0, slotSeq = 0;
        CHECK(ctx.ring.TryPop([&](EventSlot& s) { payload = ReadPayloadSeq(s); slotSeq = s.sequence; }));
        CHECK_EQ(payload, i);
        CHECK_EQ(slotSeq, i);
        c.consumed.fetch_add(1);
    }
    CHECK(!ctx.ring.TryPop([](EventSlot&) {}));

    // 清空後再送 8 筆（索引環繞到同一批槽位），全部要成功
    for (uint64_t i = 100; i < 108; ++i)
        SendSeq(ctx, i);
    CHECK_EQ(c.enqueued.load(), 2 * kSmallCapacity);
    for (uint64_t i = 100; i < 108; ++i) {
        uint64_t payload = 0;
        CHECK(ctx.ring.TryPop([&](EventSlot& s) { payload = ReadPayloadSeq(s); }));
        CHECK_EQ(payload, i);
        c.consumed.fetch_add(1);
    }

    // 過大邊界：剛好等於上限可入佇列，多 1 byte 丟棄
    static BYTE big[kMaxUserDataBytes + 1];
    EVENT_RECORD exact = MakeFakeRecord(kKernelProcessProvider, big, static_cast<USHORT>(kMaxUserDataBytes), &ctx);
    OnEventRecord(&exact);
    CHECK_EQ(c.enqueued.load(), 2 * kSmallCapacity + 1);
    EVENT_RECORD over = MakeFakeRecord(kKernelProcessProvider, big, static_cast<USHORT>(kMaxUserDataBytes + 1), &ctx);
    OnEventRecord(&over);
    CHECK_EQ(c.droppedOversize.load(), 1);
    CHECK(ctx.ring.TryPop([&](EventSlot& s) { CHECK_EQ(s.userDataLength, kMaxUserDataBytes); }));
    c.consumed.fetch_add(1);

    // 其他 provider：不計入 received，也不入佇列
    uint64_t dummy = 0;
    EVENT_RECORD other = MakeFakeRecord(kTestOtherProvider, &dummy, sizeof(dummy), &ctx);
    OnEventRecord(&other);
    CHECK_EQ(c.otherProvider.load(), 1);

    // 恆等式
    CHECK_EQ(c.received.load(), c.enqueued.load() + c.droppedQueueFull.load() + c.droppedOversize.load());
    CHECK_EQ(c.enqueued.load(), c.consumed.load());

    std::printf("  single-threaded: received=%llu enqueued=%llu droppedFull=%llu droppedOversize=%llu other=%llu\n",
                static_cast<unsigned long long>(c.received.load()), static_cast<unsigned long long>(c.enqueued.load()),
                static_cast<unsigned long long>(c.droppedQueueFull.load()),
                static_cast<unsigned long long>(c.droppedOversize.load()),
                static_cast<unsigned long long>(c.otherProvider.load()));
}

void ConcurrentFull()
{
    // 容量 8，生產者全速、消費者同時取出：一定會有丟棄，恆等式仍須成立
    constexpr uint64_t kCount = 1000000;
    ConsumerContext ctx(kSmallCapacity);
    EventCounters& c = ctx.counters;
    std::atomic<bool> producerDone{false};

    std::thread consumer([&] {
        uint64_t last = 0;
        bool first = true;
        for (;;) {
            const bool done = producerDone.load(std::memory_order_acquire);
            bool got = false;
            while (ctx.ring.TryPop([&](EventSlot& s) {
                const uint64_t v = ReadPayloadSeq(s);
                CHECK(first || v > last);
                first = false;
                last = v;
            })) {
                got = true;
                c.consumed.fetch_add(1, std::memory_order_relaxed);
            }
            if (done && !got)
                break;
            if (!got)
                std::this_thread::yield();
        }
    });

    for (uint64_t i = 0; i < kCount; ++i)
        SendSeq(ctx, i);
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    CHECK_EQ(c.received.load(), kCount);
    CHECK_EQ(c.received.load(), c.enqueued.load() + c.droppedQueueFull.load() + c.droppedOversize.load());
    CHECK_EQ(c.enqueued.load(), c.consumed.load());
    CHECK(c.droppedQueueFull.load() > 0);

    std::printf("  concurrent: received=%llu enqueued=%llu droppedFull=%llu consumed=%llu\n",
                static_cast<unsigned long long>(c.received.load()), static_cast<unsigned long long>(c.enqueued.load()),
                static_cast<unsigned long long>(c.droppedQueueFull.load()),
                static_cast<unsigned long long>(c.consumed.load()));
}

} // namespace

int main()
{
    std::printf("test_queue_full (capacity %zu):\n", kSmallCapacity);
    SingleThreadedFull();
    ConcurrentFull();
    std::printf("test_queue_full: PASS\n");
    return 0;
}
