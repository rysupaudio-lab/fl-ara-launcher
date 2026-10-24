#include "FLNativeMixBoundary.h"
#include <cerrno>
#include <memory>
#include <new>
#ifndef RYSUP_FL_MIX_BOUNDARY_WORKER_TEST_ONLY
#include "FLNativeApplication.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <limits.h>
#include <cstdlib>
#include <cstdint>
#include <string>
#endif

namespace rysup::fl_ara {
namespace {
struct Work {pthread_mutex_t* mutex;int tryResult=-1,unlockResult=-1;};
void* tryOnOwnedWorker(void* context) noexcept {
    auto& w=*static_cast<Work*>(context);
    // No allocation, logging, native callbacks or waiting lock on this worker.
    w.tryResult=pthread_mutex_trylock(w.mutex);
    if(w.tryResult==0)w.unlockResult=pthread_mutex_unlock(w.mutex);
    return nullptr;
}
#ifndef RYSUP_FL_MIX_BOUNDARY_WORKER_TEST_ONLY
template<class T>bool readAt(std::uintptr_t p,T& out)noexcept{mach_vm_size_t n=0;return p&&mach_vm_read_overwrite(mach_task_self(),p,sizeof(T),(mach_vm_address_t)&out,&n)==KERN_SUCCESS&&n==sizeof(T);}
bool imageAt(std::uintptr_t p,const std::string& expected,std::uintptr_t offset)noexcept{
    Dl_info image{};char actual[PATH_MAX];return p&&dladdr((void*)p,&image)&&image.dli_fname
        &&realpath(image.dli_fname,actual)&&expected==actual&&p-(std::uintptr_t)image.dli_fbase==offset;
}
struct Identity {std::uintptr_t engine=0,critical=0,table=0,enter=0,tryEnter=0,leave=0;};
bool nativeIdentity(Identity& id)noexcept {
    const auto& app=nativeApplication();if(!app.engineReady)return false;id.engine=app.engine.imageBase;
    std::uintptr_t lockMethod=0,unlockMethod=0;std::uint64_t size=0;
    if(!readAt(id.engine+0x1f20ab8,id.critical)||!readAt(id.critical,id.table)
        ||!imageAt(id.table,app.enginePath,0x17187e0)||!readAt(id.table,size)||size!=0x48
        ||!readAt(id.table+0xc8,lockMethod)||!imageAt(lockMethod,app.enginePath,0xa7d380)
        ||!readAt(id.table+0xd0,unlockMethod)||!imageAt(unlockMethod,app.enginePath,0xa7d3a0))return false;
    return true;
}
bool threadManager(Identity& id)noexcept {
    const auto& path=nativeApplication().enginePath;
    return readAt(id.engine+0x21ab608+0x88,id.enter)&&imageAt(id.enter,path,0x38330)
        &&readAt(id.engine+0x21ab608+0x90,id.tryEnter)&&imageAt(id.tryEnter,path,0x38350)
        &&readAt(id.engine+0x21ab608+0x98,id.leave)&&imageAt(id.leave,path,0x38380);
}
bool same(const Identity& a,const Identity& b)noexcept{return a.engine==b.engine&&a.critical==b.critical&&a.table==b.table&&a.enter==b.enter&&a.tryEnter==b.tryEnter&&a.leave==b.leave;}
#endif
}
NativeMixBoundary detail::probeInitializedMutexFromOwnedWorker(pthread_mutex_t* mutex)noexcept{
    NativeMixBoundary result;if(!mutex){result.reason=NativeMixBoundaryReason::NativeCriticalSectionMismatch;return result;}
    auto work=std::unique_ptr<Work>(new(std::nothrow)Work{mutex});
    if(!work){result.reason=NativeMixBoundaryReason::WorkerAllocationFailed;return result;}
    pthread_t thread{};result.createResult=pthread_create(&thread,nullptr,tryOnOwnedWorker,work.get());
    if(result.createResult){result.reason=NativeMixBoundaryReason::WorkerCreateFailed;return result;}
    result.joinResult=pthread_join(thread,nullptr);
    if(result.joinResult){
        // Preserve memory if a platform failure leaves worker lifetime unknown.
        // This is a failed boundary, never permission for SDK/model work.
        work.release();result.reason=NativeMixBoundaryReason::WorkerJoinFailed;return result;
    }
    result.tryLockResult=work->tryResult;result.unlockResult=work->unlockResult;
    if(result.tryLockResult==EBUSY)result.reason=NativeMixBoundaryReason::Contended;
    else if(result.tryLockResult)result.reason=NativeMixBoundaryReason::TryLockFailed;
    else if(result.unlockResult)result.reason=NativeMixBoundaryReason::WorkerUnlockFailed;
    else result.reason=NativeMixBoundaryReason::Available;
    return result;
}
#ifndef RYSUP_FL_MIX_BOUNDARY_WORKER_TEST_ONLY
NativeMixBoundary observeActualNativeMixBoundary()noexcept{
    NativeMixBoundary result;if(!pthread_main_np()){result.reason=NativeMixBoundaryReason::NotMainThread;return result;}
    if(!nativeApplicationAdmitted()||!refreshActualNativeApplication())return result;
    std::int32_t transport=-1;if(!readActualNativeTransportState(transport)||transport!=0){result.reason=NativeMixBoundaryReason::NotStopped;return result;}
    Identity before;if(!nativeIdentity(before)){result.reason=NativeMixBoundaryReason::NativeCriticalSectionMismatch;return result;}
    if(!threadManager(before)){result.reason=NativeMixBoundaryReason::NativeThreadManagerMismatch;return result;}
    // The exact sealed native TCriticalSection adds8, then FPC38330/38350/38380
    // forwards that same pointer directly to pthread lock/trylock/unlock. No
    // opaque pthread owner/depth field is read or assumed.
    result=detail::probeInitializedMutexFromOwnedWorker(reinterpret_cast<pthread_mutex_t*>(before.critical+8));
    result.nativeIdentityVerified=true;
    Identity after;if(!refreshActualNativeApplication()||!nativeIdentity(after)||!threadManager(after)||!same(before,after)
        ||!readActualNativeTransportState(transport)||transport!=0)result.reason=NativeMixBoundaryReason::NativeIdentityChanged;
    return result;
}
#endif
const char* nativeMixBoundaryReason(NativeMixBoundaryReason reason)noexcept{
    switch(reason){
    case NativeMixBoundaryReason::Available:return "available_at_probe_instant";
    case NativeMixBoundaryReason::NotMainThread:return "not_main_thread";
    case NativeMixBoundaryReason::NativeApplicationUnavailable:return "native_application_unavailable";
    case NativeMixBoundaryReason::NotStopped:return "native_transport_not_stopped";
    case NativeMixBoundaryReason::NativeCriticalSectionMismatch:return "native_critical_section_mismatch";
    case NativeMixBoundaryReason::NativeThreadManagerMismatch:return "native_fpc_thread_manager_mismatch";
    case NativeMixBoundaryReason::WorkerAllocationFailed:return "worker_allocation_failed";
    case NativeMixBoundaryReason::WorkerCreateFailed:return "worker_create_failed";
    case NativeMixBoundaryReason::WorkerJoinFailed:return "worker_join_failed";
    case NativeMixBoundaryReason::Contended:return "native_mutex_contended";
    case NativeMixBoundaryReason::TryLockFailed:return "native_mutex_trylock_failed";
    case NativeMixBoundaryReason::WorkerUnlockFailed:return "native_mutex_worker_unlock_failed";
    case NativeMixBoundaryReason::NativeIdentityChanged:return "native_identity_changed";
    }
    return "unknown";
}
}
