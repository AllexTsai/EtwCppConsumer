#pragma once

#include "event_slot.h"
#include "spsc_ring.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// 來源：logman query providers Microsoft-Windows-Kernel-Process
//   Microsoft-Windows-Kernel-Process  {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
//   0x0000000000000010  WINEVENT_KEYWORD_PROCESS
//   0x04                win:Informational
constexpr GUID kKernelProcessProvider = {
    0x22FB2CD6, 0x0E7B, 0x422B, {0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16}};
constexpr ULONGLONG kKeywordProcess = 0x0000000000000010ULL;

// 佇列容量（槽位數，必須是 2 的冪次）。每槽約 4.2KB，1024 槽約 4.3MB。
// 足以吸收行程大量啟動的突發（每秒數百到上千個行程），
// 即使消費者因 console 輸出變慢而落後約一秒也不會丟事件。
constexpr size_t kDefaultQueueCapacity = 1024;

// 計數器都只用 relaxed：它們只做統計，不用來同步其他資料。
// 結束時的讀取發生在等待執行緒結束（WaitForSingleObject / join）之後，
// 該等待本身提供 happens-before。
struct EventCounters {
    std::atomic<uint64_t> received{0};          // callback 收到、屬於訂閱 provider 的事件
    std::atomic<uint64_t> enqueued{0};          // 成功放入佇列
    std::atomic<uint64_t> droppedQueueFull{0};  // 佇列滿而丟棄
    std::atomic<uint64_t> droppedOversize{0};   // UserData 超過槽位上限而丟棄
    std::atomic<uint64_t> consumed{0};          // 消費者處理完成
    std::atomic<uint64_t> tdhErrors{0};         // TDH 呼叫失敗次數
    std::atomic<uint64_t> otherProvider{0};     // 不屬於訂閱 provider（不列入恆等式）
    std::atomic<uint64_t> withExtendedData{0};  // 帶 ExtendedData 的事件（副本不保留）
    std::atomic<uint64_t> qpcFailures{0};       // QueryPerformanceCounter 失敗次數
    std::atomic<int64_t> maxCallbackTicks{0};   // callback 單次最長耗時（QPC ticks）
};

struct ConsumerContext {
    explicit ConsumerContext(size_t queueCapacity) : ring(queueCapacity) {}
    SpscRing<EventSlot> ring;
    EventCounters counters;
};

// callback 本體（不計時）：只檢查 provider、複製進槽位、更新原子計數器。
// 不呼叫 printf / TDH，不配置記憶體，不阻塞。
void HandleEventRecord(ConsumerContext& ctx, const EVENT_RECORD& record) noexcept;

// ETW 的 EventRecordCallback。record->UserContext 必須是 ConsumerContext*。
// 在 HandleEventRecord 前後用 QueryPerformanceCounter 計時，記錄最大值。
void WINAPI OnEventRecord(PEVENT_RECORD record);
