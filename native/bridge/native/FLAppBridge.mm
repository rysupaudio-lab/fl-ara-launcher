// Owned-copy normal FL application bridge. No vendor binaries are redistributed.
// Every model operation is stopped, component-process gated and render-drained.
#include "FLNativeApplication.h"
#include "FLVST3NativeHooks.h"
#include "FLStandaloneSourceOverlay.h"
#include "FLNativeProcessObserver.h"
#include "FLNativeMixBoundary.h"
#include "FLNativeEditorSetupCachePolicy.h"
#include "FLNativeSlotPowerObserver.h"
#include "FLHeadlessNativeExport.h"
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include "FLNativeSaveGuard.h"
#include "RegistrationReadThrough.h"
#include "../host/FLStandaloneGraphDecoder.h"
#include "../host/FLStandaloneStateEnvelope.h"
#include <ARA_API/ARAVST3.h>
#import <AppKit/AppKit.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>

#ifndef FL_APP_DECODER_SHA256
#error FL_APP_DECODER_SHA256 must seal the exact compiled decoder source
#endif
namespace rysup::fl_ara::app_bridge {
namespace lab=rysup::ara_lab;
namespace hooks=rysup::fl_ara::native_hooks;
using Steinberg::kResultOk;using Steinberg::kResultFalse;
void demand(bool okay,const char* reason){if(!okay)throw std::runtime_error(reason);}
struct ObservedNativeInstance;
struct NativeSetupDiagnostic {
    std::atomic<bool> ready{false};
    std::uint64_t sequence=0,epochBefore=0,epochAfter=0;
    Steinberg::Vst::ProcessSetup actualSetup{};Steinberg::tresult originalResult=0;
    bool mainThread=false,insideNativeProcess=false,dirtyBefore=false,dirtyAfter=false,qualifiedBefore=false,qualifiedAfter=false;
};
static_assert(sizeof(Steinberg::Vst::ProcessSetup)==24,"diagnostic must retain exact ProcessSetup POD layout");
struct NativeParameterEchoSnapshot {
    lab::OrdinaryParameterSnapshot parameters;lab::StandaloneCacheTicket ticket;
    ObservedNativeInstance* controllerNode=nullptr;Steinberg::Vst::IEditController* controller=nullptr;
    std::uintptr_t actualComponent=0;
};
struct BridgeRecord {
    hooks::InstanceRecord* native=nullptr;
    std::atomic<ObservedNativeInstance*> currentControllerNode{nullptr};
    NativeInsert insert;
    lab::StandaloneInstanceIdentity identity;
    std::atomic<BridgeRecord*> next{nullptr};
    std::atomic<bool> dirty{true},fault{false},alive{true},initialPrimePending{true},nativeStopForRebuildPending{false};
    std::atomic<std::uint64_t> nativeNormalizedParameterNotifications{0},nativeHandlerParameterNotifications{0},nativeInputQueueParameterNotifications{0};
    std::atomic<std::uint64_t> nativeStopRequestedEpoch{0};std::uint64_t actualNativeStopsVerified=0;bool initialWaitNoticeShown=false;
    NativeMixBoundary lastNativeMixBoundary,lifecycleBindingMixDiagnostic;bool modelMixBoundaryAvailable=false;
    bool disabled=false,boundary=false;std::atomic<bool> active{false},processing{false},observedInactive{false},observedStopped{false};
    std::atomic<bool> setupQualified{false};bool graphApplied=false,teardown=false,pendingPlainLoad=false,synchronousTerminating=false,teardownCallerVerified=false,actualNativeSaveMetadataCapture=false;
    std::atomic<std::uint64_t> nativeSetupDiagnosticCount{0};std::atomic<std::uint32_t> nativeSetupDiagnosticSlots{0};std::array<NativeSetupDiagnostic,32> nativeSetupHistory;
    std::atomic<double> sampleRate{0};std::string error,appliedGraph,appliedModel,lastNativeStateCaller;
    std::optional<lab::FLStandaloneDecodedGraph> decoded;
    std::shared_ptr<const lab::FLNativeModelSnapshot> copied;
    std::shared_ptr<lab::StandaloneCacheGate> nativeCacheGate;
    std::unique_ptr<process_observer::NativeProcessObserver> nativeProcessObserver;
    // Storage changes only while this exact component admission is closed and
    // fully drained; every RT snapshot reader is inside that admission lease.
    std::unique_ptr<const NativeParameterEchoSnapshot> parameterEchoStorage;
    std::atomic<const NativeParameterEchoSnapshot*> parameterEchoSnapshot{nullptr};
    std::atomic<std::uint64_t> nativeInputQueueExactEchoes{0};
    std::atomic<slot_power::Binding*> nativeSlotPower{nullptr};
    std::uint32_t nativeSlotPowerRegistryIndex=std::numeric_limits<std::uint32_t>::max();
    bool nativeSlotPowerRegistered=false,nativeSlotPowerSeedAttempted=false,nativeSlotPowerKnownOnce=false,nativeSlotPowerSourcePublished=false;
    std::uint64_t nativeSlotPowerSeedSuccesses=0,nativeSlotPowerSeedRefusals=0,nativeSlotPowerSeedSkippedNotifications=0,nativeSlotPowerStopsVerified=0;
    std::optional<lab::FLStandaloneOverlayStaleHoldRecord> nativeSlotPowerStopPending;
    bool staticSignatureChecked=false,staticSignatureValid=false;
    std::int32_t staticSignatureStatus=0;
    std::string pluginBundlePath,factoryEntryImagePath,factoryEntryImageSHA256;
    std::optional<lab::PreparedEditorArchive> preparedNativeSave;
    std::atomic<std::uint64_t> nativeDirtyPendingRevision{0};
    std::uint64_t nativeDirtyRequestedRevision=0,nativeDirtyRequestsReturned=0;bool nativeDirtyRetryAfterSaveRefusal=false;
    std::optional<lab::StandaloneSavedEditorArchive> pendingArchive;
    std::optional<lab::Bytes> pendingOrdinaryComponentState;
    lab::StandaloneNativeStateLoadAuthority pendingLoad;
    std::uint64_t nativeLoadSerial=0;
};
struct ObservedNativeInstance {
    hooks::InstanceRecord* native=nullptr;std::atomic<ObservedNativeInstance*> next{nullptr};
    std::atomic<BridgeRecord*> controllerOwner{nullptr};
    std::atomic<Steinberg::Vst::IEditController*> verifiedController{nullptr};
};
void publishControllerAssociationOnMain(hooks::InstanceRecord&)noexcept;
void invalidateAssociatedARACache(BridgeRecord&)noexcept;
void publishAllControllerAssociationsOnMain()noexcept;
struct FrozenNativeSaveComponent {
    BridgeRecord* record=nullptr;bool previousProcessAllowed=false;
    std::optional<lab::PreparedEditorArchive> prepared;
    std::shared_ptr<const lab::StandaloneSavedEditorArchive> archive;
    lab::StandaloneSnapshotData actualPreflight;
    std::uint64_t serializedCalls=0,ordinaryBytes=0,envelopeBytes=0;
};
struct NativeSaveFreeze {
    NativeSaveRequest request;
    std::vector<FrozenNativeSaveComponent> components;
    bool preflightPassed=false,precommitPassed=false;
};
constexpr std::uint32_t protectedUnboundArchive=1u,genuineARAEntryObserved=2u,nativeObservedStateDirtyPending=4u;
struct Runtime;
Runtime& runtime();
struct Adapter final:lab::StandaloneOwnerAdapter,lab::StandaloneArchiveSourceValidator {
    bool onActualModelThread()const noexcept override{return pthread_main_np()!=0;}
    lab::StandaloneNativeHold verifyActualNativeHold(const lab::StandaloneInstanceIdentity&)const override;
    lab::StandaloneNativeTeardownBoundary verifyActualNativeTeardownBoundary(const lab::StandaloneInstanceIdentity&)const override;
    lab::StandaloneSnapshotData copyActualCurrentRAMAndClock(const lab::StandaloneInstanceIdentity&)override;
    void invalidateActualNativeOverlayImmediately(const std::string&,std::uint64_t)noexcept override;
    void enqueueModelThreadRebuild(const std::string&)noexcept override;
    void queueActualNativeStopForCacheRebuild(const std::string&,std::uint64_t)noexcept override;
    void queueActualNativeDocumentDirtyFromARA(const std::string&,std::uint64_t)noexcept override;
    void showARAUnavailableForInsert(const std::string&,const std::string&)noexcept override;
    lab::StandaloneArchiveRestoreValidation validateActualCurrentRAMForArchive(
        const lab::StandaloneSavedEditorArchive&,const lab::StandaloneInstanceIdentity&)override;
};
struct Runtime {
    std::shared_ptr<Adapter> adapter;
    std::unique_ptr<lab::StandaloneARACoordinator> coordinator;
    std::unique_ptr<lab::FLStandaloneSourceOverlay> overlay;
    slot_power::Registry<32> nativeSlotPowerRegistry;
    std::array<slot_power::Binding*,32> nativeSlotPowerSlots{};
    std::vector<std::unique_ptr<slot_power::Binding>> nativeSlotPowerStorage;
    std::uint64_t nextNativeSlotPowerGeneration=1;std::uintptr_t nativeSlotPowerWrapperImageBase=0;
    bool nativeSlotPowerInstallBoundary=false,nativeSlotPowerStopNoticeQueued=false;
    std::atomic<lab::FLStandaloneSourceOverlay*> overlayListener{nullptr};
    std::atomic<std::uint64_t> unsupportedOffMainARAInitializations{0},unboundArchiveRefusals{0};
    std::atomic<BridgeRecord*> head{nullptr};
    std::atomic<ObservedNativeInstance*> observedNativeInstances{nullptr};
    std::atomic<bool> nativeInstanceTrackingIncomplete{false};
    std::unique_ptr<NativeSaveFreeze> nativeSaveFreeze;
    bool nativeSaveGuardInitialized=false,nativeSaveAlertQueued=false;
    std::uint64_t separateControllerAssociationsVerified=0,nativeObservedStateDirtyRequestsReturned=0;
    std::uint64_t nativeSavePreflights=0,nativeSavePreflightsAllowed=0,nativeSavePrecommitsAllowed=0,nativeSaveFramesFinished=0,nativeSaveEnvelopes=0;
    std::string nativeSaveError;
    std::vector<std::unique_ptr<BridgeRecord>> records;
    std::atomic<bool> overlayFault{false},overlayStaleHoldQueued{false};bool nativeStopNoticeQueued=false;
    bool hooksInstalled=false,overlayInstalled=false,inTick=false,anyBoundary=false,observeNativeDSP=false,nativeReadyStatusPublished=false;
    std::string error;std::uint64_t ticks=0;
    BridgeRecord* byID(const std::string& id)const noexcept{
        for(auto* r=head.load();r;r=r->next.load())if(r->identity.instanceID==id)return r;return nullptr;
    }
};
// Intentionally process lifetime: native record callbacks may outlive app teardown.
Runtime& runtime(){static auto* r=new Runtime;return *r;}
BridgeRecord* record(hooks::InstanceRecord& n)noexcept{return static_cast<BridgeRecord*>(n.ownerData.load());}
bool nativeSlotPowerInstallationBoundary(void*)noexcept{
    return pthread_main_np()&&nativeApplicationAdmitted()&&runtime().nativeSlotPowerInstallBoundary;
}
bool nativeSlotPowerObserverReady()noexcept{const auto state=slot_power::nativeSlotPowerObserverStats();return state.installed&&state.protectionRestored;}
std::uintptr_t loadedOwnedWrapperImageBase(){
    const auto& path=nativeApplication().wrapperPath;std::uintptr_t found=0;
    for(std::uint32_t i=0;i<_dyld_image_count();++i){const char* name=_dyld_get_image_name(i);char canonical[4096];
        if(!name||!realpath(name,canonical)||path!=canonical)continue;
        const auto base=reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(i));if(found&&found!=base)return 0;found=base;
    }return found;
}
void observeLoadedNativeSlotPower(const hooks::ThreadInfo& info)noexcept{
    if(!info.isMainThread||info.insideNativeProcess||!nativeApplicationAdmitted()||currentNativeSaveTransaction())return;
    auto& rt=runtime();const auto stats=slot_power::nativeSlotPowerObserverStats();
    if(stats.installed)return; // A published restore failure remains blocked, never reinstalled/unpinned.
    try{const auto base=loadedOwnedWrapperImageBase();if(!base)return;
        struct Scope{Runtime& rt;explicit Scope(Runtime& value):rt(value){rt.nativeSlotPowerInstallBoundary=true;}~Scope(){rt.nativeSlotPowerInstallBoundary=false;}}scope(rt);
        slot_power::ObserverConfig config;config.registry=&rt.nativeSlotPowerRegistry;config.exactLoadedWrapperPath=nativeApplication().wrapperPath;
        config.actualWrapperImageBase=base;config.confirmAdmittedModelBoundary=nativeSlotPowerInstallationBoundary;
        const auto result=slot_power::installLoadedNativeSlotPowerObserver(config);
        if(result.installed&&result.protectionRestored)rt.nativeSlotPowerWrapperImageBase=base;
    }catch(...){/* The actual bind/publication paths require ready and report failure. */}
}
void nativeModuleObserved(const hooks::ModuleRecord&,const hooks::ThreadInfo& info,void*)noexcept{observeLoadedNativeSlotPower(info);}
void nativeFactoryObserved(hooks::FactoryRecord&,const hooks::ThreadInfo& info,void*)noexcept{observeLoadedNativeSlotPower(info);}
void nativeInterfaceObserved(hooks::InstanceRecord&,const hooks::ClassID&,void*,const hooks::ImageEvidence&,const hooks::ThreadInfo& info,void*)noexcept{observeLoadedNativeSlotPower(info);}
void retireNativeSlotPower(BridgeRecord& r)noexcept{if(auto* binding=r.nativeSlotPower.load(std::memory_order_acquire))binding->closePermanently();}
void finalNativeRelease(hooks::InstanceRecord& n,const hooks::ThreadInfo&,void*)noexcept{if(auto* r=record(n))retireNativeSlotPower(*r);}
void allocateNativeSlotPower(BridgeRecord& r){
    if(r.nativeSlotPower.load())return;auto& rt=runtime();
    demand(pthread_main_np()&&nativeApplicationAdmitted()&&nativeSlotPowerObserverReady(),"native slot-power observer is not safely installed");
    demand(r.native&&r.native->ownerData.load()==&r&&r.native->nativeAlive&&!r.native->nativeTerminated&&r.alive&&!r.disabled&&!r.teardown
        &&r.native->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(r.identity.actualComponent)&&!r.identity.instanceID.empty(),"native slot-power requires a genuine live bound component");
    std::size_t index=rt.nativeSlotPowerSlots.size();for(std::size_t i=0;i<rt.nativeSlotPowerSlots.size();++i)if(!rt.nativeSlotPowerSlots[i]||rt.nativeSlotPowerSlots[i]->closed()){index=i;break;}
    demand(index<rt.nativeSlotPowerSlots.size()&&rt.nextNativeSlotPowerGeneration<std::numeric_limits<std::uint64_t>::max(),"native slot-power binding capacity/generation exhausted");
    auto owned=std::make_unique<slot_power::Binding>(r.insert.effectWrapper,r.identity.actualComponent,rt.nextNativeSlotPowerGeneration++);auto* value=owned.get();
    rt.nativeSlotPowerStorage.push_back(std::move(owned));rt.nativeSlotPowerSlots[index]=value;r.nativeSlotPowerRegistryIndex=static_cast<std::uint32_t>(index);r.nativeSlotPower.store(value,std::memory_order_release);
}
template<class T>bool readNativePowerOffset(std::uintptr_t base,std::uintptr_t offset,T& value)noexcept{
    if(!base||base>UINTPTR_MAX-offset||base+offset>UINTPTR_MAX-sizeof(T))return false;mach_vm_size_t bytes=0;
    return mach_vm_read_overwrite(mach_task_self(),base+offset,sizeof(T),reinterpret_cast<mach_vm_address_t>(&value),&bytes)==KERN_SUCCESS&&bytes==sizeof(T);
}
bool nativeSlotPowerAssociationCurrent(const BridgeRecord& r)noexcept{
    std::uintptr_t plugin=0;return revalidateActualNativeInsert(r.insert)&&readNativePowerOffset(r.insert.effectWrapper,0x50a8,plugin)&&plugin==r.insert.plugin;
}
struct NativePowerWitness{std::uint8_t effectiveTag8a=2;};
bool readNativePowerWitness(const BridgeRecord& r,NativePowerWitness& value)noexcept{
    return nativeSlotPowerAssociationCurrent(r)&&readNativePowerOffset(r.insert.tag,0x8a,value.effectiveTag8a);
}
// ONLY inside the genuine stopped/drained HostLockMix copy callback. Adoption
// proof precedes registry publication. Seed evidence is then freshly repeated
// AFTER the token, with all actual ID11 notifications already observable.
void observeNativeSlotPowerAtActualMixLock(BridgeRecord& r){
    auto* binding=r.nativeSlotPower.load();demand(binding&&!binding->closed()&&binding->wrapper==r.insert.effectWrapper&&binding->actualComponent==r.identity.actualComponent,"native slot-power generation is closed or mismatched");
    if(!r.nativeSlotPowerRegistered){
        if(!nativeSlotPowerAssociationCurrent(r)){retireNativeSlotPower(r);throw std::runtime_error("native slot-power adoption owner/backlink changed");}
        demand(runtime().nativeSlotPowerRegistry.publishAtModelBoundary(r.nativeSlotPowerRegistryIndex,*binding),"native slot-power registry refused exact generation");r.nativeSlotPowerRegistered=true;
    }
    const auto current=binding->snapshotForBlock();if(current!=slot_power::Power::Unknown){
        if(!r.nativeSlotPowerSeedAttempted)++r.nativeSlotPowerSeedSkippedNotifications;r.nativeSlotPowerSeedAttempted=true;r.nativeSlotPowerKnownOnce=true;return;
    }
    if(r.nativeSlotPowerSeedAttempted)return;
    r.nativeSlotPowerSeedAttempted=true;const auto token=binding->captureInitialSeedAtStoppedBoundary();
    NativePowerWitness first,second;
    if(!readNativePowerWitness(r,first)||!readNativePowerWitness(r,second)){retireNativeSlotPower(r);throw std::runtime_error("native slot-power seed repeated owner/backlink read failed");}
    if(binding->snapshotForBlock()!=slot_power::Power::Unknown||binding->callsInFlight()){
        ++r.nativeSlotPowerSeedSkippedNotifications;r.nativeSlotPowerKnownOnce=binding->snapshotForBlock()!=slot_power::Power::Unknown;return;
    }
    const bool valid=first.effectiveTag8a<=1&&second.effectiveTag8a==first.effectiveTag8a;
    if(binding->trySeedAtStoppedBoundary(token,valid?first.effectiveTag8a:2)){++r.nativeSlotPowerSeedSuccesses;r.nativeSlotPowerKnownOnce=true;}
    else ++r.nativeSlotPowerSeedRefusals;
}

