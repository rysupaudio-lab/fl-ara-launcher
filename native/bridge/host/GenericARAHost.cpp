#include "GenericARAHost.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace rysup::ara_lab {
namespace {
void demand(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class Ref, class T> Ref ref(T* p) { return reinterpret_cast<Ref>(p); }
template<class T, class Ref> T* object(Ref p) { return reinterpret_cast<T*>(p); }
constexpr auto roles = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole;
using DocumentProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARADocumentProperties, name)>;
using ContextProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAMusicalContextProperties, color)>;
using TrackProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARARegionSequenceProperties, color)>;
using SourceProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAAudioSourceProperties, merits64BitSamples)>;
using ModProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAAudioModificationProperties, persistentID)>;
using RegionProps = ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAPlaybackRegionProperties, color)>;
struct AssertionContext {std::atomic<bool>* failed=nullptr;ModelEditObserver* observer=nullptr;};
thread_local AssertionContext assertionContext;
struct AssertionOwnerScope {
    AssertionContext previous;
    AssertionOwnerScope(std::atomic<bool>* failed,ModelEditObserver* observer):previous(assertionContext){assertionContext={failed,observer};}
    ~AssertionOwnerScope(){assertionContext=previous;}
};
void ARA_CALL araAssertion(const ARA::ARAAssertCategory, const void*, const char* reason) {
    if(assertionContext.failed)assertionContext.failed->store(true);
    std::fputs("ARA contract failure; native session is retained and this ARA operation is unavailable\n",stderr);
    if(assertionContext.observer)assertionContext.observer->reportARAContractFailure(reason);
}
ARA::ARAAssertFunction assertionAddress = &araAssertion;
bool same(const SourceDescriptor& a, const SourceDescriptor& b) {
    return a.id == b.id && a.sampleCount == b.sampleCount && a.sampleRate == b.sampleRate && a.channelCount == b.channelCount;
}
struct OperationGuard {
    bool& active;
    explicit OperationGuard(bool& value):active(value) { demand(!active,"reentrant ARA archive operation"); active=true; }
    ~OperationGuard() {active=false;}
};
void validateRegion(const RegionDescriptor& r) {
    demand(!r.id.empty() && std::isfinite(r.sourceStart) && std::isfinite(r.sourceDuration)
        && std::isfinite(r.playbackStart) && std::isfinite(r.playbackDuration)
        && std::isfinite(r.sourceStart+r.sourceDuration) && std::isfinite(r.playbackStart+r.playbackDuration)
        && r.sourceStart >= 0 && r.sourceDuration > 0 && r.playbackDuration > 0,
        "invalid ARA region identity/time geometry");
}
bool sameGraphMetadata(const Graph& a,const Graph& b) {
    if(a.documentName!=b.documentName || a.sources.size()!=b.sources.size() || a.tracks.size()!=b.tracks.size()
        || a.modifications.size()!=b.modifications.size() || a.regions.size()!=b.regions.size())return false;
    const auto sorted=[](auto list){std::sort(list.begin(),list.end(),[](const auto& x,const auto& y){return x.id<y.id;});return list;};
    const auto as=sorted(a.sources),bs=sorted(b.sources);
    for(std::size_t i=0;i<as.size();++i)if(as[i].id!=bs[i].id || as[i].name!=bs[i].name || as[i].sampleCount!=bs[i].sampleCount
        || as[i].sampleRate!=bs[i].sampleRate || as[i].channelCount!=bs[i].channelCount)return false;
    const auto at=sorted(a.tracks),bt=sorted(b.tracks);
    for(std::size_t i=0;i<at.size();++i)if(at[i].id!=bt[i].id || at[i].name!=bt[i].name)return false;
    const auto am=sorted(a.modifications),bm=sorted(b.modifications);
    for(std::size_t i=0;i<am.size();++i)if(am[i].id!=bm[i].id || am[i].sourceID!=bm[i].sourceID || am[i].name!=bm[i].name)return false;
    const auto ar=sorted(a.regions),br=sorted(b.regions);
    for(std::size_t i=0;i<ar.size();++i)if(ar[i].id!=br[i].id || ar[i].modificationID!=br[i].modificationID || ar[i].trackID!=br[i].trackID
        || ar[i].name!=br[i].name || ar[i].sourceStart!=br[i].sourceStart || ar[i].sourceDuration!=br[i].sourceDuration
        || ar[i].playbackStart!=br[i].playbackStart || ar[i].playbackDuration!=br[i].playbackDuration || ar[i].transformations!=br[i].transformations)return false;
    return true;
}
struct ValidatedGraph {
    std::map<std::string,std::shared_ptr<SourceProvider>> providers;
    std::unordered_set<std::string> tracks,modifications,regions;
};
ValidatedGraph validateCurrentGraph(const Graph& g,const HostController::ResolveSource& resolve,
                                  ARA::ARAPlaybackTransformationFlags allowed) {
    demand(bool(resolve),"immutable current source resolver required");ValidatedGraph out;
    demand(g.sources.size()<=4096 && g.tracks.size()<=4096 && g.modifications.size()<=100000 && g.regions.size()<=100000,
           "native graph exceeds explicit model bounds");
    for(const auto& s:g.sources){
        demand(!s.id.empty() && !out.providers.count(s.id) && s.sampleCount>=0 && s.channelCount>0 && s.channelCount<=64
            && std::isfinite(s.sampleRate) && s.sampleRate>0,"invalid/duplicate current source metadata");
        auto provider=resolve(s);demand(provider && same(provider->describe(),s),"current immutable source format/identity differs");
        out.providers.emplace(s.id,std::move(provider));
    }
    for(const auto& t:g.tracks)demand(!t.id.empty() && out.tracks.insert(t.id).second,"duplicate/empty current track ID");
    for(const auto& m:g.modifications)demand(!m.id.empty() && out.modifications.insert(m.id).second && out.providers.count(m.sourceID),"current modification binding missing/duplicate");
    for(const auto& r:g.regions){validateRegion(r);demand(out.regions.insert(r.id).second && out.modifications.count(r.modificationID)
        && out.tracks.count(r.trackID) && !(r.transformations & ~allowed),"current region binding/transformation missing/duplicate");}
    return out;
}
}

bool archiveObjectIDRelocationMatchesGraphs(const Graph& archived,const Graph& current,const ArchiveObjectIDRelocation& ids) {
    const auto mapping=[](const auto& oldObjects,const auto& newObjects,const auto& pairs,std::map<std::string,std::string>& out) {
        if(oldObjects.size()!=newObjects.size() || pairs.size()!=oldObjects.size())return false;
        std::unordered_set<std::string> oldIDs,newIDs,destinations;
        for(const auto& v:oldObjects)if(v.id.empty() || !oldIDs.insert(v.id).second)return false;
        for(const auto& v:newObjects)if(v.id.empty() || !newIDs.insert(v.id).second)return false;
        for(const auto& pair:pairs)if(!oldIDs.count(pair.first) || !newIDs.count(pair.second)
            || !out.emplace(pair.first,pair.second).second || !destinations.insert(pair.second).second)return false;
        return out.size()==oldIDs.size() && destinations.size()==newIDs.size();
    };
    std::map<std::string,std::string> sources,mods,regions;
    if(!mapping(archived.sources,current.sources,ids.sourceIDs,sources)
        || !mapping(archived.modifications,current.modifications,ids.modificationIDs,mods)
        || !mapping(archived.regions,current.regions,ids.regionIDs,regions))return false;
    Graph relocated=archived;
    for(auto& source:relocated.sources)source.id=sources.at(source.id);
    for(auto& mod:relocated.modifications){
        if(!sources.count(mod.sourceID))return false;
        mod.id=mods.at(mod.id);mod.sourceID=sources.at(mod.sourceID);
    }
    for(auto& region:relocated.regions){
        if(!mods.count(region.modificationID))return false;
        region.id=regions.at(region.id);region.modificationID=mods.at(region.modificationID);
    }
    return sameGraphMetadata(relocated,current);
}

