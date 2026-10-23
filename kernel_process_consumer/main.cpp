#include "etw_raii.h"
#include "event_callback.h"
#include "event_parser.h"
#include "event_slot.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cerrno>
#include <clocale>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kSessionName[] = L"EtwCppConsumer-KernelProcess";
constexpr size_t kQueueCapacity = kDefaultQueueCapacity;
// --capacity 的上限：65536 槽 × 約 4.2KB ≈ 275MB，避免打錯數字時配置過大的記憶體
constexpr size_t kMaxQueueCapacity = 65536;
constexpr DWORD kThreadJoinTimeoutMs = 5000;
constexpr DWORD kConsumerIdleWaitMs = 1; // 佇列空時的等待；實際解析度受系統計時器影響

// 程式自己定義的結束碼（啟動階段失敗時仍回傳 Win32 錯誤碼）
enum : int {
    kExitOk = 0,
    kExitEtwThreadTimeout = 2,
    kExitWaitFailed = 3,
    kExitConsumerTimeout = 4,
    kExitIdentityMismatch = 5,
    kExitProcessTraceFailed = 6,
    kExitConsumerWaitFailed = 7,
    kExitCloseTraceFailed = 8,
    kExitStopTraceFailed = 9,
    kExitSetDoneFailed = 10,
    kExitInvalidArguments = 11,
};

// 解析命令列。支援：
//   --capacity N          N 為 2 的冪次，1..kMaxQueueCapacity
//   --summary-file PATH   結束時另外把總結以 UTF-8 寫進這個檔案
// 格式錯誤時印出原因並回傳 false；不認得的參數只警告並忽略。
bool ParseArguments(int argc, char** argv, size_t& capacity, bool& fromCommandLine, const char*& summaryFile)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--summary-file") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::printf("ERROR: --summary-file requires a non-empty path\n");
                return false;
            }
            summaryFile = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--capacity") != 0) {
            std::printf("WARNING: unknown argument '%s' ignored\n", argv[i]);
            continue;
        }
        if (i + 1 >= argc) {
            std::printf("ERROR: --capacity requires a value\n");
            return false;
        }
        const char* text = argv[++i];
        // strtoull 會接受前導空白與負號，所以先確認整個字串都是十進位數字
        if (*text == '\0' || std::strspn(text, "0123456789") != std::strlen(text)) {
            std::printf("ERROR: --capacity value '%s' is not a decimal number\n", text);
            return false;
        }
        errno = 0;
        const unsigned long long value = std::strtoull(text, nullptr, 10);
        if (errno == ERANGE || value == 0 || value > kMaxQueueCapacity || (value & (value - 1)) != 0) {
            std::printf("ERROR: --capacity must be a power of two between 1 and %zu, got '%s'\n", kMaxQueueCapacity,
                        text);
            return false;
        }
        capacity = static_cast<size_t>(value);
        fromCommandLine = true;
    }
    return true;
}

void AppendFormat(std::string& out, const char* fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
        out += "<vsnprintf failed>\n";
    else
        out += buf;
}

