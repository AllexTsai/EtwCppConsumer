#include "event_parser.h"

#include <sddl.h>

#include <climits>
#include <cstdarg>
#include <cstring>
#include <cwchar>

namespace {

// 本機 MinGW tdh.h 沒有定義 TDH_IN_TYPE。
// 來源：Microsoft Learn「TDH_IN_TYPE (tdh.h)」，官方列舉沒有寫明數值，
// 依宣告順序從 0 遞增。這裡照相同順序列出，只寫到用得到的 HEXINT64。
namespace tdh_intype {
enum : USHORT {
    TDH_INTYPE_NULL,
    TDH_INTYPE_UNICODESTRING,
    TDH_INTYPE_ANSISTRING,
    TDH_INTYPE_INT8,
    TDH_INTYPE_UINT8,
    TDH_INTYPE_INT16,
    TDH_INTYPE_UINT16,
    TDH_INTYPE_INT32,
    TDH_INTYPE_UINT32,
    TDH_INTYPE_INT64,
    TDH_INTYPE_UINT64,
    TDH_INTYPE_FLOAT,
    TDH_INTYPE_DOUBLE,
    TDH_INTYPE_BOOLEAN,
    TDH_INTYPE_BINARY,
    TDH_INTYPE_GUID,
    TDH_INTYPE_POINTER,
    TDH_INTYPE_FILETIME,
    TDH_INTYPE_SYSTEMTIME,
    TDH_INTYPE_SID,
    TDH_INTYPE_HEXINT32,
    TDH_INTYPE_HEXINT64,
};
} // namespace tdh_intype

std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    int n = std::vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, args);
    va_end(args);
    if (n < 0)
        return L"<vswprintf failed>";
    return buf;
}

