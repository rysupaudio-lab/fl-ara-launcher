#pragma once
// PRIVATE RESEARCH SCAFFOLD. This is an ARA2 host, not an FL timeline adapter.
#include <ARA_API/ARAInterface.h>
#include <ARA_Library/Dispatch/ARAHostDispatch.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rysup::ara_lab {
using Bytes = std::vector<ARA::ARAByte>;
// Actual unfinished analysis/generation, or a safely closed model-revision race.
// Caller keeps the source gate closed and polls/rebuilds at its next real hold.
// Contract, unsupported format/timing and uncertain lifecycle failures are not
// retryable by this type.
class SourceCacheNotReady final : public std::runtime_error {
public:using std::runtime_error::runtime_error;
};

struct SourceDescriptor {
    std::string id, name;
    std::int64_t sampleCount = 0;
    double sampleRate = 0;
    int channelCount = 0;
};
class SourceReader {
public:
    virtual ~SourceReader() = default;
    // Planar buffers use the format requested by openReader(). Never called on
    // a realtime rendering thread. Different readers may be used concurrently.
    // Out-of-source positions must be zero-filled and still return true.
    virtual bool read(std::int64_t start, std::int64_t frames,
                      void* const channels[]) noexcept = 0;
};
class SourceProvider {
public:
    virtual ~SourceProvider() = default;
    virtual SourceDescriptor describe() const = 0;
    virtual std::unique_ptr<SourceReader> openReader(bool float64) = 0;
};
struct TrackDescriptor { std::string id, name; };
struct ModificationDescriptor { std::string id, sourceID, name; };
struct RegionDescriptor {
    std::string id, modificationID, trackID, name;
    double sourceStart = 0, sourceDuration = 0;
    double playbackStart = 0, playbackDuration = 0;
    ARA::ARAPlaybackTransformationFlags transformations = ARA::kARAPlaybackTransformationNoChanges;
};
struct Graph {
    std::string documentName;
    std::vector<SourceDescriptor> sources;
    std::vector<TrackDescriptor> tracks;
    std::vector<ModificationDescriptor> modifications;
    std::vector<RegionDescriptor> regions;
};
// first = persistent ID in the untouched archive; second = current graph ID.
using ArchivePersistentIDPair = std::pair<std::string,std::string>;
struct ArchiveObjectIDRelocation {
    std::vector<ArchivePersistentIDPair> sourceIDs,modificationIDs,regionIDs;
};
struct ModificationClone {std::string currentNewID,existingModificationID;};
struct EditorGraphRefreshReceipt {
    std::size_t retainedModifications=0,createdModifications=0,clonedModifications=0;
    std::size_t removedModifications=0,createdRegions=0,updatedRegions=0,removedRegions=0;
    std::uintptr_t unchangedDocumentController=0;
};
// Installed before document creation; immutable callback lifetime. Actual ARA
// edit notifications may arrive outside the owner/model thread. Implementations
// only invalidate their atomic audio gate and enqueue main-thread work here.
class ModelEditObserver {
public:
    virtual ~ModelEditObserver()=default;
    virtual void invalidateForARAEdit(std::uint64_t actualEditRevision) noexcept=0;
    virtual void reportARAContractFailure(const char*)noexcept {invalidateForARAEdit(0);}
};
// Pure metadata admission: complete bijections and identical physical graph
// after substituting only object IDs and their links. Tracks/names/times stay exact.
bool archiveObjectIDRelocationMatchesGraphs(const Graph& archived,const Graph& current,
                                           const ArchiveObjectIDRelocation&);
struct SavedState {
    unsigned schema = 1;
    std::string factoryID, archiveID;
    std::string pluginName, manufacturer, informationURL, pluginVersion;
    Graph graph;
    Bytes araArchive, companionState;
};
// Separate from companion state so a native getState hook can archive ARA
// without recursively invoking its intercepted companion getState again.
struct ArchiveSnapshot {
    unsigned schema = 1;
    std::string factoryID, archiveID;
    std::string pluginName, manufacturer, informationURL, pluginVersion;
    bool nativeGraphReady = false;
    Graph graph;
    Bytes araArchive;
};

