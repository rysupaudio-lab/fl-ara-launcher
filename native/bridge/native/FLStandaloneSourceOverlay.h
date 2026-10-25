#pragma once
// Standalone FL native source router. No plug-in IDs, AU owner, SDK, paths,
// launch code or native instruction patches in this public interface.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace rysup::ara_lab {
struct FLStandaloneDecodedGraph;
struct FLNativeModelSnapshot;
struct StandaloneSourceCacheBatch;

inline constexpr const char* kFLStandaloneOverlayEngineArm64SHA256 =
    "24b368d08cc861a1bf06ee8883233aa9ca579746825481bfe89f94e8e20eff2c";

using FLStandaloneNativeVoiceRead = void(*)(void*,std::uintptr_t,void*,float**,
                                         std::int32_t*,std::uint8_t*,std::uint8_t*);
enum class FLStandaloneOverlayFault : std::uint8_t {
    None=0,NativeRead,ClassSeal,RecordIdentity,PhysicalIdentity,PointerRelation,
    NativeSourceMetadata,NativeReaderMetadata,UnsupportedSourceTransform,
    OutputShape,SourcePosition,SourceRange,AmbiguousTarget,Count
};
enum class FLStandaloneOverlayOutcome : std::uint8_t {
    Inactive,Unrelated,StaleTicket,Applied,NativeZeroRead,Rejected,HeldForRebuild
};
struct FLStandaloneOverlayConfig {
    std::uintptr_t actualEngineImageBase=0;
    std::string exactLoadedEnginePath;
    std::string exactEngineArm64SliceSHA256;
    // Called only off RT. Root admits the already running owned normal FL app.
    bool (*confirmAlreadyAdmittedOwnedProcess)(void*)noexcept=nullptr;
    // Model operations require the actual native owner stopped/held, outside
    // render/state/lifecycle, with native render calls drained. No synthetic hold.
    bool (*confirmStoppedDrainedNativeModelBoundary)(void*)noexcept=nullptr;
    void* ownerContext=nullptr;
    // Mandatory stable owner-owned mailbox. Target faults only store true here;
    // root polls/exchanges it and takes detailed faults on its model thread.
    std::atomic<bool>* ownerFaultQueued=nullptr;
    // Mandatory stable mailbox for expected stale-cache playback holds. RT
    // stores true only; main drains takeStaleHoldsOnModelThread and performs a
    // verified real native STOP outside Save. No automatic resume is inferred.
    std::atomic<bool>* ownerStaleHoldQueued=nullptr;
};
struct FLStandaloneOverlayInstallResult {
    bool installed=false;
    std::uintptr_t nativeVMTSlot=0,originalFunction=0;
    std::string reason;
};