bool admitted(void*)noexcept{return nativeApplicationAdmitted();}
bool stopped(){std::int32_t value=-1;return readActualNativeTransportState(value)&&value==0;}
bool allBoundary(void*)noexcept{return runtime().anyBoundary&&pthread_main_np()&&stopped();}
class NativeMixBoundaryDeferred final:public lab::NativeModelBoundaryUnavailable {public:using lab::NativeModelBoundaryUnavailable::NativeModelBoundaryUnavailable;};
void observeAndRequireNativeMixBoundary(BridgeRecord& r){
    r.modelMixBoundaryAvailable=false;r.lastNativeMixBoundary=observeActualNativeMixBoundary();
    if(!r.lastNativeMixBoundary.available()){
        throw NativeMixBoundaryDeferred(nativeMixBoundaryReason(r.lastNativeMixBoundary.reason));
    }
    r.modelMixBoundaryAvailable=true;
}
bool hasLiveProtectedNativeArchive()noexcept{
    if(runtime().nativeInstanceTrackingIncomplete.load())return true;
    for(auto* p=runtime().observedNativeInstances.load();p;p=p->next.load())if(p->native->nativeAlive.load()&&!p->native->nativeTerminated.load()
        &&(p->native->ownerStateFlags.load()&protectedUnboundArchive))return true;
    return false;
}
bool hasLiveNativeARA(void*)noexcept{
    // Called afresh at each native file boundary. Every input is atomic and
    // every node has process lifetime; no main-thread false snapshot can erase
    // a concurrent native ARA/envelope observation.
    if(runtime().nativeInstanceTrackingIncomplete.load())return true;
    for(auto* p=runtime().observedNativeInstances.load();p;p=p->next.load())if(p->native->nativeAlive.load()&&!p->native->nativeTerminated.load()
        &&(p->native->ownerStateFlags.load()&(protectedUnboundArchive|genuineARAEntryObserved)))return true;
    return false;
}
void publishNativeARAObservation(hooks::InstanceRecord& n,std::uint32_t flag)noexcept{
    const bool saveWasActive=hasActiveNativeSaveFromAnyThread();n.ownerStateFlags.fetch_or(flag);
    const bool failedBeforeCommit=markActiveNativeSaveFailedFromAnyThread();
    // An already committing Save cannot be vetoed without stranding its target.
    // Queue actual unsaved state only around an active Save, not ordinary cold
    // loading. Actual ARA edit revisions have their separate dirty mailbox.
    if(saveWasActive||failedBeforeCommit||hasActiveNativeSaveFromAnyThread())n.ownerStateFlags.fetch_or(nativeObservedStateDirtyPending);
}
void observeGenuineARA(hooks::InstanceRecord& n)noexcept{publishNativeARAObservation(n,genuineARAEntryObserved);}
void preserveUnboundARA(hooks::InstanceRecord& n)noexcept{publishNativeARAObservation(n,protectedUnboundArchive);runtime().unboundArchiveRefusals.fetch_add(1);}
struct Boundary {
    BridgeRecord& r;bool previous=false;
    explicit Boundary(BridgeRecord& value):r(value){
        demand(pthread_main_np()&&nativeApplicationAdmitted()&&stopped(),"actual normal FL owner must be stopped on main thread");
        demand(!r.boundary&&r.alive&&!r.teardown&&revalidateActualNativeInsert(r.insert),"native insert boundary owner is unavailable/reentrant");
        previous=r.native->nativeProcessAdmission.exchangeAllowed(false);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(r.native->nativeProcessAdmission.activeCount()){
            if(std::chrono::steady_clock::now()>deadline){r.native->nativeProcessAdmission.setAllowed(previous);throw std::runtime_error("actual native process drain timed out");}
            std::this_thread::yield();
        }
        try{observeAndRequireNativeMixBoundary(r);}catch(...){r.native->nativeProcessAdmission.setAllowed(!r.initialPrimePending.load()&&previous);throw;}
        r.boundary=true;runtime().anyBoundary=true;
    }
    ~Boundary(){r.boundary=false;r.modelMixBoundaryAvailable=false;runtime().anyBoundary=false;
        r.native->nativeProcessAdmission.setAllowed(!r.initialPrimePending.load()&&r.alive.load()&&!r.teardown);}

};
struct TeardownBoundary {
    BridgeRecord& r;
    explicit TeardownBoundary(BridgeRecord& value,bool actualCallerVerified):r(value){
        demand(pthread_main_np()&&nativeApplicationAdmitted()&&actualCallerVerified,"actual native pre-terminate caller/main/run proof missing");
        demand(!r.boundary&&!r.synchronousTerminating&&r.alive&&!r.teardown&&!r.active&&!r.processing
            &&r.observedInactive&&r.observedStopped,"native component has not actually stopped/deactivated before its original terminate");
        demand(r.native&&r.native->ownerData.load()==&r&&r.native->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(r.identity.actualComponent)
            &&r.native->nativeAlive&&r.native->nativeInitialized&&!r.native->nativeTerminated,"exact known native component/lifecycle identity missing");
        r.native->nativeProcessAdmission.shutdown();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(r.native->nativeProcessAdmission.activeCount()){
            demand(std::chrono::steady_clock::now()<=deadline,"actual native process drain timed out before original terminate");std::this_thread::yield();
        }
        r.teardownCallerVerified=true;r.synchronousTerminating=true;
    }
    ~TeardownBoundary(){r.synchronousTerminating=false;r.teardownCallerVerified=false;}
};
lab::FLNativeModelOptions modelOptions(std::int32_t actualMixerInsert){
    lab::FLNativeModelOptions options;options.captureAllAudioChannels=true;options.copyQualifiedPCMOnlyForMixerInsert=actualMixerInsert;options.copyQualifiedPCM=true;
    options.requireQualifiedPCMForEveryAudioSource=false;options.requireSupportedMixerInsertForEveryAudioChannel=true;
    // Exact mappings qualified in the sealed FL 26.1.7 native RAM experiment.
    // No other encoding/reference combination may become source audio.
    options.pcmQualifications={
        {1,0,lab::FLNativeC8Presence::Zero,lab::FLNativePCMEncoding::SignedInt16LE,1,true,"native-cold-qualification/qualification-summary.json:mono-actual-PCM"},
        {2,0,lab::FLNativeC8Presence::Zero,lab::FLNativePCMEncoding::SignedInt16LE,2,true,"native-cold-qualification/qualification-summary.json:stereo-actual-PCM"}};
    return options;
}
lab::StandaloneNativeHold Adapter::verifyActualNativeHold(const lab::StandaloneInstanceIdentity& id)const{
    auto* r=runtime().byID(id.instanceID);lab::StandaloneNativeHold hold;
    if(!r)return hold;
    const auto admission=r->native->nativeProcessAdmission.snapshot();
    hold.actualOwnedNativeHold=nativeApplicationAdmitted()&&r->boundary&&r->modelMixBoundaryAvailable&&!admission.allowed;
    hold.actualNativeStopped=stopped();hold.outsideNativeRenderStateAndLifecycleCalls=r->boundary;
    hold.actualOwnerAndCurrentModelThreadVerified=pthread_main_np()&&r->alive&&revalidateActualNativeInsert(r->insert)
        &&id.actualNativeEngineOwner==nativeApplication().engineInstance;
    hold.actualNativeRenderCallsInFlight=admission.activeCount;return hold;
}
lab::StandaloneNativeTeardownBoundary Adapter::verifyActualNativeTeardownBoundary(const lab::StandaloneInstanceIdentity& id)const{
    lab::StandaloneNativeTeardownBoundary proof;auto* r=runtime().byID(id.instanceID);if(!r)return proof;
    proof.actualComponent=id.actualComponent;
    proof.exactKnownOwnedComponentVerified=nativeApplicationAdmitted()&&r->alive&&r->native&&r->native->ownerData.load()==r
        &&r->native->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(id.actualComponent)
        &&r->identity.actualComponent==id.actualComponent&&r->identity.ownProcessID==getpid()&&r->native->nativeAlive;
    proof.insideSynchronousNativeBeforeOriginalTerminate=r->synchronousTerminating&&r->teardownCallerVerified;
    proof.onActualModelThread=pthread_main_np();proof.actualOriginalSetActiveFalseObserved=r->observedInactive&&!r->active;
    proof.actualOriginalSetProcessingFalseObserved=r->observedStopped&&!r->processing;
    const auto admission=r->native->nativeProcessAdmission.snapshot();
    proof.ownNativeProcessGateClosed=!admission.allowed;proof.actualOriginalProcessCallsInFlight=admission.activeCount;
    proof.nativeOriginalTerminateNotYetCalled=r->synchronousTerminating&&!r->native->nativeTerminated;return proof;
}
lab::StandaloneSnapshotData captureActualNativeSnapshotData(BridgeRecord& target,const lab::StandaloneInstanceIdentity& identity,bool metadataOnly){
    auto* r=&target;const auto admission=r->native->nativeProcessAdmission.snapshot();
    demand(!headlessNativeExportInFlight(),"native render owns the model boundary");
    demand(pthread_main_np()&&nativeApplicationAdmitted()&&stopped()&&r->alive&&!r->teardown
        &&!admission.allowed&&admission.activeCount==0,
        "actual native metadata capture requires stopped/drained owned component");
    demand(metadataOnly?r->actualNativeSaveMetadataCapture:(r->boundary&&r->modelMixBoundaryAvailable),
        "current RAM copy needs its actual model or guarded native-save metadata boundary");
    demand(r->setupQualified&&r->sampleRate==48000,"actual native setupProcessing has not established the supported 48k profile");
    lab::FLStandaloneComponentAssociation association;association.actualComponent=identity.actualComponent;
    association.nativeOwnerObject=nativeApplication().engineInstance;association.mixerInsert=r->insert.mixerInsert;
    association.exactObjectAssociationEvidence=r->insert.evidence;association.nativePointerAssociationVerified=revalidateActualNativeInsert(r->insert);
    allocateNativeSlotPower(*r);
    const auto engineSeal=lab::FLStandaloneGraphDecoder::verifyActualLoadedEngine(nativeApplication().engine);
    std::optional<lab::CapturedFLStandaloneModel> capture;
    const bool locked=withActualNativeMixCaptureLock(r->insert,[&]{
        observeNativeSlotPowerAtActualMixLock(*r);
        capture=lab::FLStandaloneGraphDecoder::captureActualCurrentRAMAtNativeLock(engineSeal,modelOptions(r->insert.mixerInsert),association);
    });
    demand(locked&&capture.has_value(),"actual native mix-lock RAM capture failed");
    if(!metadataOnly)observeAndRequireNativeMixBoundary(*r);
    // All ARA/provider work happens AFTER native HostLockMix is released.
    auto decoded=lab::FLStandaloneGraphDecoder::decode(*capture,FL_APP_DECODER_SHA256);
    const auto& copied=capture->model();
    lab::StandaloneSnapshotData data;data.actual=identity;data.graph=decoded.graph;
    data.resolveCurrentImmutableRAMSource=decoded.resolveCurrentImmutableRAMSource;
    auto& m=data.model;m.nativeProjectIdentity=copied.currentArrangementRuntimeID;
    m.nativeEngineSHA256=nativeApplication().engine.verifiedArm64SliceSHA256;
    m.nativeRecordModelSHA256=decoded.nativeRecordModelSHA256;m.sourcePCMModelSHA256=decoded.sourcePCMModelSHA256;
    m.graphSHA256=decoded.graphSHA256;m.nativeSaveVersion=decoded.nativeSaveVersion;
    m.clock.ppq=decoded.clock.ppq;m.clock.tempoMilliBPM=decoded.clock.tempoMilliBPM;m.clock.sampleRate=r->sampleRate.load();
    m.clock.actualNativePlaying=false;m.clock.clockFormatIdentity="fl26.1.7:ppq-u16le+tempo-millibpm-u32le";
    m.clock.actualNativeClockBytes.assign(decoded.clock.actualClockBytes.begin(),decoded.clock.actualClockBytes.end());
    for(const auto& s:decoded.sources)data.sources.push_back({s.sourceID,s.nativePhysicalSourceKey,s.actualPCM_SHA256});
    for(const auto& s:decoded.modifications)data.modifications.push_back({s.modificationID,s.nativePhysicalEditKey,s.sourceID});
    for(const auto& s:decoded.regions)data.regions.push_back({s.regionID,s.nativePhysicalRegionKey,s.modificationID});
    data.actualCopiedNativeModelRecords=decoded.actualCopiedNativeModelRecords;
    for(const auto& clip:copied.clips)if(clip.kind==lab::FLNativeRecordKind::AudioChannel)data.actualVerifiedAudioRecordIndices.push_back(clip.nativeListIndex);
    data.decoderIdentity=decoded.decoderIdentity;data.decoderSourceSHA256=decoded.decoderSourceSHA256;
    data.actualCurrentNativeRAMCopiedAndVerified=decoded.actualCurrentNativeRAMCopiedAndVerified;
    data.actualDirectNativeClockReadTwiceAndEqual=decoded.clock.actualDirectNativeReadTwiceAndEqual;
    data.nativeComponentOwnerInsertAssociationVerified=association.nativePointerAssociationVerified;
    data.noNativeSerializationOrFileFallbackUsed=decoded.noNativeSerializationOrFileAudioFallbackUsed;
    if(!metadataOnly){r->copied=std::make_shared<const lab::FLNativeModelSnapshot>(copied);r->decoded=std::move(decoded);}
    return data;
}
lab::StandaloneSnapshotData Adapter::copyActualCurrentRAMAndClock(const lab::StandaloneInstanceIdentity& identity){
    auto* r=runtime().byID(identity.instanceID);demand(r&&r->boundary,"current RAM copy needs real native model boundary");
    return captureActualNativeSnapshotData(*r,identity,false);
}
void Adapter::invalidateActualNativeOverlayImmediately(const std::string& id,std::uint64_t)noexcept{
    markActiveNativeSaveFailedFromAnyThread();
    // Authority-only callback. Actual edit requests use the separate enqueue
    // mailbox; current-build maintenance must not make its own insert dirty.
    if(auto* overlay=runtime().overlayListener.load(std::memory_order_acquire))overlay->invalidateInstance(id);
}
void Adapter::enqueueModelThreadRebuild(const std::string& id)noexcept{if(auto* r=runtime().byID(id))r->dirty=true;}
void queueNativeStopForCacheRebuild(BridgeRecord& r,std::uint64_t epoch)noexcept{
    auto old=r.nativeStopRequestedEpoch.load();while(old<epoch&&!r.nativeStopRequestedEpoch.compare_exchange_weak(old,epoch)){}
    r.nativeStopForRebuildPending=true;
}
void Adapter::queueActualNativeStopForCacheRebuild(const std::string& id,std::uint64_t epoch)noexcept{
    if(auto* r=runtime().byID(id))queueNativeStopForCacheRebuild(*r,epoch);
}
bool currentNativeCacheGenerationCoherent(const BridgeRecord& r)noexcept{
    return r.alive&&!r.disabled&&!r.fault&&!r.teardown&&r.setupQualified&&r.graphApplied&&!r.dirty&&!r.initialPrimePending&&!r.pendingArchive
        &&r.preparedNativeSave&&r.nativeCacheGate&&r.nativeCacheGate->authorizes(r.preparedNativeSave->identity().currentCacheTicket);
}

