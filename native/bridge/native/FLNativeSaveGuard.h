#pragma once
#include <cstdint>
#include <string>

namespace rysup::fl_ara {
enum class NativeSaveRejection {
    None, NotMainThread, NativeFrameUnreadable, NativeOpenRouteMismatch,
    DirectTargetUnsupported, NativeTransportNotStopped, OwnerNotReady,
    NestedOrRepeatedOpen, MoveGuardUnavailable, LateSerializationFailure,
    ReentrantPreflight, OwnerPrecommitRefused, UnsupportedSaveType
};
struct NativeSaveRequest {
    std::uintptr_t actualSaveFrame=0;
    std::string targetPath,actualOpenPath;
    std::uint8_t nativeFlags=0;
    std::uint32_t actualSaveType=0;
    int openFlags=0;
    bool directedTarget=false;
};
struct NativeSaveTransaction {
    NativeSaveRequest request;
    std::uint64_t serial=0;
    bool failed=false,openAllowed=false,openAttempted=false,commitBegun=false;
    // No ARA owner at first destructive effect: track the ordinary native
    // commit so later ARA discovery cannot strand a target already moved aside.
    // No preflight/precommit, archive freeze or owner cookie is manufactured.
    bool ordinaryCommit=false;
    NativeSaveRejection rejection=NativeSaveRejection::None;
    void* ownerCookie=nullptr;
};
struct NativeSaveGuardCallbacks {
    // Freeze already-prepared strict envelopes. May use only verified owned
    // HostLockMix plus bounded read-only engine/source seal and RAM capture /
    // decode under stopped, process-drained admission. No ARA/plugin/state
    // getters, other native APIs or file writes. False denies before creation.
    bool (*preflight)(const NativeSaveRequest&,void** ownerCookie,void*) noexcept=nullptr;
    // Before the FIRST destructive unlink/move, prove every expected component
    // emitted its frozen valid envelope and all tickets/current RAM still
    // match. Same read-only preflight restrictions apply.
    bool (*precommit)(const NativeSaveRequest&,void* ownerCookie,void*) noexcept=nullptr;
    void (*rejected)(const NativeSaveRequest&,NativeSaveRejection,void*) noexcept=nullptr;
    // Main tick proves the original actual Save frame is gone before this call.
    // Release the owner freeze regardless of success/refusal; no file cleanup.
    void (*finished)(const NativeSaveTransaction&,void*) noexcept=nullptr;
    void* context=nullptr;
    // Fresh owner authority at each actual file boundary. Must read only stable
    // atomic native-record flags/lifetimes; no SDK, file operations, allocation,
    // locks or callbacks. When present it replaces the legacy live-flag snapshot.
    bool (*hasLiveARA)(void*) noexcept=nullptr;
};
// Main-thread, private admitted process only, after dlsym supplied the sealed
// loaded engine binding and BEFORE original CreateFruityInstance. Requires code
// pins and native move interception; no engine instance/readiness is required.
bool initializeNativeSaveGuard(const NativeSaveGuardCallbacks&) noexcept;
// Legacy fallback only when callbacks.hasLiveARA is absent. Pending transactions
// always remain protected even after the current owner predicate becomes false.
void setNativeSaveGuardHasLiveARA(bool) noexcept;
bool nativeSaveGuardReady() noexcept;
// Root's own main tick only. It releases transactions whose actual Save stack
// frames disappeared; no files are changed or deleted by this maintenance.
void pollNativeSaveGuardTransactions() noexcept;
const NativeSaveTransaction* currentNativeSaveTransaction() noexcept;
// Call for every actual component/ARA envelope save refusal. Same actual Save
// transaction then blocks native old-target unlink and Foundation moves.
bool markCurrentNativeSaveFailed(NativeSaveRejection=NativeSaveRejection::LateSerializationFailure) noexcept;
// RT/background safe: no allocation, locks, map, stack walk or callbacks. The
// stable transaction node lives for process lifetime. Once commit begins this
// refuses late invalidation; it must not strand a target already moved aside.
bool markActiveNativeSaveFailedFromAnyThread() noexcept;
// Atomic-only snapshot, including failed or committed saves until their actual
// native frame finishes. No lazy State initialization, allocation or callbacks.
bool hasActiveNativeSaveFromAnyThread() noexcept;
const char* nativeSaveRejection(NativeSaveRejection) noexcept;

#if defined(RYSUP_FL_SAVE_GUARD_TESTING)
namespace save_guard_testing {
// Test-only admission and synthetic frame fixture. These symbols/branches do
// not exist in production compilation. Files must lie beneath ownedRoot.
bool initialize(const NativeSaveGuardCallbacks&,const std::string& ownedRoot) noexcept;
// Reads only this process's supplied fixture bytes using the production pin
// validator. It does not replace actual loaded-image identity in production.
bool instructionPinsMatch(std::uintptr_t) noexcept;
bool setFrame(const NativeSaveRequest&) noexcept;
void clearFrame() noexcept;
int open(const char*,int,unsigned mode=0600) noexcept;
int unlink(const char*) noexcept;
}
#endif
}
