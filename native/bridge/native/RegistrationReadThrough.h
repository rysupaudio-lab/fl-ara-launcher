#pragma once
#include <cstdint>
#include <string>
namespace rysup::fl_ara {
// Configuration routing only. The original vendor validator still decides
// registration validity/version/edition. Payloads never enter private storage.
struct RegistrationReadOptions {
    std::string originalRegistryPath;
    std::string exactWinBridgePath;
};
// Fixed diagnostic codes contain no registry paths, names, identities or values.
// A C++ exception leaves the last attempted stage intact.
enum class RegistrationInitStage : std::uint32_t {
    notStarted=0, alreadyInitialized=1, sourceMetadata=2, sourceLstatFailed=3,
    sourceNotRegular=4, sourceRealpath=5, sourceRealpathFailed=6,
    bridgeRealpath=7, bridgeRealpathFailed=8, bridgeOpen=9, bridgeOpenFailed=10,
    bridgeRead=11, bridgeReadFailed=12, bridgeSealMismatch=13,
    ownerLookup=14, ownerLookupFailed=15, ownerHomeMissing=16,
    ownerExpectedRealpath=17, ownerExpectedRealpathFailed=18, ownerPathMismatch=19,
    sourceOpen=20, sourceOpenFailed=21, sourceMetadataBefore=22,
    sourceFstatBeforeFailed=23, sourceOwnerMismatch=24, sourceModeRejected=25,
    sourceSizeRejected=26, sourceRead=27, sourceReadFailed=28, sourceReadLimit=29,
    sourceMetadataAfter=30, sourceFstatAfterFailed=31, sourceChanged=32,
    sourceEncoding=33, sourceUTF8Rejected=34, sourceDoctypeRejected=35,
    sourceNulRejected=36, parsing=37, parserDuplicateNode=38,
    parserNestedValue=39, parserTypeRejected=40, parserDuplicateValue=41,
    parserValueCountLimit=42, parserTextLimit=43, parserXMLError=44, ready=45
};
struct RegistrationReadStats {
    RegistrationInitStage stage = RegistrationInitStage::notStarted;
    std::int32_t immediateErrno = 0;
    std::uint64_t bytesRead = 0;
    std::int64_t xmlErrorCode = 0;
    std::uint64_t xmlErrorLine = 0, xmlErrorColumn = 0;
    std::uint64_t parsedRegistrationNodeCount = 0, parsedRegistrationValueCount = 0;
    bool initialized = false;
    bool registrationBranchPresent = false;
    std::uint64_t registrationValueCount = 0;
    std::uint64_t registrationReads = 0;
    std::uint64_t deniedRegistrationWrites = 0;
};
bool initializeRegistrationReadThrough(const RegistrationReadOptions&) noexcept;
// Call only from the owned helper's dlsym observer. Pass the real resolved
// pointer; symbols outside the exact sealed copied WinBridge return unchanged.
void* observeRegistrationSymbol(const char* name, void* original) noexcept;
RegistrationReadStats registrationReadStats() noexcept;
}