// 把總結寫進檔案（內容全為 ASCII，以 UTF-8 無 BOM 寫入）。
// 失敗只印警告並回傳 false，不影響結束碼與關閉流程。
// 路徑以系統 ANSI 字碼頁解讀（argv 為窄字元），無法表示的 Unicode 字元會開檔失敗。
bool WriteSummaryFile(const char* path, const std::string& text)
{
    ScopedHandle file(CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        std::printf("WARNING: cannot create summary file '%s', error=%lu\n", path, GetLastError());
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(file.Get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
        std::printf("WARNING: WriteFile(summary file '%s') failed, error=%lu\n", path, GetLastError());
        return false;
    }
    if (written != text.size()) {
        std::printf("WARNING: summary file '%s' partially written (%lu of %zu bytes)\n", path, written, text.size());
        return false;
    }
    std::printf("Summary written to '%s' (%lu bytes)\n", path, written);
    return true;
}

// ---------------------------------------------------------------------------
// ProcessTrace 執行緒
// ---------------------------------------------------------------------------

struct EtwThreadArgs {
    TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
    ULONG status = ERROR_SUCCESS;
};

DWORD WINAPI ProcessTraceThread(LPVOID param)
{
    auto* args = static_cast<EtwThreadArgs*>(param);
    TRACEHANDLE handle = args->trace;
    std::printf("[etw %lu] ProcessTrace started\n", GetCurrentThreadId());
    std::fflush(stdout);
    // 返回後不輸出，狀態由主執行緒在關鍵步驟完成後印出
    args->status = ProcessTrace(&handle, 1, nullptr, nullptr);
    return args->status;
}

// ---------------------------------------------------------------------------
// 消費者執行緒：從佇列取出副本，重建 EVENT_RECORD，用 TDH 解析並輸出
// ---------------------------------------------------------------------------

// RAW_TIMESTAMP 下事件時間戳是 QPC tick。用啟動時取得的一組 (QPC, 系統時間) 當基準換算成 FILETIME。
// 即時模式的 LogfileHeader.StartTime 為 0（timestamp_probe 實測），無法用官方文件的 StartTime 基準法。
struct QpcClock {
    bool valid = false;          // ReservedFlags==1、PerfFreq==QueryPerformanceFrequency 且基準取得成功
    LONGLONG freq = 0;           // LogfileHeader.PerfFreq（事件時間戳的頻率）
    LONGLONG qpcFrequency = 0;   // QueryPerformanceFrequency（queue_delay_us 用；失敗時為 0）
    LONGLONG qpcBase = 0;
    ULONGLONG fileTimeBase = 0;
};

// QPC tick 換算成 FILETIME；無法換算時回傳 0（FormatEventText 顯示 <unavailable>）
ULONGLONG QpcToFileTime(const QpcClock& clock, LONGLONG tick)
{
    if (!clock.valid)
        return 0;
    // 拆成整數秒與餘數，避免 delta * 10^7 溢位
    const LONGLONG delta = tick - clock.qpcBase;
    const LONGLONG units = (delta / clock.freq) * 10000000LL + (delta % clock.freq) * 10000000LL / clock.freq;
    const LONGLONG fileTime = static_cast<LONGLONG>(clock.fileTimeBase) + units;
    return fileTime > 0 ? static_cast<ULONGLONG>(fileTime) : 0;
}

double TicksToUs(int64_t ticks, LONGLONG freq)
{
    return static_cast<double>(ticks) * 1e6 / static_cast<double>(freq);
}

// 單一延遲指標的累計值（不保存個別樣本）。只有消費者執行緒寫入；
// 用 atomic（relaxed）是因為逾時路徑上主執行緒可能在消費者仍執行時讀取。
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

struct LatencyStats {
    LatencyStat etwDelay;   // callbackEntryTick - eventTick
    LatencyStat queueWait;  // dequeueTick - callbackEntryTick
    LatencyStat process;    // consumerDoneTick - dequeueTick
};

// 計算 toTick - fromTick 並累計；任一 tick 為 0（無法取得）或頻率無效時不計入、計數，回傳 "n/a"
std::wstring RecordDelay(LatencyStat& stat, int64_t fromTick, int64_t toTick, LONGLONG freq)
{
    if (fromTick == 0 || toTick == 0 || freq <= 0) {
        stat.excluded.fetch_add(1, std::memory_order_relaxed);
        return L"n/a";
    }
    const int64_t ticks = toTick - fromTick;
    stat.Add(ticks);
    wchar_t buf[64];
    if (std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f", TicksToUs(ticks, freq)) < 0)
        return L"<swprintf failed>";
    return buf;
}

// 結束時的統計（console 與 --summary-file 共用格式）
void AppendLatencyStat(std::string& out, const char* name, const LatencyStat& stat, LONGLONG freq,
                       const char* separator)
{
    const uint64_t n = stat.count.load(std::memory_order_relaxed);
    AppendFormat(out, "%s_count=%llu%s", name, static_cast<unsigned long long>(n), separator);
    if (n > 0 && freq > 0) {
        AppendFormat(out, "%s_avg_us=%.1f%s", name,
                     TicksToUs(stat.sumTicks.load(std::memory_order_relaxed), freq) / static_cast<double>(n), separator);
        AppendFormat(out, "%s_max_us=%.1f%s", name, TicksToUs(stat.maxTicks.load(std::memory_order_relaxed), freq),
                     separator);
    } else {
        AppendFormat(out, "%s_avg_us=n/a%s%s_max_us=n/a%s", name, separator, name, separator);
    }
    AppendFormat(out, "%s_excluded=%llu\n", name,
                 static_cast<unsigned long long>(stat.excluded.load(std::memory_order_relaxed)));
}

struct ConsumerThreadArgs {
    ConsumerContext* ctx = nullptr;
    const QpcClock* clock = nullptr;
    LatencyStats* latency = nullptr;
    HANDLE doneEvent = nullptr;       // 設定後代表「不會再有新事件」
    DWORD waitError = ERROR_SUCCESS;
};

void DrainQueue(ConsumerContext& ctx, const QpcClock& clock, LatencyStats& latency)
{
    ParsedEvent ev;
    uint64_t sequence = 0;
    int64_t eventTick = 0;
    int64_t callbackEntryTick = 0;
    int64_t dequeueTick = 0;
    for (;;) {
        // 解析在 TryPop 的 lambda 內完成：重建的 EVENT_RECORD 指向槽位，槽位歸還前必須用完
        const bool got = ctx.ring.TryPop([&](EventSlot& slot) {
            // pop 成功：立刻取 QPC
            LARGE_INTEGER dq;
            if (QueryPerformanceCounter(&dq)) {
                dequeueTick = dq.QuadPart;
            } else {
                dequeueTick = 0;
                ctx.counters.qpcFailures.fetch_add(1, std::memory_order_relaxed);
            }
            EVENT_RECORD record = RebuildRecord(slot);
            // RAW_TIMESTAMP：槽位內是 QPC tick，ParseEvent 需要 FILETIME，只在重建的副本上換算
            record.EventHeader.TimeStamp.QuadPart = static_cast<LONGLONG>(QpcToFileTime(clock, slot.eventTick));
            ParseEvent(record, ev, ctx.counters.tdhErrors);
            sequence = slot.sequence;
            eventTick = slot.eventTick;
            callbackEntryTick = slot.callbackEntryTick;
        });
        if (!got)
            return;

        // 處理完成、輸出之前
        LARGE_INTEGER done;
        int64_t consumerDoneTick = 0;
        if (QueryPerformanceCounter(&done))
            consumerDoneTick = done.QuadPart;
        else
            ctx.counters.qpcFailures.fetch_add(1, std::memory_order_relaxed);

        ctx.counters.consumed.fetch_add(1, std::memory_order_relaxed);

        // ETW 緩衝延遲需要事件時間戳是 QPC（clock.valid），佇列等待與處理時間只需要 QPC 頻率。
        // process_us 結束點是 consumerDoneTick（輸出之前），所以不含這一筆自己的 printf。
        const std::wstring delays =
            L"  etw_delay_us=" +
            RecordDelay(latency.etwDelay, eventTick, callbackEntryTick, clock.valid ? clock.freq : 0) +
            L"  queue_wait_us=" + RecordDelay(latency.queueWait, callbackEntryTick, dequeueTick, clock.qpcFrequency) +
            L"  process_us=" + RecordDelay(latency.process, dequeueTick, consumerDoneTick, clock.qpcFrequency);
        std::wstring text = L"---------------- seq=" + std::to_wstring(sequence) + L" ----------------\n" +
                            FormatEventText(ev, delays);
        std::printf("%ls", text.c_str());
        std::fflush(stdout);
    }
}

DWORD WINAPI ConsumerThread(LPVOID param)
{
    auto* args = static_cast<ConsumerThreadArgs*>(param);
    std::printf("[consumer %lu] started\n", GetCurrentThreadId());
    std::fflush(stdout);
    for (;;) {
        DrainQueue(*args->ctx, *args->clock, *args->latency);
        const DWORD w = WaitForSingleObject(args->doneEvent, kConsumerIdleWaitMs);
        if (w == WAIT_OBJECT_0) {
            // 生產者已確定停止：最後再清空一次
            DrainQueue(*args->ctx, *args->clock, *args->latency);
            return 0;
        }
        if (w == WAIT_FAILED) {
            args->waitError = GetLastError();
            DrainQueue(*args->ctx, *args->clock, *args->latency);
            return 1;
        }
        // WAIT_TIMEOUT：繼續輪詢
    }
}

// ---------------------------------------------------------------------------
// Ctrl+C
// ---------------------------------------------------------------------------

HANDLE g_stopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType)
{
    // 先通知主執行緒，再做任何可能阻塞的輸出
    const BOOL setOk = SetEvent(g_stopEvent);
    const DWORD setError = setOk ? ERROR_SUCCESS : GetLastError();

    std::printf("[%s] [CTRL+C] received (ctrlType=%lu, thread=%lu)\n", TimestampNow().c_str(), ctrlType,
                GetCurrentThreadId());
    std::fflush(stdout);
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        std::printf("\n[ctrl] Received %s, shutting down...\n",
                    ctrlType == CTRL_C_EVENT ? "CTRL_C_EVENT" : "CTRL_BREAK_EVENT");
        std::fflush(stdout);
        if (!setOk)
            std::printf("[ctrl] SetEvent failed, error=%lu\n", setError);
        return TRUE;
    default:
        return FALSE;
    }
}

