#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstddef>
#include <cstdint>

// 單一事件 UserData 的上限。ETW 單一事件最大 64KB；Kernel-Process 的
// ProcessStart 實測約數百位元組（主要是 ImageName 路徑），4KB 約可容納
// 2000 字元的路徑。超過的事件丟棄並計數。
constexpr size_t kMaxUserDataBytes = 4096;

// 佇列槽位：callback 把 EVENT_RECORD 需要的部分複製進來，消費者再用它重建 EVENT_RECORD。
struct EventSlot {
    uint64_t sequence;                 // callback 收到的順序（只計訂閱的 provider）
    EVENT_HEADER header;               // 含 Flags（32/64 位元指標大小等），TDH 需要
    ETW_BUFFER_CONTEXT bufferContext;
    USHORT userDataLength;
    alignas(16) BYTE userData[kMaxUserDataBytes];
};

// 前提：record.UserDataLength <= kMaxUserDataBytes（由呼叫端檢查）。不配置記憶體。
void CopyToSlot(const EVENT_RECORD& record, uint64_t sequence, EventSlot& slot) noexcept;

// 用槽位內的副本重建 EVENT_RECORD。UserData 指向 slot.userData，
// 只在槽位尚未歸還給佇列之前有效。ExtendedData 不複製（Count = 0）。
EVENT_RECORD RebuildRecord(EventSlot& slot) noexcept;
