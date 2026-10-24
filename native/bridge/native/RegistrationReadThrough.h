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
struct RegistrationReadStats {
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