void Adapter::queueActualNativeDocumentDirtyFromARA(const std::string& id,std::uint64_t revision)noexcept{
    if(!revision)return;markActiveNativeSaveFailedFromAnyThread();
    if(auto* r=runtime().byID(id)){auto old=r->nativeDirtyPendingRevision.load();
        while(old<revision&&!r->nativeDirtyPendingRevision.compare_exchange_weak(old,revision)){}
    }
}
void Adapter::showARAUnavailableForInsert(const std::string& id,const std::string&)noexcept{
    markActiveNativeSaveFailedFromAnyThread();
    if(auto* r=runtime().byID(id)){if(r->nativeCacheGate)r->nativeCacheGate->invalidate();r->dirty=true;r->fault=true;}
}
lab::StandaloneArchiveRestoreValidation Adapter::validateActualCurrentRAMForArchive(
    const lab::StandaloneSavedEditorArchive& archive,const lab::StandaloneInstanceIdentity& id){
    auto* r=runtime().byID(id.instanceID);demand(r&&r->pendingLoad.actualNativeStateLoadCallOrAttestedDeferredWindow,"archive needs actual native setState/load authority");
    auto now=copyActualCurrentRAMAndClock(id);auto checked=lab::validateFLStandaloneRestoreGraph(archive,now);
    lab::StandaloneArchiveRestoreValidation result;result.current=std::move(checked.current);result.load=r->pendingLoad;
    result.objectIDRelocationVerified=checked.objectIDRelocationVerified;result.verifiedOldIncomingNativeModel=checked.verifiedOldIncomingNativeModel;
    result.objectIDs=std::move(checked.objectIDs);return result;
}
bool sealedNativeCaller(const hooks::ThreadInfo& info){
    if(!info.isMainThread||info.insideNativeProcess||!info.actualNativeCallerAddress)return false;
    Dl_info image{};if(!dladdr((void*)info.actualNativeCallerAddress,&image)||!image.dli_fname)return false;
    char path[PATH_MAX];if(!realpath(image.dli_fname,path))return false;
    const auto& app=nativeApplication();return app.admitted&&(app.wrapperPath==path||app.enginePath==path);
}
std::string nativeCallerEvidence(const hooks::ThreadInfo& info){
    Dl_info image{};demand(sealedNativeCaller(info)&&dladdr((void*)info.actualNativeCallerAddress,&image),"actual native caller evidence unavailable");
    return std::string(image.dli_fname)+":offset="+std::to_string(info.actualNativeCallerAddress-reinterpret_cast<std::uintptr_t>(image.dli_fbase));
}
lab::StandaloneNativeStateLoadAuthority loadAuthority(BridgeRecord& r,const hooks::ThreadInfo& info){
    demand(sealedNativeCaller(info)&&revalidateActualNativeInsert(r.insert),"actual native state caller/insert seal missing");
    r.lastNativeStateCaller=nativeCallerEvidence(info);writeNativeApplicationEvent("actual_native_component_setState_authority",r.identity.instanceID+" "+r.lastNativeStateCaller);
    lab::StandaloneNativeStateLoadAuthority load;load.actualNativeEngineOwner=nativeApplication().engineInstance;
    load.actualComponent=r.identity.actualComponent;load.nativeInsertKey=r.identity.nativeInsertKey;
    load.actualNativeLoadEventIdentity=r.identity.instanceID+":actual-setState:"+std::to_string(++r.nativeLoadSerial);
    load.actualNativeStateLoadCallOrAttestedDeferredWindow=true;load.exactSealedNativeCallerAndOwnerVerified=true;
    load.actualCurrentNativeLoadArgumentAcceptedAndUnconsumed=true;return load;
}
void fail(BridgeRecord& r,const std::string& reason)noexcept{
    if(!pthread_main_np()){r.fault=true;return;}
    try{retireNativeSlotPower(r);r.error=reason;r.disabled=true;r.dirty=false;r.native->nativeProcessAdmission.shutdown();if(runtime().overlay)runtime().overlay->invalidateInstance(r.identity.instanceID);
        if(runtime().coordinator&&!r.identity.instanceID.empty())runtime().coordinator->disableARAForInsert(r.identity.instanceID,reason);
        writeNativeApplicationEvent("ara_insert_unavailable",reason);r.fault=true;
    }catch(...){r.fault=true;}
}
struct SignatureDiagnostic {bool checked=false,valid=false;OSStatus status=errSecParam;};
SignatureDiagnostic signatureDiagnostic(const hooks::ModuleRecord& module){
    SignatureDiagnostic diagnostic;if(!module.actualBundle)return diagnostic;
    CFURLRef url=CFBundleCopyBundleURL(module.actualBundle);if(!url)return diagnostic;
    SecStaticCodeRef code=nullptr;diagnostic.status=SecStaticCodeCreateWithPath(url,kSecCSDefaultFlags,&code);CFRelease(url);
    if(diagnostic.status!=errSecSuccess||!code)return diagnostic;
    diagnostic.checked=true;diagnostic.status=SecStaticCodeCheckValidity(code,kSecCSStrictValidate|kSecCSCheckAllArchitectures,nullptr);
    CFRelease(code);diagnostic.valid=diagnostic.status==errSecSuccess;return diagnostic;
}
std::optional<hooks::ClassID> araClassForActualComponent(hooks::InstanceRecord& n,Steinberg::Vst::IComponent& component){
    ARA::IPlugInEntryPoint* entry=nullptr;
    if(component.queryInterface(INLINE_UID_OF(ARA::IPlugInEntryPoint),reinterpret_cast<void**>(&entry))!=kResultOk||!entry)return{};
    struct Release{ARA::IPlugInEntryPoint* p;~Release(){p->release();}} release{entry};
    observeGenuineARA(n);
    const auto* actual=entry->getFactory();demand(actual&&actual->factoryID,"actual ARA component factory ID missing");
    auto* f=n.factory->actualFactory;std::optional<hooks::ClassID> found;
    for(Steinberg::int32 index=0;index<f->countClasses();++index){Steinberg::PClassInfo info{};
        demand(f->getClassInfo(index,&info)==kResultOk,"native factory class metadata failed");
        if(std::strcmp(info.category,kARAMainFactoryClass))continue;
        ARA::IMainFactory* provider=nullptr;if(f->createInstance(info.cid,INLINE_UID_OF(ARA::IMainFactory),reinterpret_cast<void**>(&provider))!=kResultOk||!provider)continue;
        const auto* candidate=provider->getFactory();const bool match=candidate&&candidate->factoryID&&!std::strcmp(candidate->factoryID,actual->factoryID);provider->release();
        if(match){demand(!found,"ambiguous independent ARA provider for actual component");hooks::ClassID id;std::memcpy(id.data(),info.cid,16);found=id;}
    }
    demand(found.has_value(),"actual same native factory lacks independent matching ARA provider");return found;
}
void initialized(hooks::InstanceRecord& n,void*,Steinberg::FUnknown* context,Steinberg::tresult result,const hooks::ThreadInfo& info,void*)noexcept{
    observeLoadedNativeSlotPower(info);
    if(result==kResultOk&&info.isMainThread&&!info.insideNativeProcess)publishControllerAssociationOnMain(n);
    if(n.createdKind!=hooks::CreatedKind::Component||result!=kResultOk||!nativeApplicationAdmitted()||!n.actualComponent.load())return;
    if(!info.isMainThread){
        // Capability detection stays on this ACTUAL native initialize thread.
        // No ARA factory/DC/bind or borrowed component is deferred to main.
        try{ARA::IPlugInEntryPoint* entry=nullptr;
            if(n.actualComponent.load()->queryInterface(INLINE_UID_OF(ARA::IPlugInEntryPoint),reinterpret_cast<void**>(&entry))==kResultOk&&entry){
                observeGenuineARA(n);entry->release();runtime().unsupportedOffMainARAInitializations.fetch_add(1);
            }
        }catch(...){runtime().unsupportedOffMainARAInitializations.fetch_add(1);}
        return;
    }
    try{
        demand(n.factory&&n.factory->actualFactory,"native component actual factory missing");
        auto* component=n.actualComponent.load();auto araClass=araClassForActualComponent(n,*component);if(!araClass)return;
        demand(nativeSaveGuardReady()&&!currentNativeSaveTransaction(),"native project-save preservation guard is unavailable or a save is active");
        demand(nativeSlotPowerObserverReady(),"native slot-power observer is unavailable; ARA source publication is refused");
        demand(n.factory&&n.factory->module&&!n.factory->moduleAssociationAmbiguous,"native factory provenance ambiguous");
        const auto& module=*n.factory->module;
        demand(module.actualBundle&&module.stableExecutableSHA256&&module.nativeCodeLeaseRetained&&module.actualFactoryEntryImage.resolved
            &&module.actualFactoryEntryImage.stableRegularFileSHA256,"actual native-loaded module/factory hash provenance or retained code lease missing");
        // Native FL/OS already admitted and initialized this exact actual module.
        // Static signature diagnostics are not an added host policy or licensing proof.
        const auto signature=signatureDiagnostic(module);
        demand(n.setStateCalls==0&&n.activationCalls==0&&n.processingCalls==0&&n.contextRequirementCalls==0&&n.editorViewCalls==0,"ARA native binding is too late");
        NativeInsert insert;demand(resolveActualNativeInsert(reinterpret_cast<std::uintptr_t>(component),reinterpret_cast<std::uintptr_t>(context),insert),"actual native component/mixer owner chain not established");
        auto& rt=runtime();for(auto& r:rt.records)if(r->alive&&r->insert.mixerInsert==insert.mixerInsert){fail(*r,"multiple ARA components on same native mixer insert are ambiguous");throw std::runtime_error("multiple ARA components on same native mixer insert refused");}
        auto owned=std::make_unique<BridgeRecord>();auto* r=owned.get();r->native=&n;r->insert=insert;
        // Empty document/factory/role binding is a distinct native lifecycle
        // scope. Observe outer-lock state honestly; do not unlock a foreign
        // lifecycle lock or claim this is the source/model capture boundary.
        r->lifecycleBindingMixDiagnostic=observeActualNativeMixBoundary();
        writeNativeApplicationEvent("actual_native_empty_ara_bind_mix_diagnostic",nativeMixBoundaryReason(r->lifecycleBindingMixDiagnostic.reason));
        r->staticSignatureChecked=signature.checked;r->staticSignatureValid=signature.valid;r->staticSignatureStatus=signature.status;
        r->pluginBundlePath=module.canonicalBundlePath;r->factoryEntryImagePath=module.actualFactoryEntryImage.canonicalPath;
        r->factoryEntryImageSHA256=module.actualFactoryEntryImage.executableSHA256;
        r->identity.ownRunID=nativeApplication().runID;r->identity.ownProcessID=getpid();r->identity.actualComponent=reinterpret_cast<std::uintptr_t>(component);
        r->identity.actualNativeEngineOwner=nativeApplication().engineInstance;r->identity.nativeInsertKey="fl:native-mixer-insert:"+std::to_string(insert.mixerInsert);
        r->identity.pluginModuleSHA256=module.executableSHA256;r->identity.nativeEngineSHA256=nativeApplication().engine.verifiedArm64SliceSHA256;
        lab::StandaloneVST3FactoryContext factory;factory.actualSameNativeFactory=n.factory->actualFactory;
        factory.audioClassID=n.actualRequestedClassID;factory.araClassID=*araClass;
        // Hooks retain actual bundle + RTLD_NOLOAD image lease for process life.
        factory.actualAlreadyLoadedModuleLifetime={const_cast<hooks::ModuleRecord*>(&module),[](void*){}};
        lab::StandaloneFreshNativeInitialization proof;proof.originalInitializeSucceeded=true;proof.actualNativeComponentFactoryAndOwnerVerified=true;
        proof.actualLoadedComponentAndFactoryImageVerified=true;proof.beforeFirstNativeStateActivationProcessingContextOrEditor=true;proof.nativeOwnerDoesNotAlreadyInitializeARA=true;
        proof.verifyActualBorrowedNativeFreshnessImmediatelyBeforeBinding=[actual=&n,expected=insert,expectedFactory=n.factory,expectedOwner=nativeApplication().engineInstance]{
            return pthread_main_np()&&nativeApplicationAdmitted()&&actual->nativeAlive.load()&&actual->nativeInitialized.load()&&!actual->nativeTerminated.load()
                &&actual->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(expected.actualComponent)
                &&actual->actualInitializeContext.load()==reinterpret_cast<Steinberg::FUnknown*>(expected.initializeContext)
                &&actual->factory==expectedFactory&&!expectedFactory->moduleAssociationAmbiguous.load()
                &&nativeApplication().engineInstance==expectedOwner&&actual->initializeCalls.load()==1&&actual->terminateCalls.load()==0
                &&actual->setStateCalls.load()==0&&actual->activationCalls.load()==0&&actual->processingCalls.load()==0
                &&actual->contextRequirementCalls.load()==0&&actual->editorViewCalls.load()==0&&actual->nativeProcessAdmission.activeCount()==0
                &&revalidateActualNativeInsert(expected);
        };
        r->identity.instanceID=rt.coordinator->bindAfterOriginalInitialize(*component,r->identity,factory,proof);
        r->nativeCacheGate=rt.coordinator->cacheGate(r->identity.instanceID);
        if(rt.observeNativeDSP)r->nativeProcessObserver=std::make_unique<process_observer::NativeProcessObserver>(n);
        n.nativeProcessAdmission.setAllowed(false); // No first original native process before an actual qualified initial prime.
        n.ownerData=r;n.stateInterceptionEnabled=true;r->next=rt.head.load();rt.head=r;rt.records.push_back(std::move(owned));
        publishAllControllerAssociationsOnMain();
        writeNativeApplicationEvent("actual_native_ara_component_bound",r->identity.instanceID);
    }catch(const std::exception& e){writeNativeApplicationEvent("actual_native_ara_bind_refused",e.what());runtime().error=e.what();}
    catch(...){writeNativeApplicationEvent("actual_native_ara_bind_refused","unknown native SDK exception");runtime().error="Unknown native SDK exception while binding ARA";}
}
void instanceCreated(hooks::InstanceRecord& n,const hooks::ThreadInfo& info,void*)noexcept{
    // Native records themselves have process lifetime. Tracking nodes never
    // dereference a released native interface and are never reclaimed on RT.
    if(info.insideNativeProcess){runtime().nativeInstanceTrackingIncomplete=true;}
    else if(auto* node=new(std::nothrow)ObservedNativeInstance){
        node->native=&n;n.ownerControllerData=node;auto* old=runtime().observedNativeInstances.load();
        do{node->next=old;}while(!runtime().observedNativeInstances.compare_exchange_weak(old,node));
    }else{runtime().nativeInstanceTrackingIncomplete=true;}
    n.stateInterceptionEnabled=true;markActiveNativeSaveFailedFromAnyThread();
}
bool echoControllerCurrent(const BridgeRecord& r,const NativeParameterEchoSnapshot& proof)noexcept{
    auto* node=r.currentControllerNode.load(std::memory_order_acquire);
    return node&&node==proof.controllerNode&&node->native&&node->controllerOwner.load()==&r
        &&node->verifiedController.load()==proof.controller&&node->native->actualController.load()==proof.controller
        &&node->native->nativeAlive&&!node->native->nativeTerminated;
}
std::unique_ptr<const NativeParameterEchoSnapshot> captureCacheParameterEchoProof(BridgeRecord& r,const lab::StandaloneSourceCacheBatch& cache){
    const auto admission=r.native->nativeProcessAdmission.snapshot();
    demand(pthread_main_np()&&r.boundary&&!admission.allowed&&admission.activeCount==0,"parameter cache proof requires actual stopped/drained boundary");
    if(cache.modifications.empty())return {};
    auto proof=std::make_unique<NativeParameterEchoSnapshot>();proof->parameters=cache.modifications.front().restoredOrdinaryParameters;
    auto& values=proof->parameters;
    if(!values.valid||!values.count||values.count>values.capacity)return {};
    proof->ticket=cache.ticket;proof->actualComponent=r.identity.actualComponent;proof->controllerNode=r.currentControllerNode.load();
    if(!proof->controllerNode)return {};proof->controller=proof->controllerNode->verifiedController.load();
    if(!proof->controller||!echoControllerCurrent(r,*proof))return {};
    for(const auto& modification:cache.modifications){const auto& other=modification.restoredOrdinaryParameters;
        if(!other.valid||other.count!=values.count||other.ownedComponent==r.identity.actualComponent
            ||other.ownedController==reinterpret_cast<std::uintptr_t>(proof->controller))return {};
        for(std::uint32_t i=0;i<values.count;++i)if(values.rows[i].id!=other.rows[i].id||values.rows[i].normalized!=other.rows[i].normalized||values.rows[i].flags!=other.rows[i].flags)return {};
    }
    // The restored owned-child values must ALSO equal the current verified
    // native controller. This proves component/cache/UI correspondence rather
    // than suppressing a queued value based on the UI alone.
    try{
        if(proof->controller->getParameterCount()!=static_cast<int>(values.count))return {};
        for(std::uint32_t i=0;i<values.count;++i){Steinberg::Vst::ParameterInfo info{};
            if(proof->controller->getParameterInfo(i,info)!=kResultOk||info.id!=values.rows[i].id||info.flags!=values.rows[i].flags
                ||proof->controller->getParamNormalized(info.id)!=values.rows[i].normalized)return {};
        }
        if(proof->controller->getParameterCount()!=static_cast<int>(values.count)||!echoControllerCurrent(r,*proof)||!cache.gate->authorizes(cache.ticket))return {};
    }catch(...){return {};}
    return proof;
}
void refreshAtBoundary(BridgeRecord& r,bool build){
    auto& rt=runtime();publishAllControllerAssociationsOnMain();auto snapshot=rt.coordinator->captureActualCurrentSnapshot(r.identity.instanceID);
    auto* power=r.nativeSlotPower.load();demand(nativeSlotPowerObserverReady()&&power&&r.nativeSlotPowerRegistered&&!power->closed(),"native slot-power binding is unavailable at model boundary");
    rt.overlay->associateNativeSlotPowerAtNativeBoundary(r.identity.instanceID,r.identity.actualComponent,*power);
    const auto currentModel=snapshot.data().model.nativeRecordModelSHA256+":"+snapshot.data().model.sourcePCMModelSHA256+":"+std::to_string(snapshot.data().model.clock.ppq)+":"+std::to_string(snapshot.data().model.clock.tempoMilliBPM);
    if(r.pendingArchive){
        // Pure current-RAM preflight precedes any potentially failing ARA action.
        try{rt.adapter->validateActualCurrentRAMForArchive(*r.pendingArchive,r.identity);}
        catch(const std::exception& e){r.error=std::string("Waiting for matching native project RAM: ")+e.what();return;}
        rt.coordinator->restoreArchiveBeforeOriginalState(r.identity.instanceID,*r.pendingArchive,*rt.adapter,
            lab::StandaloneArchiveApplication::ExistingBoundNativeProjectReplacement);
        r.pendingArchive.reset();r.pendingLoad={};r.error.clear();r.graphApplied=true;r.appliedGraph=snapshot.data().model.graphSHA256;r.appliedModel=currentModel;r.dirty=true;
        snapshot=rt.coordinator->captureActualCurrentSnapshot(r.identity.instanceID);
    }else if(r.pendingPlainLoad&&r.graphApplied&&r.appliedGraph!=snapshot.data().model.graphSHA256){
        rt.coordinator->applyPlainNativeProjectReplacement(r.identity.instanceID,snapshot,r.pendingLoad);
        r.pendingPlainLoad=false;r.pendingLoad={};r.appliedGraph=snapshot.data().model.graphSHA256;r.appliedModel=currentModel;r.dirty=true;
    }else if(!r.graphApplied||r.appliedGraph!=snapshot.data().model.graphSHA256||r.appliedModel!=currentModel){
        rt.coordinator->refreshGraph(r.identity.instanceID,snapshot);r.graphApplied=true;r.appliedGraph=snapshot.data().model.graphSHA256;r.appliedModel=currentModel;r.dirty=true;
        r.pendingPlainLoad=false;r.pendingLoad={};
    }else if(r.pendingPlainLoad){r.pendingPlainLoad=false;r.pendingLoad={};}
    rt.coordinator->pollAtStoppedNativeBoundary(r.identity.instanceID);
    if(snapshot.data().graph.regions.empty()){
        if(build&&(r.dirty.exchange(false)||!r.preparedNativeSave||!r.nativeCacheGate->authorizes(r.preparedNativeSave->identity().currentCacheTicket))){
            // Retire the OLD unauthorized table before authorizing the fresh
            // empty ticket; never invalidate that fresh ticket on unchanged ticks.
            if(rt.overlay){rt.overlay->invalidateInstance(r.identity.instanceID);rt.overlay->removeDisabledInstanceOnModelThread(r.identity.instanceID);}
            lab::WholeSourceCacheOptions emptyOptions;emptyOptions.maximumSourceFrames=48000u*300u;
            auto empty=rt.coordinator->rebuildWholeSourceCaches(r.identity.instanceID,snapshot,emptyOptions);
            demand(empty.noSourceOverlayRequired&&empty.modifications.empty(),"empty native graph unexpectedly requested source DSP");
            r.preparedNativeSave=rt.coordinator->prepareEditorArchiveForNativeSave(r.identity.instanceID,snapshot);
        }
        if(r.preparedNativeSave&&r.nativeCacheGate->authorizes(r.preparedNativeSave->identity().currentCacheTicket)){
            r.error.clear();r.initialPrimePending=false;r.initialWaitNoticeShown=false;
        }
        return;
    }
    if(build&&r.dirty.exchange(false)&&r.graphApplied){
        lab::WholeSourceCacheOptions options;options.maximumSourceFrames=48000u*300u;options.applyOrdinaryCompanionStateBeforeActivation=true;
        auto* ordinary=new lab::fl_app::MemoryIBStream;
        const auto result=hooks::captureOriginalComponentState(*r.native,*ordinary);
        options.ordinaryCompanionStateBeforeActivation=ordinary->bytes();ordinary->release();
        demand(result==kResultOk,"actual native original-only component parameter state capture failed");
        observeAndRequireNativeMixBoundary(r);
        auto cache=rt.coordinator->rebuildWholeSourceCaches(r.identity.instanceID,snapshot,options);
        // A background native source load must not publish an obsolete PCM
        // generation after a long owned-child render. Re-copy actual selected
        // RAM/clock at the same real boundary, then compare before publication.
        const auto current=rt.coordinator->captureActualCurrentSnapshot(r.identity.instanceID);
        const auto& oldModel=snapshot.data().model;const auto& nowModel=current.data().model;
        if(oldModel.nativeRecordModelSHA256!=nowModel.nativeRecordModelSHA256||oldModel.sourcePCMModelSHA256!=nowModel.sourcePCMModelSHA256
            ||oldModel.graphSHA256!=nowModel.graphSHA256||oldModel.clock.actualNativeClockBytes!=nowModel.clock.actualNativeClockBytes
            ||snapshot.data().actualVerifiedAudioRecordIndices!=current.data().actualVerifiedAudioRecordIndices||!cache.gate->authorizes(cache.ticket)){
            rt.coordinator->invalidateSourceCachesAndRequestRebuild(r.identity.instanceID);r.dirty=true;
            throw lab::SourceCacheNotReady("actual current native RAM/clock changed during owned source-cache rendering");
        }
        for(const auto& source:cache.modifications)demand(source.reportedLatencySamples==0&&source.reportedHeadSeconds==0&&source.reportedTailSeconds==0,
            "this third-party ARA source cache reports latency/head/tail; overlay profile refuses unsupported timing");
        demand(r.decoded.has_value()&&r.copied,"source overlay requires exact current decoded model");
        if(!r.nativeSlotPowerSourcePublished&&power->snapshotForBlock()==slot_power::Power::Unknown){r.dirty=true;throw lab::NativeModelBoundaryUnavailable("initial native slot power is unknown; source publication deferred");}
        auto parameterProof=captureCacheParameterEchoProof(r,cache);
        rt.overlay->publish(r.identity.instanceID,*r.decoded,r.copied,std::move(cache));r.nativeSlotPowerSourcePublished=true;
        r.parameterEchoSnapshot.store(nullptr,std::memory_order_release);r.parameterEchoStorage=std::move(parameterProof);
        r.parameterEchoSnapshot.store(r.parameterEchoStorage.get(),std::memory_order_release);
        r.preparedNativeSave=rt.coordinator->prepareEditorArchiveForNativeSave(r.identity.instanceID,snapshot);
        r.error.clear();r.initialPrimePending=false;r.initialWaitNoticeShown=false;
        writeNativeApplicationEvent("actual_native_ara_source_cache_published",r.identity.instanceID);
    }
}
bool sameSaveRequest(const NativeSaveRequest& a,const NativeSaveRequest& b)noexcept{
    return a.actualSaveFrame&&a.actualSaveFrame==b.actualSaveFrame&&a.targetPath==b.targetPath&&a.actualOpenPath==b.actualOpenPath
        &&a.nativeFlags==b.nativeFlags&&a.actualSaveType==b.actualSaveType&&a.directedTarget==b.directedTarget;
}
void closeAndDrainNativeSaveComponent(FrozenNativeSaveComponent& frozen){
    auto& r=*frozen.record;frozen.previousProcessAllowed=r.native->nativeProcessAdmission.exchangeAllowed(false);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(r.native->nativeProcessAdmission.activeCount()){
        demand(std::chrono::steady_clock::now()<=deadline,"native process drain timed out before file creation");std::this_thread::yield();
    }
}
lab::StandaloneSnapshotData captureMetadataForNativeSave(BridgeRecord& r){
    demand(!r.actualNativeSaveMetadataCapture,"native save metadata capture reentry");
    r.actualNativeSaveMetadataCapture=true;struct Reset{bool& flag;~Reset(){flag=false;}}reset{r.actualNativeSaveMetadataCapture};
    return captureActualNativeSnapshotData(r,r.identity,true);
}
bool nativeSavePreflight(const NativeSaveRequest& request,void** cookie,void*)noexcept{
    if(cookie)*cookie=nullptr;auto& rt=runtime();++rt.nativeSavePreflights;
    if(headlessNativeExportInFlight())return false;
    try{
        demand(cookie&&pthread_main_np()&&nativeApplicationAdmitted()&&nativeSaveGuardReady(),"native save protection is unavailable");
        demand(!rt.nativeSaveFreeze&&stopped()&&!request.directedTarget&&request.actualSaveType==0,"native ARA save requires stopped full-project temporary-file route");
        demand(!hasLiveProtectedNativeArchive(),"an alive native component still has an unresolved protected ARA archive");
        std::vector<BridgeRecord*> live;
        for(auto& node:rt.records)if(node->alive&&node->native->nativeAlive&&!node->native->nativeTerminated)live.push_back(node.get());
        if(live.empty())return true;
        rt.nativeSaveFreeze=std::make_unique<NativeSaveFreeze>();auto& freeze=*rt.nativeSaveFreeze;freeze.request=request;
        *cookie=&freeze;freeze.components.reserve(live.size());
        for(auto* r:live){
            demand(!r->disabled&&r->setupQualified&&r->graphApplied&&!r->initialPrimePending&&!r->pendingArchive&&!r->dirty
                &&r->preparedNativeSave,"a bound ARA component has no exact ready prepared archive");
            FrozenNativeSaveComponent entry;entry.record=r;entry.prepared=r->preparedNativeSave;
            freeze.components.push_back(std::move(entry));closeAndDrainNativeSaveComponent(freeze.components.back());
        }
        for(auto& entry:freeze.components){
            entry.actualPreflight=captureMetadataForNativeSave(*entry.record);
            entry.archive=std::make_shared<const lab::StandaloneSavedEditorArchive>(rt.coordinator->validatePreparedEditorArchiveForNativeSave(
                entry.record->identity.instanceID,*entry.prepared,entry.actualPreflight));
            // Only frozen metadata/digests remain. The pure validator never
            // invokes a provider, so no duplicated source PCM is held by Save.
            entry.actualPreflight.resolveCurrentImmutableRAMSource={};
        }
        demand(!hasLiveProtectedNativeArchive(),"an unresolved native ARA archive appeared during save preflight");
        freeze.preflightPassed=true;++rt.nativeSavePreflightsAllowed;rt.nativeSaveError.clear();return true;
    }catch(const std::exception& e){rt.nativeSaveError=e.what();return false;}
    catch(...){rt.nativeSaveError="unknown error validating prepared native save";return false;}
}
bool nativeSavePrecommit(const NativeSaveRequest& request,void* cookie,void*)noexcept{
    auto& rt=runtime();try{
        demand(pthread_main_np()&&nativeApplicationAdmitted()&&stopped()&&!hasLiveProtectedNativeArchive(),"native save state changed before commit");
        if(!cookie){for(const auto& r:rt.records)demand(!r->alive||!r->native->nativeAlive||r->native->nativeTerminated,"new bound ARA component appeared before ordinary commit");return true;}
        demand(rt.nativeSaveFreeze.get()==cookie,"native save cookie does not belong to this owner");auto& freeze=*rt.nativeSaveFreeze;
        demand(freeze.preflightPassed&&sameSaveRequest(freeze.request,request),"native save frame/request changed before commit");
        std::size_t live=0;for(const auto& r:rt.records)if(r->alive&&r->native->nativeAlive&&!r->native->nativeTerminated)++live;
        demand(live==freeze.components.size(),"bound ARA component membership changed during native save");
        // Prove ALL required serializations before touching any native source
        // metadata. A missing later component must never be hidden by an
        // earlier component's successful SDK state call.
        for(const auto& entry:freeze.components)demand(entry.serializedCalls>0&&entry.record&&entry.record->alive&&!entry.record->disabled,
            "an expected ARA envelope was not serialized successfully");
        for(const auto& entry:freeze.components)demand(entry.prepared&&entry.archive,"native save lost its immutable prepared archive authority");
        for(auto& entry:freeze.components){
            auto current=captureMetadataForNativeSave(*entry.record);
            rt.coordinator->validatePreparedEditorArchiveForNativeSave(entry.record->identity.instanceID,*entry.prepared,current);
        }
        freeze.precommitPassed=true;++rt.nativeSavePrecommitsAllowed;return true;
    }catch(const std::exception& e){rt.nativeSaveError=e.what();return false;}
    catch(...){rt.nativeSaveError="unknown native save precommit failure";return false;}
}
void nativeSaveRejected(const NativeSaveRequest&,NativeSaveRejection reason,void*)noexcept{
    try{auto& rt=runtime();if(rt.nativeSaveError.empty())rt.nativeSaveError=nativeSaveRejection(reason);rt.nativeSaveAlertQueued=true;
        writeNativeApplicationEvent("native_ara_project_save_refused",rt.nativeSaveError);
    }catch(...){}
}
void nativeSaveFinished(const NativeSaveTransaction& transaction,void*)noexcept{
    auto& rt=runtime();++rt.nativeSaveFramesFinished;
    if(transaction.ownerCookie&&rt.nativeSaveFreeze.get()==transaction.ownerCookie){
        for(auto& entry:rt.nativeSaveFreeze->components){auto& r=*entry.record;
            r.native->nativeProcessAdmission.setAllowed(entry.previousProcessAllowed&&r.alive&&!r.disabled&&!r.teardown&&!r.initialPrimePending&&r.native->nativeAlive&&!r.native->nativeTerminated);
            // Guard commitBegun is not actual whole-file success/readback.
            // Conservatively re-announce real unsaved revisions after a refusal.
            if(transaction.failed||!rt.nativeSaveFreeze->precommitPassed)r.nativeDirtyRetryAfterSaveRefusal=true;
        }
        rt.nativeSaveFreeze.reset();
    }
}
Steinberg::tresult serializeFrozenNativeProjectState(hooks::InstanceRecord& n,BridgeRecord* r,Steinberg::IBStream* stream,
    hooks::OriginalStateCall& original,const hooks::ThreadInfo& info)noexcept{
    auto& rt=runtime();try{
        const auto* transaction=currentNativeSaveTransaction();
        demand(transaction&&transaction->openAllowed&&!transaction->failed&&info.isMainThread&&r&&sealedNativeCaller(info),"actual native project-save component caller is unavailable");
        demand(rt.nativeSaveFreeze&&transaction->ownerCookie==rt.nativeSaveFreeze.get()
            &&sameSaveRequest(transaction->request,rt.nativeSaveFreeze->request),"actual native save has no frozen owner payload");
        FrozenNativeSaveComponent* frozen=nullptr;for(auto& entry:rt.nativeSaveFreeze->components)if(entry.record==r){frozen=&entry;break;}
        const auto admission=n.nativeProcessAdmission.snapshot();
        demand(frozen&&frozen->prepared&&frozen->archive&&r->native==&n&&!admission.allowed
            &&admission.activeCount==0,"component was not frozen/drained by actual native file-open preflight");
        rt.coordinator->validatePreparedEditorArchiveForNativeSave(r->identity.instanceID,*frozen->prepared,frozen->actualPreflight);
        auto* ordinary=new lab::fl_app::MemoryIBStream;const auto result=original(ordinary);lab::Bytes bytes=ordinary->bytes();ordinary->release();
        if(result!=kResultOk){markCurrentNativeSaveFailed();return result;}
        rt.coordinator->validatePreparedEditorArchiveForNativeSave(r->identity.instanceID,*frozen->prepared,frozen->actualPreflight);
        lab::FLStandaloneStateEnvelope envelope;envelope.archive=*frozen->archive;envelope.nativeComponentState=std::move(bytes);
        auto encoded=lab::encodeFLStandaloneStateEnvelope(envelope);lab::fl_app::writeAll(*stream,encoded);
        ++frozen->serializedCalls;frozen->ordinaryBytes=envelope.nativeComponentState.size();frozen->envelopeBytes=encoded.size();++rt.nativeSaveEnvelopes;
        r->lastNativeStateCaller=nativeCallerEvidence(info);return result;
    }catch(const std::exception& e){rt.nativeSaveError=e.what();markCurrentNativeSaveFailed();return kResultFalse;}
    catch(...){rt.nativeSaveError="unknown failure serializing frozen native ARA envelope";markCurrentNativeSaveFailed();return kResultFalse;}
}
ObservedNativeInstance* controllerNode(hooks::InstanceRecord& n)noexcept{
    auto* node=static_cast<ObservedNativeInstance*>(n.ownerControllerData.load());return node&&node->native==&n?node:nullptr;
}
BridgeRecord* publishedControllerOwner(hooks::InstanceRecord& n,Steinberg::Vst::IEditController* actual)noexcept{
    auto* node=controllerNode(n);if(!node||!actual||!n.nativeAlive||!n.nativeInitialized||n.nativeTerminated
        ||n.actualController.load()!=actual||node->verifiedController.load()!=actual)return nullptr;
    auto* r=node->controllerOwner.load();if(!r||r->currentControllerNode.load()!=node||!r->alive||!r->native->nativeAlive
        ||!r->native->nativeInitialized||r->native->nativeTerminated||r->native->ownerData.load()!=r)return nullptr;
    return r;
}
void publishControllerAssociationOnMain(hooks::InstanceRecord& n)noexcept{
    if(!pthread_main_np())return;auto* node=controllerNode(n);if(!node)return;
    auto* controller=n.actualController.load();auto* context=n.actualInitializeContext.load();BridgeRecord* owner=nullptr;
    if(controller&&context&&n.nativeAlive&&n.nativeInitialized&&!n.nativeTerminated){
        NativeInsert mapped;
        if(resolveActualNativeController(reinterpret_cast<std::uintptr_t>(controller),reinterpret_cast<std::uintptr_t>(context),mapped))
            for(auto* r=runtime().head.load();r;r=r->next.load())if(r->alive&&!r->disabled&&!r->teardown&&r->native->nativeAlive&&r->native->nativeInitialized&&!r->native->nativeTerminated
                &&r->identity.actualComponent==mapped.actualComponent&&r->native->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(mapped.actualComponent)
                &&revalidateActualNativeController(r->insert,reinterpret_cast<std::uintptr_t>(controller))){owner=r;break;}
    }
    auto* old=node->controllerOwner.load();
    if(owner&&old==owner&&node->verifiedController.load()==controller&&owner->currentControllerNode.load()==node)return;
    // A real association change closes the formerly owned cache BEFORE any
    // publication gap. Unchanged main polls never unpublish a live RT route.
    if(old)invalidateAssociatedARACache(*old);
    auto* previous=owner?owner->currentControllerNode.load():nullptr;if(owner&&previous&&previous!=node)invalidateAssociatedARACache(*owner);
    node->controllerOwner=nullptr;node->verifiedController=nullptr;
    if(old){auto* expected=node;old->currentControllerNode.compare_exchange_strong(expected,nullptr);}
    if(!owner)return;
    node->verifiedController=controller;previous=owner->currentControllerNode.exchange(node);
    if(previous&&previous!=node){previous->controllerOwner=nullptr;previous->verifiedController=nullptr;}
    node->controllerOwner=owner;++runtime().separateControllerAssociationsVerified;
}
void publishAllControllerAssociationsOnMain()noexcept{
    if(!pthread_main_np())return;for(auto* node=runtime().observedNativeInstances.load();node;node=node->next.load())publishControllerAssociationOnMain(*node->native);
}
BridgeRecord* exactNativeControllerOwner(hooks::InstanceRecord& n,const hooks::ThreadInfo& info)noexcept{
    if(!info.isMainThread||info.insideNativeProcess||!sealedNativeCaller(info))return nullptr;
    publishControllerAssociationOnMain(n);return publishedControllerOwner(n,n.actualController.load());
}
Steinberg::tresult state(hooks::InstanceRecord& n,hooks::StateOperation operation,Steinberg::IBStream* stream,hooks::OriginalStateCall& original,const hooks::ThreadInfo& info,void*)noexcept{
    if(!stream){markActiveNativeSaveFailedFromAnyThread();return kResultFalse;}
    if(headlessNativeExportInFlight())return kResultFalse;
    auto* r=record(n);
    // Off-main has no authority to inspect the native Save stack. Its exact
    // hook record can still fail a live transaction through the RT-safe latch.
    if(!info.isMainThread&&markActiveNativeSaveFailedFromAnyThread()
        &&operation!=hooks::StateOperation::ComponentGetState)return kResultFalse;
    try{
        if(operation==hooks::StateOperation::ControllerSetComponentState){
            if(currentNativeSaveTransaction()){
                markCurrentNativeSaveFailed();runtime().nativeSaveError="native controller state replacement reentered an active project save";return kResultFalse;
            }
            if(!lab::fl_app::peekMagicAndRestorePosition(*stream))return original(stream);
            if(r&&(!r->alive||r->disabled||r->teardown))r=nullptr;
            if(!r)r=exactNativeControllerOwner(n,info);
            if(!r){
                // No proven cross-record controller/component association exists.
                // Leave the recognized stream position/bytes untouched, refuse
                // before original, and protect whole-file saves for this live
                // actual native record. Never guess by factory/creation order.
                preserveUnboundARA(n);
                if(info.isMainThread)runtime().error="recognized ARA controller state has no proven bound component association";
                return kResultFalse;
            }
            auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized native envelope decode failed");
            auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();return result;
        }
        if(operation==hooks::StateOperation::ComponentGetState){
            if(n.ownerStateFlags.load()&protectedUnboundArchive){markCurrentNativeSaveFailed();return kResultFalse;}
            const auto* transaction=currentNativeSaveTransaction();
            if(!transaction)return original(stream); // Presets/diagnostics remain ordinary SDK state.
            if(!r)return original(stream); // Expected bound records are checked before destructive commit.
            return serializeFrozenNativeProjectState(n,r,stream,original,info);
        }
        if(currentNativeSaveTransaction()){
            markCurrentNativeSaveFailed();runtime().nativeSaveError="native component state replacement reentered an active project save";return kResultFalse;
        }
        if(r&&!info.isMainThread){r->fault=true;return kResultFalse;}
        if(!r){
            if(lab::fl_app::peekMagicAndRestorePosition(*stream)){
                preserveUnboundARA(n);return kResultFalse;
            }
            return original(stream);
        }
        if(r->disabled){
            if(lab::fl_app::peekMagicAndRestorePosition(*stream))return kResultFalse;
            return original(stream);
        }
        // Ordinary native state restore can precede setupProcessing.
        if(operation==hooks::StateOperation::ComponentSetState&&!r->setupQualified){
            const bool magic=lab::fl_app::peekMagicAndRestorePosition(*stream);r->pendingLoad=loadAuthority(*r,info);
            if(!magic){r->pendingPlainLoad=true;const auto result=original(stream);runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}return result;}
            auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized early archive decode failed");
            demand(!r->pendingArchive,"a second native archive arrived before the first was resolved");r->pendingArchive=envelope->archive;r->dirty=true;r->initialPrimePending=true;r->native->nativeProcessAdmission.setAllowed(false);
            auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();
            runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}return result;
        }
        if(!pthread_main_np()||!stopped()){
            r->fault=false;r->error="ARA project state requires stopping native FL playback first";return kResultFalse;
        }
        Boundary boundary(*r);
        const bool magic=lab::fl_app::peekMagicAndRestorePosition(*stream);r->pendingLoad=loadAuthority(*r,info);
        if(!magic){
            const auto snapshot=runtime().coordinator->captureActualCurrentSnapshot(r->identity.instanceID);
            // A native project replacement can clear edits only if actual RAM
            // graph differs at this explicit actual component state-load call.
            if(r->graphApplied&&r->appliedGraph!=snapshot.data().model.graphSHA256){
                runtime().coordinator->applyPlainNativeProjectReplacement(r->identity.instanceID,snapshot,r->pendingLoad);
                r->appliedGraph=snapshot.data().model.graphSHA256;r->dirty=true;
            }
            const auto result=original(stream);runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}r->pendingLoad={};return result;
        }
        auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized ARA envelope decode failed");
        demand(!r->pendingArchive,"a second native archive arrived before the pending one resolved");
        r->pendingArchive=envelope->archive;r->pendingOrdinaryComponentState=envelope->nativeComponentState;
        r->initialPrimePending=true;
        // Strict metadata validation before the ordinary original. If FL has not
        // published this project's current RAM yet, retain the untouched archive
        // with this exact sealed actual load authority; tick retries at stopped RAM.
        bool currentRAMMatches=false;
        try{runtime().adapter->validateActualCurrentRAMForArchive(envelope->archive,r->identity);currentRAMMatches=true;}
        catch(const lab::NativeModelBoundaryUnavailable&){throw;}
        catch(const std::exception&){r->pendingArchive=envelope->archive;r->dirty=true;}
        if(currentRAMMatches){
            runtime().coordinator->restoreArchiveBeforeOriginalState(r->identity.instanceID,envelope->archive,*runtime().adapter,
                r->graphApplied?lab::StandaloneArchiveApplication::ExistingBoundNativeProjectReplacement:lab::StandaloneArchiveApplication::FreshBeforeFirstOriginalState);
            r->graphApplied=true;r->appliedGraph=r->decoded->graphSHA256;r->pendingLoad={};r->pendingArchive.reset();r->dirty=true;
        }
        auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();
        runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}r->pendingOrdinaryComponentState.reset();return result;
    }catch(const lab::NativeModelBoundaryUnavailable& e){
        if(!r)return kResultFalse;r->error=std::string("Native mix boundary deferred: ")+e.what();r->dirty=true;
        if(operation!=hooks::StateOperation::ComponentSetState)return kResultFalse;
        // This is still the exact sealed synchronous native load. Defer ARA,
        // retain recognized archives, and forward only original ordinary bytes.
        try{
            if(r->pendingArchive&&r->pendingOrdinaryComponentState){
                r->initialPrimePending=true;r->native->nativeProcessAdmission.setAllowed(false);
                auto* ordinary=new lab::fl_app::MemoryIBStream(*r->pendingOrdinaryComponentState);const auto result=original(ordinary);ordinary->release();
                r->pendingOrdinaryComponentState.reset();runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);
                runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);return result;
            }
            r->pendingLoad=loadAuthority(*r,info);
            if(lab::fl_app::peekMagicAndRestorePosition(*stream)){
                auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized deferred archive decode failed");
                demand(!r->pendingArchive,"a second native archive arrived before the pending one resolved");
                r->pendingArchive=envelope->archive;r->initialPrimePending=true;r->native->nativeProcessAdmission.setAllowed(false);
                auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();
                runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);return result;
            }
            r->pendingPlainLoad=true;const auto result=original(stream);runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);
            runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);return result;
        }catch(const std::exception& failure){fail(*r,failure.what());return kResultFalse;}
        catch(...){fail(*r,"unknown exception preserving deferred native state");return kResultFalse;}
    }catch(const std::exception& e){if(r)fail(*r,e.what());else if(pthread_main_np())runtime().error=e.what();return kResultFalse;}
    catch(...){if(r)fail(*r,"unknown native SDK exception in component state");return kResultFalse;}
}
void setActive(hooks::InstanceRecord& n,Steinberg::TBool active,Steinberg::tresult result,const hooks::ThreadInfo&,void*)noexcept{
    if(auto* r=record(n))try{runtime().coordinator->observeOriginalSetActiveResult(r->identity.instanceID,active,result);if(result==kResultOk){r->active=active;r->observedInactive=!active;}}catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown native SDK lifecycle exception");}
}
void ensureOverlayAtModelBoundary(){
    auto& rt=runtime();const auto& app=nativeApplication();
    demand(rt.anyBoundary&&app.engineReady,"source hook installation needs actual verified model boundary");
    if(!rt.overlay){lab::FLStandaloneOverlayConfig config;config.requireNativeSlotPower=true;config.actualEngineImageBase=app.engine.imageBase;
        config.exactLoadedEnginePath=app.enginePath;config.exactEngineArm64SliceSHA256=app.engine.verifiedArm64SliceSHA256;
        config.confirmAlreadyAdmittedOwnedProcess=admitted;config.confirmStoppedDrainedNativeModelBoundary=allBoundary;config.ownerFaultQueued=&rt.overlayFault;config.ownerStaleHoldQueued=&rt.overlayStaleHoldQueued;
        rt.overlay=std::make_unique<lab::FLStandaloneSourceOverlay>(std::move(config));rt.overlayListener.store(rt.overlay.get(),std::memory_order_release);
    }
    if(!rt.overlayInstalled){const auto installed=rt.overlay->installLoadedNativeSourceHook();rt.overlayInstalled=installed.installed;demand(installed.installed,installed.reason.c_str());}
}
// First-32 reserved immutable completion slots. Each callback owns one slot; status
// sees its POD only after release publication, including every early return.
struct NativeSetupObservation {
    BridgeRecord& owner;NativeSetupDiagnostic* slot=nullptr;
    NativeSetupObservation(BridgeRecord& r,const Steinberg::Vst::ProcessSetup& setup,Steinberg::tresult result,const hooks::ThreadInfo& info)noexcept:owner(r){
        auto count=r.nativeSetupDiagnosticCount.load(std::memory_order_relaxed);
        while(count!=std::numeric_limits<std::uint64_t>::max()&&!r.nativeSetupDiagnosticCount.compare_exchange_strong(count,count+1,std::memory_order_relaxed)){}
        auto index=r.nativeSetupDiagnosticSlots.load(std::memory_order_relaxed);
        do{if(index>=r.nativeSetupHistory.size())return;}
        while(!r.nativeSetupDiagnosticSlots.compare_exchange_strong(index,index+1,std::memory_order_relaxed));
        slot=&r.nativeSetupHistory[index];slot->sequence=index+1;slot->actualSetup=setup;slot->originalResult=result;
        slot->mainThread=info.isMainThread;slot->insideNativeProcess=info.insideNativeProcess;
        slot->epochBefore=r.nativeCacheGate?r.nativeCacheGate->currentTicket().epoch:0;
        slot->dirtyBefore=r.dirty.load();slot->qualifiedBefore=r.setupQualified.load();
    }
    ~NativeSetupObservation(){if(!slot)return;
        slot->epochAfter=owner.nativeCacheGate?owner.nativeCacheGate->currentTicket().epoch:0;
        slot->dirtyAfter=owner.dirty.load();slot->qualifiedAfter=owner.setupQualified.load();
        slot->ready.store(true,std::memory_order_release);
    }
};
void setup(hooks::InstanceRecord& n,const Steinberg::Vst::ProcessSetup& setup,Steinberg::tresult result,const hooks::ThreadInfo& info,void*)noexcept{
    auto* r=record(n);if(!r)return;
    NativeSetupObservation diagnostic(*r,setup,result,info);
    static_assert(Steinberg::Vst::kSample32==editor_setup_cache::nativeFloat32&&Steinberg::Vst::kRealtime==editor_setup_cache::nativeRealtime&&
        Steinberg::Vst::kPrefetch==editor_setup_cache::nativePrefetch&&Steinberg::Vst::kOffline==editor_setup_cache::nativeOffline,"native setup policy must match the actual SDK constants");
    // This record is bound only as EditorRenderer|EditorView. Full source PCM
    // belongs to a separately owned playback companion, so a successful native
    // buffer-size/process-mode transition does not itself change cache content.
    const auto decision=editor_setup_cache::apply({result==kResultOk,setup.sampleRate,setup.symbolicSampleSize,setup.processMode,setup.maxSamplesPerBlock},
        info.isMainThread&&!info.insideNativeProcess,r->sampleRate,r->setupQualified,r->dirty,r->nativeCacheGate.get(),
        [&]()noexcept{return currentNativeCacheGenerationCoherent(*r);});
    if(decision.action==editor_setup_cache::Action::PreserveCoherentSourceCache)return;
    if(!r->setupQualified||!info.isMainThread||!stopped()||currentNativeSaveTransaction()||headlessNativeExportInFlight())return;
    // The actual original setupProcessing has returned. Prime synchronously
    // before FL can start original processing, ONLY after the owned worker proves
    // that main holds no outer recursive native mix lock.
    try{Boundary boundary(*r);ensureOverlayAtModelBoundary();refreshAtBoundary(*r,true);}
    catch(const lab::NativeModelBoundaryUnavailable& e){r->error=std::string("Initial ARA prime deferred: ")+e.what();r->dirty=true;}
    catch(const lab::SourceCacheNotReady& e){r->error=e.what();r->dirty=true;}
    catch(const lab::FLStandaloneDecodeError& e){if(!r->graphApplied){r->error=e.what();r->dirty=true;}else fail(*r,e.what());}
    catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown exception during synchronous native setup prime");}
}
void processing(hooks::InstanceRecord& n,Steinberg::TBool enabled,Steinberg::tresult result,const hooks::ThreadInfo&,void*)noexcept{
    if(auto* r=record(n))try{runtime().coordinator->observeOriginalSetProcessingResult(r->identity.instanceID,enabled,result);if(result==kResultOk){r->processing=enabled;r->observedStopped=!enabled;}}catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown native SDK lifecycle exception");}
}
void beforeTerminate(hooks::InstanceRecord& n,void*,const hooks::ThreadInfo& info,void*)noexcept{
    auto* r=record(n);if(!r)return;retireNativeSlotPower(*r);
    try{demand(info.isMainThread&&!r->active&&!r->processing,"actual native component must deactivate and stop before ARA terminate");
        TeardownBoundary boundary(*r,sealedNativeCaller(info));
        writeNativeApplicationEvent("actual_native_before_original_terminate_authority",r->identity.instanceID+" "+nativeCallerEvidence(info));
        // Only immutable table maintenance follows this cache invalidation.
        // Native readers retire through RCU; no global engine/transport hold is
        // invented for deleting this exact already inactive component's model.
        if(auto* overlay=runtime().overlayListener.load()){
            overlay->invalidateInstance(r->identity.instanceID);
            overlay->removeDisabledInstanceOnModelThread(r->identity.instanceID);
        }
        runtime().coordinator->prepareForOriginalTerminate(r->identity.instanceID);r->teardown=true;
    }catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown native SDK lifecycle exception");}
}
void terminated(hooks::InstanceRecord& n,void*,Steinberg::tresult result,const hooks::ThreadInfo& info,void*)noexcept{
    if(auto* r=record(n))try{retireNativeSlotPower(*r);runtime().coordinator->observeOriginalTerminateResult(r->identity.instanceID,result);r->alive=false;}catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown native SDK lifecycle exception");}
}
void invalidateAssociatedARACache(BridgeRecord& r)noexcept{
    if(!r.alive||!r.native->nativeAlive||r.native->nativeTerminated||!r.nativeCacheGate)return;markActiveNativeSaveFailedFromAnyThread();
    r.nativeCacheGate->invalidate();r.dirty=true;queueNativeStopForCacheRebuild(r,r.nativeCacheGate->currentTicket().epoch);
}
void nativeProcessTelemetry(hooks::InstanceRecord& n,const hooks::NativeProcessTelemetry& observation,const hooks::ThreadInfo& info,void*)noexcept{
    if(auto* r=record(n))if(r->nativeProcessObserver)r->nativeProcessObserver->observe(n,observation,info);
}
bool exactCurrentCacheParameterEcho(const BridgeRecord& r,Steinberg::Vst::IParameterChanges* changes,Steinberg::int32 count,Steinberg::int32 sampleCount)noexcept{
    const auto* proof=r.parameterEchoSnapshot.load(std::memory_order_acquire);
    if(!proof||!changes||count<=0||count>static_cast<int>(lab::OrdinaryParameterSnapshot::capacity)||sampleCount<0
        ||!r.alive||r.dirty||!r.nativeCacheGate||r.identity.actualComponent!=proof->actualComponent
        ||reinterpret_cast<std::uintptr_t>(r.native->actualComponent.load())!=proof->actualComponent
        ||!echoControllerCurrent(r,*proof)||!r.nativeCacheGate->authorizes(proof->ticket))return false;
    std::array<Steinberg::Vst::ParamID,lab::OrdinaryParameterSnapshot::capacity> seen{};
    try{for(int i=0;i<count;++i){auto* queue=changes->getParameterData(i);if(!queue||queue->getPointCount()!=1)return false;
        const auto id=queue->getParameterId();for(int j=0;j<i;++j)if(seen[j]==id)return false;seen[i]=id;
        Steinberg::int32 offset=-1;Steinberg::Vst::ParamValue value=0;
        if(queue->getPoint(0,offset,value)!=kResultOk||offset!=0||!std::isfinite(value)||value<0||value>1)return false;
        bool match=false;for(std::uint32_t j=0;j<proof->parameters.count;++j){const auto& row=proof->parameters.rows[j];
            constexpr auto special=Steinberg::Vst::ParameterInfo::kIsReadOnly|Steinberg::Vst::ParameterInfo::kIsProgramChange|Steinberg::Vst::ParameterInfo::kIsBypass;
            if(row.id==id&&!(row.flags&special)&&row.normalized==value){match=true;break;}}
        if(!match)return false;
    }}catch(...){return false;}
    return changes->getParameterCount()==count&&!r.dirty&&echoControllerCurrent(r,*proof)&&r.nativeCacheGate->authorizes(proof->ticket);
}
void inputParameterChanges(hooks::InstanceRecord& n,Steinberg::Vst::IParameterChanges* changes,Steinberg::int32 count,Steinberg::int32 samples,const hooks::ThreadInfo&,void*)noexcept{
    if(count!=0)if(auto* r=record(n)){r->nativeInputQueueParameterNotifications.fetch_add(1);
        if(exactCurrentCacheParameterEcho(*r,changes,count,samples)){r->nativeInputQueueExactEchoes.fetch_add(1);return;}
        invalidateAssociatedARACache(*r);}
}
void controllerHandlerObserved(hooks::InstanceRecord& n,hooks::ControllerHandlerPhase,Steinberg::Vst::IEditController*,
    Steinberg::Vst::IComponentHandler*,Steinberg::tresult,const hooks::ThreadInfo& info,void*)noexcept{
    // BeforeOriginal: exact pending handler is already atomic-published by hooks.
    // Main publishes ownership before native setter can synchronously performEdit.
    if(info.isMainThread&&!info.insideNativeProcess&&!headlessNativeExportInFlight())publishControllerAssociationOnMain(n);
}
void parameterEdit(hooks::InstanceRecord*,Steinberg::Vst::IComponentHandler* actualHandler,Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const hooks::ThreadInfo&,void*)noexcept{
    if(!actualHandler)return;
    // Stable ARA owners only. The diagnostic first-controller is never routing
    // authority. A genuinely shared handler invalidates its matching ARA owners.
    for(auto* r=runtime().head.load();r;r=r->next.load())if(auto* node=r->currentControllerNode.load()){
        auto& n=*node->native;
        if((n.actualNativeComponentHandler.load()==actualHandler||n.actualPendingNativeComponentHandler.load()==actualHandler)
            &&publishedControllerOwner(n,node->verifiedController.load())==r){r->nativeHandlerParameterNotifications.fetch_add(1);invalidateAssociatedARACache(*r);}
    }
}
void parameterNormalized(hooks::InstanceRecord& n,Steinberg::Vst::IEditController* actualController,Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const hooks::ThreadInfo& info,void*)noexcept{
    if(info.isMainThread&&!info.insideNativeProcess&&!headlessNativeExportInFlight())publishControllerAssociationOnMain(n);
    if(auto* r=publishedControllerOwner(n,actualController)){r->nativeNormalizedParameterNotifications.fetch_add(1);invalidateAssociatedARACache(*r);}
}
void fault(const char*,const hooks::ModuleRecord*,const hooks::InstanceRecord* n,const hooks::ThreadInfo&,void*)noexcept{
    if(n)if(auto* r=static_cast<BridgeRecord*>(n->ownerData.load()))r->fault=true;
}
// Main-thread JSON of immutable diagnostic POD only. Never dereference a
// diagnostic address. Rejected reads may contain NaN/Inf before format guards.
NSArray* nativeRecordDiagnosticJSON(const std::array<std::uint8_t,88>& bytes){NSMutableArray* out=[NSMutableArray arrayWithCapacity:88];for(auto value:bytes)[out addObject:@(value)];return out;}
NSDictionary* finiteDoubleDiagnosticJSON(double value){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));const bool finite=std::isfinite(value);id number=finite?@(value):NSNull.null;return @{@"finite":@(finite),@"value":number,@"raw_bits":@(bits)};}
NSDictionary* physicalExpectedDiagnosticJSON(const lab::FLStandalonePhysicalIdentityExpected& v){return @{@"native_channel_index":@(v.nativeChannelIndex),@"native_mixer_insert":@(v.nativeMixerInsert),@"channel_object":@(v.channelObject),@"channel_vmt":@(v.channelVMT),
        @"sample_object":@(v.sampleObject),@"sample_vmt":@(v.sampleVMT),@"pcm_address":@(v.pcmAddress),@"source_frames":@(v.sourceFrames),
        @"source_channels":@(v.sourceChannels),@"captured_model_address":@(v.capturedModelAddress),@"captured_engine_base":@(v.capturedEngineBase),@"captured_channel_manager":@(v.capturedChannelManager),
        @"captured_manager_pointer_array":@(v.capturedManagerPointerArray),@"captured_manager_channel_count":@(v.capturedManagerChannelCount),@"captured_channel_snapshot_index":@(v.capturedChannelSnapshotIndex),@"captured_source_snapshot_index":@(v.capturedSourceSnapshotIndex),
        @"captured_channel_object":@(v.capturedChannelObject),@"captured_channel_vmt":@(v.capturedChannelVMT),@"captured_channel_type150":@(v.capturedChannelType150),@"captured_mixer_insert248":@(v.capturedMixerInsert248),
        @"complete_manager_inspection":@(v.completeManagerInspection),@"manager_member_present":@(v.managerMemberPresent),@"captured_manager_member_index":@(v.capturedManagerMemberIndex),@"captured_manager_member_object":@(v.capturedManagerMemberObject),
        @"captured_manager_member_vmt":@(v.capturedManagerMemberVMT),@"captured_manager_member_type150":@(v.capturedManagerMemberType150)};}