struct FactoryLease::Impl {
    std::shared_ptr<void> lifetime;
    const ARA::ARAFactory* factory;
    std::shared_ptr<ModelEditObserver> observer;
    std::thread::id thread = std::this_thread::get_id();
    explicit Impl(const std::shared_ptr<Companion>& child,std::shared_ptr<ModelEditObserver> ownerObserver) : lifetime(child->factoryLifetime()), factory(child->factory()),observer(std::move(ownerObserver)) {
        AssertionOwnerScope assertionScope(nullptr,observer.get());
        demand(bool(lifetime), "independent ARA factory/provider ownership required");
        demand(factory && factory->structSize >= ARA::kARAFactoryMinSize, "missing/invalid ARA factory");
        demand(factory->lowestSupportedApiGeneration <= ARA::kARAAPIGeneration_2_0_Final
            && factory->highestSupportedApiGeneration >= ARA::kARAAPIGeneration_2_0_Final,
            "this scaffold requires ARA 2.0 Final; no ARA1/draft fallback");
        demand(factory->factoryID && factory->documentArchiveID, "ARA factory identity missing");
        const ARA::SizedStruct<ARA_STRUCT_MEMBER(ARAInterfaceConfiguration, assertFunctionAddress)> config {
            ARA::kARAAPIGeneration_2_0_Final, &assertionAddress
        };
        factory->initializeARAWithConfiguration(&config);
    }
    ~Impl() { assert(thread == std::this_thread::get_id());AssertionOwnerScope assertionScope(nullptr,observer.get());factory->uninitializeARA(); }
};
FactoryLease::FactoryLease(std::unique_ptr<Impl> value) : p(std::move(value)) {}
FactoryLease::~FactoryLease() = default;
const ARA::ARAFactory* FactoryLease::get() const noexcept { return p->factory; }
void FactoryLease::requireModelThread() const { demand(p->thread == std::this_thread::get_id(), "ARA model/main thread changed"); }
bool FactoryLease::onModelThread()const noexcept{return p->thread==std::this_thread::get_id();}
std::shared_ptr<FactoryLease> FactoryLease::acquire(std::shared_ptr<Companion> child,std::shared_ptr<ModelEditObserver> observer) {
    demand(bool(child), "missing companion");
    static std::mutex mutex;
    static std::map<const ARA::ARAFactory*, std::weak_ptr<FactoryLease>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    if (auto existing = cache[child->factory()].lock()) { existing->requireModelThread(); return existing; }
    auto result = std::shared_ptr<FactoryLease>(new FactoryLease(std::make_unique<Impl>(std::move(child),std::move(observer))));
    cache[result->get()] = result;
    return result;
}

