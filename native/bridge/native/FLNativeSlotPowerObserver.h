#pragma once
#include "FLNativeSlotPowerControl.h"
#include <string>

namespace rysup::fl_ara::slot_power {
// All pointed-to own storage and callback context MUST outlive the process.
// The callback proves the current owned/admitted non-RT model boundary, never
// native power or Binding ownership. Installation independently checks images.
struct ObserverConfig {
    Registry<32>* registry=nullptr;
    std::string exactLoadedWrapperPath;
    std::uintptr_t actualWrapperImageBase=0;
    bool(*confirmAdmittedModelBoundary)(void*)noexcept=nullptr;
    void* context=nullptr;
};
enum class ObserverInstallReason : std::uint32_t {
    NotAttempted=0,Installed=1,AlreadyOwned=2,Busy=3,Configuration=4,
    Admission=5,Path=6,DiskImage=7,LoadedImage=8,UUID=9,InstructionPins=10,
    OriginalPointer=11,Page=12,CodeLifetime=13,Publication=14,
    ProtectionRestore=15,UnsupportedPointerABI=16,Exception=17
};
struct ObserverInstallResult {
    bool installed=false,protectionRestored=false;
    std::uintptr_t slot=0,original=0;
    ObserverInstallReason reason=ObserverInstallReason::NotAttempted;
};
// NON-RT only, after actual Wrapper loading and before first binding/source
// publication. No dlopen without RTLD_NOLOAD; never loads vendor code. Changes
// only the exact already-loaded non-executable global data pointer, via COW/CAS.
// Once published, original/helper/registry stay pinned until process exit.
// No restoration, uninstall, deallocation or earlier drain authority is offered.
ObserverInstallResult installLoadedNativeSlotPowerObserver(const ObserverConfig&)noexcept;
struct ObserverStats {
    bool installed=false,protectionRestored=false;
    std::uint64_t entries=0,inFlight=0,id11Entries=0,normalReturns=0;
    ObserverInstallReason lastInstallReason=ObserverInstallReason::NotAttempted;
};
// Numeric independent atomics; a diagnostic snapshot, not a coherent drain.
ObserverStats nativeSlotPowerObserverStats()noexcept;
} // namespace rysup::fl_ara::slot_power