// Concrete POD from the original native voice read. The native hook builds it
// using bounded Mach reads; no full PCM hash, allocation or SDK call occurs RT.
// Cached native PCM SHA/generation were independently checked at publication;
// their continuing authority is the batch gate's current ticket on EACH read.
// A stale ticket never authorizes old edited PCM. A fully sealed affected read
// may instead hold only its positive returned span with silence while main stops
// and rebuilds; unknown/unproven sources and native zero-read tails stay intact.
struct FLStandaloneSourceRead {
    bool voiceRecordRead=false,nativeObservationComplete=false;
    std::uintptr_t voiceObject=0,voiceVMT=0,channelObject=0,channelVMT=0;
    std::uintptr_t sampleObject=0,sampleVMT=0,readerObject=0,readerVMT=0;
    std::uintptr_t channelSample328=0,readerSample78=0;
    std::uintptr_t nativePCM90=0,readerPCM40=0,referenceC8=0,stretchProvider100=0;
    std::int32_t nativeClipID=-1,nativeMixerInsert=-1;
    std::uint16_t nativeChannelIndex=0;
    std::array<std::uint8_t,88> nativeRecord{};
    std::int64_t nativeSourceFrames=0;
    double nativeRate=0,nativeRateFactor=0,readerStep=0;
    // Original-produced voice+0x3d8 AFTER original returns: it is the position
    // original captured BEFORE its sample read. Pre-call reader+8 is diagnostic.
    double sourceStartFrame=0,preOriginalReaderPosition=0;
    bool preOriginalReaderPositionRead=false;
    std::uint8_t nativeFormat78=0,nativeFormat79=0,readerChannelsA0=0,readerFormatA1=0;
    std::int32_t readerStride48=-1,nativeBlockFrames=0,originalReadFrames=0;
    float* nativeInterleavedStereo=nullptr;
    bool outputMatchesActualNativeContext=false;
    std::uint8_t originalEnded=0,originalChanged=0;
};
// One-shot, immutable diagnostics only. No pointer in these values is ever
// dereferenced by a status reader. Expected fields are prepared off RT from the
// actual copied model; observed is copied from the already-built callback POD.
inline constexpr std::size_t kFLStandalonePhysicalIdentityDiagnosticSlots=4;
enum class FLStandalonePhysicalIdentityBranch : std::uint8_t {
    None=0,MappingAgreementA=1,ObservedPhysicalSourceB=2
};
enum class FLStandalonePhysicalIdentityMismatch : std::uint32_t {
    MissingEntry=1u<<0,MissingPhysical=1u<<1,MissingIndexed=1u<<2,
    EntryBatch=1u<<3,PhysicalBatch=1u<<4,IndexedBatch=1u<<5,
    EntryPhysicalSource=1u<<6,EntryIndexedSource=1u<<7,
    MissingVoice=1u<<8,MissingReader=1u<<9,
    ChannelObject=1u<<10,SampleObject=1u<<11,MixerInsert=1u<<12
};
struct FLStandalonePhysicalIdentityExpected {
    std::uint16_t nativeChannelIndex=0;
    std::int32_t nativeMixerInsert=-1;
    std::uintptr_t channelObject=0,channelVMT=0,sampleObject=0,sampleVMT=0,pcmAddress=0;
    std::int64_t sourceFrames=0;
    std::uint32_t sourceChannels=0;
    std::uintptr_t capturedModelAddress=0,capturedEngineBase=0,capturedChannelManager=0,capturedManagerPointerArray=0;
    std::int32_t capturedManagerChannelCount=0;
    std::uint64_t capturedChannelSnapshotIndex=0,capturedSourceSnapshotIndex=0;
    std::uintptr_t capturedChannelObject=0,capturedChannelVMT=0;
    std::int32_t capturedChannelType150=0,capturedMixerInsert248=-1;
    bool completeManagerInspection=false,managerMemberPresent=false;
    std::uint16_t capturedManagerMemberIndex=0;
    std::uintptr_t capturedManagerMemberObject=0,capturedManagerMemberVMT=0;
    std::int32_t capturedManagerMemberType150=0;
};
struct FLStandalonePhysicalIdentityCandidate {
    bool present=false,regionPresent=false;
    std::uintptr_t batchAddress=0,sourceAddress=0,actualNativeComponent=0;
    std::uint64_t captureSerial=0,ticketEpoch=0,publicationGeneration=0;
    std::int32_t nativeMixerInsert=-1,nativeClipID=-1;
    std::uint64_t regionSourceIndex=0;
    std::array<std::uint8_t,88> nativeRecord{};
    FLStandalonePhysicalIdentityExpected expected;
};
struct FLStandalonePhysicalIdentityDiagnostic {
    std::uint8_t slotIndex=0;
    FLStandalonePhysicalIdentityBranch branch=FLStandalonePhysicalIdentityBranch::None;
    std::uint32_t mismatchBits=0;
    std::uint8_t presenceMask=0,agreementMask=0,matchingBatchCount=0;
    std::uintptr_t protectedTableAddress=0,ownerBatchAddress=0,actualNativeComponent=0;
    std::uint64_t tableBatchCount=0,tableRegionCount=0,tablePointerChannelCount=0,tableIndexedChannelCount=0;
    std::uint64_t captureSerial=0,ticketEpoch=0,publicationGeneration=0;
    FLStandaloneSourceRead observed;
    FLStandalonePhysicalIdentityCandidate entry,physical,indexed;
};
// presenceMask bits0/1/2 = entry/physical/indexed present. agreementMask bits:
// 0/1/2 = that candidate batch equals owner batch; 3/4/5 = source equality for
// entry-physical / entry-indexed / physical-indexed (only when both present).
// observed doubles may be nonfinite because this records a rejected read before
// later format guards. Main JSON must serialize nonfinite as null/explicit raw
// bits, never pass them unguarded to a JSON number writer.
struct FLStandaloneOverlayCounters {
    std::uint64_t nativeCallbacks=0,unrelated=0,staleTicket=0;
    std::uint64_t appliedCallbacks=0,appliedFrames=0,zeroReadCallbacks=0,rejected=0;
    std::uint64_t faultBits=0;
    std::uint32_t readers=0;
    std::uint64_t heldCallbacks=0,heldFrames=0,staleHoldRequests=0;
};
struct FLStandaloneOverlayInstanceStats {
    std::string instanceID;
    std::int32_t nativeMixerInsert=-1;
    std::uint64_t captureSerial=0,ticketEpoch=0;
    std::size_t sources=0,regions=0;
    bool published=false,ticketAuthorized=false;
    bool staleHoldActive=false;
    FLStandaloneOverlayCounters counters;
    std::array<FLStandalonePhysicalIdentityDiagnostic,kFLStandalonePhysicalIdentityDiagnosticSlots> physicalIdentityDiagnostics{};
    std::uint8_t physicalIdentityDiagnosticReadyMask=0;
    bool physicalIdentityDiagnosticOverflow=false;
};
struct FLStandaloneOverlayFaultRecord {
    std::string instanceID;
    std::uintptr_t actualNativeComponent=0;
    std::uint64_t captureSerial=0,faultBits=0;
    FLStandaloneOverlayFault firstFault=FLStandaloneOverlayFault::None;
    std::array<FLStandalonePhysicalIdentityDiagnostic,kFLStandalonePhysicalIdentityDiagnosticSlots> physicalIdentityDiagnostics{};
    std::uint8_t physicalIdentityDiagnosticReadyMask=0;
    bool physicalIdentityDiagnosticOverflow=false;
};
struct FLStandaloneOverlayStaleHoldRecord {
    std::string instanceID;
    std::uintptr_t actualNativeComponent=0;
    std::uint64_t captureSerial=0,ticketEpoch=0;
};