struct HostController::Impl final : ARA::Host::AudioAccessControllerInterface,
                                  ARA::Host::ArchivingControllerInterface,
                                  ARA::Host::ModelUpdateControllerInterface {
    struct Source { SourceDescriptor spec; std::shared_ptr<SourceProvider> provider; ARA::ARAAudioSourceRef ara = nullptr; };
    struct Track { TrackDescriptor spec; ARA::ARARegionSequenceRef ara = nullptr; };
    struct Mod { ModificationDescriptor spec; ARA::ARAAudioModificationRef ara = nullptr; };
    struct Region { RegionDescriptor spec; ARA::ARAPlaybackRegionRef ara = nullptr; bool assigned = false; };
    struct Reader { std::shared_ptr<SourceProvider> provider; std::unique_ptr<SourceReader> reader; int channels=0; bool float64=false; };
    struct Archive { std::string id; Bytes bytes; };
    struct CacheAuxiliary {
        std::unique_ptr<OwnedPlaybackCompanion> companion;
        const ARA::ARAPlugInExtensionInstance* extension=nullptr;
        ARA::ARARegionSequenceRef sequence=nullptr;
        ARA::ARAPlaybackRegionRef region=nullptr;
        bool assigned=false;
    };
    std::shared_ptr<FactoryLease> lease;
    std::shared_ptr<Companion> companion;
    std::unique_ptr<ARA::Host::ContentAccessControllerInterface> content;
    std::unique_ptr<TransportRequests> transport;
    const std::shared_ptr<ModelEditObserver> editObserver;
    ARA::Host::DocumentControllerHostInstance host;
    std::unique_ptr<ARA::Host::DocumentController> dc;
    const ARA::ARAPlugInExtensionInstance* extension = nullptr;
    ARA::ARAPlugInInstanceRoleFlags assignedRoles = roles;
    ARA::ARAMusicalContextRef context = nullptr;
    Graph graph;
    bool configured = false;
    bool configurationAttempted = false;
    bool graphReady = false, editing = false, archiveOperation = false, restoreAttempted = false;
    bool sourceCacheOperation=false;
    std::uint64_t cacheGeneration=0;
    std::atomic<std::uint64_t> editNotificationSerial {0};
    std::vector<std::unique_ptr<CacheAuxiliary>> auxiliaries;
    std::vector<std::string> hiddenNativeTrackIDs;
    std::map<std::string, Source> sources;
    std::map<std::string, Track> tracks;
    std::map<std::string, Mod> mods;
    std::map<std::string, Region> regions;
    std::atomic<bool> dirty {false}, failed {false};
    std::unordered_set<Reader*> readers; // create/destroy only on model thread
    Archive* activeArchive = nullptr;
    static constexpr std::size_t archiveLimit = 256u * 1024u * 1024u;

    Impl(std::shared_ptr<FactoryLease> l, std::shared_ptr<Companion> c, std::string name,
         std::unique_ptr<ARA::Host::ContentAccessControllerInterface> ct, std::unique_ptr<TransportRequests> tr,
         ARA::ARAPlugInInstanceRoleFlags requestedRoles,std::shared_ptr<ModelEditObserver> observer)
        : lease(std::move(l)), companion(std::move(c)), content(std::move(ct)), transport(std::move(tr)),
          editObserver(std::move(observer)),host(this, this, content.get(), this, transport.get()), assignedRoles(requestedRoles) {
        AssertionOwnerScope assertionScope(&failed,editObserver.get());
        demand(lease && companion, "missing lease/companion");
        demand(assignedRoles && !(assignedRoles & ~roles),"unsupported/empty ARA role assignment");
        check(); demand(companion->factory() == lease->get(), "factory/companion mismatch");
        DocumentProps props {name.c_str()};
        auto* instance = lease->get()->createDocumentControllerWithDocument(&host, &props);
        demand(instance && instance->documentControllerRef && instance->documentControllerInterface, "ARA document creation failed");
        dc = std::make_unique<ARA::Host::DocumentController>(instance);
        try {
            extension = companion->bind(dc->getRef(), assignedRoles);
            demand(extension
                && (!(assignedRoles & ARA::kARAPlaybackRendererRole) || (extension->playbackRendererRef && extension->playbackRendererInterface))
                && (!(assignedRoles & ARA::kARAEditorRendererRole) || (extension->editorRendererRef && extension->editorRendererInterface))
                && (!(assignedRoles & ARA::kARAEditorViewRole) || (extension->editorViewRef && extension->editorViewInterface)), "ARA2 role binding incomplete");
        } catch (...) { dc->destroyDocumentController(); dc.reset(); throw; }
        graph.documentName = std::move(name);
    }
    void check() const {
        requireModelIdle();
        demand(companion->inactive(), "suspend child processing before model/archive operations");
    }
    void requireModelIdle() const {
        lease->requireModelThread();
        demand(!editing && !archiveOperation && !sourceCacheOperation && auxiliaries.empty(),"ARA model is editing/restoring/archiving/rendering-own-cache or retains unresolved own child; refuse reentry");
    }
    void beginEditing() { demand(!editing,"nested ARA edit cycle"); editing=true; dc->beginEditing(); }
    void endEditing() { dc->endEditing(); editing=false; }
    void notifyActualEdit()noexcept {
        dirty=true;const auto revision=++editNotificationSerial;
        if(editObserver)editObserver->invalidateForARAEdit(revision);
    }
    void notifyHostGraphInvalidation()noexcept {
        dirty=true;if(editObserver)editObserver->invalidateForARAEdit(0);
    }
    ~Impl() {
        AssertionOwnerScope assertionScope(&failed,editObserver.get());
        // Native owner MUST close child editor and suspend all render callbacks first.
        assert(companion->inactive());
        assert(!editing && !archiveOperation && !sourceCacheOperation && auxiliaries.empty());
        lease->requireModelThread();
        if (!dc) return;
        for (auto& item : regions) if (item.second.assigned) {
            if(assignedRoles & ARA::kARAPlaybackRendererRole) ARA::Host::PlaybackRenderer(extension).removePlaybackRegion(item.second.ara);
            if(assignedRoles & ARA::kARAEditorRendererRole) ARA::Host::EditorRenderer(extension).removePlaybackRegion(item.second.ara);
        }
        beginEditing();
        for (auto& item : regions) if (item.second.ara) dc->destroyPlaybackRegion(item.second.ara);
        for (auto& item : mods) if (item.second.ara) dc->destroyAudioModification(item.second.ara);
        for (auto& item : sources) if (item.second.ara) {
            dc->enableAudioSourceSamplesAccess(item.second.ara, false);
            dc->destroyAudioSource(item.second.ara);
        }
        for (auto& item : tracks) if (item.second.ara) dc->destroyRegionSequence(item.second.ara);
        if (context) dc->destroyMusicalContext(context);
        endEditing();
        dc->destroyDocumentController(); dc.reset();
        // A conforming plug-in destroys readers when source access is disabled.
        for (auto* reader : readers) delete reader;
    }
    void prepare(const Graph& g, const ResolveSource& resolve,bool requireInactive=true) {
        if(requireInactive)check();else requireModelIdle();
        demand(!configurationAttempted && !context, "configure/restore requires fresh empty document");
        configurationAttempted = true; // Failed configuration is never retried on a partially populated graph.
        demand(bool(resolve), "real source resolver required");
        std::unordered_set<std::string> ids;
        for (const auto& s : g.sources) {
            demand(!s.id.empty() && ids.insert(s.id).second && s.sampleCount >= 0
                && s.channelCount > 0 && s.channelCount <= 64 && std::isfinite(s.sampleRate) && s.sampleRate > 0,
                "invalid/duplicate source metadata");
            auto provider = resolve(s);
            demand(provider && same(provider->describe(), s), "native source identity/format changed; refuse stale archive");
            sources.emplace(s.id, Source{s, std::move(provider)});
        }
        ids.clear();
        for (const auto& t : g.tracks) {
            demand(!t.id.empty() && ids.insert(t.id).second, "invalid/duplicate track identity");
            tracks.emplace(t.id, Track{t});
        }
        ids.clear();
        for (const auto& m : g.modifications) {
            demand(!m.id.empty() && ids.insert(m.id).second && sources.count(m.sourceID), "invalid modification/source identity");
            mods.emplace(m.id, Mod{m});
        }
        ids.clear();
        for (const auto& r : g.regions) {
            validateRegion(r);
            demand(ids.insert(r.id).second && mods.count(r.modificationID) && tracks.count(r.trackID), "invalid region binding");
            demand((r.transformations & ~lease->get()->supportedPlaybackTransformationFlags) == 0,
                "child cannot perform requested region transformation");
            regions.emplace(r.id, Region{r});
        }
        graph = g;
        configured = true;
    }
    RegionProps regionProperties(const RegionDescriptor& r) const {
        return {r.transformations, r.sourceStart, r.sourceDuration, r.playbackStart, r.playbackDuration,
                context, tracks.at(r.trackID).ara, r.name.c_str(), nullptr};
    }
    void createGraph() {
        DocumentProps documentProps {graph.documentName.c_str()}; dc->updateDocumentProperties(&documentProps);
        ContextProps contextProps {"Native timeline", 0, nullptr};
        context = dc->createMusicalContext(ref<ARA::ARAMusicalContextHostRef>(this), &contextProps);
        demand(context != nullptr, "ARA musical context creation failed");
        int order = 0;
        for (const auto& item : graph.tracks) {
            auto& t = tracks.at(item.id); TrackProps props {t.spec.name.c_str(), order++, context, nullptr};
            t.ara = dc->createRegionSequence(ref<ARA::ARARegionSequenceHostRef>(&t), &props);
            demand(t.ara != nullptr, "ARA sequence creation failed");
        }
        for (auto& item : sources) {
            auto& s = item.second;
            SourceProps props {s.spec.name.c_str(), s.spec.id.c_str(), s.spec.sampleCount, s.spec.sampleRate, s.spec.channelCount,
                               ARA::kARAFalse, ARA::kARAChannelArrangementUndefined, nullptr};
            s.ara = dc->createAudioSource(ref<ARA::ARAAudioSourceHostRef>(&s), &props);
            demand(s.ara != nullptr, "ARA source creation failed"); dc->enableAudioSourceSamplesAccess(s.ara, true);
        }
        for (auto& item : mods) {
            auto& m = item.second; ModProps props {m.spec.name.c_str(), m.spec.id.c_str()};
            m.ara = dc->createAudioModification(sources.at(m.spec.sourceID).ara, ref<ARA::ARAAudioModificationHostRef>(&m), &props);
            demand(m.ara != nullptr, "ARA modification creation failed");
        }
        for (auto& item : regions) {
            auto& r = item.second; const auto props = regionProperties(r.spec);
            r.ara = dc->createPlaybackRegion(mods.at(r.spec.modificationID).ara, ref<ARA::ARAPlaybackRegionHostRef>(&r), &props);
            demand(r.ara != nullptr, "ARA region creation failed");
        }
    }
    void assign() {
        for (auto& item : regions) {
            auto& r = item.second;
            if(assignedRoles & ARA::kARAPlaybackRendererRole) ARA::Host::PlaybackRenderer(extension).addPlaybackRegion(r.ara);
            if(assignedRoles & ARA::kARAEditorRendererRole) ARA::Host::EditorRenderer(extension).addPlaybackRegion(r.ara); r.assigned = true;
        }
    }
    void refreshEditorHiddenSequences() {
        if(!(assignedRoles & ARA::kARAEditorViewRole))return;
        demand(extension && extension->editorViewInterface && extension->editorViewInterface->structSize>=ARA::kARAEditorViewInterfaceMinSize
            && extension->editorViewInterface->notifyHideRegionSequences,"editor view cannot safely hide internal cache sequences");
        std::vector<ARA::ARARegionSequenceRef> hidden;
        for(const auto& id:hiddenNativeTrackIDs)hidden.push_back(tracks.at(id).ara);
        for(const auto& aux:auxiliaries)if(aux->sequence)hidden.push_back(aux->sequence);
        ARA::Host::EditorView(extension).notifyHideRegionSequences(hidden.size(),hidden.empty()?nullptr:hidden.data());
    }
    void removeCacheAuxiliary(CacheAuxiliary& aux) {
        if(!aux.companion)return;
        if(!aux.companion->inactive())aux.companion->stopOwnedPlayback();
        demand(aux.companion->inactive(),"owned cache child remains active; preserve document and module");
        if(aux.assigned){ARA::Host::PlaybackRenderer(aux.extension).removePlaybackRegion(aux.region);aux.assigned=false;}
        if(aux.region || aux.sequence){
            beginEditing();
            if(aux.region){dc->destroyPlaybackRegion(aux.region);aux.region=nullptr;}
            if(aux.sequence){dc->destroyRegionSequence(aux.sequence);aux.sequence=nullptr;}
            endEditing();
        }
        refreshEditorHiddenSequences();
        aux.companion->disposeOwnedPlayback(); // Checked OWN original terminate/release, before success.
        aux.extension=nullptr;aux.companion.reset();
    }
    ARA::ARAAudioReaderHostRef createAudioReaderForSource(ARA::ARAAudioSourceHostRef source, bool float64) noexcept override {
        try {
            auto* s = object<Source>(source);
            auto reader = std::make_unique<Reader>(); reader->provider = s->provider;
            reader->channels=s->spec.channelCount;reader->float64=float64;
            reader->reader = reader->provider->openReader(float64);
            if (!reader->reader) { failed = true; return nullptr; }
            readers.insert(reader.get()); return ref<ARA::ARAAudioReaderHostRef>(reader.release());
        } catch (...) { failed = true; return nullptr; }
    }
    bool readAudioSamples(ARA::ARAAudioReaderHostRef r, ARA::ARASamplePosition start,
                         ARA::ARASampleCount count, void* const out[]) noexcept override {
        if (!r || !out || count < 0) { failed = true; return false; }
        auto* reader=object<Reader>(r);
        const bool ok = reader->reader->read(start, count, out);
        if (!ok) {
            failed = true;
            if ((std::uint64_t)count <= std::numeric_limits<std::size_t>::max()/(reader->float64?8:4))
                for(int channel=0;channel<reader->channels;++channel) if(out[channel])
                    std::memset(out[channel],0,(std::size_t)count*(reader->float64?8:4));
        }
        return ok;
    }
    void destroyAudioReader(ARA::ARAAudioReaderHostRef r) noexcept override {
        auto* ptr = object<Reader>(r);
        if (readers.erase(ptr)) delete ptr; else failed = true;
    }
    bool validArchive(const void* r) noexcept { if (r == activeArchive && r) return true; failed = true; return false; }
    ARA::ARASize getArchiveSize(ARA::ARAArchiveReaderHostRef r) noexcept override { return validArchive(r) ? activeArchive->bytes.size() : 0; }
    bool readBytesFromArchive(ARA::ARAArchiveReaderHostRef r, ARA::ARASize offset, ARA::ARASize count, ARA::ARAByte* out) noexcept override {
        if (!validArchive(r) || !out || offset > activeArchive->bytes.size() || count > activeArchive->bytes.size()-offset) { failed = true; return false; }
        std::memcpy(out, activeArchive->bytes.data()+offset, count); return true;
    }
    bool writeBytesToArchive(ARA::ARAArchiveWriterHostRef r, ARA::ARASize offset, ARA::ARASize count, const ARA::ARAByte* in) noexcept override {
        if (!validArchive(r) || !in || offset > archiveLimit || count > archiveLimit-offset) { failed = true; return false; }
        try { activeArchive->bytes.resize(std::max(activeArchive->bytes.size(), std::size_t(offset+count)));
            std::memcpy(activeArchive->bytes.data()+offset, in, count); return true;
        } catch (...) { failed = true; return false; }
    }
    ARA::ARAPersistentID getDocumentArchiveID(ARA::ARAArchiveReaderHostRef r) noexcept override { return validArchive(r) ? activeArchive->id.c_str() : nullptr; }
    void notifyDocumentArchivingProgress(float) noexcept override {}
    void notifyDocumentUnarchivingProgress(float) noexcept override {}
    void notifyAudioSourceAnalysisProgress(ARA::ARAAudioSourceHostRef, ARA::ARAAnalysisProgressState, float) noexcept override {}
    void notifyAudioSourceContentChanged(ARA::ARAAudioSourceHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {notifyActualEdit();}
    void notifyAudioModificationContentChanged(ARA::ARAAudioModificationHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {notifyActualEdit();}
    void notifyPlaybackRegionContentChanged(ARA::ARAPlaybackRegionHostRef, const ARA::ARAContentTimeRange*, ARA::ContentUpdateScopes) noexcept override {notifyActualEdit();}
    void notifyDocumentDataChanged() noexcept override {notifyActualEdit();}
};

HostController::HostController(std::shared_ptr<FactoryLease> lease, std::shared_ptr<Companion> child,
    std::string name, std::unique_ptr<ARA::Host::ContentAccessControllerInterface> content,
    std::unique_ptr<TransportRequests> transport, ARA::ARAPlugInInstanceRoleFlags assignedRoles,std::shared_ptr<ModelEditObserver> editObserver)
    : p(std::make_unique<Impl>(std::move(lease), std::move(child), std::move(name), std::move(content), std::move(transport), assignedRoles,std::move(editObserver))) {}
HostController::~HostController() {
    if(p && !safeForOwnedTeardown()){
        auto* preserved=p.release();(void)preserved;
        std::fputs("ARA host teardown refused during active/uncertain own lifecycle; document/module preserved\n",stderr);
    }
}
void HostController::configure(const Graph& graph, const ResolveSource& resolve) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->prepare(graph, resolve); p->beginEditing();
    try { p->createGraph(); } catch (...) { p->failed=true; p->endEditing(); throw; }
    p->endEditing(); p->assign(); p->graphReady=true; p->dirty = false;
}
void HostController::configureEditorGraphWhileActive(const Graph& graph,const ResolveSource& resolve) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();
    demand((p->assignedRoles & ARA::kARAEditorRendererRole) && !(p->assignedRoles & ARA::kARAPlaybackRendererRole),
           "active editor graph assignment forbids a playback-renderer role");
    p->prepare(graph,resolve,false);p->beginEditing();
    try {p->createGraph();}catch(...){p->failed=true;p->endEditing();throw;}
    p->endEditing();p->assign();p->graphReady=true;p->dirty=false;
}
EditorGraphRefreshReceipt HostController::reconcileEditorGraphWhileActive(const Graph& graph,const ResolveSource& resolve,
    const std::vector<ModificationClone>& explicitClones) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();
    demand(p->graphReady && !p->failed && (p->assignedRoles & ARA::kARAEditorRendererRole)
        && !(p->assignedRoles & ARA::kARAPlaybackRendererRole),"same-DC graph refresh requires a ready editor-only model");
    auto admitted=validateCurrentGraph(graph,resolve,p->lease->get()->supportedPlaybackTransformationFlags);
    std::map<std::string,std::string> clones;
    for(const auto& clone:explicitClones){
        demand(!p->mods.count(clone.currentNewID) && admitted.modifications.count(clone.currentNewID)
            && p->mods.count(clone.existingModificationID) && clones.emplace(clone.currentNewID,clone.existingModificationID).second,
               "clone must identify one actual existing modification and a unique new native modification");
        const auto target=std::find_if(graph.modifications.begin(),graph.modifications.end(),[&](const auto& m){return m.id==clone.currentNewID;});
        demand(target->sourceID==p->mods.at(clone.existingModificationID).spec.sourceID,"clone cannot replace its actual immutable source");
    }
    for(const auto& source:graph.sources){const auto old=p->sources.find(source.id);
        demand(old==p->sources.end() || same(old->second.spec,source),"stable source ID cannot change immutable format/extent");}
    for(const auto& mod:graph.modifications){const auto old=p->mods.find(mod.id);
        demand(old==p->mods.end() || old->second.spec.sourceID==mod.sourceID,"stable modification ID cannot change source ownership");}
    for(const auto& region:graph.regions){const auto old=p->regions.find(region.id);
        demand(old==p->regions.end() || old->second.spec.modificationID==region.modificationID,"stable region ID cannot change modification ownership");}
    EditorGraphRefreshReceipt receipt;receipt.unchangedDocumentController=reinterpret_cast<std::uintptr_t>(p->dc->getRef());
    // Validation above invokes no ARA mutation. Once a real mutation starts,
    // failures poison this model; the owner disables ARA for this insert only.
    try {
        for(auto& item:p->regions)if(item.second.assigned){ARA::Host::EditorRenderer(p->extension).removePlaybackRegion(item.second.ara);item.second.assigned=false;}
        p->beginEditing();
        for(auto it=p->regions.begin();it!=p->regions.end();){if(!admitted.regions.count(it->first)){
            p->dc->destroyPlaybackRegion(it->second.ara);it=p->regions.erase(it);++receipt.removedRegions;
        }else ++it;}
        int order=0;
        for(const auto& spec:graph.tracks){auto found=p->tracks.find(spec.id);
            if(found==p->tracks.end()){auto& t=p->tracks.emplace(spec.id,Impl::Track{spec}).first->second;
                TrackProps props{t.spec.name.c_str(),order++,p->context,nullptr};t.ara=p->dc->createRegionSequence(ref<ARA::ARARegionSequenceHostRef>(&t),&props);demand(t.ara,"new native track creation failed");
            }else{auto& t=found->second;t.spec=spec;TrackProps props{t.spec.name.c_str(),order++,p->context,nullptr};p->dc->updateRegionSequenceProperties(t.ara,&props);}}
        for(const auto& spec:graph.sources){auto found=p->sources.find(spec.id);
            if(found==p->sources.end()){auto& s=p->sources.emplace(spec.id,Impl::Source{spec,admitted.providers.at(spec.id)}).first->second;
                SourceProps props{s.spec.name.c_str(),s.spec.id.c_str(),s.spec.sampleCount,s.spec.sampleRate,s.spec.channelCount,ARA::kARAFalse,ARA::kARAChannelArrangementUndefined,nullptr};
                s.ara=p->dc->createAudioSource(ref<ARA::ARAAudioSourceHostRef>(&s),&props);demand(s.ara,"new native source creation failed");p->dc->enableAudioSourceSamplesAccess(s.ara,true);
            }else{auto& s=found->second;const bool changedName=s.spec.name!=spec.name;s.spec=spec;
                if(changedName){SourceProps props{s.spec.name.c_str(),s.spec.id.c_str(),s.spec.sampleCount,s.spec.sampleRate,s.spec.channelCount,ARA::kARAFalse,ARA::kARAChannelArrangementUndefined,nullptr};p->dc->updateAudioSourceProperties(s.ara,&props);}}}
        // Clone before deleting an old clip: a native duplicate can replace its
        // original in the same observed project delta without losing its edits.
        for(const auto& spec:graph.modifications){auto found=p->mods.find(spec.id);
            if(found==p->mods.end()){auto& m=p->mods.emplace(spec.id,Impl::Mod{spec}).first->second;ModProps props{m.spec.name.c_str(),m.spec.id.c_str()};
                const auto clone=clones.find(spec.id);
                if(clone!=clones.end()){m.ara=p->dc->cloneAudioModification(p->mods.at(clone->second).ara,ref<ARA::ARAAudioModificationHostRef>(&m),&props);++receipt.clonedModifications;}
                else{m.ara=p->dc->createAudioModification(p->sources.at(m.spec.sourceID).ara,ref<ARA::ARAAudioModificationHostRef>(&m),&props);++receipt.createdModifications;}
                demand(m.ara,"new native modification creation/clone failed");
            }else{auto& m=found->second;const bool changedName=m.spec.name!=spec.name;m.spec=spec;
                if(changedName){ModProps props{m.spec.name.c_str(),m.spec.id.c_str()};p->dc->updateAudioModificationProperties(m.ara,&props);}++receipt.retainedModifications;}}
        for(auto it=p->mods.begin();it!=p->mods.end();){if(!admitted.modifications.count(it->first)){
            p->dc->destroyAudioModification(it->second.ara);it=p->mods.erase(it);++receipt.removedModifications;
        }else ++it;}
        for(auto it=p->sources.begin();it!=p->sources.end();){if(!admitted.providers.count(it->first)){
            p->dc->enableAudioSourceSamplesAccess(it->second.ara,false);p->dc->destroyAudioSource(it->second.ara);it=p->sources.erase(it);
        }else ++it;}
        for(const auto& spec:graph.regions){auto found=p->regions.find(spec.id);
            if(found==p->regions.end()){auto& r=p->regions.emplace(spec.id,Impl::Region{spec}).first->second;const auto props=p->regionProperties(r.spec);
                r.ara=p->dc->createPlaybackRegion(p->mods.at(r.spec.modificationID).ara,ref<ARA::ARAPlaybackRegionHostRef>(&r),&props);demand(r.ara,"new native region creation failed");++receipt.createdRegions;
            }else{auto& r=found->second;r.spec=spec;const auto props=p->regionProperties(r.spec);p->dc->updatePlaybackRegionProperties(r.ara,&props);++receipt.updatedRegions;}}
        for(auto it=p->tracks.begin();it!=p->tracks.end();){if(!admitted.tracks.count(it->first)){
            p->dc->destroyRegionSequence(it->second.ara);it=p->tracks.erase(it);
        }else ++it;}
        DocumentProps document{graph.documentName.c_str()};p->dc->updateDocumentProperties(&document);p->graph=graph;
        p->hiddenNativeTrackIDs.erase(std::remove_if(p->hiddenNativeTrackIDs.begin(),p->hiddenNativeTrackIDs.end(),[&](const auto& id){return !p->tracks.count(id);}),p->hiddenNativeTrackIDs.end());
        p->endEditing();p->assign();p->notifyHostGraphInvalidation();
    }catch(...){p->failed=true;if(p->editing)p->endEditing();throw;}
    return receipt;
}
void HostController::replaceEditorGraphWhileActive(const Graph& graph,const ResolveSource& resolve,
    const ArchiveSnapshot* archive,const ArchiveObjectIDRelocation* relocation) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();demand(!p->failed && (p->assignedRoles & ARA::kARAEditorRendererRole)
        && !(p->assignedRoles & ARA::kARAPlaybackRendererRole),"project replacement requires a known editor-only model");
    auto admitted=validateCurrentGraph(graph,resolve,p->lease->get()->supportedPlaybackTransformationFlags);
    if(archive){
        demand(archive->schema==1 && archive->factoryID==p->lease->get()->factoryID && archive->araArchive.size()<=Impl::archiveLimit,"project archive schema/factory/extent differs");
        bool compatible=archive->archiveID==p->lease->get()->documentArchiveID;
        for(ARA::ARASize i=0;!compatible && i<p->lease->get()->compatibleDocumentArchiveIDsCount;++i)compatible=archive->archiveID==p->lease->get()->compatibleDocumentArchiveIDs[i];
        demand(compatible,"project archive identity is not supported by the actual factory");
        demand(relocation?archive->nativeGraphReady && archiveObjectIDRelocationMatchesGraphs(archive->graph,graph,*relocation):sameGraphMetadata(archive->graph,graph),
               "project archive/current graph or explicit relocation differs");
    }else demand(!relocation,"a new plain native project cannot silently request archive relocation");
    try {
        for(auto& item:p->regions)if(item.second.assigned)ARA::Host::EditorRenderer(p->extension).removePlaybackRegion(item.second.ara);
        p->beginEditing();
        for(auto& item:p->regions)p->dc->destroyPlaybackRegion(item.second.ara);p->regions.clear();
        for(auto& item:p->mods)p->dc->destroyAudioModification(item.second.ara);p->mods.clear();
        for(auto& item:p->sources){p->dc->enableAudioSourceSamplesAccess(item.second.ara,false);p->dc->destroyAudioSource(item.second.ara);}p->sources.clear();
        for(auto& item:p->tracks)p->dc->destroyRegionSequence(item.second.ara);p->tracks.clear();
        if(p->context){p->dc->destroyMusicalContext(p->context);p->context=nullptr;}p->endEditing();
        p->graph={};p->configured=false;p->configurationAttempted=false;p->graphReady=false;p->restoreAttempted=false;p->hiddenNativeTrackIDs.clear();
        const ResolveSource immutable=[providers=std::move(admitted.providers)](const SourceDescriptor& source){return providers.at(source.id);};
        if(archive)restoreArchiveOnlyImpl(*archive,graph,relocation,immutable,false);
        else{p->prepare(graph,immutable,false);p->beginEditing();p->createGraph();p->endEditing();p->assign();p->graphReady=true;}
        p->notifyHostGraphInvalidation();
    }catch(...){p->failed=true;if(p->editing)p->endEditing();throw;}
}
void HostController::restoreArchiveOnly(const ArchiveSnapshot& state, const ResolveSource& resolve) {
    restoreArchiveOnlyImpl(state,state.graph,nullptr,resolve);
}
void HostController::restoreArchiveOnlyRelocated(const ArchiveSnapshot& state,const Graph& currentGraph,
    const ArchiveObjectIDRelocation& relocation,const ResolveSource& currentSources) {
    p->check();
    demand(state.nativeGraphReady && archiveObjectIDRelocationMatchesGraphs(state.graph,currentGraph,relocation),
           "archive relocation must be complete bijective IDs with an identical physical current graph");
    restoreArchiveOnlyImpl(state,currentGraph,&relocation,currentSources);
}
void HostController::restoreArchiveOnlyImpl(const ArchiveSnapshot& state,const Graph& currentGraph,
    const ArchiveObjectIDRelocation* relocation,const ResolveSource& resolve,bool requireInactive) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    if(requireInactive)p->check();else p->requireModelIdle();
    demand(state.schema == 1 && state.factoryID == p->lease->get()->factoryID, "archive schema/factory mismatch");
    demand(!p->restoreAttempted && !p->configurationAttempted && !p->context,"archive restore requires a fresh empty bound controller");
    bool compatible = state.archiveID == p->lease->get()->documentArchiveID;
    for (ARA::ARASize i=0; !compatible && i<p->lease->get()->compatibleDocumentArchiveIDsCount; ++i)
        compatible = state.archiveID == p->lease->get()->compatibleDocumentArchiveIDs[i];
    demand(compatible && state.araArchive.size() <= Impl::archiveLimit, "incompatible/oversized ARA archive");
    if (!state.nativeGraphReady)
        demand(state.graph.sources.empty() && state.graph.tracks.empty() && state.graph.modifications.empty() && state.graph.regions.empty(),
               "unready archive must describe an actually empty document");
    else p->prepare(currentGraph, resolve);
    // Backing strings remain owned by the caller's immutable relocation for
    // the whole synchronous restore; these arrays own all pointer storage.
    std::vector<ARA::ARAPersistentID> oldSources,newSources,oldMods,newMods;
    ARA::ARARestoreObjectsFilter filter{};filter.structSize=sizeof(filter);filter.documentData=ARA::kARATrue;
    if(relocation){
        for(const auto& pair:relocation->sourceIDs){oldSources.push_back(pair.first.c_str());newSources.push_back(pair.second.c_str());}
        for(const auto& pair:relocation->modificationIDs){oldMods.push_back(pair.first.c_str());newMods.push_back(pair.second.c_str());}
        filter.audioSourceIDsCount=oldSources.size();filter.audioSourceArchiveIDs=oldSources.empty()?nullptr:oldSources.data();
        filter.audioSourceCurrentIDs=newSources.empty()?nullptr:newSources.data();
        filter.audioModificationIDsCount=oldMods.size();filter.audioModificationArchiveIDs=oldMods.empty()?nullptr:oldMods.data();
        filter.audioModificationCurrentIDs=newMods.empty()?nullptr:newMods.data();
    }
    p->restoreAttempted=true;
    OperationGuard operation(p->archiveOperation);
    Impl::Archive archive {state.archiveID, state.araArchive}; p->activeArchive = &archive;
    p->beginEditing();
    bool ok = false;
    try {
        if (state.nativeGraphReady) p->createGraph();
        else {p->graph=state.graph;DocumentProps props {p->graph.documentName.c_str()};p->dc->updateDocumentProperties(&props);}
        ok = p->dc->restoreObjectsFromArchive(ref<ARA::ARAArchiveReaderHostRef>(&archive), relocation?&filter:nullptr);
    }
    catch (...) { p->failed=true; p->endEditing(); p->activeArchive = nullptr; throw; }
    p->endEditing(); p->activeArchive = nullptr;
    if(!ok)p->failed=true;
    demand(ok && !p->failed, "ARA restore failed; reject this controller");
    if (state.nativeGraphReady) {p->assign();p->graphReady=true;}
    p->dirty = false;
}
ArchiveSnapshot HostController::captureArchiveOnly() {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle(); demand(!p->failed && (!p->configurationAttempted || p->graphReady),"cannot archive failed/partially configured document");
    OperationGuard operation(p->archiveOperation);
    p->dc->notifyModelUpdates();
    Impl::Archive archive {p->lease->get()->documentArchiveID, {}}; p->activeArchive = &archive;
    const bool ok = p->dc->storeObjectsToArchive(ref<ARA::ARAArchiveWriterHostRef>(&archive), nullptr);
    p->activeArchive = nullptr; demand(ok && !p->failed, "ARA archive write failed");
    const auto* f = p->lease->get();
    const auto text = [](const char* value) { return value ? std::string(value) : std::string(); };
    return {1, f->factoryID, archive.id, text(f->plugInName), text(f->manufacturerName),
            text(f->informationURL), text(f->version), p->graphReady, p->graph, std::move(archive.bytes)};
}
SavedState HostController::save() {
    p->check(); demand(p->graphReady,"combined standalone save requires configured document");
    auto s=captureArchiveOnly();
    return {s.schema,std::move(s.factoryID),std::move(s.archiveID),std::move(s.pluginName),std::move(s.manufacturer),
            std::move(s.informationURL),std::move(s.pluginVersion),std::move(s.graph),std::move(s.araArchive),p->companion->saveState()};
}
void HostController::restore(const SavedState& s, const ResolveSource& resolve) {
    ArchiveSnapshot archive {s.schema,s.factoryID,s.archiveID,s.pluginName,s.manufacturer,s.informationURL,s.pluginVersion,true,s.graph,s.araArchive};
    restoreArchiveOnly(archive,resolve);p->companion->restoreState(s.companionState);p->dirty=false;
}
void HostController::updateRegion(const RegionDescriptor& spec) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->check(); demand(p->graphReady && !p->failed,"region update requires ready native graph"); validateRegion(spec); auto& r = p->regions.at(spec.id);
    demand(spec.modificationID == r.spec.modificationID && p->tracks.count(spec.trackID), "rebind requires explicit graph rebuild");
    demand((spec.transformations & ~p->lease->get()->supportedPlaybackTransformationFlags) == 0, "unsupported transformation");
    const auto props = p->regionProperties(spec); p->beginEditing();
    p->dc->updatePlaybackRegionProperties(r.ara, &props); p->endEditing(); r.spec = spec;
    for (auto& saved : p->graph.regions) if (saved.id == spec.id) { saved = spec; break; }
    p->dirty = true;
}
void HostController::replaceSourceContent(const std::string& id, std::shared_ptr<SourceProvider> replacement) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->check(); demand(p->graphReady && !p->failed,"source update requires ready native graph"); auto& s = p->sources.at(id);
    demand(replacement && same(replacement->describe(), s.spec), "source format replacement requires explicit graph rebuild");
    p->dc->enableAudioSourceSamplesAccess(s.ara, false);
    s.provider = std::move(replacement); p->beginEditing();
    p->dc->updateAudioSourceContent(s.ara, nullptr, ARA::ContentUpdateScopes::everythingIsAffected());
    p->endEditing(); p->dc->enableAudioSourceSamplesAccess(s.ara, true); p->dirty = true;
}
void HostController::pollModelUpdates() {AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());p->requireModelIdle();p->dc->notifyModelUpdates();}
void HostController::pollEditorModelUpdatesWhileActive() {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();
    demand((p->assignedRoles & ARA::kARAEditorRendererRole) && !(p->assignedRoles & ARA::kARAPlaybackRendererRole),
           "active native editor polling forbids a playback-renderer role");
    p->dc->notifyModelUpdates();
}
SourceDescriptor HostController::sourceForModification(const std::string& id)const {
    p->requireModelIdle();demand(p->graphReady,"native graph is not ready");return p->sources.at(p->mods.at(id).spec.sourceID).spec;
}
Graph HostController::graphMetadata()const {
    p->requireModelIdle();
    demand(p->graphReady && p->configured && !p->failed,"graph metadata requires a ready successfully configured model");
    return p->graph;
}
std::shared_ptr<FactoryLease> HostController::factoryLeaseForOwnedPlayback()const {p->requireModelIdle();return p->lease;}
std::uintptr_t HostController::documentControllerIdentity()const {p->requireModelIdle();return reinterpret_cast<std::uintptr_t>(p->dc->getRef());}
std::uint64_t HostController::editNotificationRevision()const {p->requireModelIdle();return p->editNotificationSerial.load();}
void HostController::setEditorHiddenNativeTracks(const std::vector<std::string>& ids) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();std::unordered_set<std::string> unique;
    for(const auto& id:ids)demand(p->tracks.count(id) && unique.insert(id).second,"unknown/duplicate hidden native track ID");
    p->hiddenNativeTrackIDs=ids;p->refreshEditorHiddenSequences();
}
bool HostController::ownedPlaybackLifecycleIdle()const noexcept {
    return p && p->lease->onModelThread() && !p->editing && !p->archiveOperation && !p->sourceCacheOperation
        && p->auxiliaries.empty();
}
bool HostController::safeForOwnedTeardown()const noexcept {return ownedPlaybackLifecycleIdle() && p->companion->inactive();}
WholeModificationSourceCache HostController::renderWholeModificationSourceCache(const std::string& id,
    std::unique_ptr<OwnedPlaybackCompanion> own,const WholeSourceCacheOptions& options,const std::function<bool()>& hold) {
    AssertionOwnerScope assertionScope(&p->failed,p->editObserver.get());
    p->requireModelIdle();
    demand(p->graphReady && !p->failed && (p->assignedRoles & ARA::kARAEditorRendererRole)
        && !(p->assignedRoles & ARA::kARAPlaybackRendererRole),"source cache requires a ready editor-only document");
    demand(hold && hold(),"source cache needs the actual owned parent hold outside native calls");
    demand(own && own->factory()==p->lease->get() && own->inactive(),"additional owned playback child must be fresh/inactive with the exact same factory");
    auto& modification=p->mods.at(id);auto& source=p->sources.at(modification.spec.sourceID);
    const auto spec=source.spec;
    demand(spec.sampleRate==48000 && (spec.channelCount==1 || spec.channelCount==2) && spec.sampleCount>0
        && options.maximumSourceFrames>0 && options.maximumSourceFrames<=48000u*60u*10u
        && (std::uint64_t)spec.sampleCount<=options.maximumSourceFrames,"whole-source cache requires bounded resident 48k mono/stereo source");
    demand(options.maximumBlock>0 && options.maximumBlock<=4096,"source-cache block size outside admitted range");
    demand(options.ordinaryCompanionStateBeforeActivation.size()<=64u*1024u*1024u
        && (options.applyOrdinaryCompanionStateBeforeActivation || options.ordinaryCompanionStateBeforeActivation.empty()),
           "ordinary owned-child state must be explicitly admitted original-only bounded bytes");
    const int renderChannels=options.renderChannels?options.renderChannels:spec.channelCount;
    demand(renderChannels==spec.channelCount || (spec.channelCount==1 && renderChannels==2 && options.allowBitIdenticalMonoStereoCollapse),
           "source-cache channel mapping is unsupported; no implicit channel fallback/downmix");
    for(const auto& item:p->regions)if(item.second.spec.modificationID==id){const auto& r=item.second.spec;
        demand(r.transformations==ARA::kARAPlaybackTransformationNoChanges && r.sourceDuration==r.playbackDuration,
               "whole-source cache prototype refuses stretched/transformed native regions");}
    // Poll normal ARA state, never a private QA document/file-derived render.
    p->dc->notifyModelUpdates();
    const auto* factory=p->lease->get();demand(factory->analyzeableContentTypesCount<=64,"unbounded source-analysis capability list");
    demand(factory->analyzeableContentTypesCount==0 || factory->analyzeableContentTypes,"missing actual source-analysis capability data");
    std::vector<WholeModificationSourceCache::ContentReadiness> actualContentReadiness;
    for(ARA::ARASize i=0;i<factory->analyzeableContentTypesCount;++i){const auto type=factory->analyzeableContentTypes[i];
        if(p->dc->isAudioSourceContentAnalysisIncomplete(source.ara,type))
            throw SourceCacheNotReady("actual SDK source analysis is incomplete; poll the editor model before cache creation");}
    for(ARA::ARASize i=0;i<factory->analyzeableContentTypesCount;++i){const auto type=factory->analyzeableContentTypes[i];
        actualContentReadiness.push_back({type,p->dc->isAudioSourceContentAvailable(source.ara,type)});}
    if(p->assignedRoles & ARA::kARAEditorViewRole)
        demand(p->extension->editorViewInterface->structSize>=ARA::kARAEditorViewInterfaceMinSize
            && p->extension->editorViewInterface->notifyHideRegionSequences,"editor view cannot hide internal cache sequence");
    OperationGuard operation(p->sourceCacheOperation);
    auto auxiliary=std::make_unique<Impl::CacheAuxiliary>();auxiliary->companion=std::move(own);
    p->auxiliaries.push_back(std::move(auxiliary));auto& aux=*p->auxiliaries.back();
    bool auxiliaryRegistered=true;
    WholeModificationSourceCache cache;cache.source=spec;cache.modificationID=id;cache.generation=++p->cacheGeneration;
    cache.actualDocumentController=reinterpret_cast<std::uintptr_t>(p->dc->getRef());cache.actualOwnedPlaybackComponent=aux.companion->ownedComponentAddress();
    cache.actualAudioSource=reinterpret_cast<std::uintptr_t>(source.ara);cache.actualAudioModification=reinterpret_cast<std::uintptr_t>(modification.ara);
    cache.renderedChannels=renderChannels;cache.offline=options.offline;
    cache.actualContentReadiness=std::move(actualContentReadiness);
    try {
        aux.extension=aux.companion->bind(p->dc->getRef(),ARA::kARAPlaybackRendererRole);
        demand(aux.extension && aux.extension->playbackRendererRef && aux.extension->playbackRendererInterface,"owned child did not expose genuine playback renderer");
        if(options.applyOrdinaryCompanionStateBeforeActivation){
            aux.companion->restoreState(options.ordinaryCompanionStateBeforeActivation);
            cache.ordinaryCompanionStateApplied=true;
            cache.ordinaryCompanionStateBytes=options.ordinaryCompanionStateBeforeActivation.size();
        }
        const std::string label="_RysUp_Internal_Whole_Source_Cache_"+std::to_string(cache.generation);
        p->beginEditing();
        try {
            TrackProps sequence {label.c_str(),std::numeric_limits<ARA::ARAInt32>::max(),p->context,nullptr};
            aux.sequence=p->dc->createRegionSequence(ref<ARA::ARARegionSequenceHostRef>(&aux),&sequence);
            demand(aux.sequence!=nullptr,"internal source-cache sequence creation failed");
            const double duration=double(spec.sampleCount)/48000.0;
            RegionProps region {ARA::kARAPlaybackTransformationNoChanges,0.0,duration,0.0,duration,p->context,aux.sequence,label.c_str(),nullptr};
            aux.region=p->dc->createPlaybackRegion(modification.ara,ref<ARA::ARAPlaybackRegionHostRef>(&aux),&region);
            demand(aux.region!=nullptr,"internal full-source playback region creation failed");
            p->refreshEditorHiddenSequences();
        } catch(...){p->endEditing();throw;}
        p->endEditing();
        // This owned playback child is still genuinely inactive; the active
        // native editor receives NO assignment of this internal region.
        demand(aux.companion->inactive(),"owned playback child became active before region assignment");
        ARA::Host::PlaybackRenderer(aux.extension).addPlaybackRegion(aux.region);aux.assigned=true;
        aux.companion->startOwnedPlayback(48000,renderChannels,options.maximumBlock,options.offline);
        cache.reportedLatencySamples=aux.companion->actualLatencySamples();
        demand(cache.reportedLatencySamples==0,"nonzero owned playback latency is unsupported; no guessed alignment");
        p->dc->getPlaybackRegionHeadAndTailTime(aux.region,&cache.reportedHeadSeconds,&cache.reportedTailSeconds);
        demand(cache.reportedHeadSeconds==0 && cache.reportedTailSeconds==0,"nonzero full-source head/tail is unsupported in fixed-extent source cache");
        p->dc->notifyModelUpdates();const auto before=p->editNotificationSerial.load();
        std::vector<std::vector<float>> rendered((std::size_t)renderChannels,std::vector<float>((std::size_t)spec.sampleCount));
        std::vector<std::vector<float>> block((std::size_t)renderChannels,std::vector<float>((std::size_t)options.maximumBlock));
        std::vector<float*> buffers;for(auto& channel:block)buffers.push_back(channel.data());
        for(std::int64_t frame=0;frame<spec.sampleCount;){
            demand(hold(),"owned native hold ended during source-cache rendering");
            const int count=(int)std::min<std::int64_t>(options.maximumBlock,spec.sampleCount-frame);
            aux.companion->renderOwnedPlaybackBlock(frame,count,buffers.data());
            for(int channel=0;channel<renderChannels;++channel)for(int n=0;n<count;++n){const float value=block[(std::size_t)channel][(std::size_t)n];
                demand(std::isfinite(value),"nonfinite genuine ARA source-cache output");rendered[(std::size_t)channel][(std::size_t)(frame+n)]=value;}
            frame+=count;
        }
        demand(hold(),"owned native hold ended after source-cache rendering");
        demand(aux.companion->actualLatencySamples()==0,"owned playback latency changed during source-cache rendering");
        double head=0,tail=0;p->dc->getPlaybackRegionHeadAndTailTime(aux.region,&head,&tail);
        demand(head==0 && tail==0,"owned full-source extent changed during source-cache rendering");
        p->dc->notifyModelUpdates();
        demand(!p->failed,"actual ARA callback failed during source-cache render");
        if(p->editNotificationSerial.load()!=before)throw SourceCacheNotReady("ARA model changed during source-cache render; poll/rebuild before publication");
        if(spec.channelCount==1 && renderChannels==2){
            demand(std::memcmp(rendered[0].data(),rendered[1].data(),rendered[0].size()*sizeof(float))==0,
                   "mono source produced different L/R samples; refuse collapse/downmix");rendered.resize(1);}
        cache.channels=std::move(rendered);
        p->removeCacheAuxiliary(aux);auxiliaryRegistered=false;p->auxiliaries.pop_back();
        p->dc->notifyModelUpdates();cache.editNotificationSerial=p->editNotificationSerial.load();cache.internalRegionRemoved=true;
        demand(hold(),"owned native hold ended during source-cache cleanup");
        return cache;
    } catch(const SourceCacheNotReady&) {
        if(auxiliaryRegistered){
            try{p->removeCacheAuxiliary(aux);p->auxiliaries.pop_back();}
            catch(...){throw std::runtime_error("own source-cache cleanup failed after model revision changed; lifecycle is not retryable");}
        }
        throw;
    } catch(...) {
        if(auxiliaryRegistered){try {p->removeCacheAuxiliary(aux);p->auxiliaries.pop_back();}
            catch(...){/* Preserve real own child/DC/module for controlled root cleanup, never fake inactive. */}}
        throw;
    }
}
bool HostController::consumeDirty() noexcept { return p->dirty.exchange(false); }
bool HostController::callbackFailed() const noexcept { return p->failed.load(); }
const ARA::ARAPlugInExtensionInstance* HostController::extension() const noexcept { return p->extension; }
} // namespace rysup::ara_lab
