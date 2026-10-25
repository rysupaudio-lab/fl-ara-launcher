#pragma once
// Explicit QA mode only: genuine native FL render callbacks receive private
// scratch buffers; the driver-provided output buffers are presented as silence.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rysup::fl_ara::test_output {
using SetProperty=OSStatus (*)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,const void*,UInt32);
struct Functions {
    SetProperty setProperty=AudioUnitSetProperty;
    OSStatus (*getProperty)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,void*,UInt32*)=AudioUnitGetProperty;
    AudioComponent (*getComponent)(AudioComponentInstance)=AudioComponentInstanceGetComponent;
    OSStatus (*getDescription)(AudioComponent,AudioComponentDescription*)=AudioComponentGetDescription;
    // Non-RT only. The owner checks CURRENT admitted process and exact UID plus
    // virtual transport through actual AudioObject properties. No guessed ID.
    bool (*currentlyAdmitted)(void*) noexcept=nullptr;
    bool (*verifyExactTestDevice)(AudioDeviceID,void*) noexcept=nullptr;
    void* context=nullptr;
    OSStatus (*getPropertyInfo)(AudioUnit,AudioUnitPropertyID,AudioUnitScope,AudioUnitElement,UInt32*,Boolean*)=AudioUnitGetPropertyInfo;
};
struct Config {
    const char* exactAdmittedEnginePath=nullptr;
    const char* exactTestDeviceUID=nullptr; // must equal Pro Tools Audio Bridge 2-A_UID
    Functions functions{};
};
struct Stats {
    std::uint64_t callbackCalls=0,nativeRenderCalls=0,nativeRenderedFrames=0;
    std::uint64_t nonzeroScratchSamples=0,nonfiniteScratchSamples=0,mutedFrames=0;
    std::uint64_t unsupportedCallbacks=0,concurrentCallbacks=0,nativeErrors=0;
    std::uint64_t returnedShapeFailures=0,unsafeHardwareBuffers=0,counterOverflows=0;
    std::uint64_t registrationRefusals=0,startRefusals=0,preparedUnits=0;
    std::uint64_t writeOnlyCallbackPreparations=0;
    std::uint64_t criticalPropertyDisarms=0;
};

class FLTestOutputSink final {
public:
    static constexpr std::size_t maximumUnits=8,maximumFrames=8192;
    static constexpr std::size_t scratchBytes=maximumFrames*2*sizeof(double);
    static constexpr std::size_t maximumZeroBytes=256*1024;
    FLTestOutputSink() noexcept=default;
    FLTestOutputSink(const FLTestOutputSink&)=delete;
    FLTestOutputSink& operator=(const FLTestOutputSink&)=delete;
    // Exactly once before any registration; this object and its records MUST
    // remain alive until process exit. No device or unit is created/started.
    bool initialize(const Config&) noexcept;
    // Invoke ONLY from the owner's explicitly enabled test-mode interposer.
    // Non-target properties and non-output units forward unchanged once.
    OSStatus observedSetProperty(AudioUnit,AudioUnitPropertyID,AudioUnitScope,
        AudioUnitElement,const void*,UInt32) noexcept;
    // Non-RT, before the real AudioOutputUnitStart. False means DO NOT start.
    bool tryPrepareMutedOutputStart(AudioUnit) noexcept;
    // Numeric-only independent atomic counters (not one transactional epoch).
    Stats snapshot() const noexcept;
private:
    struct List2 { UInt32 mNumberBuffers=0;AudioBuffer mBuffers[2]{}; };
    struct Record {
        FLTestOutputSink* owner=nullptr;
        AudioUnit unit=nullptr;
        AURenderCallbackStruct original{};
        AudioStreamBasicDescription format{};
        UInt32 maximumSliceFrames=0;
        bool preparedBefore=false;
        std::atomic<bool> published{false},armed{false},rendering{false},registrationSucceeded{false};
        alignas(16) std::array<std::byte,scratchBytes> scratch{};
        List2 list{};
    };
    static OSStatus render(void*,AudioUnitRenderActionFlags*,const AudioTimeStamp*,
        UInt32,UInt32,AudioBufferList*) noexcept;
    static bool isOutputUnit(AudioUnit,const Functions&) noexcept;
    static bool validFormat(const AudioStreamBasicDescription&) noexcept;
    static bool sameFormat(const AudioStreamBasicDescription&,const AudioStreamBasicDescription&) noexcept;
    Record* find(AudioUnit) noexcept;
    bool zeroHardware(AudioBufferList*,AudioUnitRenderActionFlags*) noexcept;
    bool hardwareShape(const Record&,UInt32,const AudioBufferList*) const noexcept;
    bool returnedShape(const Record&,UInt32) const noexcept;
    void inspectScratch(const Record&,UInt32) noexcept;
    void bump(unsigned,std::uint64_t=1) noexcept;
    Functions functions{};
    std::array<char,4096> enginePath{};
    std::array<Record,maximumUnits> records{};
    std::array<std::atomic<std::uint64_t>,17> counters{};
    std::atomic<bool> initialized{false},configuring{false};
};
} // namespace rysup::fl_ara::test_output
