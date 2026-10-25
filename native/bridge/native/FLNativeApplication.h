#pragma once
#include "FLNativeModel.h"
#include "FLUnitDeviceGate.h"
#include <cstdint>
#include <functional>
#include <string>

namespace rysup::fl_ara {
struct NativeApplication {
    std::string runID,runDirectory,executablePath,executableSHA256;
    std::string enginePath,wrapperPath;
    rysup::ara_lab::FLNativeEngineBinding engine;
    std::uintptr_t startupHost=0,engineInstance=0;
    int processID=0,parentProcessID=0;
    bool startupOwnershipVerified=false,admitted=false,creationEntered=false,creationReturned=false,engineReady=false;
};
struct NativeInsert {
    std::uintptr_t actualComponent=0,initializeContext=0,hostInstance=0,plugin=0;
    std::uintptr_t audioPluginHost=0,effectWrapper=0,tag=0,host=0;
    std::int32_t mixerInsert=-1;
    std::string evidence;
};
// Control-thread diagnostics only. These numeric observations are not a
// transactional device-state guarantee and never expose device UID strings.
struct NativeOutputStartDiagnostic {
    std::uint64_t sequence=0;
    std::uint32_t stage=0,deviceID=0,currentDeviceBytes=0,uidBytes=0,transportBytes=0;
    std::int32_t currentDeviceStatus=0,uidStatus=0,transportStatus=0,originalStatus=0;
    bool uidReadAttempted=false,transportReadAttempted=false;
    bool uidMatched=false,virtualTransportMatched=false,originalInvoked=false;
};
struct NativeOutputRegistrationDiagnostic {
    std::uint32_t stage=0,reason=0,property=0,scope=0,element=0,bytes=0,callbackSource=0;
    std::uint64_t callbackImageOffset=0,sequence=0,dropped=0;
    std::int32_t originalStatus=0;
    bool recordPresent=false,claimHeld=false,originalInvoked=false,coherent=false;
};
struct NativeOutputCallbackMetadataDiagnostic {
    std::uint32_t registeredScope=0,registeredElement=0;
    std::int32_t routeGetterStatus=0,routeInfoStatus=0,inputGetterStatus=0,inputInfoStatus=0;
    std::uint32_t routeGetterBytes=0,routeInfoBytes=0,inputGetterBytes=0,inputInfoBytes=0;
    bool routeWritable=false,inputWritable=false,outputAlias=false,accepted=false,coherent=false,routeQueried=false,inputQueried=false;
    std::uint64_t sequence=0,dropped=0;
};
struct NativeAudioTestStats {
    bool presentationMuteConfigured=false;
    std::uint64_t callbackCalls=0,nonfiniteScratchSamples=0,preparedUnits=0;
    std::uint64_t nativeRenderCalls=0,nativeRenderedFrames=0,mutedFrames=0,nonzeroScratchSamples=0;
    std::uint64_t nativeErrors=0,unsupportedCallbacks=0,concurrentCallbacks=0;
    std::uint64_t unsafeHardwareBuffers=0,returnedShapeFailures=0,counterOverflows=0;
    std::uint64_t startRefusals=0,registrationRefusals=0,writeOnlyCallbackPreparations=0,criticalPropertyDisarms=0;
    NativeOutputStartDiagnostic lastOutputStart;
    NativeOutputRegistrationDiagnostic lastRegistration;
    NativeOutputCallbackMetadataDiagnostic callbackMetadata;
    bool appleDeviceProfileAdmitted=false;
    std::uint64_t deviceStartEntries=0,deviceStartAtTimeEntries=0,deviceStartOriginalCalls=0,deviceStartOriginalFailures=0;
    std::uint64_t ioProcCreateEntries=0,ioProcCreateOriginalFailures=0,ioProcDestroyEntries=0,lifecycleRefusals=0;
    std::uint64_t deviceStartRefusals=0,ioProcCaptureSuppressed=0,ioProcDestroyOriginalFailures=0;
    std::uint64_t sinkLifecycleRefusals=0;
    test_output::FLUnitDeviceGate::Stats unitDeviceGate;
    struct DeviceIODiagnostic {
        std::uint64_t sequence=0,callerOffset=0,callbackOffset=0;
        std::uint32_t api=0,stage=0,deviceID=0,pair=0,callerSource=0,callbackSource=0;
        std::int32_t originalStatus=0;
        bool originalInvoked=false,captured=false,suppressed=false;
    } lastDeviceIO;
};
struct NativeApplicationCallbacks {
    void (*admitted)(const NativeApplication&,void*) noexcept=nullptr;
    // Startup-only, after the exact engine code is loaded but BEFORE its
    // constructor/project load. False refuses this proven owned startup.
    bool (*beforeOriginalCreate)(const NativeApplication&,void*) noexcept=nullptr;
    void (*tick)(const NativeApplication&,void*) noexcept=nullptr;
    void (*creationReturned)(const NativeApplication&,int,void*) noexcept=nullptr;
    void* context=nullptr;
};
// Called by our own injected helper constructor. It verifies the executing
// private copy and parent/run before enabling any symbol/data interception.
bool initializeNativeApplication(const NativeApplicationCallbacks&) noexcept;
const NativeApplication& nativeApplication() noexcept;
NativeAudioTestStats nativeAudioTestStats() noexcept;
bool nativeApplicationAdmitted() noexcept;
bool refreshActualNativeApplication() noexcept;
bool resolveActualNativeInsert(std::uintptr_t component,std::uintptr_t context,
                              NativeInsert&) noexcept;
bool revalidateActualNativeInsert(const NativeInsert&) noexcept;
// Separate native controller -> exact current component through the sealed
// per-wrapper H/P reciprocal chain and P+818/P+828 fields. No SDK/QI calls,
// class-ID pairing, creation-order or state-payload inference.
bool resolveActualNativeController(std::uintptr_t controller,std::uintptr_t actualInitializeContext,
                                  NativeInsert&) noexcept;
bool revalidateActualNativeController(const NativeInsert&,std::uintptr_t controller) noexcept;
// Current native state, not the last observed plug-in ProcessContext: raw 0 is
// stopped, raw 1 is playing; any other state is expressly not a stopped hold.
bool readActualNativeTransportState(std::int32_t&) noexcept;
// Exact observed native TCriticalSection via original FL host methods. The
// callback must only copy bounded native RAM; no ARA/plugin/UI/file operations.
bool withActualNativeMixCaptureLock(const NativeInsert&,const std::function<void()>&);
bool requestActualNativePlaying(const NativeInsert&,bool playing) noexcept;
// Main-thread public FL SDK FHD_SetDirty(48), for document edits which ordinary
// automatable parameter notifications do not cover. True means the verified
// native dispatcher returned; runtime UI/readback must verify host dirty state.
bool requestActualNativeProjectDirty(const NativeInsert&) noexcept;
void writeNativeApplicationEvent(const char* fixedEvent,const std::string& detail={}) noexcept;
} // namespace rysup::fl_ara
