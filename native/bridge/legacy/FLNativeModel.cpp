#include "FLNativeModel.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace rysup::ara_lab {
namespace {
using Address = std::uintptr_t;
constexpr Address arrangementGlobal = 0x21ad0e0, arrangementIndexGlobal = 0x1b31720;
constexpr Address managerGlobal = 0x1f20c78;
constexpr Address managerVMT = 0x150c500;
constexpr Address arrangementVMT = 0x1377bc0, eventChannelVMT = 0x153ed60;
constexpr Address channelVMT = 0x1508640;
constexpr Address waveSampleVMT = 0x144f960;
constexpr std::size_t waveSize = 0x1b0;
constexpr std::size_t noIndex = static_cast<std::size_t>(-1);

struct Changed : FLNativeModelReadError { using FLNativeModelReadError::FLNativeModelReadError; };

[[noreturn]] void fail(const std::string& message) {
    throw FLNativeModelReadError("FLNativeModel: " + message);
}
Address add(Address address, std::size_t offset) {
    if (!address || offset > std::numeric_limits<Address>::max() - address)
        fail("invalid/overflowing address");
    return address + offset;
}
std::size_t multiply(std::size_t a, std::size_t b) {
    if (b && a > std::numeric_limits<std::size_t>::max() / b) fail("size multiplication overflow");
    return a * b;
}
void readBytes(Address address, void* destination, std::size_t length) {
    if (!length) return;
    if (!address || !destination || length > std::numeric_limits<Address>::max() - address)
        fail("invalid read range");
    mach_vm_size_t actual = 0;
    const auto result = mach_vm_read_overwrite(mach_task_self(), address, length,
        reinterpret_cast<mach_vm_address_t>(destination), &actual);
    if (result != KERN_SUCCESS || actual != length) fail("native memory read refused or incomplete");
}
template<class T> T read(Address address) {
    static_assert(std::is_trivially_copyable<T>::value, "plain copied value required");
    T value {};
    readBytes(address, &value, sizeof(value));
    return value;
}
template<class T> T field(Address object, std::size_t offset) { return read<T>(add(object, offset)); }
template<class T> T unpack(const std::uint8_t* bytes, std::size_t offset) {
    T result {};
    std::memcpy(&result, bytes + offset, sizeof(result));
    return result;
}
std::string hex(Address value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}
bool pointerAligned(Address value) { return value && !(value & (alignof(Address) - 1)); }
bool stringsEqual(const FLNativeStringField& a, const FLNativeStringField& b) {
    return a.objectOffset == b.objectOffset && a.nativeDataAddress == b.nativeDataAddress &&
        a.recognizedEncoding == b.recognizedEncoding && a.headerCaptured == b.headerCaptured &&
        a.status == b.status && a.fpcHeader == b.fpcHeader && a.utf16 == b.utf16;
}

struct Reader {
    const FLNativeEngineBinding& binding;
    const FLNativeModelOptions& options;
    Address imageEnd = 0;
    std::size_t totalPCMBytes = 0;
    mutable std::size_t totalStringBytesRead = 0;
    std::map<Address,std::array<std::uint8_t,waveSize>> sourceObjectGuards;

