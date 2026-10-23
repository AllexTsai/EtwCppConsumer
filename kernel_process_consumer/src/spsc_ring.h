#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

// 單一生產者 / 單一消費者、固定容量的無鎖環形緩衝。
//
// 槽位在建構時一次配置完成，TryPush / TryPop 都不配置記憶體、不阻塞。
// head_ 與 tail_ 是只增不減的 64 位元計數，槽位索引為 index & mask_，
// 因此容量必須是 2 的冪次。
//
// 記憶體順序：
//  - tail_ 只由生產者寫入，head_ 只由消費者寫入。
//  - 生產者寫完槽位後以 release 發布 tail_；消費者以 acquire 讀取 tail_，
//    保證看得到完整的槽位內容。
//  - 消費者讀完槽位後以 release 發布 head_；生產者以 acquire 讀取 head_，
//    保證消費者對該槽位的讀取發生在生產者覆寫之前。
//  - 讀取自己擁有的索引用 relaxed（沒有其他執行緒寫它）。
//  - 不需要 seq_cst：每個索引只有一個寫入者，不需要跨變數的全域順序。
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), slots_(Allocate(capacity))
    {
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t Capacity() const { return capacity_; }

    // 只能由生產者呼叫。佇列滿時不呼叫 fill，直接回傳 false。
    template <typename Fill>
    bool TryPush(Fill&& fill)
    {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_)
            return false;
        fill(slots_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // 只能由消費者呼叫。read 執行期間槽位仍屬於消費者，回傳後才歸還給生產者。
    template <typename Read>
    bool TryPop(Read&& read)
    {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return false;
        read(slots_[head & mask_]);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 任何執行緒都可呼叫；並行時只是近似值。先讀 head 再讀 tail，結果不會是負數。
    size_t SizeApprox() const
    {
        const uint64_t head = head_.load(std::memory_order_acquire);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

private:
    static std::unique_ptr<T[]> Allocate(size_t capacity)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SpscRing capacity must be a non-zero power of two");
        return std::make_unique<T[]>(capacity);
    }

    static constexpr size_t kCacheLine = 64;
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

    const size_t capacity_;
    const size_t mask_;
    const std::unique_ptr<T[]> slots_;

    // 分開放在不同 cache line，避免生產者與消費者互相干擾（false sharing）
    alignas(kCacheLine) std::atomic<uint64_t> head_{0};
    alignas(kCacheLine) std::atomic<uint64_t> tail_{0};
};