NSDictionary* physicalCandidateDiagnosticJSON(const lab::FLStandalonePhysicalIdentityCandidate& v){return @{@"present":@(v.present),@"region_present":@(v.regionPresent),@"batch_address":@(v.batchAddress),@"source_address":@(v.sourceAddress),
        @"actual_native_component":@(v.actualNativeComponent),@"capture_serial":@(v.captureSerial),@"ticket_epoch":@(v.ticketEpoch),@"publication_generation":@(v.publicationGeneration),
        @"native_mixer_insert":@(v.nativeMixerInsert),@"native_clip_id":@(v.nativeClipID),@"region_source_index":@(v.regionSourceIndex),@"native_record":nativeRecordDiagnosticJSON(v.nativeRecord),@"expected":physicalExpectedDiagnosticJSON(v.expected)};}
NSDictionary* physicalObservedDiagnosticJSON(const lab::FLStandaloneSourceRead& v){return @{@"voice_record_read":@(v.voiceRecordRead),@"native_observation_complete":@(v.nativeObservationComplete),@"voice_object":@(v.voiceObject),@"voice_vmt":@(v.voiceVMT),
        @"channel_object":@(v.channelObject),@"channel_vmt":@(v.channelVMT),@"sample_object":@(v.sampleObject),@"sample_vmt":@(v.sampleVMT),
        @"reader_object":@(v.readerObject),@"reader_vmt":@(v.readerVMT),@"channel_sample328":@(v.channelSample328),@"reader_sample78":@(v.readerSample78),
        @"native_pcm90":@(v.nativePCM90),@"reader_pcm40":@(v.readerPCM40),@"reference_c8":@(v.referenceC8),@"stretch_provider100":@(v.stretchProvider100),
        @"native_clip_id":@(v.nativeClipID),@"native_mixer_insert":@(v.nativeMixerInsert),@"native_channel_index":@(v.nativeChannelIndex),@"native_source_frames":@(v.nativeSourceFrames),
        @"pre_original_reader_position_read":@(v.preOriginalReaderPositionRead),@"native_format78":@(v.nativeFormat78),@"native_format79":@(v.nativeFormat79),@"reader_channels_a0":@(v.readerChannelsA0),
        @"reader_format_a1":@(v.readerFormatA1),@"reader_stride48":@(v.readerStride48),@"native_block_frames":@(v.nativeBlockFrames),@"original_read_frames":@(v.originalReadFrames),
        @"global_preview_owner_read_coherent":@(v.globalPreviewOwnerReadCoherent),@"global_preview_channel_object":@(v.globalPreviewChannelObject),@"native_interleaved_stereo":@(reinterpret_cast<std::uintptr_t>(v.nativeInterleavedStereo)),@"output_matches_actual_native_context":@(v.outputMatchesActualNativeContext),@"original_ended":@(v.originalEnded),@"original_changed":@(v.originalChanged),@"native_record":nativeRecordDiagnosticJSON(v.nativeRecord),@"native_rate":finiteDoubleDiagnosticJSON(v.nativeRate),@"native_rate_factor":finiteDoubleDiagnosticJSON(v.nativeRateFactor),@"reader_step":finiteDoubleDiagnosticJSON(v.readerStep),@"source_start_frame":finiteDoubleDiagnosticJSON(v.sourceStartFrame),@"pre_original_reader_position":finiteDoubleDiagnosticJSON(v.preOriginalReaderPosition)};}
