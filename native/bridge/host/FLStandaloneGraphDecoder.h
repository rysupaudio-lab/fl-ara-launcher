#pragma once
// Standalone FL application integration: actual copied native RAM and direct
// native clock only. No AudioUnit, ClassInfo, project serialization or file audio.
#include "GenericARAHost.h"
#include "../legacy/FLNativeModel.h"
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rysup::ara_lab {
inline constexpr std::uintptr_t kFLStandalonePPQOffset = 0x1ba1fd8;
inline constexpr std::uintptr_t kFLStandaloneTempoMilliBPMOffset = 0x1ba1f48;
struct FLStandaloneComponentAssociation {
    std::uintptr_t actualComponent=0,nativeOwnerObject=0;
    std::int32_t mixerInsert=-1;
    std::string exactObjectAssociationEvidence;
    bool nativePointerAssociationVerified=false;
};
struct FLStandaloneDecoderLimits {
    std::size_t maxNativeClips=100000;
    std::size_t maxPCMBytesPerSource=64*1024*1024,maxPCMBytesTotal=128*1024*1024;
    std::size_t maxEngineFileBytes=256*1024*1024;
};
struct FLStandaloneDirectClock {
    std::uint16_t ppq=0;
    std::uint32_t tempoMilliBPM=0;
    double bpm=0;
    // Exactly PPQ little-endian u16 followed by tempo little-endian u32.
    std::array<std::uint8_t,6> actualClockBytes{};
    std::uintptr_t engineImageBase=0,ppqAddress=0,tempoAddress=0;
    std::string engineArm64SliceSHA256;
    bool actualDirectNativeReadTwiceAndEqual=false;
};
struct FLStandaloneSourceEvidence {
    std::string sourceID,nativePhysicalSourceKey,actualPCM_SHA256;
    std::uint16_t nativeChannelIndex=0;
    std::int32_t nativeMixerInsert=-1;
    std::int64_t sampleCount=0;
    std::uint32_t channelCount=0;
    double sampleRate=0,rateFactor=0;
    std::uint8_t formatByte78=0,formatByte79=0;
    std::string qualificationEvidenceID;
};
struct FLStandaloneModificationEvidence {
    std::string modificationID,nativePhysicalEditKey,sourceID;
};
struct FLStandaloneRegionEvidence {
    std::string regionID,nativePhysicalRegionKey,modificationID,sourceID;
    std::int32_t nativeClipID=-1;
    std::uint16_t nativeChannelIndex=0;
    std::size_t nativeListIndex=0;
    std::array<std::uint8_t,88> actualRecord{};
    std::string recordSHA256,recordWithoutClipID20_SHA256;
};
struct FLStandaloneDecodedGraph {
    Graph graph;
    HostController::ResolveSource resolveCurrentImmutableRAMSource;
    FLStandaloneComponentAssociation actualAssociation;
    FLStandaloneDirectClock clock;
    std::vector<FLStandaloneSourceEvidence> sources;
    std::vector<FLStandaloneModificationEvidence> modifications;
    std::vector<FLStandaloneRegionEvidence> regions;
    // Complete ordered current native Playlist block, including non-audio rows.
    Bytes actualCopiedNativeModelRecords;
    // Verified classification from actual model.kind, never raw04 inference.
    // Includes audio rows on ALL inserts in the complete ordered native block.
    std::vector<std::size_t> actualVerifiedAudioRecordIndices;
    std::string nativeRecordModelSHA256,orderedNativeRecordsSHA256;
    std::string normalizedOrderedNativeRecordsSHA256,sourcePCMModelSHA256,graphSHA256;
    std::string nativeSaveVersion="26.1.7.5421";
    std::string decoderIdentity,decoderSourceSHA256;
    std::int32_t nativeArrangementIndex=-1;
    // Complete actual manager classification also proves a genuinely empty
    // insert. Contains no channels synthesized from prior Graph/cache state.
    std::vector<FLNativeManagerMemberSnapshot> actualNativeManagerMembers;
    std::int32_t actualNativeManagerChannelCount=0;
    bool allNativeManagerChannelsInspected=false;
    std::int32_t actualPCMCopyMixerInsertFilter=-1;
    std::size_t actualOwnedPCMBytesCopied=0;
    bool actualCurrentNativeRAMCopiedAndVerified=false;
    bool noNativeSerializationOrFileAudioFallbackUsed=false;
};
class FLStandaloneDecodeError:public std::runtime_error {
public:using std::runtime_error::runtime_error;
};
// There is no public ctor accepting a model, clock or fabricated capture bytes.
// Root captures only at its independently verified stopped/drained native owner
// boundary. This class does not acquire FL locks or mutate/stop FL itself.
class CapturedFLStandaloneModel {
public:
    const FLNativeModelSnapshot& model()const noexcept;
    const FLStandaloneDirectClock& clock()const noexcept;
    const FLStandaloneComponentAssociation& association()const noexcept;
private:
    struct Impl;
    explicit CapturedFLStandaloneModel(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> p;
    friend class FLStandaloneGraphDecoder;
};
// Minted outside FL's HostLockMix. Owns the actual loaded image/file seal;
// never exposes a caller-constructible digest as actual engine authority.
class VerifiedFLStandaloneEngine {
public:
    const FLNativeEngineBinding& binding()const noexcept;
private:
    struct Impl;
    explicit VerifiedFLStandaloneEngine(std::shared_ptr<const Impl>);
    std::shared_ptr<const Impl> p;
    friend class FLStandaloneGraphDecoder;
};
class FLStandaloneGraphDecoder {
public:
    // Main/model thread, OUTSIDE native locks: file open/stat/hash and loaded
    // image verification happen only here, before entering HostLockMix.
    static VerifiedFLStandaloneEngine verifyActualLoadedEngine(
        const FLNativeEngineBinding&,const FLStandaloneDecoderLimits& = {});
    // Main/model thread at root's actual stopped/drained HostLockMix boundary.
    // Bounded Mach reads + immutable model copy only: no file open/stat/hash,
    // serialization getter, native plug-in calls, or dlopen. The independent
    // FLNativeModel reader retains its existing read-only dladdr binding check.
    static CapturedFLStandaloneModel captureActualCurrentRAMAtNativeLock(
        const VerifiedFLStandaloneEngine&,const FLNativeModelOptions&,
        const FLStandaloneComponentAssociation&,const FLStandaloneDecoderLimits& = {});
    // Convenience wrapper OUTSIDE native locks. Verifies the actual loaded engine's ARM64 slice,
    // reads PPQ/tempo directly before and after immutable FLNativeModel::capture,
    // requires exact matching clock bytes, and retains all provenance.
    static CapturedFLStandaloneModel captureActualCurrentRAM(
        const FLNativeEngineBinding&,const FLNativeModelOptions&,
        const FLStandaloneComponentAssociation&,const FLStandaloneDecoderLimits& = {});
    // Main/model thread only. A source's native channel+actual PCM hash owns one
    // modification even when that channel currently has zero Playlist regions.
    // A complete actual manager inventory permits a genuinely empty Graph.
    // All its Playlist regions share that modification, including
    // duplicate/move/trim; no new edit object is guessed from region identity.
    static FLStandaloneDecodedGraph decode(const CapturedFLStandaloneModel&,
        const std::string& compiledDecoderSourceSHA256,
        const FLStandaloneDecoderLimits& = {});
};
} // namespace rysup::ara_lab
