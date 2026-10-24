#pragma once
// Test-only numeric observation of one genuine native VST3 process call.
// This never changes audio, queries an SDK interface, or retains audio outside
// its caller-owned fixed storage. It is not a universal plug-in qualification.
#include "FLVST3NativeHooks.h"
#include <array>
#include <atomic>
#include <cstdint>

namespace rysup::fl_ara::process_observer {
struct Snapshot {
    std::uint64_t completedPairs=0, comparedCalls=0, nativeFailures=0;
    std::uint64_t unsupportedCalls=0, identityFailures=0, unpairedAfter=0;
    std::uint64_t frameLimitSkips=0, zeroFrameCalls=0, nonfiniteSamples=0;
    std::uint64_t silentNullChannels=0, comparedSamples=0, bitDifferentSamples=0;
    std::uint64_t comparedFrames=0, inputHash=0, outputHash=0, counterOverflows=0;
    std::uint64_t nonzeroInputSamples=0, finiteSamples=0;
    std::uint64_t silentFlagContradictions=0;
    double differenceSumSquares=0, maximumAbsoluteDifference=0;
    double inputSumSquares=0, outputSumSquares=0;
    // These independent monotonic counters are sampled separately from the
    // coherently committed pair statistics above. They diagnose interference.
    std::uint64_t overlappingBefore=0, rejectedAfter=0;
};

// Saturating arithmetic also used by the bounded RT accumulator. Public for
// independently testing the otherwise practically unreachable overflow case.
bool saturatingAdd(std::uint64_t& value,std::uint64_t increment) noexcept;

class NativeProcessObserver final {
public:
    static constexpr std::uint32_t maximumFrames=8192;
    explicit NativeProcessObserver(native_hooks::InstanceRecord& bound) noexcept;
    NativeProcessObserver(const NativeProcessObserver&)=delete;
    NativeProcessObserver& operator=(const NativeProcessObserver&)=delete;
    // Call for BOTH synchronous telemetry phases on this stable instance.
    // Caller allocates and destroys this object only while native processing
    // is stopped and drained. No reset/rebinding while callbacks may execute.
    void observe(native_hooks::InstanceRecord&,const native_hooks::NativeProcessTelemetry&,
                 const native_hooks::ThreadInfo&) noexcept;
    // Main/non-RT only. Bounded attempts; false leaves output unchanged if a
    // pair is being published. No locks and no reads of the RT scratch buffer.
    bool trySnapshot(Snapshot& output) const noexcept;
private:
    static constexpr unsigned fieldCount=23;
    void publish() noexcept;
    void add(std::uint64_t& value,std::uint64_t increment=1) noexcept;
    void finish() noexcept;
    native_hooks::InstanceRecord* const boundRecord;
    const std::uint64_t boundIdentity;
    std::atomic<std::uintptr_t> activeThread{0};
    std::atomic<std::uint64_t> overlaps{0}, rejected{0};
    std::atomic<std::uint64_t> sequence{0};
    std::array<std::atomic<std::uint64_t>,fieldCount> published{};
    Snapshot totals{}; // only the successful activeThread owner writes this
    std::array<float,2*maximumFrames> input{};
    std::uintptr_t dataAddress=0, processorAddress=0;
    std::int32_t frames=0, channels=0;
    std::uint64_t inputSilenceFlags=0;
    unsigned nestedDepth=0;
    bool supported=false, frameLimited=false;
};
} // namespace rysup::fl_ara::process_observer