// An actual VST3/AU companion owns audio processing, editor creation and its
// ordinary state. bind() must occur ONCE before first setActive/setState/
// getProcessContextRequirements/editor creation, not merely while inactive.
class Companion {
public:
    virtual ~Companion() = default;
    virtual const ARA::ARAFactory* factory() const noexcept = 0;
    // Independent factory/provider + module ownership, not the bound child
    // instance. The factory must outlive every child and document controller.
    virtual std::shared_ptr<void> factoryLifetime() const = 0;
    virtual const ARA::ARAPlugInExtensionInstance* bind(
        ARA::ARADocumentControllerRef, ARA::ARAPlugInInstanceRoleFlags) = 0;
    virtual bool inactive() const noexcept = 0;
    virtual Bytes saveState() = 0;
    virtual void restoreState(const Bytes&) = 0;
};
// A separately CREATED/OWNED transient playback child, never the borrowed native
// editor component. Implementations control only this own child's SDK lifecycle.
// Fixed, model-thread readback of the exact ordinary state restored into an
// owned rendering child. Addresses are diagnostic identity values only.
struct OrdinaryParameterSnapshot {
    struct Row {std::uint32_t id=0;double normalized=0;std::int32_t flags=0;};
    static constexpr std::size_t capacity=128;
    std::array<Row,capacity> rows{};std::uint32_t count=0;
    std::uintptr_t ownedComponent=0,ownedController=0;bool valid=false;
};
class OwnedPlaybackCompanion : public Companion {
public:
    virtual std::uintptr_t ownedComponentAddress() const noexcept = 0;
    virtual OrdinaryParameterSnapshot restoredOrdinaryParameters()const noexcept{return {};}
    virtual void startOwnedPlayback(double sampleRate,int channels,int maximumBlock,bool offline) = 0;
    virtual void renderOwnedPlaybackBlock(std::int64_t samplePosition,int frames,float* const output[]) = 0;
    virtual std::uint32_t actualLatencySamples() const = 0;
    virtual void stopOwnedPlayback() = 0;
    virtual void disposeOwnedPlayback() = 0;
};
struct WholeSourceCacheOptions {
    int maximumBlock = 512;
    std::uint64_t maximumSourceFrames = 48000u * 60u;
    int renderChannels = 0; // 0 = exact source channel count; no implicit fallback.
    bool allowBitIdenticalMonoStereoCollapse = false;
    bool offline = true; // Explicit companion process mode, recorded in result.
    // Actual native component's ORIGINAL-ONLY ordinary state, captured by the
    // owner outside ARA/model calls. Never a bridge envelope or ARA archive.
    Bytes ordinaryCompanionStateBeforeActivation;
    bool applyOrdinaryCompanionStateBeforeActivation = false;
    // Optional exact-plugin asynchronous-generation adapter. Checks the actual
    // owned document/source before and after rendering; pending work must throw
    // SourceCacheNotReady, never authorize a dry or stale cache.
    // The optional third argument renews public sample access before any owned
    // renderer exists. It is empty during the post-render validation.
    std::function<void(ARA::ARADocumentControllerRef,ARA::ARAAudioSourceRef,const std::function<void()>&)> requireCurrentGeneration;
};
struct WholeModificationSourceCache {
    struct ContentReadiness {ARA::ARAContentType type;bool actualContentAvailable=false;};
    OrdinaryParameterSnapshot restoredOrdinaryParameters;
    SourceDescriptor source;
    std::string modificationID;
    std::vector<std::vector<float>> channels;
    std::uint64_t generation = 0, editNotificationSerial = 0;
    std::uintptr_t actualDocumentController = 0, actualOwnedPlaybackComponent = 0;
    std::uintptr_t actualAudioSource = 0, actualAudioModification = 0;
    int renderedChannels = 0;
    bool offline = false, internalRegionRemoved = false;
    double reportedHeadSeconds = 0, reportedTailSeconds = 0;
    std::uint32_t reportedLatencySamples = 0;
    bool ordinaryCompanionStateApplied=false;
    std::size_t ordinaryCompanionStateBytes=0;
    std::vector<ContentReadiness> actualContentReadiness;
};

// One shared lease per factory for all documents using this scaffold. All lease
// operations and final destruction must occur on the same model/main thread.
// This private component must not independently initialize a factory already
// initialized by a different host library in the same process.
class FactoryLease {
public:
    static std::shared_ptr<FactoryLease> acquire(std::shared_ptr<Companion>,std::shared_ptr<ModelEditObserver> = {});
    ~FactoryLease();
    const ARA::ARAFactory* get() const noexcept;
    void requireModelThread() const;
    bool onModelThread() const noexcept;
private:
    struct Impl;
    explicit FactoryLease(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> p;
};

// Optional native transport implementation. Omit it while unavailable: the host
// then advertises NO ARA playback-control interface, rather than no-op success.
class TransportRequests : public ARA::Host::PlaybackControllerInterface {};

class HostController {
public:
    using ResolveSource = std::function<std::shared_ptr<SourceProvider>(const SourceDescriptor&)>;
    HostController(std::shared_ptr<FactoryLease>, std::shared_ptr<Companion>,
                   std::string documentName,
                   std::unique_ptr<ARA::Host::ContentAccessControllerInterface> content = {},
                   std::unique_ptr<TransportRequests> transport = {},
                   ARA::ARAPlugInInstanceRoleFlags assignedRoles = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole,
                   std::shared_ptr<ModelEditObserver> editObserver = {});
    ~HostController();
    HostController(const HostController&) = delete;
    HostController& operator=(const HostController&) = delete;

