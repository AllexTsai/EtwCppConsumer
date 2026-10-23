#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <string>
#include <vector>

// 診斷輸出用：本地時間 HH:MM:SS.mmm
std::string FormatTimestamp(const SYSTEMTIME& st);
std::string TimestampNow();

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) : h_(h) {}
    ~ScopedHandle() { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE Get() const { return h_; }
    bool Valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    void Reset();

private:
    HANDLE h_;
};

// 啟動時關閉 QuickEdit（console 被選取時，所有寫入 stdout 的執行緒都會卡住），結束前還原。
// 所有呼叫失敗只警告，不中止。
class ConsoleModeGuard {
public:
    ConsoleModeGuard();
    ~ConsoleModeGuard() { Restore(); }
    ConsoleModeGuard(const ConsoleModeGuard&) = delete;
    ConsoleModeGuard& operator=(const ConsoleModeGuard&) = delete;

    // 可重複呼叫；ExitProcess 不會跑解構子，所以那條路徑要手動呼叫
    void Restore();

private:
    HANDLE input_ = nullptr;
    DWORD originalMode_ = 0;
    bool changed_ = false;
};

class EtwSession {
public:
    struct StopResult {
        bool attempted = false;
        ULONG status = ERROR_SUCCESS;
        ULONG eventsLost = 0;
        ULONG realTimeBuffersLost = 0;
        SYSTEMTIME before{};
        SYSTEMTIME after{};
    };

    explicit EtwSession(std::wstring name) : name_(std::move(name)) {}
    ~EtwSession() { Stop(); }
    EtwSession(const EtwSession&) = delete;
    EtwSession& operator=(const EtwSession&) = delete;

    // 啟動即時 session。遇到 ERROR_ALREADY_EXISTS（上次殘留）會先停掉再重試一次。
    ULONG Start();

    // 不輸出任何訊息；結果與時間戳由呼叫端之後再印。可重複呼叫，第二次以後不做事
    StopResult StopQuiet();
    static void PrintStopResult(const StopResult& r);

    // 解構子使用：停止後立即印出
    ULONG Stop();

    TRACEHANDLE Handle() const { return handle_; }
    const std::wstring& Name() const { return name_; }

private:
    // EVENT_TRACE_PROPERTIES 後面必須緊接著保留 session 名稱的空間
    std::vector<BYTE> MakeProperties() const;

    std::wstring name_;
    TRACEHANDLE handle_ = 0;
};

class EtwTraceHandle {
public:
    struct CloseResult {
        bool attempted = false;
        ULONG status = ERROR_SUCCESS;
        SYSTEMTIME before{};
        SYSTEMTIME after{};
    };

    explicit EtwTraceHandle(TRACEHANDLE h) : h_(h) {}
    ~EtwTraceHandle() { Close(); }
    EtwTraceHandle(const EtwTraceHandle&) = delete;
    EtwTraceHandle& operator=(const EtwTraceHandle&) = delete;

    bool Valid() const { return h_ != INVALID_PROCESSTRACE_HANDLE; }
    TRACEHANDLE Get() const { return h_; }

    // 不輸出任何訊息；結果與時間戳由呼叫端之後再印
    CloseResult CloseQuiet();
    static void PrintCloseResult(const CloseResult& r);

    // 解構子使用：關閉後立即印出
    ULONG Close();

private:
    TRACEHANDLE h_;
};
