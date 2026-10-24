// Owned-copy normal FL application bridge. No vendor binaries are redistributed.
// Every model operation is stopped, component-process gated and render-drained.
#include "FLNativeApplication.h"
#include "FLVST3NativeHooks.h"
#include "FLStandaloneSourceOverlay.h"
#include "FLNativeProcessObserver.h"
#include "FLNativeMixBoundary.h"
#include "RegistrationReadThrough.h"
#include "../host/FLStandaloneGraphDecoder.h"
#include "../host/FLStandaloneStateEnvelope.h"
#include <ARA_API/ARAVST3.h>
#import <AppKit/AppKit.h>
#include <Security/Security.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
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
struct BridgeRecord {
    hooks::InstanceRecord* native=nullptr;
    NativeInsert insert;
    lab::StandaloneInstanceIdentity identity;
    std::atomic<BridgeRecord*> next{nullptr};
    std::atomic<bool> dirty{true},fault{false},alive{true},initialPrimePending{true};
    NativeMixBoundary lastNativeMixBoundary,lifecycleBindingMixDiagnostic;bool modelMixBoundaryAvailable=false;
    bool disabled=false,boundary=false;std::atomic<bool> active{false},processing{false},observedInactive{false},observedStopped{false};
    std::atomic<bool> setupQualified{false};bool graphApplied=false,teardown=false,pendingPlainLoad=false,synchronousTerminating=false,teardownCallerVerified=false;
    std::atomic<double> sampleRate{0};std::string error,appliedGraph,appliedModel,lastNativeStateCaller;
    std::optional<lab::FLStandaloneDecodedGraph> decoded;
    std::shared_ptr<const lab::FLNativeModelSnapshot> copied;
    std::shared_ptr<lab::StandaloneCacheGate> nativeCacheGate;
    std::unique_ptr<process_observer::NativeProcessObserver> nativeProcessObserver;
    bool staticSignatureChecked=false,staticSignatureValid=false;
    std::int32_t staticSignatureStatus=0;
    std::string pluginBundlePath,factoryEntryImagePath,factoryEntryImageSHA256;
    std::optional<lab::StandaloneSavedEditorArchive> pendingArchive;
    std::optional<lab::Bytes> pendingOrdinaryComponentState;
    lab::StandaloneNativeStateLoadAuthority pendingLoad;
    std::uint64_t nativeLoadSerial=0;
};
struct Runtime;
Runtime& runtime();
struct Adapter final:lab::StandaloneOwnerAdapter,lab::StandaloneArchiveSourceValidator {
    bool onActualModelThread()const noexcept override{return pthread_main_np()!=0;}
    lab::StandaloneNativeHold verifyActualNativeHold(const lab::StandaloneInstanceIdentity&)const override;
    lab::StandaloneNativeTeardownBoundary verifyActualNativeTeardownBoundary(const lab::StandaloneInstanceIdentity&)const override;
    lab::StandaloneSnapshotData copyActualCurrentRAMAndClock(const lab::StandaloneInstanceIdentity&)override;
    void invalidateActualNativeOverlayImmediately(const std::string&,std::uint64_t)noexcept override;
    void enqueueModelThreadRebuild(const std::string&)noexcept override;
    void showARAUnavailableForInsert(const std::string&,const std::string&)noexcept override;
    lab::StandaloneArchiveRestoreValidation validateActualCurrentRAMForArchive(
        const lab::StandaloneSavedEditorArchive&,const lab::StandaloneInstanceIdentity&)override;
};
struct Runtime {
    std::shared_ptr<Adapter> adapter;
    std::unique_ptr<lab::StandaloneARACoordinator> coordinator;
    std::unique_ptr<lab::FLStandaloneSourceOverlay> overlay;
    std::atomic<lab::FLStandaloneSourceOverlay*> overlayListener{nullptr};
    std::atomic<std::uint64_t> unsupportedOffMainARAInitializations{0},unboundArchiveRefusals{0};
    std::atomic<BridgeRecord*> head{nullptr};
    std::vector<std::unique_ptr<BridgeRecord>> records;
    std::atomic<bool> overlayFault{false};
    bool hooksInstalled=false,overlayInstalled=false,inTick=false,anyBoundary=false,observeNativeDSP=false;
    std::string error;std::uint64_t ticks=0;
    BridgeRecord* byID(const std::string& id)const noexcept{
        for(auto* r=head.load();r;r=r->next.load())if(r->identity.instanceID==id)return r;return nullptr;
    }
};
// Intentionally process lifetime: native record callbacks may outlive app teardown.
Runtime& runtime(){static auto* r=new Runtime;return *r;}
BridgeRecord* record(hooks::InstanceRecord& n)noexcept{return static_cast<BridgeRecord*>(n.ownerData.load());}
bool admitted(void*)noexcept{return nativeApplicationAdmitted();}
bool stopped(){std::int32_t value=-1;return readActualNativeTransportState(value)&&value==0;}
bool allBoundary(void*)noexcept{return runtime().anyBoundary&&pthread_main_np()&&stopped();}
class NativeMixBoundaryDeferred final:public lab::NativeModelBoundaryUnavailable {public:using lab::NativeModelBoundaryUnavailable::NativeModelBoundaryUnavailable;};
void observeAndRequireNativeMixBoundary(BridgeRecord& r){
    r.modelMixBoundaryAvailable=false;r.lastNativeMixBoundary=observeActualNativeMixBoundary();
    if(!r.lastNativeMixBoundary.available()){
        r.dirty=true;throw NativeMixBoundaryDeferred(nativeMixBoundaryReason(r.lastNativeMixBoundary.reason));
    }
    r.modelMixBoundaryAvailable=true;
}
struct Boundary {
    BridgeRecord& r;bool previous=false;
    explicit Boundary(BridgeRecord& value):r(value){
        demand(pthread_main_np()&&nativeApplicationAdmitted()&&stopped(),"actual normal FL owner must be stopped on main thread");
        demand(!r.boundary&&r.alive&&!r.teardown&&revalidateActualNativeInsert(r.insert),"native insert boundary owner is unavailable/reentrant");
        previous=r.native->nativeProcessAllowed.exchange(false);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(r.native->actualNativeProcessCallsInFlight.load()){
            if(std::chrono::steady_clock::now()>deadline){r.native->nativeProcessAllowed=previous;throw std::runtime_error("actual native process drain timed out");}
            std::this_thread::yield();
        }
        try{observeAndRequireNativeMixBoundary(r);}catch(...){r.native->nativeProcessAllowed=!r.initialPrimePending.load()&&previous;throw;}
        r.boundary=true;runtime().anyBoundary=true;
    }
    ~Boundary(){r.boundary=false;r.modelMixBoundaryAvailable=false;runtime().anyBoundary=false;
        r.native->nativeProcessAllowed=!r.initialPrimePending.load()&&r.alive.load()&&!r.teardown;}

};
struct TeardownBoundary {
    BridgeRecord& r;
    explicit TeardownBoundary(BridgeRecord& value,bool actualCallerVerified):r(value){
        demand(pthread_main_np()&&nativeApplicationAdmitted()&&actualCallerVerified,"actual native pre-terminate caller/main/run proof missing");
        demand(!r.boundary&&!r.synchronousTerminating&&r.alive&&!r.teardown&&!r.active&&!r.processing
            &&r.observedInactive&&r.observedStopped,"native component has not actually stopped/deactivated before its original terminate");
        demand(r.native&&r.native->ownerData.load()==&r&&r.native->actualComponent.load()==reinterpret_cast<Steinberg::Vst::IComponent*>(r.identity.actualComponent)
            &&r.native->nativeAlive&&r.native->nativeInitialized&&!r.native->nativeTerminated,"exact known native component/lifecycle identity missing");
        r.native->nativeProcessAllowed=false;
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(r.native->actualNativeProcessCallsInFlight.load()){
            demand(std::chrono::steady_clock::now()<=deadline,"actual native process drain timed out before original terminate");std::this_thread::yield();
        }
        r.teardownCallerVerified=true;r.synchronousTerminating=true;
    }
    ~TeardownBoundary(){r.synchronousTerminating=false;r.teardownCallerVerified=false;}
};
lab::FLNativeModelOptions modelOptions(std::int32_t actualMixerInsert){
    lab::FLNativeModelOptions options;options.captureAllAudioChannels=true;options.copyQualifiedPCMOnlyForMixerInsert=actualMixerInsert;options.copyQualifiedPCM=true;
    options.requireQualifiedPCMForEveryAudioSource=false;options.requireSupportedMixerInsertForEveryAudioChannel=false;
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
    hold.actualOwnedNativeHold=nativeApplicationAdmitted()&&r->boundary&&r->modelMixBoundaryAvailable&&!r->native->nativeProcessAllowed.load();
    hold.actualNativeStopped=stopped();hold.outsideNativeRenderStateAndLifecycleCalls=r->boundary;
    hold.actualOwnerAndCurrentModelThreadVerified=pthread_main_np()&&r->alive&&revalidateActualNativeInsert(r->insert)
        &&id.actualNativeEngineOwner==nativeApplication().engineInstance;
    hold.actualNativeRenderCallsInFlight=r->native->actualNativeProcessCallsInFlight.load();return hold;
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
    proof.ownNativeProcessGateClosed=!r->native->nativeProcessAllowed.load();proof.actualOriginalProcessCallsInFlight=r->native->actualNativeProcessCallsInFlight.load();
    proof.nativeOriginalTerminateNotYetCalled=r->synchronousTerminating&&!r->native->nativeTerminated;return proof;
}
lab::StandaloneSnapshotData Adapter::copyActualCurrentRAMAndClock(const lab::StandaloneInstanceIdentity& identity){
    auto* r=runtime().byID(identity.instanceID);demand(r&&r->boundary,"current RAM copy needs real native boundary");
    demand(r->setupQualified&&r->sampleRate==48000,"actual native setupProcessing has not established the supported 48k profile");
    lab::FLStandaloneComponentAssociation association;association.actualComponent=identity.actualComponent;
    association.nativeOwnerObject=nativeApplication().engineInstance;association.mixerInsert=r->insert.mixerInsert;
    association.exactObjectAssociationEvidence=r->insert.evidence;association.nativePointerAssociationVerified=revalidateActualNativeInsert(r->insert);
    const auto engineSeal=lab::FLStandaloneGraphDecoder::verifyActualLoadedEngine(nativeApplication().engine);
    std::optional<lab::CapturedFLStandaloneModel> capture;
    const bool locked=withActualNativeMixCaptureLock(r->insert,[&]{
        capture=lab::FLStandaloneGraphDecoder::captureActualCurrentRAMAtNativeLock(engineSeal,modelOptions(r->insert.mixerInsert),association);
    });
    demand(locked&&capture.has_value(),"actual native mix-lock RAM capture failed");
    observeAndRequireNativeMixBoundary(*r);
    // All ARA/provider work happens AFTER native HostLockMix is released.
    auto decoded=lab::FLStandaloneGraphDecoder::decode(*capture,FL_APP_DECODER_SHA256);
    auto copied=std::make_shared<const lab::FLNativeModelSnapshot>(capture->model());
    lab::StandaloneSnapshotData data;data.actual=identity;data.graph=decoded.graph;
    data.resolveCurrentImmutableRAMSource=decoded.resolveCurrentImmutableRAMSource;
    auto& m=data.model;m.nativeProjectIdentity=copied->currentArrangementRuntimeID;
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
    for(const auto& clip:copied->clips)if(clip.kind==lab::FLNativeRecordKind::AudioChannel)data.actualVerifiedAudioRecordIndices.push_back(clip.nativeListIndex);
    data.decoderIdentity=decoded.decoderIdentity;data.decoderSourceSHA256=decoded.decoderSourceSHA256;
    data.actualCurrentNativeRAMCopiedAndVerified=decoded.actualCurrentNativeRAMCopiedAndVerified;
    data.actualDirectNativeClockReadTwiceAndEqual=decoded.clock.actualDirectNativeReadTwiceAndEqual;
    data.nativeComponentOwnerInsertAssociationVerified=association.nativePointerAssociationVerified;
    data.noNativeSerializationOrFileFallbackUsed=decoded.noNativeSerializationOrFileAudioFallbackUsed;
    r->decoded=std::move(decoded);r->copied=std::move(copied);return data;
}
void Adapter::invalidateActualNativeOverlayImmediately(const std::string& id,std::uint64_t)noexcept{
    if(auto* r=runtime().byID(id))r->dirty=true;
    if(auto* overlay=runtime().overlayListener.load(std::memory_order_acquire))overlay->invalidateInstance(id);
}
void Adapter::enqueueModelThreadRebuild(const std::string& id)noexcept{if(auto* r=runtime().byID(id))r->dirty=true;}
void Adapter::showARAUnavailableForInsert(const std::string& id,const std::string&)noexcept{if(auto* r=runtime().byID(id))r->fault=true;}
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
    try{r.error=reason;r.disabled=true;r.dirty=false;if(runtime().overlay)runtime().overlay->invalidateInstance(r.identity.instanceID);
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
    if(n.createdKind!=hooks::CreatedKind::Component||result!=kResultOk||!nativeApplicationAdmitted()||!n.actualComponent.load())return;
    if(!info.isMainThread){
        // Capability detection stays on this ACTUAL native initialize thread.
        // No ARA factory/DC/bind or borrowed component is deferred to main.
        try{ARA::IPlugInEntryPoint* entry=nullptr;
            if(n.actualComponent.load()->queryInterface(INLINE_UID_OF(ARA::IPlugInEntryPoint),reinterpret_cast<void**>(&entry))==kResultOk&&entry){
                entry->release();runtime().unsupportedOffMainARAInitializations.fetch_add(1);
            }
        }catch(...){runtime().unsupportedOffMainARAInitializations.fetch_add(1);}
        return;
    }
    try{
        demand(n.factory&&n.factory->actualFactory,"native component actual factory missing");
        auto* component=n.actualComponent.load();auto araClass=araClassForActualComponent(n,*component);if(!araClass)return;
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
                &&actual->contextRequirementCalls.load()==0&&actual->editorViewCalls.load()==0&&actual->actualNativeProcessCallsInFlight.load()==0
                &&revalidateActualNativeInsert(expected);
        };
        r->identity.instanceID=rt.coordinator->bindAfterOriginalInitialize(*component,r->identity,factory,proof);
        r->nativeCacheGate=rt.coordinator->cacheGate(r->identity.instanceID);
        if(rt.observeNativeDSP)r->nativeProcessObserver=std::make_unique<process_observer::NativeProcessObserver>(n);
        n.nativeProcessAllowed=false; // No first original native process before an actual qualified initial prime.
        n.ownerData=r;n.stateInterceptionEnabled=true;r->next=rt.head.load();rt.head=r;rt.records.push_back(std::move(owned));
        writeNativeApplicationEvent("actual_native_ara_component_bound",r->identity.instanceID);
    }catch(const std::exception& e){writeNativeApplicationEvent("actual_native_ara_bind_refused",e.what());runtime().error=e.what();}
    catch(...){writeNativeApplicationEvent("actual_native_ara_bind_refused","unknown native SDK exception");runtime().error="Unknown native SDK exception while binding ARA";}
}
void instanceCreated(hooks::InstanceRecord& n,const hooks::ThreadInfo&,void*)noexcept{
    // Controllers can receive setComponentState before FL associates them with
    // the component. Their handler only unwraps ordinary bytes, never restores ARA.
    n.stateInterceptionEnabled=true;
}
void refreshAtBoundary(BridgeRecord& r,bool build){
    auto& rt=runtime();auto snapshot=rt.coordinator->captureActualCurrentSnapshot(r.identity.instanceID);
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
        // The exact current native channels/modifications may remain live after
        // their last Playlist region is deleted. Preserve those ARA edits in
        // the same document, but authorize no source audio without a region.
        if(r.nativeCacheGate)r.nativeCacheGate->invalidate();
        if(rt.overlay){rt.overlay->invalidateInstance(r.identity.instanceID);rt.overlay->removeDisabledInstanceOnModelThread(r.identity.instanceID);}
        r.dirty=false;r.error.clear();r.initialPrimePending=false;return;
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
        rt.overlay->publish(r.identity.instanceID,*r.decoded,r.copied,std::move(cache));r.error.clear();r.initialPrimePending=false;
        writeNativeApplicationEvent("actual_native_ara_source_cache_published",r.identity.instanceID);
    }
}
Steinberg::tresult state(hooks::InstanceRecord& n,hooks::StateOperation operation,Steinberg::IBStream* stream,hooks::OriginalStateCall& original,const hooks::ThreadInfo& info,void*)noexcept{
    if(!stream)return kResultFalse;
    auto* r=record(n);
    try{
        if(operation==hooks::StateOperation::ControllerSetComponentState){
            if(!lab::fl_app::peekMagicAndRestorePosition(*stream))return original(stream);
            auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized native envelope decode failed");
            auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();return result;
        }
        if(r&&!info.isMainThread){r->fault=true;return kResultFalse;}
        if(!r){
            constexpr std::uint32_t refusedNativeArchive=1u;
            if(operation==hooks::StateOperation::ComponentGetState){
                if(n.ownerStateFlags.load()&refusedNativeArchive)return kResultFalse;
                return original(stream);
            }
            if(lab::fl_app::peekMagicAndRestorePosition(*stream)){
                n.ownerStateFlags.fetch_or(refusedNativeArchive);runtime().unboundArchiveRefusals.fetch_add(1);return kResultFalse;
            }
            return original(stream);
        }
        if(r->disabled){
            if(operation==hooks::StateOperation::ComponentGetState||lab::fl_app::peekMagicAndRestorePosition(*stream))return kResultFalse;
            return original(stream);
        }
        if(operation==hooks::StateOperation::ComponentGetState&&r->pendingArchive){
            r->error="Save is unavailable until the pending ARA archive matches actual current native project RAM";return kResultFalse;
        }
        if(operation==hooks::StateOperation::ComponentGetState&&!r->graphApplied&&!r->pendingArchive)return original(stream);
        // Ordinary native state restore can precede setupProcessing.
        if(operation==hooks::StateOperation::ComponentSetState&&!r->setupQualified){
            const bool magic=lab::fl_app::peekMagicAndRestorePosition(*stream);r->pendingLoad=loadAuthority(*r,info);
            if(!magic){r->pendingPlainLoad=true;const auto result=original(stream);runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}return result;}
            auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized early archive decode failed");
            demand(!r->pendingArchive,"a second native archive arrived before the first was resolved");r->pendingArchive=envelope->archive;r->dirty=true;r->initialPrimePending=true;r->native->nativeProcessAllowed=false;
            auto* ordinary=new lab::fl_app::MemoryIBStream(envelope->nativeComponentState);const auto result=original(ordinary);ordinary->release();
            runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);if(result==kResultOk){r->dirty=true;runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);}return result;
        }
        if(!pthread_main_np()||!stopped()){
            r->fault=false;r->error="ARA project state requires stopping native FL playback first";return kResultFalse;
        }
        Boundary boundary(*r);
        if(operation==hooks::StateOperation::ComponentGetState){
            demand(sealedNativeCaller(info),"ARA envelope serialization requires the actual sealed native FL state caller");
            r->lastNativeStateCaller=nativeCallerEvidence(info);writeNativeApplicationEvent("actual_native_component_getState_authority",r->identity.instanceID+" "+r->lastNativeStateCaller);
            demand(!r->pendingArchive,"save refused while recognized ARA archive still awaits matching actual native RAM");
            auto* ordinary=new lab::fl_app::MemoryIBStream;const auto result=original(ordinary);lab::Bytes bytes=ordinary->bytes();ordinary->release();
            if(result!=kResultOk)return result;
            observeAndRequireNativeMixBoundary(*r);
            refreshAtBoundary(*r,false);const auto snapshot=runtime().coordinator->captureActualCurrentSnapshot(r->identity.instanceID);
            lab::FLStandaloneStateEnvelope envelope;envelope.nativeComponentState=std::move(bytes);
            envelope.archive=runtime().coordinator->captureArchiveOnly(r->identity.instanceID,snapshot);
            lab::fl_app::writeAll(*stream,lab::encodeFLStandaloneStateEnvelope(envelope));return result;
        }
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
                r->initialPrimePending=true;r->native->nativeProcessAllowed=false;
                auto* ordinary=new lab::fl_app::MemoryIBStream(*r->pendingOrdinaryComponentState);const auto result=original(ordinary);ordinary->release();
                r->pendingOrdinaryComponentState.reset();runtime().coordinator->observeOriginalSetStateResult(r->identity.instanceID,result);
                runtime().coordinator->invalidateSourceCachesAndRequestRebuild(r->identity.instanceID);return result;
            }
            r->pendingLoad=loadAuthority(*r,info);
            if(lab::fl_app::peekMagicAndRestorePosition(*stream)){
                auto envelope=lab::decodeFLStandaloneStateEnvelope(lab::fl_app::readRemaining(*stream));demand(envelope.has_value(),"recognized deferred archive decode failed");
                demand(!r->pendingArchive,"a second native archive arrived before the pending one resolved");
                r->pendingArchive=envelope->archive;r->initialPrimePending=true;r->native->nativeProcessAllowed=false;
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
    if(!rt.overlay){lab::FLStandaloneOverlayConfig config;config.actualEngineImageBase=app.engine.imageBase;
        config.exactLoadedEnginePath=app.enginePath;config.exactEngineArm64SliceSHA256=app.engine.verifiedArm64SliceSHA256;
        config.confirmAlreadyAdmittedOwnedProcess=admitted;config.confirmStoppedDrainedNativeModelBoundary=allBoundary;config.ownerFaultQueued=&rt.overlayFault;
        rt.overlay=std::make_unique<lab::FLStandaloneSourceOverlay>(std::move(config));rt.overlayListener.store(rt.overlay.get(),std::memory_order_release);
    }
    if(!rt.overlayInstalled){const auto installed=rt.overlay->installLoadedNativeSourceHook();rt.overlayInstalled=installed.installed;demand(installed.installed,installed.reason.c_str());}
}
void setup(hooks::InstanceRecord& n,const Steinberg::Vst::ProcessSetup& setup,Steinberg::tresult result,const hooks::ThreadInfo& info,void*)noexcept{
    auto* r=record(n);if(!r)return;
    r->sampleRate=setup.sampleRate;r->setupQualified=result==kResultOk&&setup.sampleRate==48000&&setup.symbolicSampleSize==Steinberg::Vst::kSample32;r->dirty=true;
    if(r->nativeCacheGate)r->nativeCacheGate->invalidate();
    if(!r->setupQualified||!info.isMainThread||!stopped())return;
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
    auto* r=record(n);if(!r)return;
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
void terminated(hooks::InstanceRecord& n,void*,Steinberg::tresult result,const hooks::ThreadInfo&,void*)noexcept{
    if(auto* r=record(n))try{runtime().coordinator->observeOriginalTerminateResult(r->identity.instanceID,result);r->alive=false;}catch(const std::exception& e){fail(*r,e.what());}
    catch(...){fail(*r,"unknown native SDK lifecycle exception");}
}
void invalidateEveryLiveARACache()noexcept{
    // Native public parameter notifications may be RT. Shared handlers cannot
    // identify an insert: invalidate all stable published records conservatively.
    for(auto* r=runtime().head.load(std::memory_order_acquire);r;r=r->next.load(std::memory_order_acquire))if(r->alive.load()&&r->nativeCacheGate){
        r->nativeCacheGate->invalidate();r->dirty=true;
    }
}
void nativeProcessTelemetry(hooks::InstanceRecord& n,const hooks::NativeProcessTelemetry& observation,const hooks::ThreadInfo& info,void*)noexcept{
    if(auto* r=record(n))if(r->nativeProcessObserver)r->nativeProcessObserver->observe(n,observation,info);
}
void inputParameterChanges(hooks::InstanceRecord& n,Steinberg::Vst::IParameterChanges*,Steinberg::int32 count,const hooks::ThreadInfo&,void*)noexcept{
    // Queue authority identifies this exact actual component. The hook already
    // obtained the public positive count; owner performs no SDK query or lookup.
    if(count>0)if(auto* r=record(n))if(r->alive.load()&&r->nativeCacheGate){r->nativeCacheGate->invalidate();r->dirty=true;}
}
void parameterEdit(hooks::InstanceRecord*,Steinberg::Vst::IComponentHandler*,Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const hooks::ThreadInfo&,void*)noexcept{invalidateEveryLiveARACache();}
void parameterNormalized(hooks::InstanceRecord&,Steinberg::Vst::IEditController*,Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const hooks::ThreadInfo&,void*)noexcept{invalidateEveryLiveARACache();}
void fault(const char*,const hooks::ModuleRecord*,const hooks::InstanceRecord* n,const hooks::ThreadInfo&,void*)noexcept{
    if(n)if(auto* r=static_cast<BridgeRecord*>(n->ownerData.load()))r->fault=true;
}
void status(){
    auto& rt=runtime();const auto& app=nativeApplication();const auto cacheStats=rt.overlay?rt.overlay->instanceStatsOnModelThread():std::vector<lab::FLStandaloneOverlayInstanceStats>{};NSMutableArray* inserts=[NSMutableArray array];
    for(const auto& r:rt.records){NSMutableDictionary* d=[NSMutableDictionary dictionary];
        d[@"instance_id"]=@(r->identity.instanceID.c_str());d[@"native_insert"]=@(r->insert.mixerInsert);d[@"bound"]=@YES;
        d[@"static_signature_checked"]=@(r->staticSignatureChecked);d[@"static_signature_valid"]=@(r->staticSignatureValid);d[@"static_signature_status"]=@(r->staticSignatureStatus);
        d[@"static_signature_is_admission_or_license_proof"]=@NO;d[@"plugin_bundle"]=@(r->pluginBundlePath.c_str());
        d[@"factory_entry_image"]=@(r->factoryEntryImagePath.c_str());d[@"factory_entry_image_sha256"]=@(r->factoryEntryImageSHA256.c_str());
        d[@"alive"]=@(r->alive.load());d[@"graph_ready"]=@(r->graphApplied);d[@"disabled"]=@(r->disabled);d[@"cache_rebuild_pending"]=@(r->dirty.load());d[@"initial_native_process_admission_pending"]=@(r->initialPrimePending.load());
        d[@"native_mix_boundary"]=@(nativeMixBoundaryReason(r->lastNativeMixBoundary.reason));d[@"native_mix_last_probe_available"]=@(r->lastNativeMixBoundary.available());
        d[@"native_mix_create"]=@(r->lastNativeMixBoundary.createResult);d[@"native_mix_join"]=@(r->lastNativeMixBoundary.joinResult);
        d[@"native_mix_trylock"]=@(r->lastNativeMixBoundary.tryLockResult);d[@"native_mix_unlock"]=@(r->lastNativeMixBoundary.unlockResult);
        d[@"native_mix_identity_verified"]=@(r->lastNativeMixBoundary.nativeIdentityVerified);d[@"native_process_admission_allowed"]=@(r->native->nativeProcessAllowed.load());
        d[@"native_process_admission_blocked_callbacks"]=@(r->native->blockedProcessCallbacks.load());
        d[@"empty_lifecycle_bind_mix_diagnostic"]=@(nativeMixBoundaryReason(r->lifecycleBindingMixDiagnostic.reason));
        d[@"empty_lifecycle_bind_diagnostic_is_model_boundary_proof"]=@NO;
        d[@"last_native_state_caller"]=@(r->lastNativeStateCaller.c_str());d[@"archive_pending_current_ram"]=@(r->pendingArchive.has_value());d[@"error"]=@(r->error.c_str());
        d[@"cache_published"]=@NO;d[@"cache_authorized"]=@NO;
        for(const auto& cache:cacheStats)if(cache.instanceID==r->identity.instanceID){
            d[@"cache_published"]=@(cache.published);d[@"cache_authorized"]=@(cache.ticketAuthorized);
            d[@"source_applied_callbacks"]=@(cache.counters.appliedCallbacks);d[@"source_applied_frames"]=@(cache.counters.appliedFrames);
            d[@"source_rejected_callbacks"]=@(cache.counters.rejected);d[@"source_fault_bits"]=@(cache.counters.faultBits);
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
    data[@"admitted_source_profile"]=@{@"sample_rate":@48000,@"channels":@[@1,@2],@"maximum_source_seconds":@300,
        @"maximum_pcm_bytes_per_source":@(64u*1024u*1024u),@"maximum_pcm_bytes_total":@(128u*1024u*1024u),
        @"maximum_source_cache_bytes":@(256u*1024u*1024u),@"reported_latency_head_tail_must_be_zero":@YES,@"limits_are_not_runtime_coverage":@YES};
    data[@"runtime_helper_admitted"]=@(app.admitted);data[@"native_engine_ready"]=@(app.engineReady);data[@"hooks_installed"]=@(rt.hooksInstalled);
    data[@"source_overlay_installed"]=@(rt.overlayInstalled);data[@"inserts"]=inserts;data[@"error"]=@(rt.error.c_str());
    NSMutableArray* ownWindows=[NSMutableArray array];
    for(NSWindow* window in NSApp.windows){NSMutableDictionary* item=[NSMutableDictionary dictionary];
        item[@"title"]=window.title?:@"";item[@"visible"]=@(window.visible);item[@"miniaturized"]=@(window.miniaturized);
        item[@"key"]=@(window.keyWindow);item[@"main"]=@(window.mainWindow);item[@"document_edited"]=@(window.documentEdited);
        item[@"window_number"]=@(window.windowNumber);item[@"represented_url"]=window.representedURL.absoluteString?:@"";[ownWindows addObject:item];}
    const auto registration=registrationReadStats();
    data[@"registration_read_through"]=@{@"initialized":@(registration.initialized),@"branch_present":@(registration.registrationBranchPresent),
        @"value_count":@(registration.registrationValueCount),@"read_count":@(registration.registrationReads),@"denied_write_count":@(registration.deniedRegistrationWrites),
        @"vendor_license_validity_proven":@NO};
    data[@"own_windows_diagnostic_only"]=ownWindows;data[@"own_application_active"]=@(NSApp.active);
    data[@"window_metadata_is_native_graph_proof"]=@NO;
    data[@"runtime_qualified"]=@NO;data[@"run_id"]=@(app.runID.c_str());data[@"pid"]=@(getpid());
    NSData* json=[NSJSONSerialization dataWithJSONObject:data options:NSJSONWritingPrettyPrinted error:nil];
    if(json)[json writeToFile:@((app.runDirectory+"/fl-ara-status.json").c_str()) atomically:YES];
}
void tick(const NativeApplication& app,void*)noexcept{
    auto& rt=runtime();if(rt.inTick)return;rt.inTick=true;
    struct Reset{bool& flag;~Reset(){flag=false;}} reset{rt.inTick};
    try{
        if(rt.unboundArchiveRefusals.exchange(0)){
            rt.error="This native component is not ARA-bound. Its ARA archive was refused unchanged; component saving is blocked to preserve it";
            writeNativeApplicationEvent("unbound_native_ara_archive_refused",rt.error);
            NSAlert* alert=[NSAlert new];alert.messageText=@"ARA project state could not be restored";alert.informativeText=@(rt.error.c_str());
            [alert addButtonWithTitle:@"Keep session open"];if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(rt.unsupportedOffMainARAInitializations.exchange(0)){
            rt.error="An actual native ARA component initialized off the main thread; ARA binding is unsupported for that component in this run";
            writeNativeApplicationEvent("actual_native_ara_offmain_initialize_refused",rt.error);
            NSAlert* alert=[NSAlert new];alert.messageText=@"ARA unavailable for this native component";alert.informativeText=@(rt.error.c_str());
            [alert addButtonWithTitle:@"Keep session open"];if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];
        }
        if(!rt.overlay&&app.engineReady){lab::FLStandaloneOverlayConfig config;config.actualEngineImageBase=app.engine.imageBase;
            config.exactLoadedEnginePath=app.enginePath;config.exactEngineArm64SliceSHA256=app.engine.verifiedArm64SliceSHA256;
            config.confirmAlreadyAdmittedOwnedProcess=admitted;config.confirmStoppedDrainedNativeModelBoundary=allBoundary;config.ownerFaultQueued=&rt.overlayFault;
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
        for(auto& owned:rt.records){auto& r=*owned;if(!r.alive)continue;
            if(r.fault.exchange(false)){if(!r.disabled)fail(r,"ARA/native contract fault; this insert is unavailable");
                NSAlert* alert=[NSAlert new];alert.messageText=@"ARA unavailable for this FL insert";alert.informativeText=@(r.error.c_str());
                [alert addButtonWithTitle:@"Keep session open"];if(NSApp.keyWindow)[alert beginSheetModalForWindow:NSApp.keyWindow completionHandler:nil];else [alert runModal];r.fault=false;}
            if(r.disabled||!r.setupQualified||!stopped())continue;
            try{Boundary boundary(r);ensureOverlayAtModelBoundary();
                refreshAtBoundary(r,true);rt.overlay->collectRetiredOnModelThread();}
            catch(const lab::NativeModelBoundaryUnavailable& e){r.error=std::string("Native model boundary deferred: ")+e.what();r.dirty=true;}
            catch(const lab::SourceCacheNotReady& e){r.error=e.what();r.dirty=true;}
            catch(const lab::FLStandaloneDecodeError& e){
                if(!r.graphApplied){r.error=std::string("ARA is waiting for supported current native source regions: ")+e.what();r.dirty=true;}
                else fail(r,e.what());
            }
            catch(const std::exception& e){fail(r,e.what());}
            catch(...){fail(r,"unknown native SDK exception at stopped model boundary");}
        }
        if(++rt.ticks%10==0)status();
    }catch(const std::exception& e){rt.error=e.what();writeNativeApplicationEvent("ara_app_bridge_tick_refused",e.what());status();}
    catch(...){rt.error="Unknown native SDK exception in app bridge";writeNativeApplicationEvent("ara_app_bridge_tick_refused",rt.error);}
}
void onAdmitted(const NativeApplication& app,void*)noexcept{
    try{auto& rt=runtime();const char* testUID=getenv("RYSUP_FLARA_TEST_AUDIO_DEVICE_UID");rt.observeNativeDSP=testUID&&!std::strcmp(testUID,"Pro Tools Audio Bridge 2-A_UID");rt.adapter=std::make_shared<Adapter>();rt.coordinator=std::make_unique<lab::StandaloneARACoordinator>(app.runID,rt.adapter);
        hooks::Config config;config.exactOwnedExecutablePath=app.executablePath;config.exactOwnedExecutableSHA256=app.executableSHA256;
        config.exactOwnedProcessID=app.processID;config.confirmAlreadyAdmittedOwnedProcess=admitted;
        config.callbacks.instanceCreated=instanceCreated;config.callbacks.initialized=initialized;config.callbacks.state=state;
        config.callbacks.setActiveObserved=setActive;config.callbacks.setupProcessingObserved=setup;config.callbacks.setProcessingObserved=processing;
        config.callbacks.beforeTerminate=beforeTerminate;config.callbacks.terminated=terminated;config.callbacks.fault=fault;
        config.callbacks.paramEditObserved=parameterEdit;config.callbacks.paramNormalizedObserved=parameterNormalized;config.callbacks.inputParameterChangesObserved=inputParameterChanges;
        if(rt.observeNativeDSP)config.callbacks.nativeProcessTelemetry=nativeProcessTelemetry;
        const auto result=hooks::install(config);rt.hooksInstalled=result.installed;demand(result.installed,result.reason);status();
    }catch(const std::exception& e){runtime().error=e.what();writeNativeApplicationEvent("ara_app_helper_install_failed",e.what());}
    catch(...){runtime().error="Unknown exception installing native bridge";writeNativeApplicationEvent("ara_app_helper_install_failed",runtime().error);}
}
__attribute__((constructor)) static void loadBridge(){
    @autoreleasepool{NativeApplicationCallbacks callbacks;callbacks.admitted=onAdmitted;callbacks.tick=tick;
        initializeNativeApplication(callbacks);}
}
} // namespace rysup::fl_ara::app_bridge