const char* WaitResultName(DWORD r)
{
    switch (r) {
    case WAIT_OBJECT_0: return "WAIT_OBJECT_0";
    case WAIT_TIMEOUT: return "WAIT_TIMEOUT";
    case WAIT_FAILED: return "WAIT_FAILED";
    default: return "other";
    }
}

} // namespace

int main(int argc, char** argv)
{
    // 讓 %ls 能輸出任何 Unicode 字元（例如中文路徑）
    if (!SetConsoleOutputCP(CP_UTF8))
        std::printf("SetConsoleOutputCP(CP_UTF8) failed, error=%lu (continuing)\n", GetLastError());
    if (!std::setlocale(LC_ALL, ".UTF8"))
        std::printf("setlocale(.UTF8) failed (continuing)\n");

    size_t queueCapacity = kQueueCapacity;
    bool capacityFromCommandLine = false;
    const char* summaryFile = nullptr;
    if (!ParseArguments(argc, argv, queueCapacity, capacityFromCommandLine, summaryFile)) {
        std::printf("Usage: kernel_process_consumer.exe [--capacity N] [--summary-file PATH]   (default N=%zu)\n",
                    kQueueCapacity);
        return kExitInvalidArguments;
    }

    // 第一個建立、最後一個解構：所有 return 路徑都會還原 console 模式
    ConsoleModeGuard consoleMode;

    LARGE_INTEGER qpcFrequency{};
    if (!QueryPerformanceFrequency(&qpcFrequency)) {
        std::printf("WARNING: QueryPerformanceFrequency failed, error=%lu; callback duration not reported\n",
                    GetLastError());
        qpcFrequency.QuadPart = 0;
    }

    ScopedHandle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent.Valid()) {
        std::printf("CreateEventW(stop) failed, error=%lu\n", GetLastError());
        return 1;
    }
    g_stopEvent = stopEvent.Get();

    ScopedHandle doneEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!doneEvent.Valid()) {
        std::printf("CreateEventW(done) failed, error=%lu\n", GetLastError());
        return 1;
    }

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::printf("SetConsoleCtrlHandler failed, error=%lu\n", GetLastError());
        return 1;
    }

    // 佇列在這裡一次配置完成；callback 之後不再配置記憶體
    std::unique_ptr<ConsumerContext> ctxHolder;
    try {
        ctxHolder = std::make_unique<ConsumerContext>(queueCapacity);
    } catch (const std::bad_alloc&) {
        std::printf("ERROR: cannot allocate queue of %zu slots (%zu bytes)\n", queueCapacity,
                    queueCapacity * sizeof(EventSlot));
        return 1;
    }
    ConsumerContext& ctx = *ctxHolder;
    std::printf("Queue: capacity=%zu slots (%s), slot size=%zu bytes, max UserData=%zu bytes, total=%zu bytes\n",
                ctx.ring.Capacity(), capacityFromCommandLine ? "from --capacity" : "default", sizeof(EventSlot),
                kMaxUserDataBytes, ctx.ring.Capacity() * sizeof(EventSlot));

    EtwSession session(kSessionName);
    ULONG status = session.Start();
    if (status != ERROR_SUCCESS) {
        if (status == ERROR_ACCESS_DENIED)
            std::printf("ERROR_ACCESS_DENIED: run as Administrator\n");
        return static_cast<int>(status);
    }
    std::printf("Session started, handle=0x%llx\n", static_cast<unsigned long long>(session.Handle()));

    status = EnableTraceEx2(session.Handle(), &kKernelProcessProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, kKeywordProcess, 0, 0, nullptr);
    std::printf("EnableTraceEx2(Microsoft-Windows-Kernel-Process %ls, Level=%u, Keyword=0x%llx) returned %lu\n",
                FormatGuid(kKernelProcessProvider).c_str(), TRACE_LEVEL_INFORMATION,
                static_cast<unsigned long long>(kKeywordProcess), status);
    if (status != ERROR_SUCCESS)
        return static_cast<int>(status);

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<LPWSTR>(kSessionName);
    // RAW_TIMESTAMP：EVENT_HEADER.TimeStamp 保留 ClientContext 指定的原始格式（此程式為 QPC），
    // 方便與 callback / 消費者的 QueryPerformanceCounter 直接相減（timestamp_probe 驗證過的設定）
    logfile.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    logfile.EventRecordCallback = OnEventRecord;
    logfile.Context = &ctx;

    EtwTraceHandle trace(OpenTraceW(&logfile));
    if (!trace.Valid()) {
        std::printf("OpenTraceW failed, error=%lu\n", GetLastError());
        return 1;
    }
    std::printf("OpenTraceW succeeded, trace handle=0x%llx\n", static_cast<unsigned long long>(trace.Get()));

    // 時鐘檢查與 QPC -> FILETIME 換算基準（在任何執行緒啟動前完成，之後唯讀）
    QpcClock clock;
    {
        const TRACE_LOGFILE_HEADER& hdr = logfile.LogfileHeader;
        clock.freq = hdr.PerfFreq.QuadPart;
        clock.qpcFrequency = qpcFrequency.QuadPart;
        LARGE_INTEGER baseTick;
        const BOOL baseOk = QueryPerformanceCounter(&baseTick);
        FILETIME baseFileTime;
        GetSystemTimePreciseAsFileTime(&baseFileTime);
        if (!baseOk)
            ctx.counters.qpcFailures.fetch_add(1, std::memory_order_relaxed);
        clock.qpcBase = baseOk ? baseTick.QuadPart : 0;
        clock.fileTimeBase = (static_cast<ULONGLONG>(baseFileTime.dwHighDateTime) << 32) | baseFileTime.dwLowDateTime;
        clock.valid = baseOk && hdr.ReservedFlags == 1 && clock.freq > 0 && clock.freq == qpcFrequency.QuadPart;
        std::printf("Clock: ReservedFlags=%lu (1=QPC), PerfFreq=%lld, QueryPerformanceFrequency=%lld, base QPC %s -> %s\n",
                    hdr.ReservedFlags, static_cast<long long>(clock.freq),
                    static_cast<long long>(qpcFrequency.QuadPart), baseOk ? "ok" : "FAILED",
                    clock.valid ? "timestamps converted from QPC" : "WARNING: timestamps and etw_delay_us unavailable");
    }

    // 先啟動消費者，再啟動 ProcessTrace
    ConsumerThreadArgs consumerArgs;
    consumerArgs.ctx = &ctx;
    consumerArgs.clock = &clock;
    LatencyStats latency; // 宣告在執行緒 handle 之前：比消費者執行緒活得久
    consumerArgs.latency = &latency;
    consumerArgs.doneEvent = doneEvent.Get();
    ScopedHandle consumerThread(CreateThread(nullptr, 0, ConsumerThread, &consumerArgs, 0, nullptr));
    if (!consumerThread.Valid()) {
        std::printf("CreateThread(consumer) failed, error=%lu\n", GetLastError());
        return 1;
    }

    EtwThreadArgs etwArgs;
    etwArgs.trace = trace.Get();
    ScopedHandle etwThread(CreateThread(nullptr, 0, ProcessTraceThread, &etwArgs, 0, nullptr));
    if (!etwThread.Valid()) {
        std::printf("CreateThread(ProcessTrace) failed, error=%lu\n", GetLastError());
        // 消費者已在執行：通知它結束並等待，不能讓它比 ctx 活得久
        if (!SetEvent(doneEvent.Get()))
            std::printf("SetEvent(done) failed, error=%lu\n", GetLastError());
        const DWORD w = WaitForSingleObject(consumerThread.Get(), kThreadJoinTimeoutMs);
        if (w != WAIT_OBJECT_0) {
            std::printf("Consumer did not finish (%s), exiting process\n", WaitResultName(w));
            std::fflush(stdout);
            consoleMode.Restore();
            ExitProcess(1);
        }
        return 1;
    }

    std::printf("Listening for process start/stop events. Press Ctrl+C to stop.\n");
    std::fflush(stdout);

    HANDLE waits[3] = {stopEvent.Get(), etwThread.Get(), consumerThread.Get()};
    const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
    const DWORD waitError = (wait == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
    SYSTEMTIME tWoke;
    GetLocalTime(&tWoke);

    // ---- 關鍵步驟：這段之內不可有任何 printf，結果與時間戳都先存在區域變數 ----

    // 1. 停止 session
    const EtwSession::StopResult stopResult = session.StopQuiet();

    // 2. 等 ProcessTrace 返回（工作執行緒結束），最多 kThreadJoinTimeoutMs
    SYSTEMTIME tEtwBefore;
    SYSTEMTIME tEtwAfter;
    GetLocalTime(&tEtwBefore);
    const DWORD etwJoin = WaitForSingleObject(etwThread.Get(), kThreadJoinTimeoutMs);
    const DWORD etwJoinError = (etwJoin == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
    GetLocalTime(&tEtwAfter);

    // 3. 通知消費者「不會再有新事件」
    SYSTEMTIME tDone;
    GetLocalTime(&tDone);
    const BOOL doneOk = SetEvent(doneEvent.Get());
    const DWORD doneError = doneOk ? ERROR_SUCCESS : GetLastError();

    // 4. 等消費者清空佇列後結束，最多 kThreadJoinTimeoutMs
    SYSTEMTIME tConsumerBefore;
    SYSTEMTIME tConsumerAfter;
    GetLocalTime(&tConsumerBefore);
    const DWORD consumerJoin = WaitForSingleObject(consumerThread.Get(), kThreadJoinTimeoutMs);
    const DWORD consumerJoinError = (consumerJoin == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
    GetLocalTime(&tConsumerAfter);

    DWORD etwExit = 0;
    BOOL etwExitOk = FALSE;
    DWORD etwExitError = ERROR_SUCCESS;
    if (etwJoin == WAIT_OBJECT_0) {
        etwExitOk = GetExitCodeThread(etwThread.Get(), &etwExit);
        if (!etwExitOk)
            etwExitError = GetLastError();
    }
    DWORD consumerExit = 0;
    BOOL consumerExitOk = FALSE;
    DWORD consumerExitError = ERROR_SUCCESS;
    if (consumerJoin == WAIT_OBJECT_0) {
        consumerExitOk = GetExitCodeThread(consumerThread.Get(), &consumerExit);
        if (!consumerExitOk)
            consumerExitError = GetLastError();
    }

    // 5. 關閉 trace
    const EtwTraceHandle::CloseResult closeResult = trace.CloseQuiet();

    // ---- 關鍵步驟結束，以下才輸出 ----

    int exitCode = kExitOk;
    auto setExitCode = [&](int code) {
        if (exitCode == kExitOk)
            exitCode = code;
    };

    std::printf("[%s] [SHUTDOWN] main thread woke up (wait result=%lu)\n", FormatTimestamp(tWoke).c_str(), wait);
    if (wait == WAIT_FAILED) {
        std::printf("WaitForMultipleObjects failed, error=%lu\n", waitError);
        setExitCode(kExitWaitFailed);
    } else if (wait == WAIT_OBJECT_0 + 1) {
        std::printf("ProcessTrace thread exited before Ctrl+C\n");
    } else if (wait == WAIT_OBJECT_0 + 2) {
        std::printf("Consumer thread exited before Ctrl+C\n");
    }

    EtwSession::PrintStopResult(stopResult);
    if (stopResult.status != ERROR_SUCCESS)
        setExitCode(kExitStopTraceFailed);

    std::printf("[%s] [SHUTDOWN] before WaitForSingleObject(ProcessTrace thread, %lu ms)\n",
                FormatTimestamp(tEtwBefore).c_str(), kThreadJoinTimeoutMs);
    std::printf("[%s] [SHUTDOWN] after WaitForSingleObject(ProcessTrace thread), result=%s\n",
                FormatTimestamp(tEtwAfter).c_str(), WaitResultName(etwJoin));
    if (etwJoin == WAIT_OBJECT_0) {
        if (!etwExitOk) {
            std::printf("GetExitCodeThread(ProcessTrace thread) failed, error=%lu\n", etwExitError);
        } else {
            std::printf("ProcessTrace returned %lu\n", etwExit);
            if (etwExit != ERROR_SUCCESS)
                setExitCode(kExitProcessTraceFailed);
        }
    } else if (etwJoin == WAIT_TIMEOUT) {
        std::printf("TIMEOUT: ProcessTrace thread did not finish within %lu ms\n", kThreadJoinTimeoutMs);
        setExitCode(kExitEtwThreadTimeout);
    } else {
        std::printf("WaitForSingleObject(ProcessTrace thread) failed, error=%lu\n", etwJoinError);
        setExitCode(kExitWaitFailed);
    }

    std::printf("[%s] [SHUTDOWN] SetEvent(done) -> %s\n", FormatTimestamp(tDone).c_str(), doneOk ? "OK" : "FAILED");
    if (!doneOk) {
        std::printf("SetEvent(done) failed, error=%lu\n", doneError);
        setExitCode(kExitSetDoneFailed);
    }

    std::printf("[%s] [SHUTDOWN] before WaitForSingleObject(consumer thread, %lu ms)\n",
                FormatTimestamp(tConsumerBefore).c_str(), kThreadJoinTimeoutMs);
    std::printf("[%s] [SHUTDOWN] after WaitForSingleObject(consumer thread), result=%s\n",
                FormatTimestamp(tConsumerAfter).c_str(), WaitResultName(consumerJoin));
    if (consumerJoin == WAIT_OBJECT_0) {
        if (!consumerExitOk) {
            std::printf("GetExitCodeThread(consumer thread) failed, error=%lu\n", consumerExitError);
        } else {
            std::printf("Consumer thread exit code=%lu\n", consumerExit);
            if (consumerExit != 0) {
                std::printf("Consumer WaitForSingleObject(done) failed, error=%lu\n", consumerArgs.waitError);
                setExitCode(kExitConsumerWaitFailed);
            }
        }
    } else if (consumerJoin == WAIT_TIMEOUT) {
        std::printf("TIMEOUT: consumer thread did not finish within %lu ms\n", kThreadJoinTimeoutMs);
        setExitCode(kExitConsumerTimeout);
    } else {
        std::printf("WaitForSingleObject(consumer thread) failed, error=%lu\n", consumerJoinError);
        setExitCode(kExitWaitFailed);
    }

    EtwTraceHandle::PrintCloseResult(closeResult);
    if (closeResult.attempted && closeResult.status != ERROR_SUCCESS)
        setExitCode(kExitCloseTraceFailed);

    // 計數器與恆等式
    const EventCounters& c = ctx.counters;
    const uint64_t received = c.received.load(std::memory_order_relaxed);
    const uint64_t enqueued = c.enqueued.load(std::memory_order_relaxed);
    const uint64_t droppedFull = c.droppedQueueFull.load(std::memory_order_relaxed);
    const uint64_t droppedOversize = c.droppedOversize.load(std::memory_order_relaxed);
    const uint64_t consumed = c.consumed.load(std::memory_order_relaxed);
    std::printf("Counters:\n");
    std::printf("  received (Kernel-Process)        = %llu\n", static_cast<unsigned long long>(received));
    std::printf("  enqueued                         = %llu\n", static_cast<unsigned long long>(enqueued));
    std::printf("  dropped (queue full)             = %llu\n", static_cast<unsigned long long>(droppedFull));
    std::printf("  dropped (oversize > %zu bytes) = %llu\n", kMaxUserDataBytes,
                static_cast<unsigned long long>(droppedOversize));
    std::printf("  consumed                         = %llu\n", static_cast<unsigned long long>(consumed));
    std::printf("  TDH errors                       = %llu\n",
                static_cast<unsigned long long>(c.tdhErrors.load(std::memory_order_relaxed)));
    std::printf("  other provider (not in identity) = %llu\n",
                static_cast<unsigned long long>(c.otherProvider.load(std::memory_order_relaxed)));
    std::printf("  with ExtendedData (not copied)   = %llu\n",
                static_cast<unsigned long long>(c.withExtendedData.load(std::memory_order_relaxed)));
    std::printf("  QPC failures                     = %llu\n",
                static_cast<unsigned long long>(c.qpcFailures.load(std::memory_order_relaxed)));

    const bool threadsFinished = (etwJoin == WAIT_OBJECT_0 && consumerJoin == WAIT_OBJECT_0);
    bool id1 = false;
    bool id2 = false;
    if (threadsFinished) {
        id1 = (received == enqueued + droppedFull + droppedOversize);
        id2 = (enqueued == consumed);
        std::printf("Identity 1: received(%llu) == enqueued + droppedFull + droppedOversize (%llu) : %s\n",
                    static_cast<unsigned long long>(received),
                    static_cast<unsigned long long>(enqueued + droppedFull + droppedOversize), id1 ? "OK" : "MISMATCH");
        std::printf("Identity 2: enqueued(%llu) == consumed(%llu) : %s\n", static_cast<unsigned long long>(enqueued),
                    static_cast<unsigned long long>(consumed), id2 ? "OK" : "MISMATCH");
        if (!id1 || !id2)
            setExitCode(kExitIdentityMismatch);
    } else {
        std::printf("Identities not evaluated: a thread is still running\n");
    }

    const int64_t maxTicks = c.maxCallbackTicks.load(std::memory_order_relaxed);
    if (qpcFrequency.QuadPart > 0)
        std::printf("Max callback duration: %.3f us (%lld ticks @ %lld Hz)\n",
                    static_cast<double>(maxTicks) * 1e6 / static_cast<double>(qpcFrequency.QuadPart),
                    static_cast<long long>(maxTicks), static_cast<long long>(qpcFrequency.QuadPart));

    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE))
        std::printf("SetConsoleCtrlHandler(remove) failed, error=%lu\n", GetLastError());

    // 延遲統計（excluded = QPC 失敗或時鐘無效而未計入的事件數）
    std::string latencyText;
    AppendLatencyStat(latencyText, "etw_delay", latency.etwDelay, clock.valid ? clock.freq : 0, "  ");
    AppendLatencyStat(latencyText, "queue_wait", latency.queueWait, clock.qpcFrequency, "  ");
    AppendLatencyStat(latencyText, "process", latency.process, clock.qpcFrequency, "  ");
    std::printf("Latency:\n%s", latencyText.c_str());

    std::printf("Summary: exit code=%d\n", exitCode);
    std::fflush(stdout);

    // --summary-file：關鍵步驟都已完成，寫檔失敗只警告，不改變結束碼
    if (summaryFile != nullptr) {
        std::string s;
        AppendFormat(s, "queue_capacity=%zu\n", ctx.ring.Capacity());
        AppendFormat(s, "received=%llu\n", static_cast<unsigned long long>(received));
        AppendFormat(s, "enqueued=%llu\n", static_cast<unsigned long long>(enqueued));
        AppendFormat(s, "dropped_queue_full=%llu\n", static_cast<unsigned long long>(droppedFull));
        AppendFormat(s, "dropped_oversize=%llu\n", static_cast<unsigned long long>(droppedOversize));
        AppendFormat(s, "consumed=%llu\n", static_cast<unsigned long long>(consumed));
        if (threadsFinished) {
            AppendFormat(s, "identity1=%s (received %llu == enqueued + droppedFull + droppedOversize %llu)\n",
                         id1 ? "OK" : "MISMATCH", static_cast<unsigned long long>(received),
                         static_cast<unsigned long long>(enqueued + droppedFull + droppedOversize));
            AppendFormat(s, "identity2=%s (enqueued %llu == consumed %llu)\n", id2 ? "OK" : "MISMATCH",
                         static_cast<unsigned long long>(enqueued), static_cast<unsigned long long>(consumed));
        } else {
            AppendFormat(s, "identity1=NOT_EVALUATED (a thread is still running)\n");
            AppendFormat(s, "identity2=NOT_EVALUATED (a thread is still running)\n");
        }
        if (stopResult.attempted) {
            AppendFormat(s, "stop_status=%lu\n", stopResult.status);
            AppendFormat(s, "events_lost=%lu\n", stopResult.eventsLost);
            AppendFormat(s, "realtime_buffers_lost=%lu\n", stopResult.realTimeBuffersLost);
        } else {
            AppendFormat(s, "stop_status=NOT_ATTEMPTED\n");
        }
        if (qpcFrequency.QuadPart > 0)
            AppendFormat(s, "max_callback_us=%.3f\n",
                         static_cast<double>(maxTicks) * 1e6 / static_cast<double>(qpcFrequency.QuadPart));
        else
            AppendFormat(s, "max_callback_us=UNAVAILABLE\n");
        AppendFormat(s, "max_callback_ticks=%lld\n", static_cast<long long>(maxTicks));
        AppendFormat(s, "qpc_frequency=%lld\n", static_cast<long long>(qpcFrequency.QuadPart));
        AppendLatencyStat(s, "etw_delay", latency.etwDelay, clock.valid ? clock.freq : 0, "\n");
        AppendLatencyStat(s, "queue_wait", latency.queueWait, clock.qpcFrequency, "\n");
        AppendLatencyStat(s, "process", latency.process, clock.qpcFrequency, "\n");
        AppendFormat(s, "exit_code=%d\n", exitCode);
        WriteSummaryFile(summaryFile, s);
        std::fflush(stdout);
    }

    // 任一執行緒可能仍在執行時，不能讓解構子釋放 ctx / 參數：直接結束行程。
    // ExitProcess 不跑解構子，先手動還原 console 模式。
    if (!threadsFinished) {
        consoleMode.Restore();
        ExitProcess(static_cast<UINT>(exitCode));
    }
    return exitCode;
}
