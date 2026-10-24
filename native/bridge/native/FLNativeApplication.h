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
    bool admitted=false,creationEntered=false,creationReturned=false,engineReady=false;
};
struct NativeInsert {
    std::uintptr_t actualComponent=0,initializeContext=0,hostInstance=0,plugin=0;
    std::uintptr_t audioPluginHost=0,effectWrapper=0,tag=0,host=0;
    std::int32_t mixerInsert=-1;
    std::string evidence;
};
struct NativeApplicationCallbacks {
    void (*admitted)(const NativeApplication&,void*) noexcept=nullptr;
    void (*tick)(const NativeApplication&,void*) noexcept=nullptr;
    void (*creationReturned)(const NativeApplication&,int,void*) noexcept=nullptr;
    void* context=nullptr;
};
// Called by our own injected helper constructor. It verifies the executing
// private copy and parent/run before enabling any symbol/data interception.
bool initializeNativeApplication(const NativeApplicationCallbacks&) noexcept;
const NativeApplication& nativeApplication() noexcept;
bool nativeApplicationAdmitted() noexcept;
bool refreshActualNativeApplication() noexcept;
bool resolveActualNativeInsert(std::uintptr_t component,std::uintptr_t context,
                              NativeInsert&) noexcept;
bool revalidateActualNativeInsert(const NativeInsert&) noexcept;
// Current native state, not the last observed plug-in ProcessContext: raw 0 is
// stopped, raw 1 is playing; any other state is expressly not a stopped hold.
bool readActualNativeTransportState(std::int32_t&) noexcept;
// Exact observed native TCriticalSection via original FL host methods. The
// callback must only copy bounded native RAM; no ARA/plugin/UI/file operations.
bool withActualNativeMixCaptureLock(const NativeInsert&,const std::function<void()>&);
bool requestActualNativePlaying(const NativeInsert&,bool playing) noexcept;
void writeNativeApplicationEvent(const char* fixedEvent,const std::string& detail={}) noexcept;
} // namespace rysup::fl_ara