NSDictionary* physicalIdentityDiagnosticJSON(const lab::FLStandalonePhysicalIdentityDiagnostic& v){return @{@"slot_index":@(v.slotIndex),@"mismatch_bits":@(v.mismatchBits),@"presence_mask":@(v.presenceMask),@"agreement_mask":@(v.agreementMask),
        @"matching_batch_count":@(v.matchingBatchCount),@"protected_table_address":@(v.protectedTableAddress),@"owner_batch_address":@(v.ownerBatchAddress),@"actual_native_component":@(v.actualNativeComponent),
        @"table_batch_count":@(v.tableBatchCount),@"table_region_count":@(v.tableRegionCount),@"table_pointer_channel_count":@(v.tablePointerChannelCount),@"table_indexed_channel_count":@(v.tableIndexedChannelCount),
        @"capture_serial":@(v.captureSerial),@"ticket_epoch":@(v.ticketEpoch),@"publication_generation":@(v.publicationGeneration),@"branch":@(static_cast<std::uint8_t>(v.branch)),@"observed":physicalObservedDiagnosticJSON(v.observed),@"entry":physicalCandidateDiagnosticJSON(v.entry),@"physical":physicalCandidateDiagnosticJSON(v.physical),@"indexed":physicalCandidateDiagnosticJSON(v.indexed)};}
