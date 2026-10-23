// 6c. 壓力測試：生產者與消費者兩個執行緒、數百萬筆。
// 生產者呼叫正式的 callback（OnEventRecord），每筆 UserData 帶序號、可變長度內容與校驗值。
// 消費者確認：沒有重複、沒有損壞、序號嚴格遞增；結束時確認計數器恆等式，
// 並逐筆比對「入佇列的集合」與「消費者收到的集合」完全相同。
#include "event_callback.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kCount = 5000000;
constexpr size_t kCapacity = kDefaultQueueCapacity;
constexpr uint64_t kOversizeEvery = 1009;     // 這些序號送出過大的事件
constexpr uint64_t kOtherProviderEvery = 997; // 這些序號用其他 provider
constexpr size_t kMaxWords = 30;

uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

size_t WordCount(uint64_t seq) { return 1 + static_cast<size_t>(seq % kMaxWords); }

// 版面：[seq][words...][checksum]，checksum = FNV-1a over 前面所有位元組
uint64_t Checksum(const BYTE* data, size_t len)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

USHORT BuildPayload(uint64_t seq, BYTE* buf)
{
    const size_t words = WordCount(seq);
    std::memcpy(buf, &seq, 8);
    for (size_t i = 0; i < words; ++i) {
        const uint64_t w = SplitMix64(seq * 64 + i);
        std::memcpy(buf + 8 + i * 8, &w, 8);
    }
    const size_t bodyLen = 8 + words * 8;
    const uint64_t sum = Checksum(buf, bodyLen);
    std::memcpy(buf + bodyLen, &sum, 8);
    return static_cast<USHORT>(bodyLen + 8);
}

} // namespace

int main()
{
    ConsumerContext ctx(kCapacity);
    EventCounters& c = ctx.counters;

    // 預先配置的旗標陣列：producer 記錄哪些序號成功入佇列，consumer 記錄收到哪些
    std::vector<uint8_t> enqueuedFlag(kCount, 0);
    std::vector<uint8_t> consumedFlag(kCount, 0);
    std::atomic<bool> producerDone{false};

    uint64_t expectedOversize = 0;
    uint64_t expectedOther = 0;
    uint64_t providerAttempts = 0;

    const auto t0 = std::chrono::steady_clock::now();

    std::thread consumer([&] {
        uint64_t lastPayloadSeq = 0;
        uint64_t lastSlotSeq = 0;
        bool first = true;
        for (;;) {
            const bool done = producerDone.load(std::memory_order_acquire);
            bool got = false;
            while (ctx.ring.TryPop([&](EventSlot& s) {
                CHECK(IsEqualGUID(s.header.ProviderId, kKernelProcessProvider));
                CHECK(s.userDataLength >= 24);
                uint64_t seq = 0;
                std::memcpy(&seq, s.userData, 8);
                CHECK(seq < kCount);
                // 長度、內容、校驗值
                const size_t words = WordCount(seq);
                CHECK_EQ(s.userDataLength, 8 + words * 8 + 8);
                for (size_t i = 0; i < words; ++i) {
                    uint64_t w = 0;
                    std::memcpy(&w, s.userData + 8 + i * 8, 8);
                    CHECK_EQ(w, SplitMix64(seq * 64 + i));
                }
                uint64_t sum = 0;
                std::memcpy(&sum, s.userData + 8 + words * 8, 8);
                CHECK_EQ(sum, Checksum(s.userData, 8 + words * 8));
                // 嚴格遞增（同時保證沒有重複）
                CHECK(first || seq > lastPayloadSeq);
                CHECK(first || s.sequence > lastSlotSeq);
                first = false;
                lastPayloadSeq = seq;
                lastSlotSeq = s.sequence;
                CHECK(consumedFlag[seq] == 0);
                consumedFlag[seq] = 1;
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

    alignas(16) static BYTE buffer[kMaxUserDataBytes + 64];
    for (uint64_t seq = 0; seq < kCount; ++seq) {
        EVENT_RECORD r;
        if (seq % kOtherProviderEvery == 0 && seq != 0) {
            const USHORT len = BuildPayload(seq, buffer);
            r = MakeFakeRecord(kTestOtherProvider, buffer, len, &ctx);
            ++expectedOther;
        } else if (seq % kOversizeEvery == 0 && seq != 0) {
            r = MakeFakeRecord(kKernelProcessProvider, buffer, static_cast<USHORT>(kMaxUserDataBytes + 1), &ctx);
            ++expectedOversize;
            ++providerAttempts;
        } else {
            const USHORT len = BuildPayload(seq, buffer);
            r = MakeFakeRecord(kKernelProcessProvider, buffer, len, &ctx);
            ++providerAttempts;
        }
        // enqueued 只有這個執行緒會改，可以用前後差值判斷這筆是否入佇列
        const uint64_t before = c.enqueued.load(std::memory_order_relaxed);
        OnEventRecord(&r);
        if (c.enqueued.load(std::memory_order_relaxed) != before)
            enqueuedFlag[seq] = 1;
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const uint64_t received = c.received.load();
    const uint64_t enqueued = c.enqueued.load();
    const uint64_t droppedFull = c.droppedQueueFull.load();
    const uint64_t droppedOversize = c.droppedOversize.load();
    const uint64_t consumed = c.consumed.load();

    CHECK_EQ(received, providerAttempts);
    CHECK_EQ(c.otherProvider.load(), expectedOther);
    CHECK_EQ(droppedOversize, expectedOversize);
    CHECK_EQ(received, enqueued + droppedFull + droppedOversize); // 恆等式 1
    CHECK_EQ(enqueued, consumed);                                 // 恆等式 2

    // 入佇列的集合 == 消費者收到的集合
    uint64_t mismatches = 0;
    for (uint64_t i = 0; i < kCount; ++i)
        if (enqueuedFlag[i] != consumedFlag[i])
            ++mismatches;
    CHECK_EQ(mismatches, 0);

    LARGE_INTEGER freq{};
    CHECK(QueryPerformanceFrequency(&freq));
    const double maxUs =
        static_cast<double>(c.maxCallbackTicks.load()) * 1e6 / static_cast<double>(freq.QuadPart);

    std::printf("test_queue_stress: PASS\n");
    std::printf("  records sent=%llu (provider=%llu, other=%llu), capacity=%zu, %.2f s\n",
                static_cast<unsigned long long>(kCount), static_cast<unsigned long long>(providerAttempts),
                static_cast<unsigned long long>(expectedOther), kCapacity, seconds);
    std::printf("  received=%llu enqueued=%llu droppedFull=%llu droppedOversize=%llu consumed=%llu\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(enqueued),
                static_cast<unsigned long long>(droppedFull), static_cast<unsigned long long>(droppedOversize),
                static_cast<unsigned long long>(consumed));
    std::printf("  identity1 %llu == %llu, identity2 %llu == %llu, set mismatches=%llu\n",
                static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(enqueued + droppedFull + droppedOversize),
                static_cast<unsigned long long>(enqueued), static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(mismatches));
    std::printf("  max callback duration=%.3f us, qpcFailures=%llu\n", maxUs,
                static_cast<unsigned long long>(c.qpcFailures.load()));
    return 0;
}
