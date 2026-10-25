#include "StandaloneCacheInvalidation.h"
#include "StandaloneARACoordinator.h"
#include "VST3ARAEntry.h"
#include "OwnedVST3PlaybackCompanion.h"
#include "StandaloneARAEditRelay.h"
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace rysup::ara_lab {
namespace {
void demand(bool good,const char* reason){if(!good)throw std::runtime_error(reason);}
int processID(){
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}
bool validSHA(const std::string& v){return v.size()==64 && v.find_first_not_of("0123456789abcdef")==std::string::npos;}
bool sameClock(const StandaloneNativeClock& a,const StandaloneNativeClock& b){
    return a.ppq==b.ppq && a.tempoMilliBPM==b.tempoMilliBPM && a.sampleRate==b.sampleRate
        && a.clockFormatIdentity==b.clockFormatIdentity && a.actualNativeClockBytes==b.actualNativeClockBytes;
}
bool sameModel(const StandaloneNativeModelAuthority& a,const StandaloneNativeModelAuthority& b){
    return a.nativeEngineSHA256==b.nativeEngineSHA256 && a.nativeRecordModelSHA256==b.nativeRecordModelSHA256
        && a.sourcePCMModelSHA256==b.sourcePCMModelSHA256 && a.graphSHA256==b.graphSHA256
        && a.nativeSaveVersion==b.nativeSaveVersion && sameClock(a.clock,b.clock);
}
ArchiveObjectIDRelocation identityMap(const Graph& graph){ArchiveObjectIDRelocation ids;
    for(const auto& row:graph.sources)ids.sourceIDs.emplace_back(row.id,row.id);
    for(const auto& row:graph.modifications)ids.modificationIDs.emplace_back(row.id,row.id);
    for(const auto& row:graph.regions)ids.regionIDs.emplace_back(row.id,row.id);return ids;
}
bool sameGraph(const Graph& a,const Graph& b){return archiveObjectIDRelocationMatchesGraphs(a,b,identityMap(a));}
struct Operation {bool& value;explicit Operation(bool& flag):value(flag){demand(!flag,"standalone model operation reentry");flag=true;}~Operation(){value=false;}};
}

struct CapturedStandaloneSnapshot::Impl {
    StandaloneSnapshotData data;
    std::string coordinatorRun;
    std::uint64_t serial=0;
};
CapturedStandaloneSnapshot::CapturedStandaloneSnapshot(std::shared_ptr<const Impl> value):p(std::move(value)){}
const StandaloneSnapshotData& CapturedStandaloneSnapshot::data()const noexcept{return p->data;}
std::uint64_t CapturedStandaloneSnapshot::captureSerial()const noexcept{return p->serial;}
struct PreparedEditorArchive::Impl {
    PreparedEditorArchiveIdentity identity;
    StandaloneSavedEditorArchive archive;
    std::shared_ptr<const void> captureLifetime;
    const StandaloneSnapshotData* actualSnapshot=nullptr;
    std::string coordinatorRun;
};
PreparedEditorArchive::PreparedEditorArchive(std::shared_ptr<const Impl> value):p(std::move(value)){}
const PreparedEditorArchiveIdentity& PreparedEditorArchive::identity()const noexcept{return p->identity;}

struct StandaloneARACoordinator::Impl {
    struct Observer final : ModelEditObserver {
        std::string id;
        std::shared_ptr<StandaloneCacheGate> gate=std::make_shared<StandaloneCacheGate>();
        std::shared_ptr<StandaloneOwnerAdapter> adapter;
        std::atomic<bool> contractFailed{false};
        StandaloneARAEditRelay actualDirty;
        StandaloneCacheTicket invalidateForCurrentRebuild()noexcept{
            return invalidateStandaloneCacheAuthority(*gate,
                [&](std::uint64_t epoch)noexcept{adapter->invalidateActualNativeOverlayImmediately(id,epoch);},
                [&](std::uint64_t epoch)noexcept{adapter->queueActualNativeStopForCacheRebuild(id,epoch);});
        }
        void invalidateForARAEdit(std::uint64_t revision)noexcept override{
            invalidateStandaloneCacheForEdit(*gate,revision,
                [&](std::uint64_t epoch)noexcept{adapter->invalidateActualNativeOverlayImmediately(id,epoch);},
                [&](std::uint64_t epoch)noexcept{adapter->queueActualNativeStopForCacheRebuild(id,epoch);},
                [&](std::uint64_t value)noexcept{actualDirty.forwardActualRevision(value,[&](std::uint64_t actual)noexcept{
                    adapter->queueActualNativeDocumentDirtyFromARA(id,actual);});},
                [&]()noexcept{adapter->enqueueModelThreadRebuild(id);});
        }
        void reportARAContractFailure(const char*)noexcept override{
            contractFailed=true;gate->closePermanently();adapter->invalidateActualNativeOverlayImmediately(id,gate->currentTicket().epoch);
            adapter->queueActualNativeStopForCacheRebuild(id,gate->currentTicket().epoch);
            try{adapter->showARAUnavailableForInsert(id,"ARA contract failure in this insert; native session remains open");}catch(...){}
        }
    };
    struct Record {
        StandaloneInstanceIdentity identity;
        StandaloneVST3FactoryContext factory;
        std::shared_ptr<Steinberg::IPluginFactory> retainedNativeFactory;
        std::shared_ptr<VST3ARAEntry> entry;
        std::shared_ptr<FactoryLease> lease;
        std::unique_ptr<HostController> host;
        std::shared_ptr<Observer> observer;
        std::shared_ptr<const CapturedStandaloneSnapshot::Impl> applied;
        std::shared_ptr<const CapturedStandaloneSnapshot::Impl> currentCacheSnapshot;
        StandaloneCacheTicket currentCacheTicket;
        std::uint64_t currentCacheEditRevision=0;
        bool currentCacheNeedsNoOverlay=false;
        std::atomic<bool> alive{true},active{false},processing{false},uncertain{false},disabled{false};
        std::atomic<bool> activationEver{false},processingEver{false},fresh{true};
        std::atomic<std::uint32_t> stateCalls{0};
        bool graphReady=false,preparedTerminate=false,terminated=false;
        std::set<std::string> consumedLoadEvents;
        std::string phase="fresh-bound-editor-only";
    };
    std::string run;
    std::shared_ptr<StandaloneOwnerAdapter> adapter;
    std::thread::id modelThread=std::this_thread::get_id();
    mutable std::mutex recordsMutex;
    std::map<std::string,std::shared_ptr<Record>> records;
    std::map<std::uintptr_t,std::string> liveComponents;
    std::uint64_t instanceSerial=0,snapshotSerial=0;
    bool operation=false;
    Impl(std::string ownRun,std::shared_ptr<StandaloneOwnerAdapter> owner):run(std::move(ownRun)),adapter(std::move(owner)){
        demand(adapter && !run.empty() && run.size()<=128,"actual standalone owner adapter and bounded run identity required");requireMain();
    }
    void requireMain()const{demand(adapter->onActualModelThread() && std::this_thread::get_id()==modelThread,"standalone coordinator requires actual native model thread");}
    std::shared_ptr<Record> find(const std::string& id)const{std::lock_guard<std::mutex> guard(recordsMutex);auto it=records.find(id);demand(it!=records.end(),"unknown standalone native insert");return it->second;}
    bool actuallyInactive(const std::shared_ptr<Record>& r)const noexcept{return r->alive && !r->active && !r->processing && !r->uncertain;}
    void requireNativeBoundary(const std::shared_ptr<Record>& r,bool allowDisabled=false)const{
        requireMain();demand(r->host && r->alive && !r->preparedTerminate && !r->terminated && (allowDisabled || (!r->disabled && !r->observer->contractFailed)),"standalone native insert unavailable");
        const auto hold=adapter->verifyActualNativeHold(r->identity);
        demand(hold.actualOwnedNativeHold && hold.actualNativeStopped && hold.outsideNativeRenderStateAndLifecycleCalls
            && hold.actualOwnerAndCurrentModelThreadVerified && hold.actualNativeRenderCallsInFlight==0 && !r->uncertain,
               "actual standalone owner must be stopped, held, outside original child calls and render-drained");
    }
    void requireHold(const std::shared_ptr<Record>& r,bool allowDisabled=false)const{
        requireNativeBoundary(r,allowDisabled);
        demand(r->host->ownedPlaybackLifecycleIdle(),"own ARA model/cache lifecycle is not idle");
    }
    void validateSnapshot(const std::shared_ptr<Record>& r,const StandaloneSnapshotData& d,bool verifyImmutableProviders=true)const{
        const auto& i=d.actual;const auto& expected=r->identity;const auto& m=d.model;
        demand(i.instanceID==expected.instanceID && i.ownRunID==run && i.ownProcessID==processID()
            && i.actualComponent==expected.actualComponent && i.actualNativeEngineOwner==expected.actualNativeEngineOwner
            && i.nativeInsertKey==expected.nativeInsertKey && i.pluginModuleSHA256==expected.pluginModuleSHA256
            && i.nativeEngineSHA256==expected.nativeEngineSHA256,"snapshot is not bound to this actual current native insert/owner");
        demand(d.actualCurrentNativeRAMCopiedAndVerified && d.actualDirectNativeClockReadTwiceAndEqual
            && d.nativeComponentOwnerInsertAssociationVerified && d.noNativeSerializationOrFileFallbackUsed,
               "actual copied native RAM/direct clock/owner authority missing; serialization/file fallback forbidden");
        demand(!m.nativeProjectIdentity.empty() && m.nativeEngineSHA256==expected.nativeEngineSHA256
            && validSHA(m.nativeRecordModelSHA256) && validSHA(m.sourcePCMModelSHA256) && validSHA(m.graphSHA256)
            && !m.nativeSaveVersion.empty() && !d.decoderIdentity.empty() && validSHA(d.decoderSourceSHA256),"native model/decoder evidence missing");
        const auto& c=m.clock;
        demand(c.ppq>=24 && c.ppq<=9600 && c.ppq%4==0 && c.tempoMilliBPM>=10000 && c.tempoMilliBPM<=522000
            && c.sampleRate==48000 && !c.actualNativePlaying && !c.clockFormatIdentity.empty()
            && !c.actualNativeClockBytes.empty() && c.actualNativeClockBytes.size()<=4096,"actual stopped native clock is unavailable/outside admitted 48k profile");
        demand(d.actualCopiedNativeModelRecords.size()<=32u*1024u*1024u && d.graph.sources.size()<=4096
            && d.graph.tracks.size()<=4096 && d.graph.modifications.size()<=100000 && d.graph.regions.size()<=100000,"native snapshot exceeds bounds");
        demand(d.actualVerifiedAudioRecordIndices.size()<=100000 && std::is_sorted(d.actualVerifiedAudioRecordIndices.begin(),d.actualVerifiedAudioRecordIndices.end())
            && std::adjacent_find(d.actualVerifiedAudioRecordIndices.begin(),d.actualVerifiedAudioRecordIndices.end())==d.actualVerifiedAudioRecordIndices.end(),"actual native audio row classifications are duplicate/unsorted/unbounded");
        demand((!verifyImmutableProviders || d.resolveCurrentImmutableRAMSource) && d.sources.size()==d.graph.sources.size()
            && d.modifications.size()==d.graph.modifications.size() && d.regions.size()==d.graph.regions.size(),"native source/edit/region authority cardinality differs");
        std::map<std::string,const StandaloneSourceIdentity*> sources;std::map<std::string,const StandaloneModificationIdentity*> mods;
        std::map<std::string,const StandaloneRegionIdentity*> regions;
        for(const auto& s:d.sources)demand(!s.sourceID.empty() && !s.nativePhysicalSourceKey.empty() && validSHA(s.actualPCM_SHA256)
            && sources.emplace(s.sourceID,&s).second,"native source physical/PCM identity missing/duplicate");
        for(const auto& s:d.graph.sources){demand(sources.count(s.id) && s.sampleRate==48000 && (s.channelCount==1 || s.channelCount==2) && s.sampleCount>0,"unqualified or unbound current native source");
            if(verifyImmutableProviders){auto provider=d.resolveCurrentImmutableRAMSource(s);demand(provider!=nullptr,"actual current immutable RAM provider absent");const auto actual=provider->describe();
                demand(actual.id==s.id && actual.sampleRate==s.sampleRate && actual.sampleCount==s.sampleCount && actual.channelCount==s.channelCount,"actual current RAM provider format/identity differs");}}
        for(const auto& mod:d.modifications)demand(!mod.modificationID.empty() && !mod.nativePhysicalEditKey.empty() && sources.count(mod.sourceID)
            && mods.emplace(mod.modificationID,&mod).second,"native modification physical/source identity missing/duplicate");
        for(const auto& mod:d.graph.modifications)demand(mods.count(mod.id) && mods.at(mod.id)->sourceID==mod.sourceID,"Graph modification is not actual native edit/source identity");
        for(const auto& region:d.regions)demand(!region.regionID.empty() && !region.nativePhysicalRegionKey.empty() && mods.count(region.modificationID)
            && regions.emplace(region.regionID,&region).second,"native region physical/modification identity missing/duplicate");
        for(const auto& region:d.graph.regions)demand(regions.count(region.id) && regions.at(region.id)->modificationID==region.modificationID,"Graph region is not actual native clip/edit identity");
    }
    void validateContinuity(const Record& record,const StandaloneSnapshotData& now)const{
        if(!record.applied)return;const auto& old=record.applied->data;
        demand(old.model.nativeProjectIdentity==now.model.nativeProjectIdentity,"a native project replacement needs its explicit load authority, never ordinary refresh");
        std::map<std::string,StandaloneSourceIdentity> sources;for(const auto& s:old.sources)sources.emplace(s.sourceID,s);
        std::map<std::string,StandaloneModificationIdentity> mods;for(const auto& m:old.modifications)mods.emplace(m.modificationID,m);
        std::map<std::string,StandaloneRegionIdentity> regions;for(const auto& r:old.regions)regions.emplace(r.regionID,r);
        for(const auto& s:now.sources){auto found=sources.find(s.sourceID);if(found!=sources.end())demand(found->second.nativePhysicalSourceKey==s.nativePhysicalSourceKey && found->second.actualPCM_SHA256==s.actualPCM_SHA256,"stable source ID changed actual physical/content identity");}
        for(const auto& m:now.modifications){auto found=mods.find(m.modificationID);if(found!=mods.end())demand(found->second.nativePhysicalEditKey==m.nativePhysicalEditKey && found->second.sourceID==m.sourceID,"stable modification ID changed native physical/source identity");}
        for(const auto& r:now.regions){auto found=regions.find(r.regionID);if(found!=regions.end())demand(found->second.nativePhysicalRegionKey==r.nativePhysicalRegionKey && found->second.modificationID==r.modificationID,"stable region ID changed native physical/edit identity");}
    }
    void validateLoad(const std::shared_ptr<Record>& r,const StandaloneNativeStateLoadAuthority& load)const{
        demand(load.actualComponent==r->identity.actualComponent && load.actualNativeEngineOwner==r->identity.actualNativeEngineOwner
            && load.nativeInsertKey==r->identity.nativeInsertKey && !load.actualNativeLoadEventIdentity.empty()
            && load.actualNativeStateLoadCallOrAttestedDeferredWindow && load.exactSealedNativeCallerAndOwnerVerified
            && load.actualCurrentNativeLoadArgumentAcceptedAndUnconsumed,"actual standalone native state-load scope/owner/accepted argument authority missing");
        demand(!r->consumedLoadEvents.count(load.actualNativeLoadEventIdentity) && r->consumedLoadEvents.size()<4096,"native state-load event was already consumed or bounded session exhausted");
    }
    std::shared_ptr<const CapturedStandaloneSnapshot::Impl> makeSnapshot(StandaloneSnapshotData data){
        demand(snapshotSerial<std::numeric_limits<std::uint64_t>::max(),"native snapshot serial exhausted");auto snapshot=std::make_shared<CapturedStandaloneSnapshot::Impl>();
        snapshot->data=std::move(data);snapshot->coordinatorRun=run;snapshot->serial=++snapshotSerial;return snapshot;
    }
    void validateToken(const std::shared_ptr<Record>& r,const CapturedStandaloneSnapshot& snapshot)const{
        demand(snapshot.p && snapshot.p->coordinatorRun==run && snapshot.p->serial && (!r->applied || snapshot.p->serial>=r->applied->serial),"stale/foreign standalone capture token");validateSnapshot(r,snapshot.data());
    }
};

StandaloneARACoordinator::StandaloneARACoordinator(std::string run,std::shared_ptr<StandaloneOwnerAdapter> adapter):p(std::make_unique<Impl>(std::move(run),std::move(adapter))){}
StandaloneARACoordinator::~StandaloneARACoordinator(){
    std::lock_guard<std::mutex> guard(p->recordsMutex);bool live=false;
    for(const auto& item:p->records)if(item.second->host || item.second->entry){item.second->observer->gate->closePermanently();live=true;}
    if(live){auto* retained=new decltype(p->records)(std::move(p->records));(void)retained;std::fputs("Standalone ARA coordinator: native owner did not finish teardown; own model/module retained\n",stderr);}
}
std::string StandaloneARACoordinator::bindAfterOriginalInitialize(Steinberg::Vst::IComponent& component,
    StandaloneInstanceIdentity identity,const StandaloneVST3FactoryContext& factory,const StandaloneFreshNativeInitialization& proof){
    p->requireMain();Operation operation(p->operation);
    demand(proof.originalInitializeSucceeded && proof.actualNativeComponentFactoryAndOwnerVerified && proof.actualLoadedComponentAndFactoryImageVerified
        && proof.beforeFirstNativeStateActivationProcessingContextOrEditor && proof.nativeOwnerDoesNotAlreadyInitializeARA,"native bind is late/unverified or its factory is already managed by another ARA host");
    demand(factory.actualSameNativeFactory && factory.actualAlreadyLoadedModuleLifetime && identity.actualNativeEngineOwner
        && !identity.nativeInsertKey.empty() && validSHA(identity.pluginModuleSHA256) && validSHA(identity.nativeEngineSHA256),"actual standalone native factory/module/owner identity required");
    demand((!identity.ownProcessID || identity.ownProcessID==processID()) && (identity.ownRunID.empty() || identity.ownRunID==p->run)
        && (!identity.actualComponent || identity.actualComponent==reinterpret_cast<std::uintptr_t>(&component)),"binding identity names a different process/native component");
    {std::lock_guard<std::mutex> guard(p->recordsMutex);demand(!p->liveComponents.count(reinterpret_cast<std::uintptr_t>(&component)),"native component already ARA-bound");}
    auto record=std::make_shared<Impl::Record>();record->identity=std::move(identity);
    record->identity.instanceID=p->run+":vst3:"+std::to_string(++p->instanceSerial);record->identity.ownRunID=p->run;record->identity.ownProcessID=processID();record->identity.actualComponent=reinterpret_cast<std::uintptr_t>(&component);
    record->factory=factory;factory.actualSameNativeFactory->addRef();record->retainedNativeFactory={factory.actualSameNativeFactory,[](auto* value){value->release();}};
    Steinberg::PClassInfo audio{},ara{};bool foundAudio=false,foundARA=false;
    for(Steinberg::int32 i=0;i<factory.actualSameNativeFactory->countClasses();++i){Steinberg::PClassInfo info{};demand(factory.actualSameNativeFactory->getClassInfo(i,&info)==Steinberg::kResultOk,"actual native class lookup failed");
        if(!std::memcmp(info.cid,factory.audioClassID.data(),16)){audio=info;foundAudio=true;}
        if(!std::memcmp(info.cid,factory.araClassID.data(),16)){ara=info;foundARA=true;}}
    demand(foundAudio && foundARA && !std::strcmp(audio.category,kVstAudioEffectClass) && !std::strcmp(ara.category,kARAMainFactoryClass) && !std::strcmp(audio.name,ara.name),"actual native audio/ARA class pair differs");
    ARA::IMainFactory* provider=nullptr;
    demand(factory.actualSameNativeFactory->createInstance(ara.cid,INLINE_UID_OF(ARA::IMainFactory),reinterpret_cast<void**>(&provider))==Steinberg::kResultOk && provider,"actual same native factory lacks matching independent ARA provider");
    std::unique_ptr<ARA::IMainFactory,void(*)(ARA::IMainFactory*)> ownProvider(provider,[](auto* value){value->release();});
    const std::weak_ptr<Impl::Record> weak=record;
    VST3ARAEntry::NativeLifecycle lifecycle;
    lifecycle.freshBeforeFirstBinding=[weak]{auto r=weak.lock();return r && r->fresh.load() && !r->stateCalls.load() && !r->activationEver.load() && !r->processingEver.load();};
    lifecycle.inactive=[weak]{auto r=weak.lock();return r && r->alive.load() && !r->active.load() && !r->processing.load() && !r->uncertain.load();};
    lifecycle.verifyBorrowedNativeFreshnessImmediatelyBeforeBind=proof.verifyActualBorrowedNativeFreshnessImmediatelyBeforeBinding;
    lifecycle.saveState=[]()->Bytes{throw std::logic_error("standalone native owner must call its original component state exactly once outside ARA archive capture");};
    lifecycle.restoreState=[](const Bytes&){throw std::logic_error("ARA archive must never call native original component state recursively");};
    record->observer=std::make_shared<Impl::Observer>();record->observer->id=record->identity.instanceID;record->observer->adapter=p->adapter;
    record->entry=std::make_shared<VST3ARAEntry>(component,*provider,factory.actualAlreadyLoadedModuleLifetime,std::move(lifecycle));record->lease=FactoryLease::acquire(record->entry,record->observer);
    record->host=std::make_unique<HostController>(record->lease,record->entry,"Native standalone ARA insert",nullptr,nullptr,ARA::kARAEditorRendererRole|ARA::kARAEditorViewRole,record->observer);
    record->fresh=false;
    {std::lock_guard<std::mutex> guard(p->recordsMutex);p->liveComponents.emplace(record->identity.actualComponent,record->identity.instanceID);p->records.emplace(record->identity.instanceID,record);}
    return record->identity.instanceID;
}
CapturedStandaloneSnapshot StandaloneARACoordinator::captureActualCurrentSnapshot(const std::string& id){
    p->requireMain();Operation operation(p->operation);auto record=p->find(id);p->requireHold(record);
    auto current=p->adapter->copyActualCurrentRAMAndClock(record->identity);p->requireHold(record);p->validateSnapshot(record,current);
    if(!record->applied || !sameModel(record->applied->data.model,current.model) || !sameGraph(record->applied->data.graph,current.graph)
        || record->applied->data.actualVerifiedAudioRecordIndices!=current.actualVerifiedAudioRecordIndices)record->observer->invalidateForARAEdit(0);
    return CapturedStandaloneSnapshot(p->makeSnapshot(std::move(current)));
}
EditorGraphRefreshReceipt StandaloneARACoordinator::refreshGraph(const std::string& id,const CapturedStandaloneSnapshot& snapshot){
    p->requireMain();Operation operation(p->operation);auto r=p->find(id);p->requireHold(r);p->validateToken(r,snapshot);p->validateContinuity(*r,snapshot.data());
    std::vector<ModificationClone> clones;for(const auto& clone:snapshot.data().explicitNativeClones){demand(clone.actualNativeCloneVerified && !clone.actualNativeCloneEvidence.empty(),"modification clone lacks actual native clone authority");clones.push_back(clone.objectIDs);}
    r->observer->invalidateForARAEdit(0);EditorGraphRefreshReceipt receipt;bool mutation=false;
    try{
        if(!r->graphReady){mutation=true;r->host->configureEditorGraphWhileActive(snapshot.data().graph,snapshot.data().resolveCurrentImmutableRAMSource);receipt.unchangedDocumentController=r->host->documentControllerIdentity();}
        else if(!sameGraph(r->host->graphMetadata(),snapshot.data().graph)){mutation=true;receipt=r->host->reconcileEditorGraphWhileActive(snapshot.data().graph,snapshot.data().resolveCurrentImmutableRAMSource,clones);}
        else receipt.unchangedDocumentController=r->host->documentControllerIdentity();
        p->requireHold(r);r->applied=snapshot.p;r->graphReady=true;r->phase="current-native-graph-ready-cache-invalid";return receipt;
    }catch(...){if(mutation)disableARAForInsert(id,"ARA native graph update failed; native session remains open");throw;}
}
void StandaloneARACoordinator::applyPlainNativeProjectReplacement(const std::string& id,const CapturedStandaloneSnapshot& snapshot,const StandaloneNativeStateLoadAuthority& load){
    p->requireMain();Operation operation(p->operation);auto r=p->find(id);r->observer->invalidateForARAEdit(0);
    try{p->requireHold(r);p->validateToken(r,snapshot);p->validateLoad(r,load);
        r->host->replaceEditorGraphWhileActive(snapshot.data().graph,snapshot.data().resolveCurrentImmutableRAMSource);p->requireHold(r);
        r->applied=snapshot.p;r->graphReady=true;r->consumedLoadEvents.insert(load.actualNativeLoadEventIdentity);r->phase="explicit-plain-native-project-replaced-prior-edits-cleared";
    }catch(...){disableARAForInsert(id,"ARA project replacement failed; native session remains open");throw;}
}
StandaloneSavedEditorArchive StandaloneARACoordinator::captureArchiveOnly(const std::string& id,const CapturedStandaloneSnapshot& snapshot){
    p->requireMain();Operation operation(p->operation);auto r=p->find(id);p->requireHold(r);p->validateToken(r,snapshot);
    demand(r->graphReady && r->applied && sameModel(r->applied->data.model,snapshot.data().model) && sameGraph(r->host->graphMetadata(),snapshot.data().graph),"native archive requires applied exact current RAM/clock/graph, never stale model metadata");
    StandaloneSavedEditorArchive saved;saved.pluginModuleSHA256=r->identity.pluginModuleSHA256;saved.factoryID=r->entry->factory()->factoryID;saved.nativeInsertKey=r->identity.nativeInsertKey;
    saved.native=snapshot.data().model;saved.actualCopiedNativeModelRecords=snapshot.data().actualCopiedNativeModelRecords;
    saved.actualVerifiedAudioRecordIndices=snapshot.data().actualVerifiedAudioRecordIndices;saved.ara=r->host->captureArchiveOnly();p->requireHold(r);return saved;
}
PreparedEditorArchive StandaloneARACoordinator::prepareEditorArchiveForNativeSave(const std::string& id,const CapturedStandaloneSnapshot& snapshot){
    p->requireMain();demand(!p->operation,"native-save preparation cannot reenter an ARA model operation");auto r=p->find(id);p->requireHold(r);
    demand(snapshot.p && snapshot.p->coordinatorRun==p->run && r->graphReady && r->applied && r->currentCacheSnapshot
        && standaloneNativeSaveSnapshotMatches(r->currentCacheSnapshot->data,snapshot.data(),false),"native-save preparation requires the exact current published source-cache model");
    const auto ticket=r->currentCacheTicket;const auto editRevision=r->host->editNotificationRevision();
    demand(r->observer->gate->authorizes(ticket) && editRevision==r->currentCacheEditRevision
        && r->currentCacheNeedsNoOverlay==snapshot.data().graph.regions.empty(),"native-save preparation requires an authorized stable source-cache or genuine no-overlay batch");
    // The ordinary native getState is not called or observed here. This public
    // method runs the genuine ARA capture at its normal worker-free boundary.
    auto saved=captureArchiveOnly(id,snapshot);
    p->requireHold(r);
    if(r->host->editNotificationRevision()!=editRevision || !r->observer->gate->authorizes(ticket))
        throw SourceCacheNotReady("ARA archive preparation flushed a model edit; rebuild caches before preparing native save");
    auto prepared=std::make_shared<PreparedEditorArchive::Impl>();prepared->archive=std::move(saved);
    prepared->coordinatorRun=p->run;prepared->captureLifetime=snapshot.p;prepared->actualSnapshot=&snapshot.data();
    prepared->identity={r->identity,snapshot.data().model,ticket,snapshot.captureSerial(),editRevision,r->currentCacheNeedsNoOverlay};
    if(r->host->editNotificationRevision()!=editRevision || !r->observer->gate->authorizes(ticket))
        throw SourceCacheNotReady("ARA model changed while completing native-save preparation");
    return PreparedEditorArchive(std::move(prepared));
}
StandaloneSavedEditorArchive StandaloneARACoordinator::validatePreparedEditorArchiveForNativeSave(const std::string& id,
    const PreparedEditorArchive& prepared,const StandaloneSnapshotData& actualCurrentNativeRAM)const{
    // Intentionally NO requireMain/requireHold adapter callback, provider
    // callback, native read, model polling or SDK call in this path. The owner
    // supplies its actual direct-RAM capture made at native file-open preflight.
    demand(std::this_thread::get_id()==p->modelThread && !p->operation,"prepared native save validation requires its model thread outside ARA operations");
    auto r=p->find(id);
    demand(prepared.p && prepared.p->coordinatorRun==p->run && prepared.p->actualSnapshot && prepared.p->captureLifetime
        && r->host && r->alive && !r->disabled && !r->observer->contractFailed && !r->uncertain && !r->preparedTerminate && !r->terminated
        && r->graphReady && r->applied && r->currentCacheSnapshot,"prepared native archive is unavailable or belongs to a different coordinator");
    p->validateSnapshot(r,actualCurrentNativeRAM,false);
    const auto& identity=prepared.p->identity;const auto& original=*prepared.p->actualSnapshot;
    demand(identity.actual.instanceID==id && identity.actual.actualComponent==r->identity.actualComponent
        && identity.currentCacheTicket.epoch==r->currentCacheTicket.epoch
        && identity.actualEditNotificationRevision==r->currentCacheEditRevision
        && identity.noSourceOverlayRequired==r->currentCacheNeedsNoOverlay
        && standaloneNativeSaveSnapshotMatches(original,actualCurrentNativeRAM)
        && standaloneNativeSaveSnapshotMatches(r->currentCacheSnapshot->data,actualCurrentNativeRAM,false)
        && standaloneNativeSaveSnapshotMatches(r->applied->data,actualCurrentNativeRAM,false),
           "prepared native archive no longer matches actual insert/source/graph/records/PCM/clock/cache authority");
    demand(r->host->editNotificationRevision()==identity.actualEditNotificationRevision
        && r->observer->gate->authorizes(identity.currentCacheTicket),"prepared native archive is stale, dirty or lacks its current cache ticket");
    auto archive=prepared.p->archive;
    demand(r->host->editNotificationRevision()==identity.actualEditNotificationRevision
        && r->observer->gate->authorizes(identity.currentCacheTicket),"prepared native archive became dirty while freezing the save transaction");
    return archive;
}
StandaloneArchiveRestoreReceipt StandaloneARACoordinator::restoreArchiveBeforeOriginalState(const std::string& id,const StandaloneSavedEditorArchive& saved,
    StandaloneArchiveSourceValidator& validator,StandaloneArchiveApplication mode){
    p->requireMain();Operation operation(p->operation);auto r=p->find(id);r->observer->invalidateForARAEdit(0);
    bool mutationStarted=false;
    try{
    p->requireHold(r);
    demand(saved.schema==1 && saved.pluginModuleSHA256==r->identity.pluginModuleSHA256 && saved.factoryID==r->entry->factory()->factoryID
        && saved.ara.factoryID==saved.factoryID && saved.nativeInsertKey==r->identity.nativeInsertKey,"archive does not belong to exact actual plug-in/factory/native insert");
    if(mode==StandaloneArchiveApplication::FreshBeforeFirstOriginalState)demand(!r->graphReady && !r->applied && !r->stateCalls.load() && !r->activationEver.load() && !r->processingEver.load() && p->actuallyInactive(r),"fresh archive restore must precede all native state/activation/processing");
    auto validation=validator.validateActualCurrentRAMForArchive(saved,r->identity);p->requireHold(r);p->validateSnapshot(r,validation.current);p->validateLoad(r,validation.load);
    const auto& now=validation.current;
    demand(saved.actualVerifiedAudioRecordIndices==now.actualVerifiedAudioRecordIndices,"actual native audio/non-audio row classification changed across archive load");
    demand(saved.native.nativeEngineSHA256==now.model.nativeEngineSHA256 && saved.native.nativeSaveVersion==now.model.nativeSaveVersion
        && sameClock(saved.native.clock,now.model.clock) && saved.native.sourcePCMModelSHA256==now.model.sourcePCMModelSHA256,"archive native engine/version/clock/actual source PCM differs");
    if(validation.objectIDRelocationVerified){
        demand(sameModel(saved.native,validation.verifiedOldIncomingNativeModel)
            && archiveObjectIDRelocationMatchesGraphs(saved.ara.graph,now.graph,validation.objectIDs),"actual old native input proof or full physical object-ID mapping differs");
    }else demand(validation.objectIDs.sourceIDs.empty() && validation.objectIDs.modificationIDs.empty() && validation.objectIDs.regionIDs.empty()
        && sameModel(saved.native,now.model) && sameGraph(saved.ara.graph,now.graph),"identity archive restore differs; no implicit relocation/fallback permitted");
    const auto dc=r->host->documentControllerIdentity();
        const auto* ids=validation.objectIDRelocationVerified?&validation.objectIDs:nullptr;
        mutationStarted=true;
        if(mode==StandaloneArchiveApplication::ExistingBoundNativeProjectReplacement)r->host->replaceEditorGraphWhileActive(now.graph,now.resolveCurrentImmutableRAMSource,&saved.ara,ids);
        else if(ids)r->host->restoreArchiveOnlyRelocated(saved.ara,now.graph,*ids,now.resolveCurrentImmutableRAMSource);
        else r->host->restoreArchiveOnly(saved.ara,now.resolveCurrentImmutableRAMSource);
        p->requireHold(r);r->applied=p->makeSnapshot(std::move(validation.current));r->graphReady=saved.ara.nativeGraphReady;
        r->consumedLoadEvents.insert(validation.load.actualNativeLoadEventIdentity);r->phase="actual-native-editor-archive-restored-before-original-state";
        return {r->host->documentControllerIdentity()==dc,validation.objectIDRelocationVerified,false,dc,validation.load.actualNativeLoadEventIdentity,r->applied->data.model,validation.objectIDs};
    }catch(const NativeModelBoundaryUnavailable&){
        if(mutationStarted)disableARAForInsert(id,"Native model boundary failed after ARA archive mutation started; native session remains open");
        throw;
    }catch(...){disableARAForInsert(id,"ARA project archive validation or restore failed; native session remains open");throw;}
}
void StandaloneARACoordinator::pollAtStoppedNativeBoundary(const std::string& id){p->requireMain();Operation operation(p->operation);auto r=p->find(id);p->requireHold(r);r->host->pollEditorModelUpdatesWhileActive();p->requireHold(r);}
StandaloneSourceCacheBatch StandaloneARACoordinator::rebuildWholeSourceCaches(const std::string& id,const CapturedStandaloneSnapshot& snapshot,const WholeSourceCacheOptions& options){
    p->requireMain();Operation operation(p->operation);auto r=p->find(id);p->requireHold(r);p->validateToken(r,snapshot);
    demand(r->graphReady && r->applied && sameModel(r->applied->data.model,snapshot.data().model) && sameGraph(r->host->graphMetadata(),snapshot.data().graph),"source cache requires exact applied current native model/clock");
    const auto ticket=r->observer->invalidateForCurrentRebuild();
    const auto editRevision=r->host->editNotificationRevision();
    StandaloneSourceCacheBatch batch;batch.instanceID=id;batch.ticket=ticket;batch.captureSerial=snapshot.captureSerial();batch.actualEditNotificationRevision=editRevision;batch.native=snapshot.data().model;batch.gate=r->observer->gate;
    const auto plan=planStandaloneSourceCaches(snapshot.data().graph);batch.noSourceOverlayRequired=plan.noSourceOverlayRequired;
    batch.preservedUnreferencedModificationIDs=plan.preservedUnreferencedModificationIDs;
    for(const auto& modificationID:plan.renderModificationIDs){
        p->requireHold(r);OwnedVST3PlaybackFactoryContext factory;factory.actualSameNativeFactory=r->factory.actualSameNativeFactory;
        factory.audioClassID=r->factory.audioClassID;factory.araClassID=r->factory.araClassID;factory.alreadyLoadedModuleLifetime=r->factory.actualAlreadyLoadedModuleLifetime;
        factory.initializedFactoryLease=r->lease;factory.forbiddenBorrowedNativeComponent=r->identity.actualComponent;
        const auto hold=[&]{try{p->requireNativeBoundary(r);return true;}catch(...){return false;}};
        // Actual DSP/ARA only. Missing content or unsupported real head/tail/
        // latency refuses publication. No private QA render or fitted alignment.
        batch.modifications.push_back(r->host->renderWholeModificationSourceCache(modificationID,OwnedVST3PlaybackCompanion::create(factory),options,hold));
    }
    p->requireHold(r);
    demand(!r->host->callbackFailed(),"actual ARA source callback failed during source cache build");
    if(r->host->editNotificationRevision()!=editRevision || !r->observer->gate->publishIfUnchanged(ticket))
        throw SourceCacheNotReady("actual ARA/native model changed during source cache render; poll/capture/rebuild before publication");
    r->currentCacheSnapshot=snapshot.p;r->currentCacheTicket=ticket;r->currentCacheEditRevision=editRevision;
    r->currentCacheNeedsNoOverlay=batch.noSourceOverlayRequired;
    r->phase=batch.noSourceOverlayRequired?"current-native-model-authorized-no-source-overlay":"current-native-source-caches-published";return batch;
}
std::shared_ptr<StandaloneCacheGate> StandaloneARACoordinator::cacheGate(const std::string& id)const{return p->find(id)->observer->gate;}
void StandaloneARACoordinator::observeOriginalSetActiveResult(const std::string& id,bool active,Steinberg::tresult result){auto r=p->find(id);
    if(result==Steinberg::kResultOk){r->active=active;if(active)r->activationEver=true;}else{r->uncertain=true;disableARAForInsert(id,"Native plug-in activation result is uncertain; ARA disabled for this insert");}}
void StandaloneARACoordinator::observeOriginalSetProcessingResult(const std::string& id,bool processing,Steinberg::tresult result){auto r=p->find(id);
    if(result==Steinberg::kResultOk){r->processing=processing;if(processing)r->processingEver=true;}else{r->uncertain=true;disableARAForInsert(id,"Native plug-in processing result is uncertain; ARA disabled for this insert");}}
void StandaloneARACoordinator::observeOriginalSetStateResult(const std::string& id,Steinberg::tresult result){auto r=p->find(id);
    if(r->stateCalls.fetch_add(1)==std::numeric_limits<std::uint32_t>::max()){r->stateCalls=std::numeric_limits<std::uint32_t>::max();disableARAForInsert(id,"Native plug-in state-call counter exhausted");}
    if(result!=Steinberg::kResultOk)disableARAForInsert(id,"Native plug-in state restore failed; ARA disabled for this insert and native session retained");}
void StandaloneARACoordinator::invalidateSourceCachesAndRequestRebuild(const std::string& id)noexcept{try{p->find(id)->observer->invalidateForARAEdit(0);}catch(...){}}
void StandaloneARACoordinator::disableARAForInsert(const std::string& id,const std::string& reason)noexcept{try{auto r=p->find(id);r->disabled=true;r->observer->gate->closePermanently();p->adapter->invalidateActualNativeOverlayImmediately(id,r->observer->gate->currentTicket().epoch);p->adapter->queueActualNativeStopForCacheRebuild(id,r->observer->gate->currentTicket().epoch);p->adapter->showARAUnavailableForInsert(id,reason);}catch(...){}}
StandaloneCoordinatorRecord StandaloneARACoordinator::record(const std::string& id)const{p->requireMain();auto r=p->find(id);const bool disabled=r->disabled || r->observer->contractFailed;return {r->identity,bool(r->host),r->graphReady,disabled,r->active.load(),r->processing.load(),r->uncertain.load(),r->activationEver.load(),r->processingEver.load(),r->stateCalls.load(),r->applied?r->applied->serial:0,r->observer->gate->currentTicket().epoch,disabled?"disabled-for-this-insert":r->phase};}
Graph StandaloneARACoordinator::graphMetadata(const std::string& id)const{p->requireMain();auto r=p->find(id);demand(r->host && r->graphReady && !r->disabled,"standalone current graph unavailable");return r->host->graphMetadata();}
void StandaloneARACoordinator::prepareForOriginalTerminate(const std::string& id){p->requireMain();Operation operation(p->operation);auto r=p->find(id);
    demand(r->host && r->alive && !r->preparedTerminate && !r->terminated,"standalone own ARA teardown outside a live unprepared component");
    const auto boundary=p->adapter->verifyActualNativeTeardownBoundary(r->identity);
    demand(boundary.authorizesKnownComponent(r->identity.actualComponent),"exact actual native pre-terminate component boundary is missing or not drained");
    demand(p->actuallyInactive(r) && r->host->safeForOwnedTeardown(),"actual native component must stop processing/deactivate and drain before own ARA teardown");
    r->observer->gate->closePermanently();p->adapter->invalidateActualNativeOverlayImmediately(id,r->observer->gate->currentTicket().epoch);
    r->host.reset();r->entry.reset();r->graphReady=false;r->preparedTerminate=true;r->phase="prepared-before-original-native-terminate";}
void StandaloneARACoordinator::observeOriginalTerminateResult(const std::string& id,Steinberg::tresult result){p->requireMain();Operation operation(p->operation);auto r=p->find(id);
    demand(r->preparedTerminate && !r->host && !r->entry,"native terminate arrived before own ARA teardown");
    if(result!=Steinberg::kResultOk){r->uncertain=true;disableARAForInsert(id,"Native plug-in termination failed; factory/module retained");return;}
    r->alive=false;r->terminated=true;r->lease.reset();r->retainedNativeFactory.reset();r->factory.actualAlreadyLoadedModuleLifetime.reset();r->phase="original-native-terminate-succeeded";
    std::lock_guard<std::mutex> guard(p->recordsMutex);p->liveComponents.erase(r->identity.actualComponent);
}
} // namespace rysup::ara_lab
