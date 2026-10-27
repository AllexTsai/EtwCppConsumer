#pragma once

#include <atomic>
#include <cstdint>

// 單一延遲指標的累計值（不保存個別樣本）。每個實例只能有一個寫入執行緒
// （消費者或 ETW callback）；Add 不配置記憶體、不阻塞，可在 callback 內呼叫。
// 用 atomic（relaxed）是因為逾時路徑上主執行緒可能在寫入者仍執行時讀取。
struct LatencyStat {
    std::atomic<uint64_t> count{0};
    std::atomic<int64_t> sumTicks{0};
    std::atomic<int64_t> maxTicks{0};
    std::atomic<uint64_t> excluded{0}; // 缺少 tick（QPC 失敗）或時鐘無效，不計入

    void Add(int64_t ticks)
    {
        const uint64_t n = count.load(std::memory_order_relaxed);
        if (n == 0 || ticks > maxTicks.load(std::memory_order_relaxed))
            maxTicks.store(ticks, std::memory_order_relaxed);
        sumTicks.fetch_add(ticks, std::memory_order_relaxed);
        count.store(n + 1, std::memory_order_relaxed);
    }
};
