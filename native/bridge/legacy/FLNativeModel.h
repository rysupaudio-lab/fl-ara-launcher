#pragma once
// PRIVATE, read-only FL 26.1 ARM64 model research. No native FL calls or writes.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rysup::ara_lab {

inline constexpr const char* kFLNativeModelArm64SHA256 =
    "24b368d08cc861a1bf06ee8883233aa9ca579746825481bfe89f94e8e20eff2c";

struct FLNativeEngineBinding {
    // Address of the ALREADY loaded FLEngine Mach-O header, not its dyld slide.
    // This exact image has preferred base zero, so its offsets add to this base.
    std::uintptr_t imageBase = 0;
    // The caller must have verified the owning loaded image's actual ARM64
    // slice. This reader verifies the binding/header/path, not the file digest.
    std::string verifiedArm64SliceSHA256;
    std::string exactLoadedImagePath;
};

enum class FLNativeRecordKind {
    EmptyOrDeleted, Pattern, AudioChannel, OtherChannel, Unresolved
};
enum class FLNativePCMEncoding { Unknown, SignedInt16LE, Float32LE };
enum class FLNativeC8Presence { Zero, Nonzero };

struct FLNativePCMQualification {
    // An explicit, independently established mapping; the reader supplies no
    // default meaning for either native byte or the +0xc8 reference.
    std::uint8_t formatByte78 = 0, formatByte79 = 0;
    FLNativeC8Presence c8Presence = FLNativeC8Presence::Zero;
    FLNativePCMEncoding encoding = FLNativePCMEncoding::Unknown;
    std::uint32_t channelCount = 0;
    bool contiguousInterleavedPCMAt90Qualified = false;
    std::string evidenceID;
};

struct FLNativeModelLimits {
    std::size_t maxArrangements = 128;
    std::size_t maxClips = 100000;
    std::size_t maxChannels = 65536;
    std::size_t maxSources = 4096;
    std::size_t maxUTF16CodeUnits = 8192;
    std::size_t maxStringBytesReadTotal = 8 * 1024 * 1024;
    std::uint64_t maxSourceFrames = 48000ULL * 60 * 60 * 24;
    std::size_t maxPCMBytesPerSource = 64 * 1024 * 1024;
    std::size_t maxPCMBytesTotal = 128 * 1024 * 1024;
    unsigned maxParentDepth = 16;
    unsigned maxCoherenceAttempts = 3;
};

struct FLNativeModelOptions {
    // Standalone application capture: inspect the complete actual manager array
    // and retain every native type-4 audio channel even without Playlist rows.
    bool captureAllAudioChannels = false;
    FLNativeModelLimits limits;
    // False captures descriptors only. True copies PCM only under an EXACT
    // explicit qualification above. Unknown formats never fall back to a file.
    bool copyQualifiedPCM = false;
    // -1 copies all qualified sources (original reader behavior). A selected
    // 0..500 insert copies PCM only for actual type-4 channels routed there;
    // every other channel/source is still captured/revalidated as metadata.
    std::int32_t copyQualifiedPCMOnlyForMixerInsert = -1;
    bool requireQualifiedPCMForEveryAudioSource = false;
    bool requireSupportedMixerInsertForEveryAudioChannel = false;
    // Optional display metadata must not establish audio/source identity.
    // Unknown native string headers stay raw and are never decoded by guessing.
    bool requireRecognizedStrings = false;
    std::vector<FLNativePCMQualification> pcmQualifications;
};

struct FLNativeStringField {
    std::uint32_t objectOffset = 0;
    std::uintptr_t nativeDataAddress = 0;
    bool recognizedEncoding = false, headerCaptured = false;
    std::string status;
    std::uint16_t rawHeaderCodePage = 0, rawHeaderElementSize = 0;
    std::int64_t rawHeaderReferenceCount = 0, rawHeaderLength = 0;
    // Native UTF-16 retained verbatim; no invented path/name semantic label.
    std::u16string utf16;
    // The raw 24 bytes at data-24 are retained where readable. This matches
    // shipped UTF-16 literal headers, but unknown dynamic headers remain RAW.
    // No length is used until code page, element size, reference count, signed
    // length, and bounds pass. A null or unreadable field has no captured header.
    std::array<std::uint8_t,24> fpcHeader {};
};

