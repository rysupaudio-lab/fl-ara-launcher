#pragma once
// Owner-neutral native VST3/ARA coordination. No AudioUnit/CFPropertyList,
// app launching, file import, plug-in-specific API, audio device or UI code.
#include "GenericARAHost.h"
#include "StandaloneCacheGate.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <array>
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