    Reader(const FLNativeEngineBinding& b, const FLNativeModelOptions& o) : binding(b), options(o) {
        if (binding.verifiedArm64SliceSHA256 != kFLNativeModelArm64SHA256)
            fail("owning ARM64 slice is not the pinned research image");
        if (!binding.imageBase || binding.exactLoadedImagePath.empty()) fail("missing loaded-image binding");
        Dl_info info {};
        if (!dladdr(reinterpret_cast<const void*>(binding.imageBase), &info) ||
            reinterpret_cast<Address>(info.dli_fbase) != binding.imageBase || !info.dli_fname ||
            binding.exactLoadedImagePath != info.dli_fname)
            fail("actual loaded-image base/path does not match binding");
        const std::string path = info.dli_fname;
        const auto slash = path.find_last_of('/');
        const std::string filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (filename != "FLEngine.dylib" && filename != "FLEngine.arm64.dylib" && filename != "FLEngine_x64.dylib")
            fail("bound image is not FLEngine");
        const auto header = read<mach_header_64>(binding.imageBase);
        if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 ||
            header.filetype != MH_DYLIB || !header.ncmds || header.ncmds > 1024 ||
            header.sizeofcmds > 1024 * 1024)
            fail("unexpected loaded ARM64 Mach-O header");
        std::vector<std::uint8_t> commands(header.sizeofcmds);
        readBytes(add(binding.imageBase, sizeof(header)), commands.data(), commands.size());
        std::size_t cursor = 0;
        std::uint64_t largestEnd = 0;
        bool zeroBasedText = false;
        for (std::uint32_t i = 0; i < header.ncmds; ++i) {
            if (cursor > commands.size() || commands.size() - cursor < sizeof(load_command))
                fail("truncated native load commands");
            const auto command = unpack<load_command>(commands.data(), cursor);
            if (command.cmdsize < sizeof(load_command) || command.cmdsize > commands.size() - cursor)
                fail("invalid native load command size");
            if (command.cmd == LC_SEGMENT_64) {
                if (command.cmdsize < sizeof(segment_command_64)) fail("short native segment command");
                const auto segment = unpack<segment_command_64>(commands.data(), cursor);
                if (segment.vmaddr > 512ULL * 1024 * 1024 ||
                    segment.vmsize > 512ULL * 1024 * 1024 - segment.vmaddr)
                    fail("unexpected native image range");
                if (!std::strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) && segment.vmaddr == 0)
                    zeroBasedText = true;
                largestEnd = std::max(largestEnd, segment.vmaddr + segment.vmsize);
            }
            cursor += command.cmdsize;
        }
        if (!zeroBasedText || cursor != commands.size() || largestEnd <= arrangementGlobal + sizeof(Address))
            fail("native image does not contain the proved model offsets");
        imageEnd = add(binding.imageBase, static_cast<std::size_t>(largestEnd));
    }

    bool inImage(Address address, std::size_t bytes = sizeof(Address)) const {
        return address >= binding.imageBase && address < imageEnd && bytes <= imageEnd - address;
    }
    Address offset(Address value) const { return add(binding.imageBase, value); }

    Address validateObject(Address object, Address expectedVMTOffset, std::size_t expectedSize) const {
        if (!pointerAligned(object)) fail("unaligned/null native object");
        const Address actual = read<Address>(object);
        Address current = actual;
        std::set<Address> visited;
        for (unsigned depth = 0; depth < options.limits.maxParentDepth; ++depth) {
            if (!pointerAligned(current) || !inImage(current, 0x20) || !visited.insert(current).second)
                fail("invalid/cyclic FPC class descriptor");
            const auto size = read<std::uint64_t>(current);
            const auto negativeSize = field<std::int64_t>(current, 8);
            if (size < expectedSize || size > 4 * 1024 * 1024 ||
                negativeSize != -static_cast<std::int64_t>(size))
                fail("invalid native class instance size: actual=" + std::to_string(actual - binding.imageBase) +
                     " current=" + std::to_string(current - binding.imageBase) + " expected=" + std::to_string(expectedVMTOffset) +
                     " size=" + std::to_string(size) + " negative=" + std::to_string(negativeSize) + " minimum=" + std::to_string(expectedSize));
            if (current == offset(expectedVMTOffset)) {
                if (size != expectedSize) fail("pinned native base-class size changed");
                return actual;
            }
            // FPC stores a POINTER TO the parent VMT slot at VMT+0x10.
            const Address parentSlot = field<Address>(current, 0x10);
            if (!pointerAligned(parentSlot) || !inImage(parentSlot)) fail("invalid FPC parent slot");
            current = read<Address>(parentSlot);
        }
        fail("native object does not derive from the expected pinned class");
    }

    FLNativeStringField stringField(Address object, std::uint32_t offsetValue) const {
        FLNativeStringField result;
        result.objectOffset = offsetValue;
        result.nativeDataAddress = field<Address>(object, offsetValue);
        const auto unqualified = [&](const std::string& status) -> FLNativeStringField {
            result.recognizedEncoding = false;
            result.utf16.clear();
            result.status = status;
            if (options.requireRecognizedStrings) {
                std::ostringstream diagnostic;
                diagnostic << "native string metadata unqualified: status=" << status
                    << " object=" << hex(object) << " offset=" << hex(offsetValue)
                    << " data=" << hex(result.nativeDataAddress)
                    << " headerCaptured=" << result.headerCaptured
                    << " codePage=" << result.rawHeaderCodePage
                    << " elementSize=" << result.rawHeaderElementSize
                    << " referenceCount=" << result.rawHeaderReferenceCount
                    << " length=" << result.rawHeaderLength << " raw24=";
                for (const auto byte : result.fpcHeader)
                    diagnostic << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
                fail(diagnostic.str());
            }
            return result;
        };
        if (!result.nativeDataAddress) { result.status = "null_field"; return result; }
        if (result.nativeDataAddress < result.fpcHeader.size() || (result.nativeDataAddress & 1))
            return unqualified("unqualified_string_data_pointer_no_decode");
        const Address headerAddress = result.nativeDataAddress - result.fpcHeader.size();
        try { readBytes(headerAddress, result.fpcHeader.data(), result.fpcHeader.size()); }
        catch (const FLNativeModelReadError&) { return unqualified("unreadable_raw_string_header_no_decode"); }
        result.headerCaptured = true;
        result.rawHeaderElementSize = unpack<std::uint16_t>(result.fpcHeader.data(), 2);
        result.rawHeaderCodePage = unpack<std::uint16_t>(result.fpcHeader.data(), 0);
        result.rawHeaderReferenceCount = unpack<std::int64_t>(result.fpcHeader.data(), 8);
        result.rawHeaderLength = unpack<std::int64_t>(result.fpcHeader.data(), 16);
        if (result.rawHeaderElementSize != 2 || result.rawHeaderCodePage != 1200 ||
            result.rawHeaderReferenceCount < -1 || result.rawHeaderLength < 0)
            return unqualified("unrecognized_raw_string_header_no_decode");
        if (static_cast<std::uint64_t>(result.rawHeaderLength) > options.limits.maxUTF16CodeUnits)
            return unqualified("raw_string_length_exceeds_limit_no_decode");
        const auto stringBytes = multiply(static_cast<std::size_t>(result.rawHeaderLength), sizeof(char16_t));
        if (stringBytes > options.limits.maxStringBytesReadTotal ||
            totalStringBytesRead > options.limits.maxStringBytesReadTotal - stringBytes)
            fail("native UnicodeString copies exceed the total read budget");
        totalStringBytesRead += stringBytes;
        result.utf16.resize(static_cast<std::size_t>(result.rawHeaderLength));
        char16_t terminator = 1;
        try {
            readBytes(result.nativeDataAddress, result.utf16.data(), stringBytes);
            terminator = read<char16_t>(add(result.nativeDataAddress, stringBytes));
        } catch (const FLNativeModelReadError&) {
            return unqualified("recognized_header_but_unreadable_string_body_no_decode");
        }
        if (terminator != 0) return unqualified("recognized_header_but_missing_terminator_no_decode");
        std::array<std::uint8_t,24> after {};
        readBytes(headerAddress, after.data(), after.size());
        if (after != result.fpcHeader || field<Address>(object, offsetValue) != result.nativeDataAddress)
            throw Changed("native UnicodeString changed while copying");
        result.recognizedEncoding = true;
        result.status = "recognized_bounded_utf16_copied";
        return result;
    }

    FLNativeSourceSnapshot source(Address sample, bool copyPCMForThisSource) {
        FLNativeSourceSnapshot result;
        result.waveSampleObject = sample;
        result.runtimeSourceID = "fl-native-source@" + hex(sample);
        result.actualWaveSampleVMT = validateObject(sample, waveSampleVMT, waveSize);
        std::array<std::uint8_t,waveSize> before {}, after {};
        readBytes(sample, before.data(), before.size());
        result.nativeFrameCount98 = unpack<std::int64_t>(before.data(), 0x98);
        result.nativeRate88 = unpack<double>(before.data(), 0x88);
        result.nativeRateFactor1A0 = unpack<double>(before.data(), 0x1a0);
        result.nativeFormatByte78 = before[0x78];
        result.nativeFormatByte79 = before[0x79];
        result.nativePCMAddress90 = unpack<Address>(before.data(), 0x90);
        result.nativeReferenceC8 = unpack<Address>(before.data(), 0xc8);
        if (result.nativeFrameCount98 < 0 ||
            static_cast<std::uint64_t>(result.nativeFrameCount98) > options.limits.maxSourceFrames ||
            !std::isfinite(result.nativeRate88) || result.nativeRate88 <= 0 ||
            !std::isfinite(result.nativeRateFactor1A0) || result.nativeRateFactor1A0 <= 0)
            fail("invalid/beyond-limit native source frame/rate descriptors");
        constexpr std::uint32_t offsets[] {0xb0, 0xb8, 0x108, 0x110};
        for (std::size_t i = 0; i < result.strings.size(); ++i) result.strings[i] = stringField(sample, offsets[i]);
        const FLNativePCMQualification* qualification = nullptr;
        for (const auto& candidate : options.pcmQualifications) {
            if (candidate.formatByte78 != result.nativeFormatByte78 ||
                candidate.formatByte79 != result.nativeFormatByte79 ||
                (candidate.c8Presence == FLNativeC8Presence::Nonzero) != (result.nativeReferenceC8 != 0)) continue;
            if (qualification) fail("ambiguous native PCM qualification mapping");
            qualification = &candidate;
        }
        if (!qualification || !qualification->contiguousInterleavedPCMAt90Qualified ||
            qualification->evidenceID.empty() || qualification->channelCount == 0 ||
            qualification->channelCount > 32 || qualification->encoding == FLNativePCMEncoding::Unknown) {
            result.pcmStatus = "unknown_format_or_residency_no_fallback";
            if (options.requireQualifiedPCMForEveryAudioSource) fail("native PCM format/residency is unqualified");
        } else {
            std::size_t bytesPerSample = 0;
            switch (qualification->encoding) {
                case FLNativePCMEncoding::SignedInt16LE: bytesPerSample = 2; break;
                case FLNativePCMEncoding::Float32LE: bytesPerSample = 4; break;
                default: fail("unsupported explicit PCM qualification");
            }
            result.qualifiedEncoding = qualification->encoding;
            result.qualifiedChannelCount = qualification->channelCount;
            result.pcmQualificationEvidenceID = qualification->evidenceID;
            result.pcmStatus = "qualified_descriptor_only";
            if (options.copyQualifiedPCM && !copyPCMForThisSource)
                result.pcmStatus = "qualified_descriptor_only_outside_selected_insert";
            if (options.copyQualifiedPCM && copyPCMForThisSource) {
                const auto bytes = multiply(static_cast<std::size_t>(result.nativeFrameCount98),
                    multiply(qualification->channelCount, bytesPerSample));
                if (bytes > options.limits.maxPCMBytesPerSource || bytes > options.limits.maxPCMBytesTotal ||
                    totalPCMBytes > options.limits.maxPCMBytesTotal - bytes)
                    fail("qualified native PCM copy exceeds configured byte budget");
                if (bytes && !result.nativePCMAddress90) fail("qualified nonempty native PCM pointer is null");
                result.pcmBytes.resize(bytes);
                readBytes(result.nativePCMAddress90, result.pcmBytes.data(), bytes);
                std::vector<std::uint8_t> repeated(bytes);
                readBytes(result.nativePCMAddress90, repeated.data(), bytes);
                if (repeated != result.pcmBytes) throw Changed("native PCM content changed while copying");
                totalPCMBytes += bytes;
                result.pcmStatus = "qualified_native_pcm_copied_repeated_read_matched";
            }
        }
        readBytes(sample, after.data(), after.size());
        if (before != after) throw Changed("native TWAVSample metadata changed while copying");
        for (const auto& original : result.strings) {
            const auto repeated = stringField(sample, original.objectOffset);
            if (!stringsEqual(repeated, original)) throw Changed("native source string changed while copying");
        }
        sourceObjectGuards.emplace(sample, before);
        return result;
    }

    void verifySourceAgain(const FLNativeSourceSnapshot& original) const {
        std::array<std::uint8_t,waveSize> repeated {};
        readBytes(original.waveSampleObject, repeated.data(), repeated.size());
        const auto found = sourceObjectGuards.find(original.waveSampleObject);
        if (found == sourceObjectGuards.end() || repeated != found->second)
            throw Changed("native source metadata changed after another source was copied");
        for (const auto& string : original.strings) {
            const auto repeatedString = stringField(original.waveSampleObject, string.objectOffset);
            if (!stringsEqual(repeatedString, string))
                throw Changed("native source string changed after another source was copied");
        }
    }
};

