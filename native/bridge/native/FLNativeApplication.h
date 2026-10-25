#pragma once
#include "FLNativeModel.h"
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
struct NativeAudioTestStats {
    bool presentationMuteConfigured=false;
    std::uint64_t nativeRenderCalls=0,nativeRenderedFrames=0,mutedFrames=0,nonzeroScratchSamples=0;
    std::uint64_t nativeErrors=0,unsupportedCallbacks=0,concurrentCallbacks=0;
    std::uint64_t unsafeHardwareBuffers=0,returnedShapeFailures=0,counterOverflows=0;
    std::uint64_t startRefusals=0,registrationRefusals=0,writeOnlyCallbackPreparations=0,criticalPropertyDisarms=0;
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