    // Configure only an empty document. Provider identity/rate/length/channel
    // count must match every saved native source descriptor exactly.
    void configure(const Graph&, const ResolveSource&);
    // Explicit ARA2 editor-role exception: EditorRenderer region assignment is
    // permitted while the companion is active. Requires a fresh graph, model
    // thread/outside edit/archive, EditorRenderer present and NO Playback role.
    // Caller must control/drain its own render boundary; no inactive flag is
    // queried or changed here. Initial binding and teardown remain inactive.
    void configureEditorGraphWhileActive(const Graph&, const ResolveSource&);
    // Same actual document and existing source/modification objects. Stable
    // IDs must keep immutable source format and source/modification bindings.
    // Explicit clones call the genuine SDK cloneAudioModification. Region
    // move/trim and track changes preserve the modification's plug-in edits.
    // Editor-only roles, model idle; caller must hold/drain real native audio.
    EditorGraphRefreshReceipt reconcileEditorGraphWhileActive(
        const Graph&,const ResolveSource&,const std::vector<ModificationClone>& explicitClones = {});
    // Verified native project replacement: clear old source/modification edit
    // objects, keep the bound document/controller, create current objects and
    // optionally restore the real archive through its explicit SDK ID map.
    // An omitted archive is a deliberate new native project, never a fallback.
    void replaceEditorGraphWhileActive(const Graph&,const ResolveSource&,
        const ArchiveSnapshot* actualArchive = nullptr,const ArchiveObjectIDRelocation* relocation = nullptr);
    void restore(const SavedState&, const ResolveSource&);
    SavedState save();
    // The ARA archive rule is model-thread + outside any edit/restore cycle.
    // Unlike save(), this may run during active companion rendering and NEVER
    // queries Companion::inactive() or calls Companion::saveState(). A fresh
    // bound but unconfigured controller returns a real empty-DC archive with
    // nativeGraphReady=false. Failed/partially configured controllers refuse.
    ArchiveSnapshot captureArchiveOnly();
    // Restore into a fresh, already-bound inactive controller, BEFORE the
    // native owner restores ordinary companion state or activates processing.
    // Never calls Companion::restoreState(). A marked empty-DC snapshot leaves
    // the controller available for later authoritative native configure().
    void restoreArchiveOnly(const ArchiveSnapshot&, const ResolveSource&);
    // Recreate CURRENT objects then pass OLD archive IDs -> CURRENT persistent
    // IDs through the actual SDK ARARestoreObjectsFilter. Opaque archive bytes
    // are untouched. Full region mapping is validated as host metadata; the
    // SDK persists/maps audio sources and modifications, not playback regions.
    void restoreArchiveOnlyRelocated(const ArchiveSnapshot&,const Graph& currentGraph,
                                    const ArchiveObjectIDRelocation&,const ResolveSource& currentSources);
    // Real host/native Playlist events should call these while rendering is
    // suspended. No FL event source is implemented by this scaffold.
    void updateRegion(const RegionDescriptor&);
    // Same source identity/format, new immutable backing snapshot. Existing
    // readers are disabled/drained before the provider is replaced.
    void replaceSourceContent(const std::string& sourceID, std::shared_ptr<SourceProvider>);
    void pollModelUpdates();
    // Explicit role-scoped alias for an active native editor companion; ARA
    // notifyModelUpdates needs only model-thread/non-editing state.
    void pollEditorModelUpdatesWhileActive();
    // Copy only the actually configured model metadata. No plug-in/archive
    // calls, model notification polling or companion lifecycle queries occur.
    // Model thread and idle, successfully configured graph are required.
    Graph graphMetadata() const;
    // Synchronous real ARA playback on an OWN additional child bound to THIS
    // editor DC. Only 48 kHz/no-stretch, zero-latency/head/tail fixed extent is
    // admitted. Internal full-source region never enters Graph/editorRenderer
    // assignments; it and the own child are removed before return/archive.
    // Predicate reads the actual owned hold, outside native calls; no GUI wait.
    WholeModificationSourceCache renderWholeModificationSourceCache(
        const std::string& modificationID,std::unique_ptr<OwnedPlaybackCompanion>,
        const WholeSourceCacheOptions&,
        const std::function<bool()>& verifyActualOwnedHoldOutsideNativeCalls);
    SourceDescriptor sourceForModification(const std::string& modificationID) const;
    std::shared_ptr<FactoryLease> factoryLeaseForOwnedPlayback() const;
    std::uintptr_t documentControllerIdentity() const;
    std::uint64_t editNotificationRevision() const;
    void setEditorHiddenNativeTracks(const std::vector<std::string>& stableTrackIDs);
    bool ownedPlaybackLifecycleIdle() const noexcept;
    bool safeForOwnedTeardown() const noexcept;
    bool consumeDirty() noexcept;
    bool callbackFailed() const noexcept;
    // Native adapter can forward selection to this view after binding. Its
    // lifetime ends with the child instance/controller; no editor is created.
    const ARA::ARAPlugInExtensionInstance* extension() const noexcept;
private:
    void restoreArchiveOnlyImpl(const ArchiveSnapshot&,const Graph& currentGraph,
                               const ArchiveObjectIDRelocation*,const ResolveSource&,bool requireInactive = true);
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace rysup::ara_lab
