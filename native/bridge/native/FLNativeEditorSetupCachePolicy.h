#pragma once
// SDK-free policy for an already ARA-editor-only native component. Its source
// cache was rendered by a SEPARATE owned playback companion. Native block-size
// and realtime/prefetch/offline configuration alone do not edit that source.
#include <atomic>
#include <cstdint>

namespace rysup::fl_ara::editor_setup_cache {
inline constexpr std::int32_t nativeFloat32=0;
inline constexpr std::int32_t nativeRealtime=0,nativePrefetch=1,nativeOffline=2;
inline constexpr std::int32_t maximumNativeBlockFrames=65536;

struct ActualNativeSetup {
    bool originalSucceeded=false;
    double sampleRate=0;
    std::int32_t symbolicSampleSize=-1,processMode=-1,maxSamplesPerBlock=0;
};
enum class Action : std::uint8_t { PreserveCoherentSourceCache,InvalidateForRebuild };
struct Decision {Action action=Action::InvalidateForRebuild;bool setupQualified=false;};

inline bool supported(const ActualNativeSetup& value)noexcept {
    return value.originalSucceeded&&value.sampleRate==48000.0&&value.symbolicSampleSize==nativeFloat32&&
        value.processMode>=nativeRealtime&&value.processMode<=nativeOffline&&
        value.maxSamplesPerBlock>0&&value.maxSamplesPerBlock<=maximumNativeBlockFrames;
}

// Called AFTER the exact original native setup call. The owner must pass its
// real setup/dirty atomics and actual gate, not mirrored authority flags.
// mayReadModelAuthority is true only on the actual main/model thread outside
// native process calls. The query is the owner's existing full coherence check
// (including ready/prime/dirty/archive/ticket state), not a cached boolean.
template<class CacheGate,class CurrentCacheCoherent>
Decision apply(const ActualNativeSetup& value,bool mayReadModelAuthority,
        std::atomic<double>& actualSampleRate,std::atomic<bool>& actualSetupQualified,
        std::atomic<bool>& actualDirty,CacheGate* actualGate,CurrentCacheCoherent&& currentCacheCoherent)noexcept {
    const bool previousQualified=actualSetupQualified.load(std::memory_order_acquire);
    const bool qualified=supported(value);
    const bool preserve=qualified&&previousQualified&&mayReadModelAuthority&&actualGate&&currentCacheCoherent();
    actualSampleRate.store(value.sampleRate,std::memory_order_release);
    actualSetupQualified.store(qualified,std::memory_order_release);
    if(preserve){
        // Deliberately NO dirty=false, ticket publication, prime reset or epoch
        // write. A concurrent actual edit may close its gate at ANY point; this
        // path never reopens it or overwrites the edit's queued dirty request.
        return {Action::PreserveCoherentSourceCache,true};
    }
    // First/unready/incompatible/failed setup retains strict invalidation and
    // the owner's existing stopped-boundary prime/rebuild path.
    actualDirty.store(true,std::memory_order_release);
    if(actualGate)actualGate->invalidate();
    return {Action::InvalidateForRebuild,qualified};
}
} // namespace rysup::fl_ara::editor_setup_cache
