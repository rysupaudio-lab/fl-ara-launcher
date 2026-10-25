#pragma once
// Owner-neutral native VST3/ARA coordination. No AudioUnit/CFPropertyList,
// app launching, file import, plug-in-specific API, audio device or UI code.
#include "GenericARAHost.h"
#include "StandaloneCacheGate.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace rysup::ara_lab {
// Actual native model boundary is temporarily unavailable (for example a
// verified owner worker-mix EBUSY). Retry only before any ARA model mutation;
// this is not a permission to continue after an uncertain SDK operation.
class NativeModelBoundaryUnavailable : public std::runtime_error {
public:using std::runtime_error::runtime_error;
};
struct StandaloneInstanceIdentity {
    std::string instanceID,ownRunID,nativeInsertKey;
    int ownProcessID=0;
    std::uintptr_t actualComponent=0,actualNativeEngineOwner=0;
    std::string pluginModuleSHA256,nativeEngineSHA256;
};
struct StandaloneNativeClock {
    std::uint32_t ppq=0,tempoMilliBPM=0;
    double sampleRate=0;
    std::int64_t nativeSamplePosition=0;
    bool actualNativePlaying=false;
    std::string clockFormatIdentity;
    // Exact copied clock bytes from the sealed native owner. No serialization
    // getter, saved-file clock, ProcessContext guess or assumed default clock.
    Bytes actualNativeClockBytes;
};
struct StandaloneSourceIdentity {
    std::string sourceID,nativePhysicalSourceKey,actualPCM_SHA256;
};
struct StandaloneModificationIdentity {
    std::string modificationID,nativePhysicalEditKey,sourceID;
};
struct StandaloneRegionIdentity {
    std::string regionID,nativePhysicalRegionKey,modificationID;
};
struct StandaloneExplicitNativeClone {
    ModificationClone objectIDs;
    bool actualNativeCloneVerified=false;
    std::string actualNativeCloneEvidence;
};
struct StandaloneNativeModelAuthority {
    std::string nativeProjectIdentity,nativeEngineSHA256,nativeRecordModelSHA256;
    std::string sourcePCMModelSHA256,graphSHA256,nativeSaveVersion;
    StandaloneNativeClock clock;
};
struct StandaloneSnapshotData {
    StandaloneInstanceIdentity actual;
    StandaloneNativeModelAuthority model;
    Graph graph;
    HostController::ResolveSource resolveCurrentImmutableRAMSource;
    std::vector<StandaloneSourceIdentity> sources;
    std::vector<StandaloneModificationIdentity> modifications;
    std::vector<StandaloneRegionIdentity> regions;
    std::vector<StandaloneExplicitNativeClone> explicitNativeClones;
    Bytes actualCopiedNativeModelRecords;
    // Actual native channel-kind classification supplied by the typed owner
    // decoder. Codec may mask a qualified audio-only field only at these rows.
    std::vector<std::size_t> actualVerifiedAudioRecordIndices;
    std::string decoderIdentity,decoderSourceSHA256;
    bool actualCurrentNativeRAMCopiedAndVerified=false;
    bool actualDirectNativeClockReadTwiceAndEqual=false;
    bool nativeComponentOwnerInsertAssociationVerified=false;
    bool noNativeSerializationOrFileFallbackUsed=false;
};
// Pure, bounded-data comparison used at native file-open preflight. It never
// invokes the resolver, a SourceProvider, owner adapter, ARA or native SDK.
// Transport position can be ignored only when checking full-source cache
// provenance, since full-source PCM is independent of the stopped playhead.
inline bool standaloneNativeSaveSnapshotMatches(const StandaloneSnapshotData& a,const StandaloneSnapshotData& b,
                                                bool compareTransportPosition=true){
    const auto bounded=[](const StandaloneSnapshotData& s){return s.sources.size()<=4096 && s.modifications.size()<=100000 && s.regions.size()<=100000
        && s.graph.sources.size()<=4096 && s.graph.tracks.size()<=4096 && s.graph.modifications.size()<=100000 && s.graph.regions.size()<=100000
        && s.actualCopiedNativeModelRecords.size()<=32u*1024u*1024u && s.actualVerifiedAudioRecordIndices.size()<=100000;};
    if(!bounded(a) || !bounded(b))return false;
    if(!a.actualCurrentNativeRAMCopiedAndVerified || !b.actualCurrentNativeRAMCopiedAndVerified
        || !a.actualDirectNativeClockReadTwiceAndEqual || !b.actualDirectNativeClockReadTwiceAndEqual
        || !a.nativeComponentOwnerInsertAssociationVerified || !b.nativeComponentOwnerInsertAssociationVerified
        || !a.noNativeSerializationOrFileFallbackUsed || !b.noNativeSerializationOrFileFallbackUsed)return false;
    const auto& ai=a.actual;const auto& bi=b.actual;
    if(ai.instanceID!=bi.instanceID || ai.ownRunID!=bi.ownRunID || ai.ownProcessID!=bi.ownProcessID
        || ai.nativeInsertKey!=bi.nativeInsertKey || ai.actualComponent!=bi.actualComponent || ai.actualNativeEngineOwner!=bi.actualNativeEngineOwner
        || ai.pluginModuleSHA256!=bi.pluginModuleSHA256 || ai.nativeEngineSHA256!=bi.nativeEngineSHA256)return false;
    const auto& am=a.model;const auto& bm=b.model;const auto& ac=am.clock;const auto& bc=bm.clock;
    if(am.nativeProjectIdentity!=bm.nativeProjectIdentity || am.nativeEngineSHA256!=bm.nativeEngineSHA256
        || am.nativeRecordModelSHA256!=bm.nativeRecordModelSHA256 || am.sourcePCMModelSHA256!=bm.sourcePCMModelSHA256
        || am.graphSHA256!=bm.graphSHA256 || am.nativeSaveVersion!=bm.nativeSaveVersion
        || ac.ppq!=bc.ppq || ac.tempoMilliBPM!=bc.tempoMilliBPM || ac.sampleRate!=bc.sampleRate
        || ac.actualNativePlaying || bc.actualNativePlaying || ac.clockFormatIdentity!=bc.clockFormatIdentity
        || ac.actualNativeClockBytes!=bc.actualNativeClockBytes
        || (compareTransportPosition && ac.nativeSamplePosition!=bc.nativeSamplePosition)
        || a.actualCopiedNativeModelRecords!=b.actualCopiedNativeModelRecords
        || a.actualVerifiedAudioRecordIndices!=b.actualVerifiedAudioRecordIndices
        || a.decoderIdentity!=b.decoderIdentity || a.decoderSourceSHA256!=b.decoderSourceSHA256)return false;
    const auto sorted=[](auto list,auto key){std::sort(list.begin(),list.end(),[&](const auto& x,const auto& y){return key(x)<key(y);});return list;};
    const auto sourcesA=sorted(a.sources,[](const auto& x){return x.sourceID;}),sourcesB=sorted(b.sources,[](const auto& x){return x.sourceID;});
    const auto modsA=sorted(a.modifications,[](const auto& x){return x.modificationID;}),modsB=sorted(b.modifications,[](const auto& x){return x.modificationID;});
    const auto regionsA=sorted(a.regions,[](const auto& x){return x.regionID;}),regionsB=sorted(b.regions,[](const auto& x){return x.regionID;});
    if(sourcesA.size()!=sourcesB.size() || modsA.size()!=modsB.size() || regionsA.size()!=regionsB.size())return false;
    for(std::size_t i=0;i<sourcesA.size();++i)if(sourcesA[i].sourceID!=sourcesB[i].sourceID
        || sourcesA[i].nativePhysicalSourceKey!=sourcesB[i].nativePhysicalSourceKey || sourcesA[i].actualPCM_SHA256!=sourcesB[i].actualPCM_SHA256)return false;
    for(std::size_t i=0;i<modsA.size();++i)if(modsA[i].modificationID!=modsB[i].modificationID
        || modsA[i].nativePhysicalEditKey!=modsB[i].nativePhysicalEditKey || modsA[i].sourceID!=modsB[i].sourceID)return false;
    for(std::size_t i=0;i<regionsA.size();++i)if(regionsA[i].regionID!=regionsB[i].regionID
        || regionsA[i].nativePhysicalRegionKey!=regionsB[i].nativePhysicalRegionKey || regionsA[i].modificationID!=regionsB[i].modificationID)return false;
    const auto& ag=a.graph;const auto& bg=b.graph;
    if(ag.documentName!=bg.documentName || ag.sources.size()!=bg.sources.size() || ag.tracks.size()!=bg.tracks.size()
        || ag.modifications.size()!=bg.modifications.size() || ag.regions.size()!=bg.regions.size())return false;
    const auto sourceA=sorted(ag.sources,[](const auto& x){return x.id;}),sourceB=sorted(bg.sources,[](const auto& x){return x.id;});
    const auto trackA=sorted(ag.tracks,[](const auto& x){return x.id;}),trackB=sorted(bg.tracks,[](const auto& x){return x.id;});
    const auto modA=sorted(ag.modifications,[](const auto& x){return x.id;}),modB=sorted(bg.modifications,[](const auto& x){return x.id;});
    const auto regionA=sorted(ag.regions,[](const auto& x){return x.id;}),regionB=sorted(bg.regions,[](const auto& x){return x.id;});
    for(std::size_t i=0;i<sourceA.size();++i)if(sourceA[i].id!=sourceB[i].id || sourceA[i].name!=sourceB[i].name
        || sourceA[i].sampleCount!=sourceB[i].sampleCount || sourceA[i].sampleRate!=sourceB[i].sampleRate || sourceA[i].channelCount!=sourceB[i].channelCount)return false;
    for(std::size_t i=0;i<trackA.size();++i)if(trackA[i].id!=trackB[i].id || trackA[i].name!=trackB[i].name)return false;
    for(std::size_t i=0;i<modA.size();++i)if(modA[i].id!=modB[i].id || modA[i].sourceID!=modB[i].sourceID || modA[i].name!=modB[i].name)return false;
    for(std::size_t i=0;i<regionA.size();++i)if(regionA[i].id!=regionB[i].id || regionA[i].modificationID!=regionB[i].modificationID
        || regionA[i].trackID!=regionB[i].trackID || regionA[i].name!=regionB[i].name || regionA[i].transformations!=regionB[i].transformations
        || regionA[i].sourceStart!=regionB[i].sourceStart || regionA[i].sourceDuration!=regionB[i].sourceDuration
        || regionA[i].playbackStart!=regionB[i].playbackStart || regionA[i].playbackDuration!=regionB[i].playbackDuration)return false;
    return true;
}
// Constructible only by the coordinator after it invokes an actual native
// current-RAM source at a verified stopped/drained owner boundary.
class CapturedStandaloneSnapshot {
public:
    const StandaloneSnapshotData& data()const noexcept;
    std::uint64_t captureSerial()const noexcept;
private:
    struct Impl;
    explicit CapturedStandaloneSnapshot(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> p;
    friend class StandaloneARACoordinator;
};
struct StandaloneNativeHold {
    bool actualOwnedNativeHold=false,actualNativeStopped=false;
    bool outsideNativeRenderStateAndLifecycleCalls=false;
    bool actualOwnerAndCurrentModelThreadVerified=false;
    std::uint32_t actualNativeRenderCallsInFlight=0;
};
// Distinct from model/render admission. An inactive effect may be removed while
// the song plays, or after the global engine has gone away. This proof concerns
// only the exact known native component's synchronous pre-terminate callback.
struct StandaloneNativeTeardownBoundary {
    std::uintptr_t actualComponent=0;
    bool exactKnownOwnedComponentVerified=false;
    bool insideSynchronousNativeBeforeOriginalTerminate=false;
    bool onActualModelThread=false;
    bool actualOriginalSetActiveFalseObserved=false;
    bool actualOriginalSetProcessingFalseObserved=false;
    bool ownNativeProcessGateClosed=false;
    std::uint32_t actualOriginalProcessCallsInFlight=~std::uint32_t{0};
    bool nativeOriginalTerminateNotYetCalled=false;
    constexpr bool authorizesKnownComponent(std::uintptr_t known)const noexcept{
        return known && actualComponent==known && exactKnownOwnedComponentVerified
            && insideSynchronousNativeBeforeOriginalTerminate && onActualModelThread
            && actualOriginalSetActiveFalseObserved && actualOriginalSetProcessingFalseObserved
            && ownNativeProcessGateClosed && actualOriginalProcessCallsInFlight==0
            && nativeOriginalTerminateNotYetCalled;
    }
};
class StandaloneOwnerAdapter {
public:
    virtual ~StandaloneOwnerAdapter()=default;
    virtual bool onActualModelThread()const noexcept=0;
    virtual StandaloneNativeHold verifyActualNativeHold(const StandaloneInstanceIdentity&)const=0;
    // Default closed so an older adapter can never accidentally authorize this
    // separate path. No global transport/clock/current-RAM predicate applies.
    virtual StandaloneNativeTeardownBoundary verifyActualNativeTeardownBoundary(const StandaloneInstanceIdentity&)const{return {};}
    virtual StandaloneSnapshotData copyActualCurrentRAMAndClock(const StandaloneInstanceIdentity&)=0;
    // May run on an ARA notification thread. These must only close the actual
    // atomic overlay gate/enqueue work; never call ARA/native SDK or wait here.
    virtual void invalidateActualNativeOverlayImmediately(const std::string&,std::uint64_t epoch)noexcept=0;
    virtual void enqueueModelThreadRebuild(const std::string&)noexcept=0;
    // Queue-only request. Main owner coalesces it, verifies actual transport,
    // stops outside native Save, rebuilds while held, and never auto-resumes.
    virtual void queueActualNativeStopForCacheRebuild(const std::string&,std::uint64_t invalidatedEpoch)noexcept{(void)invalidatedEpoch;}
    // Actual nonzero ARA revisions only. Queue an atomic pending revision here;
    // native document dirty calls run later on the actual owner/main thread.
    virtual void queueActualNativeDocumentDirtyFromARA(const std::string&,std::uint64_t)noexcept{}
    virtual void showARAUnavailableForInsert(const std::string&,const std::string& reason)noexcept=0;
};
struct StandaloneFreshNativeInitialization {
    bool originalInitializeSucceeded=false;
    bool actualNativeComponentFactoryAndOwnerVerified=false;
    bool actualLoadedComponentAndFactoryImageVerified=false;
    bool beforeFirstNativeStateActivationProcessingContextOrEditor=false;
    bool nativeOwnerDoesNotAlreadyInitializeARA=false;
    // Optional strict borrowed-native owner check. The normal-app bridge
    // supplies this to reread actual hook counters/main/alive/context/owner
    // after factory/DC creation and immediately before the actual ARA bind.
    // This callback must be pure observation: no SDK, native engine call or wait.
    std::function<bool()> verifyActualBorrowedNativeFreshnessImmediatelyBeforeBinding;
};
struct StandaloneVST3FactoryContext {
    Steinberg::IPluginFactory* actualSameNativeFactory=nullptr;
    std::array<char,16> audioClassID{},araClassID{};
    std::shared_ptr<void> actualAlreadyLoadedModuleLifetime;
};
// An immutable PCM batch is usable only while its ticket is current. Root must
// test the ticket at EVERY actual source overlay read, not merely publication.
struct StandaloneSourceCachePlan {
    bool noSourceOverlayRequired=false;
    std::vector<std::string> renderModificationIDs,preservedUnreferencedModificationIDs;
};
// Pure model-thread plan. Existing source/modification models with no current
// Playlist regions stay in the ARA document for Undo/archive, not the overlay.
inline StandaloneSourceCachePlan planStandaloneSourceCaches(const Graph& graph){
    StandaloneSourceCachePlan plan;plan.noSourceOverlayRequired=graph.regions.empty();
    std::set<std::string> known,referenced;
    for(const auto& mod:graph.modifications)if(mod.id.empty() || !known.insert(mod.id).second)
        throw std::invalid_argument("source-cache graph has duplicate/empty modification identity");
    for(const auto& region:graph.regions){if(!known.count(region.modificationID))
        throw std::invalid_argument("source-cache region has no actual modification model");referenced.insert(region.modificationID);}
    for(const auto& mod:graph.modifications)
        (referenced.count(mod.id)?plan.renderModificationIDs:plan.preservedUnreferencedModificationIDs).push_back(mod.id);
    return plan;
}
struct StandaloneSourceCacheBatch {
    std::string instanceID;
    StandaloneCacheTicket ticket;
    std::uint64_t captureSerial=0,actualEditNotificationRevision=0;
    StandaloneNativeModelAuthority native;
    std::vector<WholeModificationSourceCache> modifications;
    bool noSourceOverlayRequired=false;
    std::vector<std::string> preservedUnreferencedModificationIDs;
    std::shared_ptr<StandaloneCacheGate> gate;
};
struct StandaloneSavedEditorArchive {
    unsigned schema=1;
    std::string pluginModuleSHA256,factoryID,nativeInsertKey;
    StandaloneNativeModelAuthority native;
    Bytes actualCopiedNativeModelRecords;
    std::vector<std::size_t> actualVerifiedAudioRecordIndices;
    ArchiveSnapshot ara;
};
struct PreparedEditorArchiveIdentity {
    StandaloneInstanceIdentity actual;
    StandaloneNativeModelAuthority native;
    StandaloneCacheTicket currentCacheTicket;
    std::uint64_t captureSerial=0,actualEditNotificationRevision=0;
    bool noSourceOverlayRequired=false;
};
// Minted only after genuine ARA archive capture at the owned model boundary.
// Copying preserves immutable authority; no unvalidated archive getter exists.
// Ordinary native component bytes are deliberately absent.
class PreparedEditorArchive {
public:
    const PreparedEditorArchiveIdentity& identity()const noexcept;
private:
    struct Impl;
    explicit PreparedEditorArchive(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> p;
    friend class StandaloneARACoordinator;
};
struct StandaloneNativeStateLoadAuthority {
    std::uintptr_t actualNativeEngineOwner=0,actualComponent=0;
    std::string nativeInsertKey,actualNativeLoadEventIdentity;
    bool actualNativeStateLoadCallOrAttestedDeferredWindow=false;
    bool exactSealedNativeCallerAndOwnerVerified=false;
    bool actualCurrentNativeLoadArgumentAcceptedAndUnconsumed=false;
};
struct StandaloneArchiveRestoreValidation {
    StandaloneSnapshotData current;
    StandaloneNativeStateLoadAuthority load;
    // A specific native ID regeneration proof, independently checked against
    // actual old native input records and current RAM by the owner decoder.
    bool objectIDRelocationVerified=false;
    StandaloneNativeModelAuthority verifiedOldIncomingNativeModel;
    ArchiveObjectIDRelocation objectIDs;
};
class StandaloneArchiveSourceValidator {
public:
    virtual ~StandaloneArchiveSourceValidator()=default;
    virtual StandaloneArchiveRestoreValidation validateActualCurrentRAMForArchive(
        const StandaloneSavedEditorArchive&,const StandaloneInstanceIdentity&)=0;
};
enum class StandaloneArchiveApplication {FreshBeforeFirstOriginalState,ExistingBoundNativeProjectReplacement};
struct StandaloneArchiveRestoreReceipt {
    bool sameDocumentController=true,objectIDsRelocated=false,originalComponentStateCalled=false;
    std::uintptr_t actualDocumentController=0;
    std::string nativeLoadEventIdentity;
    StandaloneNativeModelAuthority current;
    ArchiveObjectIDRelocation objectIDs;
};
struct StandaloneCoordinatorRecord {
    StandaloneInstanceIdentity identity;
    bool bound=false,graphReady=false,disabled=false;
    bool nativeActive=false,nativeProcessing=false,lifecycleUncertain=false;
    bool nativeActivationEverObserved=false,nativeProcessingEverObserved=false;
    std::uint32_t originalStateCalls=0;
    std::uint64_t appliedCaptureSerial=0,cacheEpoch=0;
    std::string phase;
};
class StandaloneARACoordinator {
public:
    explicit StandaloneARACoordinator(std::string ownRunID,std::shared_ptr<StandaloneOwnerAdapter>);
    ~StandaloneARACoordinator();
    StandaloneARACoordinator(const StandaloneARACoordinator&)=delete;
    StandaloneARACoordinator& operator=(const StandaloneARACoordinator&)=delete;
    // Borrow only the actual just-initialized component. Query independent ARA
    // provider from its exact native factory; retain factory/module separately.
    std::string bindAfterOriginalInitialize(Steinberg::Vst::IComponent&,
        StandaloneInstanceIdentity,const StandaloneVST3FactoryContext&,
        const StandaloneFreshNativeInitialization&);
    CapturedStandaloneSnapshot captureActualCurrentSnapshot(const std::string&);
    EditorGraphRefreshReceipt refreshGraph(const std::string&,const CapturedStandaloneSnapshot&);
    // Only a real native new-project/load transition may clear prior edits.
    void applyPlainNativeProjectReplacement(const std::string&,const CapturedStandaloneSnapshot&,
                                            const StandaloneNativeStateLoadAuthority&);
    StandaloneSavedEditorArchive captureArchiveOnly(const std::string&,const CapturedStandaloneSnapshot&);
    PreparedEditorArchive prepareEditorArchiveForNativeSave(const std::string&,const CapturedStandaloneSnapshot&);
    // Pure metadata/atomic validation at actual native file-open preflight:
    // does not request a worker-free boundary or enter any native/ARA SDK call.
    StandaloneSavedEditorArchive validatePreparedEditorArchiveForNativeSave(const std::string&,
        const PreparedEditorArchive&,const StandaloneSnapshotData& actualCurrentNativeRAM)const;
    StandaloneArchiveRestoreReceipt restoreArchiveBeforeOriginalState(const std::string&,
        const StandaloneSavedEditorArchive&,StandaloneArchiveSourceValidator&,StandaloneArchiveApplication);
    void pollAtStoppedNativeBoundary(const std::string&);
    StandaloneSourceCacheBatch rebuildWholeSourceCaches(const std::string&,const CapturedStandaloneSnapshot&,
                                                       const WholeSourceCacheOptions&);
    std::shared_ptr<StandaloneCacheGate> cacheGate(const std::string&)const;
    void observeOriginalSetActiveResult(const std::string&,bool,Steinberg::tresult);
    void observeOriginalSetProcessingResult(const std::string&,bool,Steinberg::tresult);
    void observeOriginalSetStateResult(const std::string&,Steinberg::tresult);
    // Call immediately on actual native Playlist/source/project change events.
    // Safe off the model thread: invalidate first, then coalesce queued rebuild.
    void invalidateSourceCachesAndRequestRebuild(const std::string&)noexcept;
    void disableARAForInsert(const std::string&,const std::string& reason)noexcept;
    StandaloneCoordinatorRecord record(const std::string&)const;
    Graph graphMetadata(const std::string&)const;
    // Native owner closes editor and stops/drains real rendering first. This
    // deletes only owned ARA model/provider refs; never calls native terminate.
    void prepareForOriginalTerminate(const std::string&);
    void observeOriginalTerminateResult(const std::string&,Steinberg::tresult);
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace rysup::ara_lab
