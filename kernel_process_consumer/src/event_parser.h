#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// TDH 解析結果。所有資料都是自己擁有的副本，不指向 EVENT_RECORD。
struct ParsedProperty {
    std::wstring name;
    USHORT inType = 0;
    std::wstring value;
    bool ok = false;
};

struct ParsedEvent {
    GUID providerId{};
    USHORT id = 0;
    UCHAR version = 0;
    UCHAR opcode = 0;
    USHORT task = 0;
    ULONGLONG timestamp = 0;
    ULONG headerProcessId = 0;
    ULONG headerThreadId = 0;
    ULONG tdhInfoStatus = ERROR_SUCCESS;
    std::wstring taskName;
    std::wstring opcodeName;
    std::vector<ParsedProperty> properties;
};

bool operator==(const ParsedProperty& a, const ParsedProperty& b);
bool operator==(const ParsedEvent& a, const ParsedEvent& b);

// 用 TDH 解析一筆事件。每個 TDH 呼叫失敗都會讓 tdhErrors 加一，並把錯誤碼記在結果中。
// 會配置記憶體，不可在 ETW callback 內呼叫（測試除外）。
// 前提：record.EventHeader.TimeStamp 是 FILETIME（0 代表無法取得）。
// 以 PROCESS_TRACE_MODE_RAW_TIMESTAMP 取得的原始時間戳，呼叫端必須先換算成 FILETIME。
void ParseEvent(const EVENT_RECORD& record, ParsedEvent& out, std::atomic<uint64_t>& tdhErrors);

// 事件內容的多行文字（不含分隔線）。timestampSuffix 會附加在 TimeStamp 那一行的行尾。
std::wstring FormatEventText(const ParsedEvent& ev, const std::wstring& timestampSuffix = L"");

// 兩筆解析結果的第一個差異；相同時回傳空字串
std::wstring DescribeDifference(const ParsedEvent& a, const ParsedEvent& b);

std::wstring FormatGuid(const GUID& g);
std::wstring FormatFileTimeLocal(ULONGLONG value);
