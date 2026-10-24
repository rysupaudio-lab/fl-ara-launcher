#include "FLNativeTransport.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <dlfcn.h>
#include <pthread.h>
#include <limits.h>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace rysup::fl_ara {namespace {
template<class T>bool readAt(std::uintptr_t p,T& out)noexcept{mach_vm_size_t n=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),(mach_vm_address_t)&out,&n)==KERN_SUCCESS&&n==sizeof(T);}
bool engineAddress(std::uintptr_t p,std::uintptr_t requiredOffset=0)noexcept{
    const auto& app=nativeApplication();Dl_info info{};char path[PATH_MAX];
    return p&&dladdr((void*)p,&info)&&info.dli_fname&&realpath(info.dli_fname,path)
        &&app.enginePath==path&&(std::uintptr_t)info.dli_fbase==app.engine.imageBase
        &&(!requiredOffset||p-app.engine.imageBase==requiredOffset);
}
struct Permission {std::uintptr_t core=0,receiver=0,table=0,method=0;};
bool permissionIdentity(Permission& p)noexcept{
    const auto base=nativeApplication().engine.imageBase;std::uintptr_t coreTable=0;std::uint64_t size=0;
    // This is actual TCore, not a fabricated AppMain/AudioUnit parent.
    return readAt(base+0x1b9fa38,p.core)&&readAt(p.core,coreTable)&&engineAddress(coreTable,0x14cbe90)
        &&readAt(p.core+8,p.receiver)&&readAt(p.receiver,p.table)&&engineAddress(p.table)
        &&readAt(p.table,size)&&size>=8&&size<=4*1024*1024
        &&readAt(p.table+0x160,p.method)&&engineAddress(p.method);
}
bool same(const Permission& a,const Permission& b)noexcept{return a.core==b.core&&a.receiver==b.receiver&&a.table==b.table&&a.method==b.method;}
}
NativeTransportReceipt requestNativePlayingState(const NativeInsert& insert,bool playing)noexcept{
    NativeTransportReceipt r;if(!pthread_main_np()){r.refusal=NativeTransportRefusal::NotMainThread;return r;}
    if(!revalidateActualNativeInsert(insert)){r.refusal=NativeTransportRefusal::NativeOwnerUnavailable;return r;}
    if(!readActualNativeTransportState(r.stateBefore)){r.refusal=NativeTransportRefusal::TransportStateUnavailable;return r;}
    if((r.stateBefore==0&&!playing)||(r.stateBefore==1&&playing)){
        r.stateAfter=r.stateBefore;r.outcome=NativeTransportOutcome::AlreadyInRequestedState;return r;
    }
    const bool verified=requestActualNativePlaying(insert,playing);
    if(!readActualNativeTransportState(r.stateAfter)||!verified){r.refusal=NativeTransportRefusal::NativePlayingRequestNotVerified;return r;}
    r.nativeCallReturned=true;r.outcome=NativeTransportOutcome::PlayingStateVerified;return r;
}
NativeTransportReceipt requestNativePlaybackPositionWhileStopped(const NativeInsert& insert,double seconds)noexcept{
    NativeTransportReceipt r;r.requestedSeconds=seconds;
    if(!pthread_main_np()){r.refusal=NativeTransportRefusal::NotMainThread;return r;}
    if(!std::isfinite(seconds)||seconds<0||!std::isfinite(seconds*1000.0)){r.refusal=NativeTransportRefusal::InvalidPosition;return r;}
    if(!revalidateActualNativeInsert(insert)){r.refusal=NativeTransportRefusal::NativeOwnerUnavailable;return r;}
    if(!readActualNativeTransportState(r.stateBefore)){r.refusal=NativeTransportRefusal::TransportStateUnavailable;return r;}
    if(r.stateBefore!=0){r.refusal=NativeTransportRefusal::PositionRequiresStoppedTransport;return r;}
    r.boundary=observeActualNativeMixBoundary();if(!r.boundary.available()){r.refusal=NativeTransportRefusal::NativeBoundaryUnavailable;return r;}
    Permission p;if(!permissionIdentity(p)){r.refusal=NativeTransportRefusal::NativeCoreMismatch;return r;}
    r.actualCore=p.core;r.actualPermissionReceiver=p.receiver;r.actualPermissionMethod=p.method;
    // Reproduce native81ce50's original virtual permission query, unchanged.
    using PermissionMethod=std::int32_t(*)(void*);
    r.nativePermissionGranted=((PermissionMethod)p.method)((void*)p.receiver)!=0;
    if(!r.nativePermissionGranted){r.refusal=NativeTransportRefusal::NativePermissionDenied;return r;}
    Permission after;if(!permissionIdentity(after)||!same(p,after)||!revalidateActualNativeInsert(insert)){
        r.refusal=NativeTransportRefusal::NativeIdentityChanged;return r;
    }
    r.boundary=observeActualNativeMixBoundary();if(!r.boundary.available()){r.refusal=NativeTransportRefusal::NativeBoundaryUnavailable;return r;}
    const auto base=nativeApplication().engine.imageBase;
    const auto convert=base+0x7d31b0,seek=base+0x325d60;
    if(!engineAddress(convert,0x7d31b0)||!engineAddress(seek,0x325d60)){
        r.refusal=NativeTransportRefusal::NativePermissionUnavailable;return r;
    }
    using Convert=double(*)(double);r.convertedNativeTicks=((Convert)convert)(seconds*1000.0);
    if(!std::isfinite(r.convertedNativeTicks)||r.convertedNativeTicks<0||r.convertedNativeTicks>std::numeric_limits<std::int32_t>::max()){
        r.refusal=NativeTransportRefusal::NativeClockConversionInvalid;return r;
    }
    using Seek=void(*)(std::uint32_t,double);((Seek)seek)(0,r.convertedNativeTicks);
    r.nativeCallReturned=true;
    if(!readActualNativeTransportState(r.stateAfter)||r.stateAfter!=0||!permissionIdentity(after)||!same(p,after)||!revalidateActualNativeInsert(insert)){
        r.refusal=NativeTransportRefusal::NativeIdentityChanged;return r;
    }
    r.outcome=NativeTransportOutcome::NativeSeekReturned;
    return r;
}
const char* nativeTransportRefusal(NativeTransportRefusal reason)noexcept{
    switch(reason){
    case NativeTransportRefusal::None:return "none";
    case NativeTransportRefusal::NotMainThread:return "not_main_thread";
    case NativeTransportRefusal::NativeOwnerUnavailable:return "native_owner_unavailable";
    case NativeTransportRefusal::TransportStateUnavailable:return "native_transport_state_unavailable";
    case NativeTransportRefusal::NativePlayingRequestNotVerified:return "native_playing_request_not_verified";
    case NativeTransportRefusal::PositionRequiresStoppedTransport:return "position_requires_stopped_transport";
    case NativeTransportRefusal::InvalidPosition:return "invalid_position";
    case NativeTransportRefusal::NativeBoundaryUnavailable:return "native_outer_mix_lock_not_clear";
    case NativeTransportRefusal::NativeCoreMismatch:return "native_core_or_permission_receiver_mismatch";
    case NativeTransportRefusal::NativePermissionUnavailable:return "native_permission_or_seek_function_unavailable";
    case NativeTransportRefusal::NativePermissionDenied:return "native_permission_denied";
    case NativeTransportRefusal::NativeIdentityChanged:return "native_identity_changed";
    case NativeTransportRefusal::NativeClockConversionInvalid:return "native_clock_conversion_invalid";
    }
    return "unknown";
}
}