class FLStandaloneSourceOverlay {
public:
    explicit FLStandaloneSourceOverlay(FLStandaloneOverlayConfig);
    ~FLStandaloneSourceOverlay();
    FLStandaloneSourceOverlay(const FLStandaloneSourceOverlay&)=delete;
    FLStandaloneSourceOverlay& operator=(const FLStandaloneSourceOverlay&)=delete;
    // Actual engine must ALREADY be loaded and owner admitted. Independently
    // checks loaded path, sealed ARM64 file slice, Mach header, exact source VMT
    // slot/function. Changes one private COW DATA pointer, never instructions.
    FLStandaloneOverlayInstallResult installLoadedNativeSourceHook();
    // Main/model thread at root's actual held/drained native boundary. Batch
    // cache belongs to this exact decoded current model and shared gate/ticket.
    // Multiple instances must have unique native mixer inserts. Regions on one
    // physical channel explicitly share sourceID+":modification"; no source-wide
    // inference across another channel, insert or uncaptured region is made.
    void publish(const std::string& instanceID,const FLStandaloneDecodedGraph&,
                 std::shared_ptr<const FLNativeModelSnapshot>,StandaloneSourceCacheBatch);
    // Safe on notification/render threads: no allocation, lock, SDK or native
    // call. Closes all currently matching instance tickets before returning.
    void invalidateInstance(const std::string& instanceID)noexcept;
    // Main/model removal and retirement. invalidateInstance closes immediately;
    // removing a table never frees storage on a native/notification thread.
    void removeInstanceAtNativeBoundary(const std::string& instanceID);
    // TABLE-ONLY teardown cleanup: owning model thread + same admitted process,
    // but no global stopped/engine/model/ARA boundary is invented. Every matching
    // current/retired ticket must ALREADY be unauthorized. Never closes a live
    // ticket itself. Old immutable floats remain retained until Readers drains.
    void removeDisabledInstanceOnModelThread(const std::string& instanceID);
    void collectRetiredOnModelThread();
    // AFTER original has been forwarded ONCE. Public for root observation and
    // offline controls; does not call native or dereference native addresses.
    // Only a qualified returned float span is written. Native flags/count and
    // zero-read/tail buffers are never modified.
    FLStandaloneOverlayOutcome processObservedAfterOriginal(const FLStandaloneSourceRead&)noexcept;
    FLStandaloneOverlayCounters counters()const noexcept;
    std::vector<FLStandaloneOverlayInstanceStats> instanceStatsOnModelThread()const;
    std::vector<FLStandaloneOverlayFaultRecord> takeFaultsOnModelThread();
    std::vector<FLStandaloneOverlayStaleHoldRecord> takeStaleHoldsOnModelThread();
    // Actual root native hold/drain required. Restores only our own source VMT
    // data slot to its original, drains readers, retires tables on model thread.
    void shutdownAtNativeBoundary();
private:
    struct Impl;
    std::unique_ptr<Impl> p;
    void processNativeAfterOriginal(void*,void*,float**,std::int32_t*,std::uint8_t*,std::uint8_t*,double,bool)noexcept;
    friend struct FLStandaloneOverlayNativeThunk;
#ifdef FL_STANDALONE_SOURCE_OVERLAY_OFFLINE_TEST_MAIN
    friend struct FLStandaloneSourceOverlayOfflineAccess;
#endif
};
} // namespace rysup::ara_lab