NSArray* physicalIdentityHistoryDiagnosticJSON(const lab::FLStandaloneOverlayInstanceStats& cache){NSMutableArray* history=[NSMutableArray array];
    for(std::size_t i=0;i<cache.physicalIdentityDiagnostics.size();++i)if(cache.physicalIdentityDiagnosticReadyMask&(std::uint8_t{1}<<i))[history addObject:physicalIdentityDiagnosticJSON(cache.physicalIdentityDiagnostics[i])];return history;
}
bool status(){
    auto& rt=runtime();const auto& app=nativeApplication();const auto cacheStats=rt.overlay?rt.overlay->instanceStatsOnModelThread():std::vector<lab::FLStandaloneOverlayInstanceStats>{};NSMutableArray* inserts=[NSMutableArray array];
    for(const auto& r:rt.records){NSMutableDictionary* d=[NSMutableDictionary dictionary];
        const auto* controller=r->currentControllerNode.load();
        d[@"controller_association_published"]=@(controller&&publishedControllerOwner(*controller->native,controller->verifiedController.load())==r.get());
        d[@"controller_native_identity"]=@(controller?controller->native->identity:0);
        d[@"controller_accepted_handler_address"]=@(controller?reinterpret_cast<std::uintptr_t>(controller->native->actualNativeComponentHandler.load()):0);
        d[@"controller_pending_handler_address"]=@(controller?reinterpret_cast<std::uintptr_t>(controller->native->actualPendingNativeComponentHandler.load()):0);
        d[@"scoped_normalized_parameter_notifications"]=@(r->nativeNormalizedParameterNotifications.load());
        d[@"scoped_handler_parameter_notifications"]=@(r->nativeHandlerParameterNotifications.load());
        d[@"scoped_input_queue_parameter_notifications"]=@(r->nativeInputQueueParameterNotifications.load());
        d[@"scoped_input_queue_exact_current_cache_echoes"]=@(r->nativeInputQueueExactEchoes.load());d[@"current_cache_parameter_echo_proof_available"]=@(r->parameterEchoSnapshot.load()!=nullptr);
        d[@"initial_wait_notice_shown"]=@(r->initialWaitNoticeShown);
        d[@"native_stop_for_cache_rebuild_pending"]=@(r->nativeStopForRebuildPending.load());d[@"native_stop_requested_epoch"]=@(r->nativeStopRequestedEpoch.load());d[@"actual_native_stops_verified"]=@(r->actualNativeStopsVerified);
        d[@"instance_id"]=@(r->identity.instanceID.c_str());d[@"native_insert"]=@(r->insert.mixerInsert);d[@"bound"]=@YES;
        d[@"static_signature_checked"]=@(r->staticSignatureChecked);d[@"static_signature_valid"]=@(r->staticSignatureValid);d[@"static_signature_status"]=@(r->staticSignatureStatus);
        d[@"static_signature_is_admission_or_license_proof"]=@NO;d[@"plugin_bundle"]=@(r->pluginBundlePath.c_str());
        d[@"factory_entry_image"]=@(r->factoryEntryImagePath.c_str());d[@"factory_entry_image_sha256"]=@(r->factoryEntryImageSHA256.c_str());
        d[@"alive"]=@(r->alive.load());d[@"graph_ready"]=@(r->graphApplied);d[@"disabled"]=@(r->disabled);d[@"cache_rebuild_pending"]=@(r->dirty.load());d[@"initial_native_process_admission_pending"]=@(r->initialPrimePending.load());
        d[@"native_mix_boundary"]=@(nativeMixBoundaryReason(r->lastNativeMixBoundary.reason));d[@"native_mix_last_probe_available"]=@(r->lastNativeMixBoundary.available());
        d[@"native_mix_create"]=@(r->lastNativeMixBoundary.createResult);d[@"native_mix_join"]=@(r->lastNativeMixBoundary.joinResult);
        d[@"native_mix_trylock"]=@(r->lastNativeMixBoundary.tryLockResult);d[@"native_mix_unlock"]=@(r->lastNativeMixBoundary.unlockResult);
        const auto admission=r->native->nativeProcessAdmission.snapshot();
        d[@"native_mix_identity_verified"]=@(r->lastNativeMixBoundary.nativeIdentityVerified);d[@"native_process_admission_allowed"]=@(admission.allowed);
        d[@"native_process_admission_active_count"]=@(admission.activeCount);d[@"native_process_admission_generation"]=@(admission.generation);d[@"native_process_admission_permanently_closed"]=@(admission.permanentlyClosed);
        d[@"native_process_admission_blocked_callbacks"]=@(r->native->blockedProcessCallbacks.load());
        NSMutableArray* setupHistory=[NSMutableArray array];std::uint32_t setupReadyMask=0;
        for(std::size_t i=0;i<r->nativeSetupHistory.size();++i){const auto& event=r->nativeSetupHistory[i];if(!event.ready.load(std::memory_order_acquire))continue;setupReadyMask|=std::uint32_t{1}<<i;
            const bool finite=std::isfinite(event.actualSetup.sampleRate);
            [setupHistory addObject:@{@"sequence":@(event.sequence),@"process_mode":@(event.actualSetup.processMode),@"symbolic_sample_size":@(event.actualSetup.symbolicSampleSize),
                @"max_samples_per_block":@(event.actualSetup.maxSamplesPerBlock),@"sample_rate_finite":@(finite),@"sample_rate":@(finite?event.actualSetup.sampleRate:0.0),
                @"original_result":@(event.originalResult),@"main_thread":@(event.mainThread),@"inside_native_process":@(event.insideNativeProcess),
                @"cache_epoch_before":@(event.epochBefore),@"cache_epoch_after":@(event.epochAfter),@"dirty_before":@(event.dirtyBefore),@"dirty_after":@(event.dirtyAfter),
                @"setup_qualified_before":@(event.qualifiedBefore),@"setup_qualified_after":@(event.qualifiedAfter)}];}
        const auto setupCount=r->nativeSetupDiagnosticCount.load();d[@"native_setup_diagnostic_count"]=@(setupCount);d[@"native_setup_history"]=setupHistory;d[@"native_setup_history_capacity"]=@32;d[@"native_setup_history_ready_mask"]=@(setupReadyMask);
        d[@"native_setup_history_overflow"]=@(setupCount>32?setupCount-32:0);d[@"native_setup_history_count_saturated"]=@(setupCount==std::numeric_limits<std::uint64_t>::max());d[@"native_setup_history_completed_only"]=@YES;
        d[@"native_set_active_calls"]=@(r->native->activationCalls.load());d[@"native_set_processing_calls"]=@(r->native->processingCalls.load());
        d[@"native_set_active_enabled"]=@(r->active.load());d[@"native_set_processing_enabled"]=@(r->processing.load());
        const auto* power=r->nativeSlotPower.load(std::memory_order_acquire);
        d[@"native_slot_power_binding_present"]=@(power!=nullptr);d[@"native_slot_power_registry_index"]=@(r->nativeSlotPowerRegistryIndex);
        d[@"native_slot_power_registered"]=@(r->nativeSlotPowerRegistered);d[@"native_slot_power_seed_attempted"]=@(r->nativeSlotPowerSeedAttempted);
        d[@"native_slot_power_known_once"]=@(r->nativeSlotPowerKnownOnce);d[@"native_slot_power_source_published"]=@(r->nativeSlotPowerSourcePublished);
        d[@"native_slot_power_wrapper"]=@(power?power->wrapper:0);d[@"native_slot_power_actual_component"]=@(power?power->actualComponent:0);
        d[@"native_slot_power_generation"]=@(power?power->generation:0);d[@"native_slot_power_snapshot"]=@(static_cast<std::uint8_t>(power?power->snapshotForBlock():slot_power::Power::Unknown));
        d[@"native_slot_power_closed"]=@(power&&power->closed());d[@"native_slot_power_calls_in_flight"]=@(power?power->callsInFlight():0);
        d[@"native_slot_power_seed_successes"]=@(r->nativeSlotPowerSeedSuccesses);d[@"native_slot_power_seed_refusals"]=@(r->nativeSlotPowerSeedRefusals);
        d[@"native_slot_power_seed_skipped_notifications"]=@(r->nativeSlotPowerSeedSkippedNotifications);d[@"native_slot_power_stop_pending"]=@(r->nativeSlotPowerStopPending.has_value());
        d[@"native_slot_power_stops_verified"]=@(r->nativeSlotPowerStopsVerified);
        d[@"empty_lifecycle_bind_mix_diagnostic"]=@(nativeMixBoundaryReason(r->lifecycleBindingMixDiagnostic.reason));
        d[@"empty_lifecycle_bind_diagnostic_is_model_boundary_proof"]=@NO;
        d[@"prepared_ara_archive_available"]=@(r->preparedNativeSave.has_value());
        d[@"prepared_ara_archive_ticket_authorized"]=@(r->preparedNativeSave&&r->nativeCacheGate->authorizes(r->preparedNativeSave->identity().currentCacheTicket));
        d[@"native_dirty_pending_ara_revision"]=@(r->nativeDirtyPendingRevision.load());d[@"native_dirty_last_requested_revision"]=@(r->nativeDirtyRequestedRevision);
        d[@"native_dirty_dispatcher_requests_returned"]=@(r->nativeDirtyRequestsReturned);d[@"native_dirty_ui_readback_proven"]=@NO;
        d[@"last_native_state_caller"]=@(r->lastNativeStateCaller.c_str());d[@"archive_pending_current_ram"]=@(r->pendingArchive.has_value());d[@"error"]=@(r->error.c_str());
        d[@"cache_published"]=@NO;d[@"cache_authorized"]=@NO;
        for(const auto& cache:cacheStats)if(cache.instanceID==r->identity.instanceID){
            d[@"cache_published"]=@(cache.published);d[@"cache_authorized"]=@(cache.ticketAuthorized);d[@"cache_ticket_epoch"]=@(cache.ticketEpoch);d[@"cache_capture_serial"]=@(cache.captureSerial);d[@"source_stale_hold_active"]=@(cache.staleHoldActive);
            d[@"source_held_callbacks"]=@(cache.counters.heldCallbacks);d[@"source_held_frames"]=@(cache.counters.heldFrames);d[@"source_stale_hold_requests"]=@(cache.counters.staleHoldRequests);
            d[@"source_applied_callbacks"]=@(cache.counters.appliedCallbacks);d[@"source_applied_frames"]=@(cache.counters.appliedFrames);
            d[@"source_native_power_disabled_original_callbacks"]=@(cache.counters.nativePowerDisabledOriginalCallbacks);d[@"source_native_power_disabled_original_frames"]=@(cache.counters.nativePowerDisabledOriginalFrames);
            d[@"source_native_power_unknown_held_callbacks"]=@(cache.counters.nativePowerUnknownHeldCallbacks);d[@"source_native_power_unknown_held_frames"]=@(cache.counters.nativePowerUnknownHeldFrames);
            d[@"source_native_slot_power_required"]=@(cache.nativeSlotPowerRequired);d[@"source_native_slot_power_binding_present"]=@(cache.nativeSlotPowerBindingPresent);
            d[@"source_native_slot_power_wrapper"]=@(cache.nativeSlotPowerWrapper);d[@"source_native_slot_power_component"]=@(cache.nativeSlotPowerActualComponent);
            d[@"source_native_slot_power_generation"]=@(cache.nativeSlotPowerGeneration);d[@"source_native_slot_power_snapshot"]=@(cache.nativeSlotPowerSnapshot);
            d[@"source_rejected_callbacks"]=@(cache.counters.rejected);d[@"source_fault_bits"]=@(cache.counters.faultBits);
            d[@"physical_identity_diagnostic_ready_mask"]=@(cache.physicalIdentityDiagnosticReadyMask);d[@"physical_identity_diagnostic_overflow"]=@(cache.physicalIdentityDiagnosticOverflow);
            d[@"physical_identity_diagnostics"]=physicalIdentityHistoryDiagnosticJSON(cache);d[@"physical_identity_diagnostic_addresses_are_values_only"]=@YES;
        }
        if(r->nativeProcessObserver){process_observer::Snapshot observed;
            if(r->nativeProcessObserver->trySnapshot(observed)){
                NSMutableDictionary* p=[NSMutableDictionary dictionary];p[@"test_device_only"]=@YES;
                p[@"completed_pairs"]=@(observed.completedPairs);p[@"compared_calls"]=@(observed.comparedCalls);p[@"compared_frames"]=@(observed.comparedFrames);
                p[@"compared_samples"]=@(observed.comparedSamples);p[@"nonzero_input_samples"]=@(observed.nonzeroInputSamples);p[@"bit_different_samples"]=@(observed.bitDifferentSamples);
                p[@"max_absolute_difference"]=@(observed.maximumAbsoluteDifference);
                const double count=observed.finiteSamples?double(observed.finiteSamples):1.0;
                p[@"difference_rms"]=@(std::sqrt(observed.differenceSumSquares/count));p[@"input_rms"]=@(std::sqrt(observed.inputSumSquares/count));p[@"output_rms"]=@(std::sqrt(observed.outputSumSquares/count));
                p[@"input_hash_u64"]=@(std::to_string(observed.inputHash).c_str());p[@"output_hash_u64"]=@(std::to_string(observed.outputHash).c_str());
                p[@"native_failures"]=@(observed.nativeFailures);p[@"unsupported_calls"]=@(observed.unsupportedCalls);p[@"identity_failures"]=@(observed.identityFailures);
                p[@"nonfinite_samples"]=@(observed.nonfiniteSamples);p[@"overlapping_before"]=@(observed.overlappingBefore);p[@"rejected_after"]=@(observed.rejectedAfter);
                p[@"frame_limit_skips"]=@(observed.frameLimitSkips);p[@"counter_overflows"]=@(observed.counterOverflows);p[@"silent_flag_contradictions"]=@(observed.silentFlagContradictions);
                d[@"native_process_observation"]=p;
            }
        }
        [inserts addObject:d];}
    NSMutableDictionary* data=[NSMutableDictionary dictionary];data[@"schema"]=@"rysup.fl-ara-runtime-status/1";
    const auto audioTest=nativeAudioTestStats();
    data[@"native_audio_test_presentation"]=@{@"configured":@(audioTest.presentationMuteConfigured),@"native_render_calls":@(audioTest.nativeRenderCalls),
        @"native_rendered_frames":@(audioTest.nativeRenderedFrames),@"native_nonzero_scratch_samples":@(audioTest.nonzeroScratchSamples),@"muted_driver_frames":@(audioTest.mutedFrames),
        @"native_errors":@(audioTest.nativeErrors),@"unsupported_callbacks":@(audioTest.unsupportedCallbacks),@"concurrent_callbacks":@(audioTest.concurrentCallbacks),
        @"unsafe_hardware_buffers":@(audioTest.unsafeHardwareBuffers),@"returned_shape_failures":@(audioTest.returnedShapeFailures),@"counter_overflows":@(audioTest.counterOverflows),
        @"start_refusals":@(audioTest.startRefusals),@"registration_refusals":@(audioTest.registrationRefusals),@"write_only_callback_preparations":@(audioTest.writeOnlyCallbackPreparations),@"critical_property_disarms":@(audioTest.criticalPropertyDisarms),@"configuration_is_render_proof":@NO};
    NSMutableDictionary* presentation=[data[@"native_audio_test_presentation"] mutableCopy];
    presentation[@"callback_calls"]=@(audioTest.callbackCalls);presentation[@"native_nonfinite_scratch_samples"]=@(audioTest.nonfiniteScratchSamples);presentation[@"prepared_units"]=@(audioTest.preparedUnits);
    const auto& outputStart=audioTest.lastOutputStart;
    presentation[@"last_output_start"]=@{@"sequence":@(outputStart.sequence),@"stage":@(outputStart.stage),@"device_id":@(outputStart.deviceID),
        @"current_device_status":@(outputStart.currentDeviceStatus),@"current_device_bytes":@(outputStart.currentDeviceBytes),
        @"uid_read_attempted":@(outputStart.uidReadAttempted),@"uid_status":@(outputStart.uidStatus),@"uid_bytes":@(outputStart.uidBytes),@"uid_matched":@(outputStart.uidMatched),
        @"transport_read_attempted":@(outputStart.transportReadAttempted),@"transport_status":@(outputStart.transportStatus),@"transport_bytes":@(outputStart.transportBytes),@"virtual_transport_matched":@(outputStart.virtualTransportMatched),
        @"original_invoked":@(outputStart.originalInvoked),@"original_status":@(outputStart.originalStatus)};
    const auto& outputRegistration=audioTest.lastRegistration;
    presentation[@"last_registration"]=@{@"stage":@(outputRegistration.stage),@"reason":@(outputRegistration.reason),@"property":@(outputRegistration.property),
        @"scope":@(outputRegistration.scope),@"element":@(outputRegistration.element),@"bytes":@(outputRegistration.bytes),
        @"callback_source":@(outputRegistration.callbackSource),@"callback_image_offset":@(outputRegistration.callbackImageOffset),
        @"original_status":@(outputRegistration.originalStatus),@"record_present":@(outputRegistration.recordPresent),@"claim_held":@(outputRegistration.claimHeld),
        @"original_invoked":@(outputRegistration.originalInvoked),@"coherent":@(outputRegistration.coherent),@"sequence":@(outputRegistration.sequence),@"dropped":@(outputRegistration.dropped)};
    const auto& callbackMetadata=audioTest.callbackMetadata;
    presentation[@"callback_metadata"]=@{@"registered_scope":@(callbackMetadata.registeredScope),@"registered_element":@(callbackMetadata.registeredElement),
        @"route_getter_status":@(callbackMetadata.routeGetterStatus),@"route_getter_bytes":@(callbackMetadata.routeGetterBytes),
        @"route_info_status":@(callbackMetadata.routeInfoStatus),@"route_info_bytes":@(callbackMetadata.routeInfoBytes),@"route_writable":@(callbackMetadata.routeWritable),
        @"input_getter_status":@(callbackMetadata.inputGetterStatus),@"input_getter_bytes":@(callbackMetadata.inputGetterBytes),
        @"input_info_status":@(callbackMetadata.inputInfoStatus),@"input_info_bytes":@(callbackMetadata.inputInfoBytes),@"input_writable":@(callbackMetadata.inputWritable),
        @"output_alias":@(callbackMetadata.outputAlias),@"accepted":@(callbackMetadata.accepted),@"coherent":@(callbackMetadata.coherent),
        @"route_queried":@(callbackMetadata.routeQueried),@"input_queried":@(callbackMetadata.inputQueried),@"sequence":@(callbackMetadata.sequence),@"dropped":@(callbackMetadata.dropped)};
    presentation[@"apple_device_profile_admitted"]=@(audioTest.appleDeviceProfileAdmitted);
    presentation[@"device_start_entries"]=@(audioTest.deviceStartEntries);presentation[@"device_start_at_time_entries"]=@(audioTest.deviceStartAtTimeEntries);
    presentation[@"device_start_original_calls"]=@(audioTest.deviceStartOriginalCalls);presentation[@"device_start_original_failures"]=@(audioTest.deviceStartOriginalFailures);
    presentation[@"device_start_refusals"]=@(audioTest.deviceStartRefusals);presentation[@"io_proc_create_entries"]=@(audioTest.ioProcCreateEntries);
    presentation[@"io_proc_create_original_failures"]=@(audioTest.ioProcCreateOriginalFailures);presentation[@"io_proc_capture_suppressed"]=@(audioTest.ioProcCaptureSuppressed);
    presentation[@"io_proc_destroy_entries"]=@(audioTest.ioProcDestroyEntries);presentation[@"io_proc_destroy_original_failures"]=@(audioTest.ioProcDestroyOriginalFailures);
    presentation[@"lifecycle_refusals"]=@(audioTest.lifecycleRefusals);presentation[@"sink_lifecycle_refusals"]=@(audioTest.sinkLifecycleRefusals);
    const auto& unitGate=audioTest.unitDeviceGate;
    presentation[@"unit_device_gate"]=@{@"scopes_begun":@(unitGate.scopesBegun),@"scope_refusals":@(unitGate.scopeRefusals),@"creates_captured":@(unitGate.createsCaptured),
        @"create_refusals":@(unitGate.createRefusals),@"starts_allowed":@(unitGate.startsAllowed),@"start_refusals":@(unitGate.startRefusals),@"revocations":@(unitGate.revocations),
        @"last_reason":@(unitGate.lastReason),@"last_device":@(unitGate.lastDevice),@"last_stage":@(unitGate.lastStage),@"poisoned":@(unitGate.poisoned)};
    const auto& deviceIO=audioTest.lastDeviceIO;
    presentation[@"last_device_io"]=@{@"sequence":@(deviceIO.sequence),@"caller_offset":@(deviceIO.callerOffset),@"callback_offset":@(deviceIO.callbackOffset),
        @"api":@(deviceIO.api),@"stage":@(deviceIO.stage),@"device_id":@(deviceIO.deviceID),@"pair":@(deviceIO.pair),@"caller_source":@(deviceIO.callerSource),@"callback_source":@(deviceIO.callbackSource),
        @"original_status":@(deviceIO.originalStatus),@"original_invoked":@(deviceIO.originalInvoked),@"captured":@(deviceIO.captured),@"suppressed":@(deviceIO.suppressed)};
    data[@"native_audio_test_presentation"]=presentation;
    data[@"admitted_source_profile"]=@{@"sample_rate":@48000,@"channels":@[@1,@2],@"maximum_source_seconds":@300,
        @"maximum_pcm_bytes_per_source":@(64u*1024u*1024u),@"maximum_pcm_bytes_total":@(128u*1024u*1024u),
        @"maximum_source_cache_bytes":@(256u*1024u*1024u),@"reported_latency_head_tail_must_be_zero":@YES,@"limits_are_not_runtime_coverage":@YES};
    data[@"runtime_helper_admitted"]=@(app.admitted);data[@"native_engine_ready"]=@(app.engineReady);data[@"hooks_installed"]=@(rt.hooksInstalled);
    data[@"native_project_save_guard"]=@{@"initialized":@(rt.nativeSaveGuardInitialized),@"ready":@(nativeSaveGuardReady()),
        @"protected_unbound_archive_live":@(hasLiveProtectedNativeArchive()),@"record_tracking_incomplete":@(rt.nativeInstanceTrackingIncomplete.load()),
        @"preflights":@(rt.nativeSavePreflights),@"preflights_allowed":@(rt.nativeSavePreflightsAllowed),@"precommits_allowed":@(rt.nativeSavePrecommitsAllowed),
        @"envelopes_serialized":@(rt.nativeSaveEnvelopes),@"native_frames_finished":@(rt.nativeSaveFramesFinished),@"active_owner_freeze":@(rt.nativeSaveFreeze!=nullptr),
        @"last_error":@(rt.nativeSaveError.c_str()),@"whole_file_runtime_save_proven":@NO};
    const auto powerObserver=slot_power::nativeSlotPowerObserverStats();
    data[@"native_slot_power_observer"]=@{@"installed":@(powerObserver.installed),@"protection_restored":@(powerObserver.protectionRestored),
        @"ready":@(powerObserver.installed&&powerObserver.protectionRestored),@"entries":@(powerObserver.entries),@"in_flight":@(powerObserver.inFlight),
        @"id11_entries":@(powerObserver.id11Entries),@"normal_returns":@(powerObserver.normalReturns),@"install_reason":@(static_cast<std::uint32_t>(powerObserver.lastInstallReason))};
    data[@"controller_state_association_checks_passed"]=@(rt.separateControllerAssociationsVerified);
    data[@"native_observed_state_dirty_requests_returned"]=@(rt.nativeObservedStateDirtyRequestsReturned);
    const auto sourceGlobalCounters=rt.overlay?rt.overlay->counters():lab::FLStandaloneOverlayCounters{};
    data[@"source_overlay_global_counters"]=@{@"qualified_preview_unrelated_callbacks":@(sourceGlobalCounters.qualifiedPreviewUnrelatedCallbacks)};
    data[@"source_overlay_installed"]=@(rt.overlayInstalled);data[@"inserts"]=inserts;data[@"error"]=@(rt.error.c_str());
    NSMutableArray* ownWindows=[NSMutableArray array];
    for(NSWindow* window in NSApp.windows){NSMutableDictionary* item=[NSMutableDictionary dictionary];
        NSRect frame=window.frame;item[@"frame"]=@{@"x":@(frame.origin.x),@"y":@(frame.origin.y),@"width":@(frame.size.width),@"height":@(frame.size.height)};
        item[@"backing_scale_factor"]=@(window.backingScaleFactor);
        if(window.screen){NSRect screenFrame=window.screen.frame;item[@"screen_frame"]=@{@"x":@(screenFrame.origin.x),@"y":@(screenFrame.origin.y),@"width":@(screenFrame.size.width),@"height":@(screenFrame.size.height)};item[@"screen_backing_scale_factor"]=@(window.screen.backingScaleFactor);}
        item[@"title"]=window.title?:@"";item[@"visible"]=@(window.visible);item[@"miniaturized"]=@(window.miniaturized);
        item[@"key"]=@(window.keyWindow);item[@"main"]=@(window.mainWindow);item[@"document_edited"]=@(window.documentEdited);
        item[@"window_number"]=@(window.windowNumber);item[@"represented_url"]=window.representedURL.absoluteString?:@"";[ownWindows addObject:item];}
    const auto registration=registrationReadStats();
    data[@"registration_read_through"]=@{@"initialized":@(registration.initialized),@"branch_present":@(registration.registrationBranchPresent),
        @"init_stage":@(static_cast<std::uint32_t>(registration.stage)),@"init_errno":@(registration.immediateErrno),@"source_bytes_read":@(registration.bytesRead),
        @"xml_error_code":@(registration.xmlErrorCode),@"xml_error_line":@(registration.xmlErrorLine),@"xml_error_column":@(registration.xmlErrorColumn),
        @"parsed_registration_node_count":@(registration.parsedRegistrationNodeCount),@"parsed_registration_value_count":@(registration.parsedRegistrationValueCount),
        @"value_count":@(registration.registrationValueCount),@"read_count":@(registration.registrationReads),@"denied_write_count":@(registration.deniedRegistrationWrites),
        @"vendor_license_validity_proven":@NO};
    NSScreen* primary=NSApp?NSScreen.screens.firstObject:nil;if(primary){NSRect frame=primary.frame;data[@"primary_screen_frame"]=@{@"x":@(frame.origin.x),@"y":@(frame.origin.y),@"width":@(frame.size.width),@"height":@(frame.size.height)};data[@"primary_screen_backing_scale_factor"]=@(primary.backingScaleFactor);}
    data[@"window_geometry_appkit_coordinates"]=@YES;
    data[@"own_windows_diagnostic_only"]=ownWindows;data[@"own_application_active"]=@(NSApp.active);
    data[@"window_metadata_is_native_graph_proof"]=@NO;
    data[@"runtime_qualified"]=@NO;data[@"run_id"]=@(app.runID.c_str());data[@"pid"]=@(getpid());
    NSData* json=[NSJSONSerialization dataWithJSONObject:data options:NSJSONWritingPrettyPrinted error:nil];
    return json&&[json writeToFile:@((app.runDirectory+"/fl-ara-status.json").c_str()) atomically:YES];
}
// Power uncertainty is a separate STOP reason. It carries no cache ticket
// and must never be converted into an edit, invalidation or source rebuild.
bool nativeSlotPowerHoldMatchesRecord(const BridgeRecord& r,const lab::FLStandaloneOverlayStaleHoldRecord& hold)noexcept{
    if(hold.reason!=lab::FLStandaloneOverlayHoldReason::NativeSlotPowerUnknown||!r.alive||r.teardown||r.disabled||!r.native
        ||r.identity.instanceID!=hold.instanceID||r.identity.actualComponent!=hold.actualNativeComponent
        ||r.native->ownerData.load()!=&r||!r.native->nativeAlive||r.native->nativeTerminated
        ||reinterpret_cast<std::uintptr_t>(r.native->actualComponent.load())!=hold.actualNativeComponent)return false;
    const auto* binding=r.nativeSlotPower.load(std::memory_order_acquire);
    if(binding!=hold.nativeSlotPowerBinding)return false;
    if(!binding)return hold.nativeSlotPowerBindingMissing;
    return !hold.nativeSlotPowerBindingMissing&&binding->actualComponent==hold.actualNativeComponent
        &&binding->wrapper==hold.nativeSlotPowerWrapper&&binding->wrapper==r.insert.effectWrapper
        &&binding->generation==hold.nativeSlotPowerGeneration&&binding->snapshotForBlock()==slot_power::Power::Unknown;
}
bool nativeSlotPowerHoldStillCurrent(const BridgeRecord& r,const lab::FLStandaloneOverlayStaleHoldRecord& hold){
    return nativeSlotPowerHoldMatchesRecord(r,hold)&&runtime().overlay
        &&runtime().overlay->nativeSlotPowerHoldStillUnknownOnModelThread(hold);
}
void drainNativeSlotPowerStop(BridgeRecord& r){
    if(!r.nativeSlotPowerStopPending)return;
    const auto& hold=*r.nativeSlotPowerStopPending;
    if(!nativeSlotPowerHoldStillCurrent(r,hold)){r.nativeSlotPowerStopPending.reset();return;}
    std::int32_t raw=-1;
    if(!readActualNativeTransportState(raw)||(raw!=0&&raw!=1)){
        r.error="Native slot power is unknown; STOP awaits a verified native transport";return;
    }
    const bool wasPlaying=raw==1;
    if(wasPlaying){
        // Immediately before STOP: completed notification, replacement or ended
        // component discards this request without changing cache authority.
        if(!nativeSlotPowerHoldStillCurrent(r,hold)){r.nativeSlotPowerStopPending.reset();return;}
        if(!requestActualNativePlaying(r.insert,false)){
            r.error="Native slot power is unknown; the native STOP request was refused";return;
        }
    }
    if(!readActualNativeTransportState(raw)||raw!=0){
        r.error="Native slot power is unknown; the native STOP request was not verified";return;
    }
    r.nativeSlotPowerStopPending.reset();
    if(wasPlaying){++r.nativeSlotPowerStopsVerified;runtime().nativeSlotPowerStopNoticeQueued=true;
        writeNativeApplicationEvent("actual_native_slot_power_unknown_stopped",r.identity.instanceID);}
}
void tick(const NativeApplication& app,void*)noexcept{
    if(!pthread_main_np()||!nativeApplicationAdmitted())return;
    if(headlessNativeExportInFlight())return; // Native export may pump a nested run loop.
    pollNativeSaveGuardTransactions();
    // A vendor may pump a nested run loop while its native Save frame owns a
    // foreign lock. Never enter any model/cache/ARA SDK operation there.
    if(currentNativeSaveTransaction())return;
    auto& rt=runtime();if(rt.inTick)return;rt.inTick=true;
    struct Reset{bool& flag;~Reset(){flag=false;}} reset{rt.inTick};
    try{
        // Publish actual engine readiness before a source refresh or modal can
        // occupy the model thread. No graph/ARA qualification is inferred.
        if(!rt.nativeReadyStatusPublished&&app.admitted&&app.engineReady)rt.nativeReadyStatusPublished=status();
        if(rt.unboundArchiveRefusals.exchange(0)){
            rt.error="This native component is not ARA-bound. Its ARA archive was refused unchanged; component saving is blocked to preserve it";
            writeNativeApplicationEvent("unbound_native_ara_archive_refused",rt.error);
            NSAlert* alert=[NSAlert new];alert.messageText=@"ARA project state could not be restored";alert.informativeText=@(rt.error.c_str());
            [alert addButtonWithTitle:@"Keep session open"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(rt.unsupportedOffMainARAInitializations.exchange(0)){
            rt.error="An actual native ARA component initialized off the main thread; ARA binding is unsupported for that component in this run";
            writeNativeApplicationEvent("actual_native_ara_offmain_initialize_refused",rt.error);
            NSAlert* alert=[NSAlert new];alert.messageText=@"ARA unavailable for this native component";alert.informativeText=@(rt.error.c_str());
            [alert addButtonWithTitle:@"Keep session open"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(!rt.overlay&&app.engineReady){lab::FLStandaloneOverlayConfig config;config.requireNativeSlotPower=true;config.actualEngineImageBase=app.engine.imageBase;
            config.exactLoadedEnginePath=app.enginePath;config.exactEngineArm64SliceSHA256=app.engine.verifiedArm64SliceSHA256;
            config.confirmAlreadyAdmittedOwnedProcess=admitted;config.confirmStoppedDrainedNativeModelBoundary=allBoundary;config.ownerFaultQueued=&rt.overlayFault;config.ownerStaleHoldQueued=&rt.overlayStaleHoldQueued;
            rt.overlay=std::make_unique<lab::FLStandaloneSourceOverlay>(std::move(config));
            rt.overlayListener.store(rt.overlay.get(),std::memory_order_release);

        }
        if(rt.overlayFault.exchange(false)&&rt.overlay)for(const auto& f:rt.overlay->takeFaultsOnModelThread())if(auto* r=rt.byID(f.instanceID)){
            using Fault=lab::FLStandaloneOverlayFault;
            const auto bit=[](Fault value){return std::uint64_t{1}<<static_cast<unsigned>(value);};
            const auto refreshable=bit(Fault::RecordIdentity)|bit(Fault::PhysicalIdentity)|bit(Fault::PointerRelation)|bit(Fault::NativeSourceMetadata);
            if(f.faultBits&&(f.faultBits&~refreshable)==0){
                // The first native read after a real Playlist/source edit closes
                // its ticket immediately. Rebuild only at the next actual stop.
                r->dirty=true;r->error="Native Playlist/source changed; ARA source cache awaits actual stopped refresh";
                rt.coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);
            }else fail(*r,"actual native source read violates this source-overlay profile");
        }
        if(rt.overlayStaleHoldQueued.exchange(false)&&rt.overlay)for(const auto& hold:rt.overlay->takeStaleHoldsOnModelThread())if(auto* r=rt.byID(hold.instanceID)){
            if(hold.reason==lab::FLStandaloneOverlayHoldReason::NativeSlotPowerUnknown){
                if(nativeSlotPowerHoldStillCurrent(*r,hold))r->nativeSlotPowerStopPending=hold;
                continue; // No dirty/cache authority/rebuild operation for power.
            }
            if(!r->alive||r->identity.actualComponent!=hold.actualNativeComponent||currentNativeCacheGenerationCoherent(*r))continue;
            // hold.ticketEpoch identifies the STALE PUBLISHED ticket. Its
            // difference from gate.currentTicket() is expected, not obsolescence.
            r->dirty=true;queueNativeStopForCacheRebuild(*r,hold.ticketEpoch);
        }
        for(auto& owned:rt.records)drainNativeSlotPowerStop(*owned);
        for(auto& owned:rt.records){auto& r=*owned;if(!r.alive||r.teardown||!r.nativeStopForRebuildPending.exchange(false))continue;
            if(currentNativeCacheGenerationCoherent(r))continue;
            std::int32_t raw=-1;
            if(!readActualNativeTransportState(raw)||(raw!=0&&raw!=1)){
                r.error="ARA cache hold awaits a verified stopped native transport";r.nativeStopForRebuildPending=true;continue;
            }
            const bool wasPlaying=raw==1;
            if((wasPlaying&&!requestActualNativePlaying(r.insert,false))||!readActualNativeTransportState(raw)||raw!=0){
                r.error="ARA cache is held; the actual native STOP request was not verified";r.nativeStopForRebuildPending=true;continue;
            }
            if(wasPlaying){++r.actualNativeStopsVerified;rt.nativeStopNoticeQueued=true;writeNativeApplicationEvent("actual_native_ara_cache_hold_stopped",r.identity.instanceID);}
            r.dirty=true; // Remain stopped through the qualified cache rebuild.
        }
        for(auto& owned:rt.records){auto& r=*owned;if(!r.alive)continue;
            if(r.fault.exchange(false)){if(!r.disabled)fail(r,"ARA/native contract fault; this insert is unavailable");
                NSAlert* alert=[NSAlert new];alert.messageText=@"ARA unavailable for this FL insert";alert.informativeText=@(r.error.c_str());
                [alert addButtonWithTitle:@"Keep session open"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];r.fault=false;}
            if(r.disabled||!r.setupQualified||!stopped())continue;
            // An Unknown power notification alone never turns a coherent ARA
            // cache into model work, including an irreversibly closed binding.
            const auto* power=r.nativeSlotPower.load(std::memory_order_acquire);
            if(power&&power->snapshotForBlock()==slot_power::Power::Unknown&&currentNativeCacheGenerationCoherent(r))continue;
            try{Boundary boundary(r);ensureOverlayAtModelBoundary();
                refreshAtBoundary(r,true);rt.overlay->collectRetiredOnModelThread();}
            catch(const lab::NativeModelBoundaryUnavailable& e){r.error=std::string("Native model boundary deferred: ")+e.what();if(!currentNativeCacheGenerationCoherent(r))r.dirty=true;}
            catch(const lab::SourceCacheNotReady& e){r.error=e.what();r.dirty=true;}
            catch(const lab::FLStandaloneDecodeError& e){
                if(!r.graphApplied){r.error=std::string("ARA is waiting for supported current native source regions: ")+e.what();r.dirty=true;}
                else fail(r,e.what());
            }
            catch(const std::exception& e){fail(r,e.what());}
            catch(...){fail(r,"unknown native SDK exception at stopped model boundary");}
        }
        // A genuine-entry/protected-envelope publication may lose the guard's
        // precommit failure CAS because commit has already begun. Announce it
        // conservatively after the native frame, never claim the old Save
        // included it. Clear before dispatch so concurrent publication survives.
        for(auto* p=rt.observedNativeInstances.load();p;p=p->next.load()){
            auto& n=*p->native;if(!n.nativeAlive||n.nativeTerminated||!(n.ownerStateFlags.load()&nativeObservedStateDirtyPending))continue;
            n.ownerStateFlags.fetch_and(~nativeObservedStateDirtyPending);NativeInsert target;bool resolved=false;
            if(n.nativeInitialized){
                if(auto* component=n.actualComponent.load())resolved=resolveActualNativeInsert(reinterpret_cast<std::uintptr_t>(component),reinterpret_cast<std::uintptr_t>(n.actualInitializeContext.load()),target);
                else if(auto* controller=n.actualController.load())resolved=resolveActualNativeController(reinterpret_cast<std::uintptr_t>(controller),reinterpret_cast<std::uintptr_t>(n.actualInitializeContext.load()),target);
            }
            if(resolved&&requestActualNativeProjectDirty(target))++rt.nativeObservedStateDirtyRequestsReturned;
            else n.ownerStateFlags.fetch_or(nativeObservedStateDirtyPending);
        }
        for(auto& node:rt.records){auto& r=*node;if(!r.alive||r.teardown||r.disabled)continue;
            const auto pending=r.nativeDirtyPendingRevision.load();
            if(pending>r.nativeDirtyRequestedRevision||r.nativeDirtyRetryAfterSaveRefusal){
                if(requestActualNativeProjectDirty(r.insert)){r.nativeDirtyRequestedRevision=pending;r.nativeDirtyRetryAfterSaveRefusal=false;++r.nativeDirtyRequestsReturned;}
            }
        }
        std::string initialWaitReason;
        for(auto& node:rt.records){auto& r=*node;if(r.alive&&!r.teardown&&!r.disabled&&r.initialPrimePending&&r.native->blockedProcessCallbacks.load()>0&&!r.initialWaitNoticeShown){
            r.initialWaitNoticeShown=true;if(initialWaitReason.empty())initialWaitReason=r.error.empty()?"Source audio preparation is still pending for this insert.":r.error;
        }}
        if(!initialWaitReason.empty()){
            NSAlert* alert=[NSAlert new];alert.messageText=@"ARA is still preparing an insert";
            alert.informativeText=@((std::string("Playback for this insert is held until its source audio is ready. Stop playback to let preparation finish. ")+initialWaitReason).c_str());
            [alert addButtonWithTitle:@"Keep session open"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(rt.nativeStopNoticeQueued){rt.nativeStopNoticeQueued=false;
            NSAlert* alert=[NSAlert new];alert.messageText=@"Playback stopped for an ARA edit";
            alert.informativeText=@"Native FL playback was stopped while changed ARA source caches rebuild. Playback remains stopped; press Play when the insert is ready.";
            [alert addButtonWithTitle:@"Keep stopped"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(rt.nativeSlotPowerStopNoticeQueued){rt.nativeSlotPowerStopNoticeQueued=false;
            NSAlert* alert=[NSAlert new];alert.messageText=@"Playback stopped while an FL effect changes power";
            alert.informativeText=@"ARA playback is held until FL reports this effect slot's enabled state. The prepared ARA audio is preserved.";
            [alert addButtonWithTitle:@"Keep stopped"];status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(rt.nativeSaveAlertQueued){rt.nativeSaveAlertQueued=false;
            NSAlert* alert=[NSAlert new];alert.messageText=@"FL project save was refused to preserve ARA state";
            alert.informativeText=@(rt.nativeSaveError.c_str());[alert addButtonWithTitle:@"Keep session open"];
            status();if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];}
        if(++rt.ticks%10==0)status();
        bool renderReady=!rt.records.empty()&&!rt.nativeSaveFreeze&&stopped();
        for(const auto& owned:rt.records)if(!currentNativeCacheGenerationCoherent(*owned))renderReady=false;
        pollHeadlessNativeExport(renderReady);

    }catch(const std::exception& e){rt.error=e.what();writeNativeApplicationEvent("ara_app_bridge_tick_refused",e.what());status();}
    catch(...){rt.error="Unknown native SDK exception in app bridge";writeNativeApplicationEvent("ara_app_bridge_tick_refused",rt.error);}
}
void onAdmitted(const NativeApplication& app,void*)noexcept{
    try{auto& rt=runtime();const char* testUID=getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID");rt.observeNativeDSP=testUID&&!std::strcmp(testUID,"Pro Tools Audio Bridge 2-A_UID");rt.adapter=std::make_shared<Adapter>();rt.coordinator=std::make_unique<lab::StandaloneARACoordinator>(app.runID,rt.adapter);
        hooks::Config config;config.exactOwnedExecutablePath=app.executablePath;config.exactOwnedExecutableSHA256=app.executableSHA256;
        config.exactOwnedProcessID=app.processID;config.confirmAlreadyAdmittedOwnedProcess=admitted;
        config.callbacks.moduleObserved=nativeModuleObserved;config.callbacks.factoryObserved=nativeFactoryObserved;config.callbacks.interfaceObserved=nativeInterfaceObserved;
        config.callbacks.finalReleaseObserved=finalNativeRelease;config.callbacks.instanceCreated=instanceCreated;config.callbacks.initialized=initialized;config.callbacks.state=state;
        config.callbacks.setActiveObserved=setActive;config.callbacks.setupProcessingObserved=setup;config.callbacks.setProcessingObserved=processing;
        config.callbacks.beforeTerminate=beforeTerminate;config.callbacks.terminated=terminated;config.callbacks.fault=fault;
        config.callbacks.controllerHandlerObserved=controllerHandlerObserved;config.callbacks.paramEditObserved=parameterEdit;config.callbacks.paramNormalizedObserved=parameterNormalized;config.callbacks.inputParameterChangesObserved=inputParameterChanges;
        if(rt.observeNativeDSP)config.callbacks.nativeProcessTelemetry=nativeProcessTelemetry;
        const auto result=hooks::install(config);rt.hooksInstalled=result.installed;demand(result.installed,result.reason);status();
    }catch(const std::exception& e){runtime().error=e.what();writeNativeApplicationEvent("ara_app_helper_install_failed",e.what());}
    catch(...){runtime().error="Unknown exception installing native bridge";writeNativeApplicationEvent("ara_app_helper_install_failed",runtime().error);}
}
bool beforeOriginalCreate(const NativeApplication& app,void*)noexcept{
    try{auto& rt=runtime();
        demand(app.startupOwnershipVerified&&app.admitted&&app.engine.imageBase&&rt.hooksInstalled,
            "owned native engine code/hooks are unavailable before Create");
        // dlsym has now provided the exact loaded engine binding and code pins.
        // Admission runs earlier, when this binding intentionally does not exist.
        NativeSaveGuardCallbacks saveCallbacks;saveCallbacks.preflight=nativeSavePreflight;saveCallbacks.precommit=nativeSavePrecommit;
        saveCallbacks.rejected=nativeSaveRejected;saveCallbacks.finished=nativeSaveFinished;saveCallbacks.hasLiveARA=hasLiveNativeARA;
        rt.nativeSaveGuardInitialized=initializeNativeSaveGuard(saveCallbacks);
        demand(rt.nativeSaveGuardInitialized&&nativeSaveGuardReady(),
            "required native project-save startup protection is unavailable");
        writeNativeApplicationEvent("native_project_save_startup_guard_ready");return true;
    }catch(const std::exception& e){runtime().error=e.what();writeNativeApplicationEvent("native_project_save_startup_guard_unavailable",e.what());return false;}
    catch(...){return false;}
}
__attribute__((constructor)) static void loadBridge(){
    @autoreleasepool{NativeApplicationCallbacks callbacks;callbacks.admitted=onAdmitted;callbacks.beforeOriginalCreate=beforeOriginalCreate;callbacks.tick=tick;
        const bool initialized=initializeNativeApplication(callbacks);
        if(!initialized&&nativeApplication().startupOwnershipVerified)_exit(78);}
}
} // namespace rysup::fl_ara::app_bridge