struct ListState {
    Address arrangements = 0, arrangement = 0, primary = 0, buffer = 0, secondary = 0;
    Address manager = 0, actualManagerVMT = 0, channelPointers = 0;
    std::int64_t arrangementHigh = -1;
    std::int32_t currentIndex = 0, stride = 0, count = 0, capacity = 0, channelCount = 0;
    std::vector<Address> arrangementObjects, channelObjects;
    bool operator==(const ListState& x) const {
        return arrangements == x.arrangements && arrangement == x.arrangement && primary == x.primary &&
            buffer == x.buffer && secondary == x.secondary && manager == x.manager &&
            actualManagerVMT == x.actualManagerVMT && channelPointers == x.channelPointers && arrangementHigh == x.arrangementHigh &&
            currentIndex == x.currentIndex && stride == x.stride && count == x.count &&
            capacity == x.capacity && channelCount == x.channelCount &&
            arrangementObjects == x.arrangementObjects && channelObjects == x.channelObjects;
    }
};

ListState listState(const Reader& r) {
    ListState s;
    s.arrangements = read<Address>(r.offset(arrangementGlobal));
    s.currentIndex = read<std::int32_t>(r.offset(arrangementIndexGlobal));
    if (!pointerAligned(s.arrangements) || s.arrangements < 8) fail("native arrangement array is absent/invalid");
    s.arrangementHigh = read<std::int64_t>(s.arrangements - 8);
    if (s.arrangementHigh < 0 || static_cast<std::uint64_t>(s.arrangementHigh) >= r.options.limits.maxArrangements ||
        s.currentIndex < 0 || s.currentIndex > s.arrangementHigh) fail("native arrangement count/current index out of bounds");
    s.arrangementObjects.resize(static_cast<std::size_t>(s.arrangementHigh) + 1);
    readBytes(s.arrangements, s.arrangementObjects.data(), multiply(s.arrangementObjects.size(), sizeof(Address)));
    s.arrangement = s.arrangementObjects[static_cast<std::size_t>(s.currentIndex)];
    r.validateObject(s.arrangement, arrangementVMT, 0x23418);
    s.primary = field<Address>(s.arrangement, 0x18);
    s.secondary = field<Address>(s.arrangement, 0x20);
    r.validateObject(s.primary, eventChannelVMT, 0x50);
    r.validateObject(s.secondary, eventChannelVMT, 0x50);
    s.buffer = field<Address>(s.primary, 8);
    s.stride = field<std::int32_t>(s.primary, 0x10);
    s.count = field<std::int32_t>(s.primary, 0x14);
    s.capacity = field<std::int32_t>(s.primary, 0x18);
    if (s.stride != 88 || s.count < 0 || s.capacity < s.count ||
        static_cast<std::uint64_t>(s.count) > r.options.limits.maxClips ||
        static_cast<std::uint64_t>(s.capacity) > r.options.limits.maxClips || (s.count && !s.buffer))
        fail("native primary Playlist stride/count/capacity invalid");
    s.manager = read<Address>(r.offset(managerGlobal));
    if (!pointerAligned(s.manager)) fail("native channel manager absent/unaligned");
    s.actualManagerVMT = r.validateObject(s.manager, managerVMT, 0x28);
    s.channelPointers = field<Address>(s.manager, 8);
    s.channelCount = field<std::int32_t>(s.manager, 0x10);
    if (s.channelCount < 0 || static_cast<std::uint64_t>(s.channelCount) > r.options.limits.maxChannels ||
        (s.channelCount && !pointerAligned(s.channelPointers))) fail("native channel-manager count/array invalid");
    s.channelObjects.resize(static_cast<std::size_t>(s.channelCount));
    readBytes(s.channelPointers, s.channelObjects.data(), multiply(s.channelObjects.size(), sizeof(Address)));
    return s;
}

