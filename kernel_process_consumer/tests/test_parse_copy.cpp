// 6d. 副本解析一致：同一批真實 ETW 事件，比較
//   A：在 callback 內直接對原始 EVENT_RECORD 用 TDH 解析（僅測試這樣做）
//   B：經正式路徑（HandleEventRecord -> 佇列槽位 -> RebuildRecord）後，在消費者執行緒用 TDH 解析
// 兩者所有欄位必須相同。需要管理員權限；權限不足回傳 77（CTest 視為 SKIP）。
#include "etw_raii.h"
#include "event_callback.h"
#include "event_parser.h"
#include "test_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kTestSessionName[] = L"EtwCppConsumer-ParseCopyTest";
constexpr int kProcessesToSpawn = 10;

struct TestContext {
    TestContext() : ctx(kDefaultQueueCapacity) {}
    ConsumerContext ctx;
    std::vector<ParsedEvent> direct; // 只由 callback 執行緒寫入，索引 == 序號
    std::atomic<uint64_t> directTdhErrors{0};
};

void WINAPI TestCallback(PEVENT_RECORD record)
{
    auto* t = static_cast<TestContext*>(record->UserContext);
    if (IsEqualGUID(record->EventHeader.ProviderId, kKernelProcessProvider)) {
        ParsedEvent ev;
        ParseEvent(*record, ev, t->directTdhErrors);
        t->direct.push_back(std::move(ev));
    }
    // 正式路徑：與 OnEventRecord 相同的本體
    HandleEventRecord(t->ctx, *record);
}

void SpawnProcesses()
{
    for (int i = 0; i < kProcessesToSpawn; ++i) {
        wchar_t cmdLine[] = L"cmd.exe /c exit";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        CHECK(CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        ScopedHandle process(pi.hProcess);
        ScopedHandle thread(pi.hThread);
        CHECK(WaitForSingleObject(process.Get(), 10000) == WAIT_OBJECT_0);
    }
}

} // namespace

int main()
{
    TestContext t;
    t.direct.reserve(4096);

    EtwSession session(kTestSessionName);
    ULONG status = session.Start();
    if (status == ERROR_ACCESS_DENIED) {
        std::printf("test_parse_copy: SKIP (ERROR_ACCESS_DENIED, needs Administrator)\n");
        return 77;
    }
    CHECK_EQ(status, ERROR_SUCCESS);

    status = EnableTraceEx2(session.Handle(), &kKernelProcessProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, kKeywordProcess, 0, 0, nullptr);
    CHECK_EQ(status, ERROR_SUCCESS);

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<LPWSTR>(kTestSessionName);
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback = TestCallback;
    logfile.Context = &t;
    EtwTraceHandle trace(OpenTraceW(&logfile));
    CHECK(trace.Valid());

    // 消費者：從副本重建並解析，(序號, 結果) 存起來
    std::vector<std::pair<uint64_t, ParsedEvent>> fromCopy;
    std::atomic<bool> producerDone{false};
    std::thread consumer([&] {
        for (;;) {
            const bool done = producerDone.load(std::memory_order_acquire);
            bool got = false;
            ParsedEvent ev;
            uint64_t seq = 0;
            while (t.ctx.ring.TryPop([&](EventSlot& slot) {
                EVENT_RECORD rebuilt = RebuildRecord(slot);
                ParseEvent(rebuilt, ev, t.ctx.counters.tdhErrors);
                seq = slot.sequence;
            })) {
                got = true;
                t.ctx.counters.consumed.fetch_add(1);
                fromCopy.emplace_back(seq, ev);
            }
            if (done && !got)
                break;
            if (!got)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    ULONG processTraceStatus = ERROR_SUCCESS;
    TRACEHANDLE handle = trace.Get();
    std::thread etw([&] { processTraceStatus = ProcessTrace(&handle, 1, nullptr, nullptr); });

    SpawnProcesses();
    Sleep(2500); // FlushTimer = 1 秒，等即時緩衝送達

    CHECK_EQ(session.Stop(), ERROR_SUCCESS);
    etw.join();
    CHECK_EQ(processTraceStatus, ERROR_SUCCESS);
    producerDone.store(true, std::memory_order_release);
    consumer.join();
    CHECK_EQ(trace.Close(), ERROR_SUCCESS);

    const EventCounters& c = t.ctx.counters;
    std::printf("received=%llu enqueued=%llu droppedFull=%llu droppedOversize=%llu consumed=%llu "
                "withExtendedData=%llu direct=%zu fromCopy=%zu\n",
                static_cast<unsigned long long>(c.received.load()), static_cast<unsigned long long>(c.enqueued.load()),
                static_cast<unsigned long long>(c.droppedQueueFull.load()),
                static_cast<unsigned long long>(c.droppedOversize.load()),
                static_cast<unsigned long long>(c.consumed.load()),
                static_cast<unsigned long long>(c.withExtendedData.load()), t.direct.size(), fromCopy.size());

    CHECK_EQ(c.droppedQueueFull.load(), 0);
    CHECK_EQ(c.droppedOversize.load(), 0);
    CHECK_EQ(t.direct.size(), c.received.load());
    CHECK_EQ(fromCopy.size(), t.direct.size());
    CHECK_EQ(c.enqueued.load(), c.consumed.load());
    CHECK_EQ(t.directTdhErrors.load(), 0);
    CHECK_EQ(c.tdhErrors.load(), 0);

    // 逐筆、逐欄位比對
    size_t fieldsCompared = 0;
    int cmdStarts = 0;
    for (const auto& [seq, fromSlot] : fromCopy) {
        CHECK(seq < t.direct.size());
        const ParsedEvent& fromCallback = t.direct[seq];
        const std::wstring diff = DescribeDifference(fromCallback, fromSlot);
        if (!diff.empty()) {
            std::printf("MISMATCH at seq=%llu: %ls\n", static_cast<unsigned long long>(seq), diff.c_str());
            return 1;
        }
        CHECK(fromCallback == fromSlot);
        CHECK_EQ(fromSlot.tdhInfoStatus, ERROR_SUCCESS);
        CHECK(!fromSlot.properties.empty());
        fieldsCompared += 11 + fromSlot.properties.size();
        for (const ParsedProperty& p : fromSlot.properties) {
            CHECK(p.ok);
            if (fromSlot.id == 1 && p.name == L"ImageName") {
                const size_t len = p.value.size();
                if (len >= 8 && _wcsicmp(p.value.c_str() + len - 8, L"\\cmd.exe") == 0)
                    ++cmdStarts;
            }
        }
    }
    CHECK(cmdStarts >= kProcessesToSpawn);

    std::printf("test_parse_copy: PASS (%zu events, %zu fields compared, %d cmd.exe ProcessStart events)\n",
                fromCopy.size(), fieldsCompared, cmdStarts);
    return 0;
}
