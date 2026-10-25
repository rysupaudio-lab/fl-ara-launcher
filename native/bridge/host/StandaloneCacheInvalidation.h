#pragma once
#include "StandaloneCacheGate.h"
#include <cstdint>
namespace rysup::ara_lab {
// Authority maintenance is distinct from a new model-change request. Callbacks
// are stable owner atomic/mailbox operations only, including on an edit thread.
template<class CloseOverlay,class QueueStop>
StandaloneCacheTicket invalidateStandaloneCacheAuthority(StandaloneCacheGate& gate,
    CloseOverlay closeOverlay,QueueStop queueStop)noexcept{
    const auto invalidated=gate.invalidate();
    closeOverlay(invalidated.epoch);queueStop(invalidated.epoch);
    // The owner may close the same published gate again while retiring overlay
    // authority. Capture the final current ticket, never the preceding epoch.
    return gate.currentTicket();
}
template<class CloseOverlay,class QueueStop,class NotifyRevision,class QueueRebuild>
void invalidateStandaloneCacheForEdit(StandaloneCacheGate& gate,std::uint64_t revision,
    CloseOverlay closeOverlay,QueueStop queueStop,NotifyRevision notifyRevision,QueueRebuild queueRebuild)noexcept{
    invalidateStandaloneCacheAuthority(gate,closeOverlay,queueStop);
    notifyRevision(revision);
    // Owner coalesces via its atomic dirty bit. Never reset a second queued
    // latch inside a rebuild: an edit arriving there must remain requested.
    queueRebuild();
}
}
