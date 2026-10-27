#include "etw_raii.h"

#include <cstdio>

std::string FormatTimestamp(const SYSTEMTIME& st)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    return buf;
}

std::string TimestampNow()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return FormatTimestamp(st);
}

// ---------------------------------------------------------------------------

void ScopedHandle::Reset()
{
    if (Valid() && !CloseHandle(h_))
        std::printf("CloseHandle failed, error=%lu\n", GetLastError());
    h_ = nullptr;
}

// ---------------------------------------------------------------------------

ConsoleModeGuard::ConsoleModeGuard()
{
    input_ = GetStdHandle(STD_INPUT_HANDLE);
    if (input_ == INVALID_HANDLE_VALUE || input_ == nullptr) {
        std::printf("WARNING: GetStdHandle(STD_INPUT_HANDLE) failed, error=%lu; QuickEdit not changed\n",
                    GetLastError());
        return;
    }
    if (!GetConsoleMode(input_, &originalMode_)) {
        std::printf("WARNING: GetConsoleMode(stdin) failed, error=%lu; QuickEdit not changed\n", GetLastError());
        return;
    }
    DWORD newMode = (originalMode_ & ~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(input_, newMode)) {
        std::printf("WARNING: SetConsoleMode(stdin, 0x%04lX) failed, error=%lu; QuickEdit not changed\n", newMode,
                    GetLastError());
        return;
    }
    changed_ = true;
    std::printf("Console input mode 0x%04lX -> 0x%04lX (QuickEdit disabled)\n", originalMode_, newMode);
}

void ConsoleModeGuard::Restore()
{
    if (!changed_)
        return;
    changed_ = false;
    if (!SetConsoleMode(input_, originalMode_))
        std::printf("WARNING: SetConsoleMode(stdin, restore 0x%04lX) failed, error=%lu\n", originalMode_,
                    GetLastError());
    else
        std::printf("Console input mode restored to 0x%04lX\n", originalMode_);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------

std::vector<BYTE> EtwSession::MakeProperties() const
{
    const size_t nameBytes = (name_.size() + 1) * sizeof(wchar_t);
    std::vector<BYTE> buffer(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    props->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    // 緩衝區設定：0 = 由 ETW 決定；FlushTimer 預設 1 秒，讓即時事件盡快送到 consumer
    props->BufferSize = config_.bufferSizeKb;       // KB
    props->MinimumBuffers = config_.minimumBuffers;
    props->MaximumBuffers = config_.maximumBuffers;
    props->FlushTimer = config_.flushTimerSec;      // 秒
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buffer;
}

EtwSession::QueryResult EtwSession::Query() const
{
    QueryResult r;
    if (handle_ == 0)
        return r;
    std::vector<BYTE> buffer = MakeProperties();
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    r.status = ControlTraceW(handle_, nullptr, props, EVENT_TRACE_CONTROL_QUERY);
    if (r.status != ERROR_SUCCESS)
        return r;
    r.bufferSizeKb = props->BufferSize;
    r.minimumBuffers = props->MinimumBuffers;
    r.maximumBuffers = props->MaximumBuffers;
    r.flushTimerSec = props->FlushTimer;
    r.numberOfBuffers = props->NumberOfBuffers;
    r.freeBuffers = props->FreeBuffers;
    r.eventsLost = props->EventsLost;
    r.buffersWritten = props->BuffersWritten;
    r.logBuffersLost = props->LogBuffersLost;
    r.realTimeBuffersLost = props->RealTimeBuffersLost;
    return r;
}

ULONG EtwSession::Start()
{
    std::vector<BYTE> props = MakeProperties();
    TRACEHANDLE handle = 0;
    ULONG status = StartTraceW(&handle, name_.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()));
    std::printf("StartTraceW(\"%ls\") returned %lu\n", name_.c_str(), status);

    if (status == ERROR_ALREADY_EXISTS) {
        std::printf("Session \"%ls\" already exists (leftover from a previous run). "
                    "Stopping it with ControlTraceW and retrying...\n", name_.c_str());
        std::vector<BYTE> stopProps = MakeProperties();
        ULONG stopStatus = ControlTraceW(0, name_.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(stopProps.data()),
                                         EVENT_TRACE_CONTROL_STOP);
        std::printf("ControlTraceW(STOP leftover) returned %lu\n", stopStatus);
        if (stopStatus != ERROR_SUCCESS)
            return stopStatus;

        props = MakeProperties();
        status = StartTraceW(&handle, name_.c_str(), reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props.data()));
        std::printf("StartTraceW(retry) returned %lu\n", status);
    }

    if (status == ERROR_SUCCESS)
        handle_ = handle;
    return status;
}

EtwSession::StopResult EtwSession::StopQuiet()
{
    StopResult r;
    if (handle_ == 0)
        return r;
    std::vector<BYTE> buffer = MakeProperties();
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    r.attempted = true;
    GetLocalTime(&r.before);
    r.status = ControlTraceW(handle_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
    GetLocalTime(&r.after);
    r.eventsLost = props->EventsLost;
    r.realTimeBuffersLost = props->RealTimeBuffersLost;
    r.numberOfBuffers = props->NumberOfBuffers;
    r.freeBuffers = props->FreeBuffers;
    r.buffersWritten = props->BuffersWritten;
    r.logBuffersLost = props->LogBuffersLost;
    handle_ = 0;
    return r;
}

void EtwSession::PrintStopResult(const StopResult& r)
{
    if (!r.attempted)
        return;
    std::printf("[%s] [SHUTDOWN] before ControlTraceW(STOP)\n", FormatTimestamp(r.before).c_str());
    std::printf("[%s] [SHUTDOWN] after ControlTraceW(STOP)\n", FormatTimestamp(r.after).c_str());
    std::printf("ControlTraceW(STOP) returned %lu (EventsLost=%lu, RealTimeBuffersLost=%lu)\n", r.status,
                r.eventsLost, r.realTimeBuffersLost);
    std::fflush(stdout);
}

ULONG EtwSession::Stop()
{
    StopResult r = StopQuiet();
    PrintStopResult(r);
    return r.status;
}

// ---------------------------------------------------------------------------

EtwTraceHandle::CloseResult EtwTraceHandle::CloseQuiet()
{
    CloseResult r;
    if (!Valid())
        return r;
    r.attempted = true;
    GetLocalTime(&r.before);
    r.status = CloseTrace(h_);
    GetLocalTime(&r.after);
    h_ = INVALID_PROCESSTRACE_HANDLE;
    return r;
}

void EtwTraceHandle::PrintCloseResult(const CloseResult& r)
{
    if (!r.attempted)
        return;
    std::printf("[%s] [SHUTDOWN] before CloseTrace\n", FormatTimestamp(r.before).c_str());
    std::printf("[%s] [SHUTDOWN] after CloseTrace\n", FormatTimestamp(r.after).c_str());
    if (r.status == ERROR_CTX_CLOSE_PENDING)
        std::printf("CloseTrace returned ERROR_CTX_CLOSE_PENDING (%lu): ProcessTrace still running\n", r.status);
    else
        std::printf("CloseTrace returned %lu\n", r.status);
    std::fflush(stdout);
}

ULONG EtwTraceHandle::Close()
{
    CloseResult r = CloseQuiet();
    PrintCloseResult(r);
    return r.status;
}