std::wstring FormatSystemTime(const SYSTEMTIME& st)
{
    return Format(L"%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

std::wstring HexDump(const std::vector<BYTE>& data)
{
    std::wstring out = L"0x";
    for (BYTE b : data)
        out += Format(L"%02X", b);
    return out;
}

template <typename T>
bool ReadExact(const std::vector<BYTE>& data, T& out)
{
    if (data.size() != sizeof(T))
        return false;
    std::memcpy(&out, data.data(), sizeof(T));
    return true;
}

std::wstring FormatPropertyValue(USHORT inType, const std::vector<BYTE>& data)
{
    using namespace tdh_intype;
    switch (inType) {
    case TDH_INTYPE_UNICODESTRING: {
        size_t chars = data.size() / sizeof(wchar_t);
        std::wstring s(reinterpret_cast<const wchar_t*>(data.data()), chars);
        size_t nul = s.find(L'\0');
        if (nul != std::wstring::npos)
            s.resize(nul);
        return s;
    }
    case TDH_INTYPE_ANSISTRING: {
        std::string a(reinterpret_cast<const char*>(data.data()), data.size());
        size_t nul = a.find('\0');
        if (nul != std::string::npos)
            a.resize(nul);
        if (a.empty())
            return L"";
        int len = MultiByteToWideChar(CP_ACP, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
        if (len <= 0)
            return Format(L"<MultiByteToWideChar failed, error=%lu>", GetLastError());
        std::wstring w(static_cast<size_t>(len), L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, a.data(), static_cast<int>(a.size()), &w[0], len) <= 0)
            return Format(L"<MultiByteToWideChar failed, error=%lu>", GetLastError());
        return w;
    }
    case TDH_INTYPE_INT8:   { INT8 v;   if (ReadExact(data, v)) return Format(L"%d", v); break; }
    case TDH_INTYPE_UINT8:  { UINT8 v;  if (ReadExact(data, v)) return Format(L"%u", v); break; }
    case TDH_INTYPE_INT16:  { INT16 v;  if (ReadExact(data, v)) return Format(L"%d", v); break; }
    case TDH_INTYPE_UINT16: { UINT16 v; if (ReadExact(data, v)) return Format(L"%u", v); break; }
    case TDH_INTYPE_INT32:  { INT32 v;  if (ReadExact(data, v)) return Format(L"%d", v); break; }
    case TDH_INTYPE_UINT32: { UINT32 v; if (ReadExact(data, v)) return Format(L"%u", v); break; }
    case TDH_INTYPE_INT64:  { INT64 v;  if (ReadExact(data, v)) return Format(L"%lld", v); break; }
    case TDH_INTYPE_UINT64: { UINT64 v; if (ReadExact(data, v)) return Format(L"%llu", v); break; }
    case TDH_INTYPE_FLOAT:  { float v;  if (ReadExact(data, v)) return Format(L"%g", v); break; }
    case TDH_INTYPE_DOUBLE: { double v; if (ReadExact(data, v)) return Format(L"%g", v); break; }
    case TDH_INTYPE_BOOLEAN: { INT32 v; if (ReadExact(data, v)) return v ? L"true" : L"false"; break; }
    case TDH_INTYPE_GUID:   { GUID v;   if (ReadExact(data, v)) return FormatGuid(v); break; }
    case TDH_INTYPE_HEXINT32: { UINT32 v; if (ReadExact(data, v)) return Format(L"0x%08X", v); break; }
    case TDH_INTYPE_HEXINT64: { UINT64 v; if (ReadExact(data, v)) return Format(L"0x%016llX", v); break; }
    case TDH_INTYPE_POINTER: {
        // 指標大小依事件來源是 32 或 64 位元而定
        UINT32 v32; UINT64 v64;
        if (ReadExact(data, v64)) return Format(L"0x%016llX", v64);
        if (ReadExact(data, v32)) return Format(L"0x%08X", v32);
        break;
    }
    case TDH_INTYPE_FILETIME: { UINT64 v; if (ReadExact(data, v)) return FormatFileTimeLocal(v); break; }
    case TDH_INTYPE_SYSTEMTIME: { SYSTEMTIME v; if (ReadExact(data, v)) return FormatSystemTime(v); break; }
    case TDH_INTYPE_SID: {
        if (data.empty() || !IsValidSid(const_cast<BYTE*>(data.data())))
            return L"<invalid SID> " + HexDump(data);
        LPWSTR str = nullptr;
        if (!ConvertSidToStringSidW(const_cast<BYTE*>(data.data()), &str))
            return Format(L"<ConvertSidToStringSidW failed, error=%lu>", GetLastError());
        std::wstring s = str;
        if (LocalFree(str) != nullptr)
            s += Format(L" <LocalFree failed, error=%lu>", GetLastError());
        return s;
    }
    default:
        return Format(L"<InType %u not decoded> ", inType) + HexDump(data);
    }
    return Format(L"<size %zu does not match InType %u> ", data.size(), inType) + HexDump(data);
}

std::wstring StringAtOffset(const BYTE* base, ULONG offset)
{
    if (offset == 0)
        return L"";
    return reinterpret_cast<const wchar_t*>(base + offset);
}

void ParseProperties(PEVENT_RECORD record, const TRACE_EVENT_INFO* info, ParsedEvent& out,
                     std::atomic<uint64_t>& tdhErrors)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(info);
    for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
        const EVENT_PROPERTY_INFO& epi = info->EventPropertyInfoArray[i];
        ParsedProperty prop;
        prop.name = StringAtOffset(base, epi.NameOffset);

        if (epi.Flags & PropertyStruct) {
            prop.value = Format(L"<struct with %u members, not decoded>", epi.structType.NumOfStructMembers);
            prop.ok = true;
            out.properties.push_back(std::move(prop));
            continue;
        }
        prop.inType = epi.nonStructType.InType;

        PROPERTY_DATA_DESCRIPTOR desc{};
        desc.PropertyName = reinterpret_cast<ULONGLONG>(base + epi.NameOffset);
        desc.ArrayIndex = ULONG_MAX; // 整個屬性（含陣列）

        ULONG size = 0;
        ULONG status = TdhGetPropertySize(record, 0, nullptr, 1, &desc, &size);
        if (status != ERROR_SUCCESS) {
            prop.value = Format(L"<TdhGetPropertySize failed, error=%lu, EventId=%u>", status,
                                record->EventHeader.EventDescriptor.Id);
            tdhErrors.fetch_add(1, std::memory_order_relaxed);
            out.properties.push_back(std::move(prop));
            continue;
        }

        std::vector<BYTE> data(size);
        if (size > 0) {
            status = TdhGetProperty(record, 0, nullptr, 1, &desc, size, data.data());
            if (status != ERROR_SUCCESS) {
                prop.value = Format(L"<TdhGetProperty failed, error=%lu, EventId=%u>", status,
                                    record->EventHeader.EventDescriptor.Id);
                tdhErrors.fetch_add(1, std::memory_order_relaxed);
                out.properties.push_back(std::move(prop));
                continue;
            }
        }

        bool isArray = (epi.Flags & PropertyParamCount) || epi.count > 1;
        prop.value = isArray ? (L"<array> " + HexDump(data)) : FormatPropertyValue(prop.inType, data);
        prop.ok = true;
        out.properties.push_back(std::move(prop));
    }
}

} // namespace

