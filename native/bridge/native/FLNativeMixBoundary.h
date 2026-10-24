#pragma once
#include <pthread.h>

namespace rysup::fl_ara {
enum class NativeMixBoundaryReason {
    Available, NotMainThread, NativeApplicationUnavailable, NotStopped,
    NativeCriticalSectionMismatch, NativeThreadManagerMismatch,
    WorkerAllocationFailed, WorkerCreateFailed, WorkerJoinFailed,
    Contended, TryLockFailed, WorkerUnlockFailed, NativeIdentityChanged
};
struct NativeMixBoundary {
    NativeMixBoundaryReason reason=NativeMixBoundaryReason::NativeApplicationUnavailable;
    int createResult=-1,joinResult=-1,tryLockResult=-1,unlockResult=-1;
    bool nativeIdentityVerified=false;
    bool available() const noexcept{return reason==NativeMixBoundaryReason::Available;}
};
// Main thread only. Call after releasing our bounded native capture lock and
// draining our own render/read leases, immediately before any ARA model/archive
// work. A successful OWN worker trylock proves this main thread does not hold a
// recursive outer FL mix lock at this instant. It does NOT hold FL's lock across
// subsequent work or prove another thread cannot acquire it later.
NativeMixBoundary observeActualNativeMixBoundary() noexcept;
const char* nativeMixBoundaryReason(NativeMixBoundaryReason) noexcept;
namespace detail {
// Pure worker mechanism exposed only for its standalone meaningful regression
// test. Caller must supply one initialized, live pthread_mutex_t and preserve it
// throughout the call. Never calls a DAW, changes a mutex type or probes bytes.
NativeMixBoundary probeInitializedMutexFromOwnedWorker(pthread_mutex_t*) noexcept;
}
}