struct FLNativeSourceSnapshot {
    // Runtime identity only; neither pointer nor its ID is a persistent file ID.
    std::string runtimeSourceID;
    std::uintptr_t waveSampleObject = 0, actualWaveSampleVMT = 0;
    std::int64_t nativeFrameCount98 = 0;
    double nativeRate88 = 0, nativeRateFactor1A0 = 0;
    std::uint8_t nativeFormatByte78 = 0, nativeFormatByte79 = 0;
    std::uintptr_t nativePCMAddress90 = 0, nativeReferenceC8 = 0;
    std::array<FLNativeStringField,4> strings;
    FLNativePCMEncoding qualifiedEncoding = FLNativePCMEncoding::Unknown;
    std::uint32_t qualifiedChannelCount = 0;
    std::string pcmQualificationEvidenceID, pcmStatus;
    // Bytes are copied without sample conversion. Empty when unqualified,
    // descriptor-only, or the qualified source contains zero frames.
    std::vector<std::uint8_t> pcmBytes;
};

struct FLNativeChannelSnapshot {
    std::uint16_t managerIndex = 0;
    std::uintptr_t channelObject = 0, actualChannelVMT = 0;
    std::int32_t nativeChannelType150 = 0;
    // Current binary's native setter 0x756470 and project loader 0x41100c.
    // Native loader accepts 0..501. Only 0..500 are qualified here; 501 remains
    // captured but unsupported until its special routing meaning is established.
    std::int32_t nativeMixerInsert248 = -1;
    bool nativeMixerInsertSupported = false;
    // +0x328 points DIRECTLY to TChanWAVSample/TWAVSample. It is not the
    // unrelated il.ChannelSample.TChannelSample class or a +0x10 container.
    std::uintptr_t nativeWaveSampleObject328 = 0, actualNativeWaveSampleVMT = 0;
    std::size_t sourceSnapshotIndex = static_cast<std::size_t>(-1);
};

struct FLNativeClipSnapshot {
    std::size_t nativeListIndex = 0;
    std::array<std::uint8_t,88> record {};
    std::int32_t nativeClipID20 = -1;
    std::uint32_t packedTypeAndChannel04 = 0;
    std::uint16_t channelIndex06 = 0;
    std::int32_t timelineStartTicks00 = 0, timelineLengthTicks08 = 0;
    std::int16_t nativeTrack0C = 0;
    float nativeSourceStart18 = 0, nativeSourceEnd1C = 0;
    FLNativeRecordKind kind = FLNativeRecordKind::Unresolved;
    std::size_t channelSnapshotIndex = static_cast<std::size_t>(-1);
    std::string sourceResolution;
};

struct FLNativeManagerMemberSnapshot {
    std::uint16_t managerIndex = 0;
    std::uintptr_t actualChannelObject = 0, actualChannelVMT = 0;
    std::int32_t nativeChannelType150 = 0;
};

struct FLNativeModelSnapshot {
    std::uintptr_t engineImageBase = 0;
    std::string loadedImagePath, verifiedArm64SliceSHA256;
    std::uintptr_t arrangementArray = 0, currentArrangementObject = 0;
    std::int32_t currentArrangementIndex = 0;
    std::vector<std::uintptr_t> arrangementObjects;
    // Derived from the actual current object address, expressly runtime-scoped.
    std::string currentArrangementRuntimeID;
    bool persistentArrangementIDQualified = false;
    std::uintptr_t primaryEventChannel = 0, primaryEventBuffer = 0;
    std::int32_t nativeRecordStride = 0, nativeRecordCount = 0, nativeRecordCapacity = 0;
    std::uintptr_t secondaryEventChannel = 0;
    std::uintptr_t nativeChannelManager = 0, nativeChannelPointerArray = 0;
    std::uintptr_t actualChannelManagerVMT = 0;
    std::int32_t nativeChannelCount = 0;
    std::vector<FLNativeClipSnapshot> clips;
    std::vector<FLNativeChannelSnapshot> channels;
    // Present only when the complete actual manager array was inspected and
    // revalidated before return. Classification is not inferred from Playlist.
    bool allNativeManagerChannelsInspected = false;
    std::int32_t pcmCopyFilterMixerInsert = -1;
    std::size_t pcmBytesCopied = 0;
    std::vector<FLNativeManagerMemberSnapshot> nativeManagerMembers;
    std::vector<FLNativeSourceSnapshot> sources;
    unsigned coherenceAttempt = 0;
    // Equal repeated reads, not a native writer-lock or atomic snapshot claim.
    bool repeatedReadsMatched = false;
    bool nativeMoveTrimNotificationsQualified = false;
    bool nativeSaveReloadIdentityQualified = false;
};

class FLNativeModelReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class FLNativeModel {
public:
    // macOS process MAIN/MODEL thread only, otherwise throws before reading.
    // Returns immutable copied data; never expose its native pointers as live
    // reader handles. Root must invoke outside its native mutation callbacks.
    // This does not acquire/call an FL mutex, invoke FL, mutate FL, or open files.
    static std::shared_ptr<const FLNativeModelSnapshot> capture(
        const FLNativeEngineBinding&, const FLNativeModelOptions& = {});
};

} // namespace rysup::ara_lab
