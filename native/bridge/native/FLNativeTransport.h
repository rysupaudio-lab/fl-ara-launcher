#pragma once
#include "FLNativeApplication.h"
#include "FLNativeMixBoundary.h"
#include <cstdint>

namespace rysup::fl_ara {
enum class NativeTransportOutcome {
    Refused, AlreadyInRequestedState, PlayingStateVerified,
    // The genuine void native seek returned. Separate native runtime readback
    // must verify audible/current position; this is not a verified completion.
    NativeSeekReturned
};
enum class NativeTransportRefusal {
    None, NotMainThread, NativeOwnerUnavailable, TransportStateUnavailable,
    NativePlayingRequestNotVerified, PositionRequiresStoppedTransport,
    InvalidPosition, NativeBoundaryUnavailable, NativeCoreMismatch,
    NativePermissionUnavailable, NativePermissionDenied,
    NativeIdentityChanged, NativeClockConversionInvalid
};
struct NativeTransportReceipt {
    NativeTransportOutcome outcome=NativeTransportOutcome::Refused;
    NativeTransportRefusal refusal=NativeTransportRefusal::None;
    NativeMixBoundary boundary;
    std::int32_t stateBefore=-1,stateAfter=-1;
    double requestedSeconds=0,convertedNativeTicks=0;
    std::uintptr_t actualCore=0,actualPermissionReceiver=0,actualPermissionMethod=0;
    bool nativePermissionGranted=false,nativeCallReturned=false;
    bool nativePositionReadbackVerified=false;
};
// These helper APIs do not advertise an ARA PlaybackController interface.
// Root must defer plug-in ARA requests onto its own next main-thread boundary;
// never call them inline from a plug-in/ARA/native state callback.
NativeTransportReceipt requestNativePlayingState(const NativeInsert&,bool playing) noexcept;
// The exact native public setSongPos source route, preserving its own permission
// query and clock conversion. Initially admitted only while fully stopped.
NativeTransportReceipt requestNativePlaybackPositionWhileStopped(const NativeInsert&,double seconds) noexcept;
const char* nativeTransportRefusal(NativeTransportRefusal) noexcept;
// No loop or cycle-range helper exists: native loop seams are not qualified.
}
