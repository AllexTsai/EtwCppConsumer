#include "event_callback.h"

void HandleEventRecord(ConsumerContext& ctx, const EVENT_RECORD& record, int64_t callbackEntryTick) noexcept
{
    EventCounters& c = ctx.counters;

    if (!IsEqualGUID(record.EventHeader.ProviderId, kKernelProcessProvider)) {
        c.otherProvider.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint64_t sequence = c.received.fetch_add(1, std::memory_order_relaxed);
    if (record.ExtendedDataCount > 0)
        c.withExtendedData.fetch_add(1, std::memory_order_relaxed);

    if (record.UserDataLength > kMaxUserDataBytes) {
        c.droppedOversize.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const bool pushed = ctx.ring.TryPush(
        [&](EventSlot& slot) noexcept { CopyToSlot(record, sequence, callbackEntryTick, slot); });
    if (pushed)
        c.enqueued.fetch_add(1, std::memory_order_relaxed);
    else
        c.droppedQueueFull.fetch_add(1, std::memory_order_relaxed);
}

void WINAPI OnEventRecord(PEVENT_RECORD record)
{
    // 一進入就取 QPC：同時作為槽位的 callbackEntryTick 與 callback 耗時的起點
    LARGE_INTEGER start;
    const BOOL startOk = QueryPerformanceCounter(&start);

    auto* ctx = static_cast<ConsumerContext*>(record->UserContext);
    if (!startOk)
        ctx->counters.qpcFailures.fetch_add(1, std::memory_order_relaxed);

    HandleEventRecord(*ctx, *record, startOk ? start.QuadPart : 0);

    if (!startOk)
        return;
    LARGE_INTEGER end;
    if (QueryPerformanceCounter(&end)) {
        const int64_t elapsed = end.QuadPart - start.QuadPart;
        // 只有 callback 執行緒會寫入，所以 load + store 不會有競爭
        if (elapsed > ctx->counters.maxCallbackTicks.load(std::memory_order_relaxed))
            ctx->counters.maxCallbackTicks.store(elapsed, std::memory_order_relaxed);
    } else {
        ctx->counters.qpcFailures.fetch_add(1, std::memory_order_relaxed);
    }
}
