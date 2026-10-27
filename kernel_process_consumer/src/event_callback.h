#pragma once

#include "event_slot.h"
#include "latency_stat.h"
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
    LatencyStat callbackDuration;               // callback 耗時統計（只有 callback 執行緒寫入）
    std::atomic<uint64_t> dataReadySetEventFailures{0}; // notify 模式 SetEvent(dataReady) 失敗次數
};

struct ConsumerContext {
    explicit ConsumerContext(size_t queueCapacity) : ring(queueCapacity) {}
    SpscRing<EventSlot> ring;
    EventCounters counters;
    // notify 模式：auto-reset 事件，callback 每次 TryPush 成功後 SetEvent；
    // nullptr = poll 模式（不通知）。由呼叫端擁有（RAII），必須比 ProcessTrace 執行緒活得久。
    HANDLE dataReadyEvent = nullptr;
    // TEST ONLY: 人為延遲 callback，用來驗證遺失計數，不代表效能。
    // 大於 0 時 OnEventRecord 在處理完成後 Sleep 這麼多毫秒；0 = 正常行為（預設）。
    DWORD testCallbackDelayMs = 0;
};

// callback 本體（不計時）：只檢查 provider、複製進槽位、更新原子計數器；
// notify 模式下入佇列成功後 SetEvent（不阻塞、不配置記憶體）。不呼叫 printf / TDH。
// callbackEntryTick：callback 進入時的 QPC 值，存進槽位；0 表示無法取得（測試可省略）。
void HandleEventRecord(ConsumerContext& ctx, const EVENT_RECORD& record, int64_t callbackEntryTick = 0) noexcept;

// ETW 的 EventRecordCallback。record->UserContext 必須是 ConsumerContext*。
// 一進入就呼叫 QueryPerformanceCounter（存進槽位，並用來計算 callback 耗時的最大值）。
void WINAPI OnEventRecord(PEVENT_RECORD record);
