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
    // Actual sealed FL profile supplies 0xcd7bc0. Zero is reserved for the
    // independent synthetic/AUHAL metadata profiles, not FL runtime admission.
    std::uint64_t expectedOutputCallbackOffset=0;
};
enum class RegistrationStage:std::uint32_t {None=0,RefusedBeforeOriginal=1,OriginalFailed=2,Registered=3,Removed=4};
enum class RegistrationReason:std::uint32_t {
    None=0,NotInitialized=1,NotAdmitted=2,ConfigurationBusy=3,UnsupportedScope=4,
    UnsupportedElement=5,MissingData=6,WrongDataBytes=7,SelfCallback=8,NoCallbackImage=9,
    CallbackPathUnresolved=10,CallbackNotEngine=11,CallbackChanged=12,RefconChanged=13,
    RegistrationRouteChanged=14,PoolExhausted=15,OriginalSetterFailed=16,CallbackOffsetMismatch=17
};
enum class CallbackSource:std::uint32_t {NotInspected=0,ExactEngine=1,OwnedExecutable=2,OtherImage=3,NoImage=4,SelfWrapper=5};
struct RegistrationDiagnostic {
    RegistrationStage stage=RegistrationStage::None;
    RegistrationReason reason=RegistrationReason::None;
    std::uint32_t property=0,scope=0,element=0,bytes=0;
    CallbackSource callbackSource=CallbackSource::NotInspected;
    std::uint64_t callbackImageOffset=0,sequence=0,dropped=0;
    OSStatus originalStatus=0;
    bool recordPresent=false,claimHeld=false,originalInvoked=false,coherent=false;
};
struct CallbackMetadataDiagnostic {
    std::uint32_t registeredScope=0,registeredElement=0;
    OSStatus routeGetterStatus=0,routeInfoStatus=0,inputGetterStatus=0,inputInfoStatus=0;
    std::uint32_t routeGetterBytes=0,routeInfoBytes=0,inputGetterBytes=0,inputInfoBytes=0;
    bool routeWritable=false,inputWritable=false,outputAlias=false,accepted=false,coherent=false,routeQueried=false,inputQueried=false;
    std::uint64_t sequence=0,dropped=0;
};
struct Stats {
    std::uint64_t callbackCalls=0,nativeRenderCalls=0,nativeRenderedFrames=0;
    std::uint64_t nonzeroScratchSamples=0,nonfiniteScratchSamples=0,mutedFrames=0;
    std::uint64_t unsupportedCallbacks=0,concurrentCallbacks=0,nativeErrors=0;
    std::uint64_t returnedShapeFailures=0,unsafeHardwareBuffers=0,counterOverflows=0;
    std::uint64_t registrationRefusals=0,startRefusals=0,preparedUnits=0;
    std::uint64_t writeOnlyCallbackPreparations=0;
    std::uint64_t criticalPropertyDisarms=0;
    RegistrationDiagnostic lastRegistration{};
    CallbackMetadataDiagnostic callbackMetadata{};
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
        AudioUnitScope registeredScope=0;AudioUnitElement registeredElement=0;
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
    void publishRegistration(const RegistrationDiagnostic&) noexcept;
    RegistrationDiagnostic registrationSnapshot() const noexcept;
    void publishMetadata(const CallbackMetadataDiagnostic&) noexcept;
    CallbackMetadataDiagnostic metadataSnapshot() const noexcept;
    Functions functions{};
    std::array<char,4096> enginePath{};
    std::array<char,4096> ownedExecutablePath{};
    std::uint64_t expectedOutputCallbackOffset=0;
    std::array<Record,maximumUnits> records{};
    std::array<std::atomic<std::uint64_t>,17> counters{};
    std::atomic<bool> initialized{false},configuring{false};
    std::atomic<bool> diagnosticPublishing{false};
    std::atomic<std::uint64_t> diagnosticSequence{0},diagnosticDropped{0};
    std::array<std::atomic<std::uint64_t>,10> diagnosticFields{};
    std::atomic<std::uint64_t> metadataSequence{0};
    std::array<std::atomic<std::uint64_t>,15> metadataFields{};
};
} // namespace rysup::fl_ara::test_output