std::shared_ptr<const FLNativeModelSnapshot> captureOnce(const FLNativeEngineBinding& binding,
    const FLNativeModelOptions& options, unsigned attempt) {
    Reader reader(binding, options);
    const auto initial = listState(reader);
    const auto byteCount = multiply(static_cast<std::size_t>(initial.count), 88);
    std::vector<std::uint8_t> recordBytes(byteCount);
    readBytes(initial.buffer, recordBytes.data(), recordBytes.size());
    auto result = std::make_shared<FLNativeModelSnapshot>();
    result->engineImageBase = binding.imageBase;
    result->loadedImagePath = binding.exactLoadedImagePath;
    result->verifiedArm64SliceSHA256 = binding.verifiedArm64SliceSHA256;
    result->arrangementArray = initial.arrangements;
    result->currentArrangementObject = initial.arrangement;
    result->currentArrangementIndex = initial.currentIndex;
    result->arrangementObjects = initial.arrangementObjects;
    result->currentArrangementRuntimeID = "fl-native-arrangement@" + hex(initial.arrangement);
    result->primaryEventChannel = initial.primary;
    result->primaryEventBuffer = initial.buffer;
    result->secondaryEventChannel = initial.secondary;
    result->nativeRecordStride = initial.stride;
    result->nativeRecordCount = initial.count;
    result->nativeRecordCapacity = initial.capacity;
    result->nativeChannelManager = initial.manager;
    result->actualChannelManagerVMT = initial.actualManagerVMT;
    result->nativeChannelPointerArray = initial.channelPointers;
    result->nativeChannelCount = initial.channelCount;
    result->coherenceAttempt = attempt;
    std::map<std::uint16_t,std::size_t> channelIndices;
    std::map<Address,std::size_t> sourceIndices;
    std::set<Address> sourcePCMRequests;
    std::set<std::int32_t> audioClipIDs;
    auto captureChannel = [&](std::uint16_t managerIndex) -> std::size_t {
        if (managerIndex >= initial.channelObjects.size()) fail("actual manager channel index out of bounds");
        auto found = channelIndices.find(managerIndex);
        if (found == channelIndices.end()) {
            FLNativeChannelSnapshot channel;
            channel.managerIndex = managerIndex;
            channel.channelObject = initial.channelObjects[managerIndex];
            channel.actualChannelVMT = reader.validateObject(channel.channelObject, channelVMT, 0x7e0);
            channel.nativeChannelType150 = field<std::int32_t>(channel.channelObject, 0x150);
            channel.nativeMixerInsert248 = field<std::int32_t>(channel.channelObject, 0x248);
            if (channel.nativeMixerInsert248 < 0 || channel.nativeMixerInsert248 > 501)
                fail("native channel mixer insert outside the native loader's 0..501 range");
            channel.nativeMixerInsertSupported = channel.nativeMixerInsert248 <= 500;
            if (channel.nativeChannelType150 == 4) {
                if (options.requireSupportedMixerInsertForEveryAudioChannel && !channel.nativeMixerInsertSupported)
                    fail("native mixer insert 501 is captured but not qualified for ARA routing");
                const Address wave = field<Address>(channel.channelObject, 0x328);
                channel.nativeWaveSampleObject328 = wave;
                if (wave) {
                    channel.actualNativeWaveSampleVMT = reader.validateObject(wave, waveSampleVMT, waveSize);
                    const bool selectedForPCM = options.copyQualifiedPCMOnlyForMixerInsert < 0 ||
                        channel.nativeMixerInsert248 == options.copyQualifiedPCMOnlyForMixerInsert;
                    auto sourceFound = sourceIndices.find(wave);
                    if (sourceFound == sourceIndices.end()) {
                        if (result->sources.size() >= options.limits.maxSources)
                            fail("native source count exceeds the configured limit");
                        sourceFound = sourceIndices.emplace(wave, result->sources.size()).first;
                        result->sources.push_back(reader.source(wave, selectedForPCM));
                        if (selectedForPCM) sourcePCMRequests.insert(wave);
                    } else if (selectedForPCM && !sourcePCMRequests.count(wave)) {
                        // An actual wave object may be shared across channels.
                        // Upgrade its metadata-only copy only when an actually
                        // selected channel references it; never allocate twice.
                        result->sources[sourceFound->second] = reader.source(wave, true);
                        sourcePCMRequests.insert(wave);
                    }
                    channel.sourceSnapshotIndex = sourceFound->second;
                } else if (options.requireQualifiedPCMForEveryAudioSource) {
                    fail("native audio channel has no TWAVSample; no file fallback");
                }
            }
            found = channelIndices.emplace(managerIndex, result->channels.size()).first;
            result->channels.push_back(std::move(channel));
        }
        return found->second;
    };
    if (options.captureAllAudioChannels) {
        result->nativeManagerMembers.reserve(initial.channelObjects.size());
        for (std::size_t i = 0; i < initial.channelObjects.size(); ++i) {
            if (i > std::numeric_limits<std::uint16_t>::max()) fail("actual manager index exceeds native u16 channel identity");
            const auto object = initial.channelObjects[i];
            const auto actualVMT = reader.validateObject(object, channelVMT, 0x7e0);
            const auto type = field<std::int32_t>(object, 0x150);
            result->nativeManagerMembers.push_back({static_cast<std::uint16_t>(i), object, actualVMT, type});
            if (type == 4) captureChannel(static_cast<std::uint16_t>(i));
        }
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(initial.count); ++i) {
        FLNativeClipSnapshot clip;
        clip.nativeListIndex = i;
        std::memcpy(clip.record.data(), recordBytes.data() + i * 88, 88);
        clip.nativeClipID20 = unpack<std::int32_t>(clip.record.data(), 0x20);
        clip.packedTypeAndChannel04 = unpack<std::uint32_t>(clip.record.data(), 4);
        clip.channelIndex06 = unpack<std::uint16_t>(clip.record.data(), 6);
        clip.timelineStartTicks00 = unpack<std::int32_t>(clip.record.data(), 0);
        clip.timelineLengthTicks08 = unpack<std::int32_t>(clip.record.data(), 8);
        clip.nativeTrack0C = unpack<std::int16_t>(clip.record.data(), 0xc);
        clip.nativeSourceStart18 = unpack<float>(clip.record.data(), 0x18);
        clip.nativeSourceEnd1C = unpack<float>(clip.record.data(), 0x1c);
        if (clip.packedTypeAndChannel04 == 0xffffffffU) {
            clip.kind = FLNativeRecordKind::EmptyOrDeleted;
            clip.sourceResolution = "native_empty_or_deleted_marker";
        } else if (static_cast<std::int32_t>(clip.packedTypeAndChannel04) >= 0x50000000) {
            clip.kind = FLNativeRecordKind::Pattern;
            clip.sourceResolution = "native_pattern_classification_no_audio_source";
        } else if (clip.channelIndex06 >= initial.channelObjects.size()) {
            // Preserve the actual record instead of fabricating a source.
            clip.kind = FLNativeRecordKind::Unresolved;
            clip.sourceResolution = "native_channel_index_out_of_range";
        } else {
            clip.channelSnapshotIndex = captureChannel(clip.channelIndex06);
            const auto& channel = result->channels[clip.channelSnapshotIndex];
            if (channel.nativeChannelType150 == 4) {
                clip.kind = FLNativeRecordKind::AudioChannel;
                clip.sourceResolution = channel.sourceSnapshotIndex != noIndex ?
                    "actual_native_channel_and_TWAVSample_copied" : "native_audio_channel_has_null_sample_no_fallback";
                if (clip.nativeClipID20 < 0 || !audioClipIDs.insert(clip.nativeClipID20).second)
                    fail("audio clip native identity is unallocated or duplicated");
            } else {
                clip.kind = FLNativeRecordKind::OtherChannel;
                clip.sourceResolution = "actual_native_channel_type_is_not_audio_4";
            }
        }
        result->clips.push_back(std::move(clip));
    }
    for (const auto& source : result->sources) reader.verifySourceAgain(source);
    // Validate all source identities again after every source was copied.
    for (const auto& channel : result->channels) {
        if (reader.validateObject(channel.channelObject, channelVMT, 0x7e0) != channel.actualChannelVMT ||
            field<std::int32_t>(channel.channelObject, 0x150) != channel.nativeChannelType150 ||
            field<std::int32_t>(channel.channelObject, 0x248) != channel.nativeMixerInsert248)
            throw Changed("native channel identity/type changed while copying");
        if (channel.nativeChannelType150 == 4) {
            if (field<Address>(channel.channelObject, 0x328) != channel.nativeWaveSampleObject328)
                throw Changed("native source object association changed while copying");
            if (channel.nativeWaveSampleObject328 &&
                (reader.validateObject(channel.nativeWaveSampleObject328, waveSampleVMT, waveSize) != channel.actualNativeWaveSampleVMT ||
                 channel.sourceSnapshotIndex == noIndex ||
                 channel.nativeWaveSampleObject328 != result->sources[channel.sourceSnapshotIndex].waveSampleObject))
                throw Changed("native source class/identity changed while copying");
        }
    }
    for (const auto& member : result->nativeManagerMembers) {
        if (member.managerIndex >= initial.channelObjects.size() ||
            initial.channelObjects[member.managerIndex] != member.actualChannelObject ||
            reader.validateObject(member.actualChannelObject, channelVMT, 0x7e0) != member.actualChannelVMT ||
            field<std::int32_t>(member.actualChannelObject, 0x150) != member.nativeChannelType150)
            throw Changed("native manager member/classification changed while copying");
    }
    const auto beforeRepeat = listState(reader);
    if (!(initial == beforeRepeat)) throw Changed("native Playlist/channel list metadata changed while copying");
    std::vector<std::uint8_t> repeatedRecordBytes(byteCount);
    readBytes(initial.buffer, repeatedRecordBytes.data(), repeatedRecordBytes.size());
    if (repeatedRecordBytes != recordBytes || !(initial == listState(reader)))
        throw Changed("native Playlist records/metadata changed during repeated copy");
    result->pcmCopyFilterMixerInsert = options.copyQualifiedPCMOnlyForMixerInsert;
    result->pcmBytesCopied = reader.totalPCMBytes;
    result->allNativeManagerChannelsInspected = options.captureAllAudioChannels &&
        result->nativeManagerMembers.size() == initial.channelObjects.size();
    result->repeatedReadsMatched = true;
    return result;
}
} // namespace

std::shared_ptr<const FLNativeModelSnapshot> FLNativeModel::capture(
    const FLNativeEngineBinding& binding, const FLNativeModelOptions& options) {
    if (!pthread_main_np()) fail("capture requires the owning process main/model thread");
    if (options.copyQualifiedPCMOnlyForMixerInsert < -1 || options.copyQualifiedPCMOnlyForMixerInsert > 500)
        fail("selected PCM copy mixer insert is outside -1/all or 0..500");
    if (!options.limits.maxCoherenceAttempts || options.limits.maxCoherenceAttempts > 8 ||
        !options.limits.maxParentDepth || options.limits.maxParentDepth > 64 ||
        !options.limits.maxArrangements || !options.limits.maxClips || !options.limits.maxChannels ||
        !options.limits.maxSources)
        fail("invalid capture limits");
    for (unsigned attempt = 1; attempt <= options.limits.maxCoherenceAttempts; ++attempt) {
        try { return captureOnce(binding, options, attempt); }
        catch (const Changed&) { if (attempt == options.limits.maxCoherenceAttempts) throw; }
    }
    fail("coherent native capture unavailable");
}
} // namespace rysup::ara_lab