std::wstring FormatGuid(const GUID& g)
{
    return Format(L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2],
                  g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// FILETIME（UTC）轉成本地時間字串，每個 Win32 呼叫都檢查
std::wstring FormatFileTimeLocal(ULONGLONG value)
{
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(value & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(value >> 32);
    SYSTEMTIME utc;
    if (!FileTimeToSystemTime(&ft, &utc))
        return Format(L"<FileTimeToSystemTime failed, error=%lu, raw=%llu>", GetLastError(), value);
    SYSTEMTIME local;
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        return Format(L"<SystemTimeToTzSpecificLocalTime failed, error=%lu, raw=%llu>", GetLastError(), value);
    return FormatSystemTime(local);
}

bool operator==(const ParsedProperty& a, const ParsedProperty& b)
{
    return a.name == b.name && a.inType == b.inType && a.value == b.value && a.ok == b.ok;
}

bool operator==(const ParsedEvent& a, const ParsedEvent& b)
{
    return DescribeDifference(a, b).empty();
}

std::wstring DescribeDifference(const ParsedEvent& a, const ParsedEvent& b)
{
    if (!IsEqualGUID(a.providerId, b.providerId))
        return L"providerId: " + FormatGuid(a.providerId) + L" vs " + FormatGuid(b.providerId);
    if (a.id != b.id) return Format(L"id: %u vs %u", a.id, b.id);
    if (a.version != b.version) return Format(L"version: %u vs %u", a.version, b.version);
    if (a.opcode != b.opcode) return Format(L"opcode: %u vs %u", a.opcode, b.opcode);
    if (a.task != b.task) return Format(L"task: %u vs %u", a.task, b.task);
    if (a.timestamp != b.timestamp) return Format(L"timestamp: %llu vs %llu", a.timestamp, b.timestamp);
    if (a.headerProcessId != b.headerProcessId)
        return Format(L"headerProcessId: %lu vs %lu", a.headerProcessId, b.headerProcessId);
    if (a.headerThreadId != b.headerThreadId)
        return Format(L"headerThreadId: %lu vs %lu", a.headerThreadId, b.headerThreadId);
    if (a.tdhInfoStatus != b.tdhInfoStatus)
        return Format(L"tdhInfoStatus: %lu vs %lu", a.tdhInfoStatus, b.tdhInfoStatus);
    if (a.taskName != b.taskName) return L"taskName: " + a.taskName + L" vs " + b.taskName;
    if (a.opcodeName != b.opcodeName) return L"opcodeName: " + a.opcodeName + L" vs " + b.opcodeName;
    if (a.properties.size() != b.properties.size())
        return Format(L"property count: %zu vs %zu", a.properties.size(), b.properties.size());
    for (size_t i = 0; i < a.properties.size(); ++i) {
        const ParsedProperty& pa = a.properties[i];
        const ParsedProperty& pb = b.properties[i];
        if (!(pa == pb))
            return Format(L"property[%zu]: ", i) + pa.name + L"=" + pa.value + Format(L" (InType %u, ok %d)", pa.inType, pa.ok) +
                   L" vs " + pb.name + L"=" + pb.value + Format(L" (InType %u, ok %d)", pb.inType, pb.ok);
    }
    return L"";
}

void ParseEvent(const EVENT_RECORD& record, ParsedEvent& ev, std::atomic<uint64_t>& tdhErrors)
{
    // TDH 的參數型別不是 const，但不會修改事件內容
    PEVENT_RECORD rec = const_cast<PEVENT_RECORD>(&record);
    const EVENT_HEADER& h = record.EventHeader;

    ev = ParsedEvent{};
    ev.providerId = h.ProviderId;
    ev.id = h.EventDescriptor.Id;
    ev.version = h.EventDescriptor.Version;
    ev.opcode = h.EventDescriptor.Opcode;
    ev.task = h.EventDescriptor.Task;
    ev.timestamp = static_cast<ULONGLONG>(h.TimeStamp.QuadPart); // 依前提為 FILETIME（見 event_parser.h）
    ev.headerProcessId = h.ProcessId;
    ev.headerThreadId = h.ThreadId;

    ULONG size = 0;
    ULONG status = TdhGetEventInformation(rec, 0, nullptr, nullptr, &size);
    std::vector<BYTE> buffer;
    if (status == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize(size);
        status = TdhGetEventInformation(rec, 0, nullptr, reinterpret_cast<TRACE_EVENT_INFO*>(buffer.data()), &size);
    }
    ev.tdhInfoStatus = status;

    if (status == ERROR_SUCCESS) {
        const auto* info = reinterpret_cast<const TRACE_EVENT_INFO*>(buffer.data());
        const BYTE* base = buffer.data();
        ev.taskName = StringAtOffset(base, info->TaskNameOffset);
        ev.opcodeName = StringAtOffset(base, info->OpcodeNameOffset);
        ParseProperties(rec, info, ev, tdhErrors);
    } else {
        tdhErrors.fetch_add(1, std::memory_order_relaxed);
    }
}

std::wstring FormatEventText(const ParsedEvent& ev, const std::wstring& timestampSuffix)
{
    std::wstring text;
    text += Format(L"EventId=%u  Version=%u  Task=%u(%ls)  Opcode=%u(%ls)\n", ev.id, ev.version, ev.task,
                   ev.taskName.c_str(), ev.opcode, ev.opcodeName.c_str());
    // 0 是無法換算的標記（FILETIME 0 = 1601 年，不可能是真實事件時間），不要把它當日期印出
    if (ev.timestamp == 0)
        text += L"TimeStamp=<unavailable>" + timestampSuffix + L"\n";
    else
        text += L"TimeStamp=" + FormatFileTimeLocal(ev.timestamp) + Format(L" (raw FILETIME %llu)", ev.timestamp) +
                timestampSuffix + L"\n";
    text += Format(L"Header.ProcessId=%lu  Header.ThreadId=%lu\n", ev.headerProcessId, ev.headerThreadId);

    if (ev.tdhInfoStatus != ERROR_SUCCESS) {
        text += Format(L"TdhGetEventInformation failed, error=%lu, EventId=%u\n", ev.tdhInfoStatus, ev.id);
        return text;
    }

    const ParsedProperty* payloadPid = nullptr;
    for (const ParsedProperty& p : ev.properties) {
        // 值可能很長（完整路徑），不經過 Format 的固定緩衝區
        text += Format(L"  %-20ls [InType %2u] = ", p.name.c_str(), p.inType) + p.value + L"\n";
        if (p.ok && _wcsicmp(p.name.c_str(), L"ProcessID") == 0)
            payloadPid = &p;
    }
    if (payloadPid) {
        bool same = std::wcstoul(payloadPid->value.c_str(), nullptr, 10) == ev.headerProcessId;
        text += Format(L"  >> Header.ProcessId=%lu vs Payload.ProcessID=", ev.headerProcessId) +
                payloadPid->value + (same ? L" : SAME\n" : L" : DIFFERENT\n");
    }
    return text;
}
