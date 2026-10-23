#include "event_slot.h"

#include <cstring>

void CopyToSlot(const EVENT_RECORD& record, uint64_t sequence, EventSlot& slot) noexcept
{
    slot.sequence = sequence;
    slot.header = record.EventHeader;
    slot.bufferContext = record.BufferContext;
    slot.userDataLength = record.UserDataLength;
    if (record.UserDataLength > 0)
        std::memcpy(slot.userData, record.UserData, record.UserDataLength);
}

EVENT_RECORD RebuildRecord(EventSlot& slot) noexcept
{
    EVENT_RECORD record{};
    record.EventHeader = slot.header;
    record.BufferContext = slot.bufferContext;
    record.ExtendedDataCount = 0;
    record.ExtendedData = nullptr;
    record.UserDataLength = slot.userDataLength;
    record.UserData = slot.userDataLength > 0 ? slot.userData : nullptr;
    record.UserContext = nullptr;
    return record;
}
