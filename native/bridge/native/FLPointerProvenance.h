#pragma once
#include <cstdint>
#include <ptrauth.h>
#if !__has_feature(ptrauth_intrinsics)
#error FLNativeApplication requires -fptrauth-intrinsics for Apple ARM64e code provenance
#endif
namespace rysup::fl_ara::pointer_provenance {
// Comparison only. Never call this stripped pointer: original typed pointers
// retain their signatures when forwarded to actual Apple APIs.
inline std::uintptr_t codeAddress(std::uintptr_t pointer)noexcept{
    return reinterpret_cast<std::uintptr_t>(ptrauth_strip(reinterpret_cast<void*>(pointer),ptrauth_key_function_pointer));
}
}
