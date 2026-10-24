#include "VST3ARAEntry.h"
#include <stdexcept>
#include <cstring>
#include <utility>

namespace rysup::ara_lab {
namespace {
struct IndependentFactoryOwner {
    std::shared_ptr<void> module;
    ARA::IMainFactory& provider;
    IndependentFactoryOwner(std::shared_ptr<void> m, ARA::IMainFactory& p) : module(std::move(m)), provider(p) { provider.addRef(); }
    ~IndependentFactoryOwner() { provider.release(); }
};
}
VST3ARAEntry::VST3ARAEntry(Steinberg::FUnknown& component, ARA::IMainFactory& factoryProvider,
    std::shared_ptr<void> owner, NativeLifecycle native)
    : moduleLifetime(std::move(owner)), lifecycle(std::move(native)) {
    if (!moduleLifetime || !lifecycle.freshBeforeFirstBinding || !lifecycle.inactive
        || !lifecycle.saveState || !lifecycle.restoreState)
        throw std::runtime_error("VST3 module ownership and real lifecycle/state callbacks required");
    if (!lifecycle.freshBeforeFirstBinding() || !inactive())
        throw std::runtime_error("ARA binding needs a fresh inactive VST3 component");
    independentFactoryLifetime = std::make_shared<IndependentFactoryOwner>(moduleLifetime, factoryProvider);
    araFactory = factoryProvider.getFactory();
    // Inline SDK IIDs avoid static FUID objects and their extra linker/runtime
    // dependencies. queryInterface supplies the references released below.
    if (component.queryInterface(INLINE_UID_OF(ARA::IPlugInEntryPoint), reinterpret_cast<void**>(&entry)) != Steinberg::kResultOk || !entry)
        throw std::runtime_error("child has no genuine ARA VST3 entry point");
    if (component.queryInterface(INLINE_UID_OF(ARA::IPlugInEntryPoint2), reinterpret_cast<void**>(&entry2)) != Steinberg::kResultOk || !entry2) {
        entry->release(); entry = nullptr;
        throw std::runtime_error("child has no ARA2 role-binding interface");
    }
    const auto* childFactory = entry->getFactory();
    if (!araFactory || !childFactory || !araFactory->factoryID || !childFactory->factoryID
        || std::strcmp(araFactory->factoryID, childFactory->factoryID) != 0) {
        entry2->release(); entry2 = nullptr; entry->release(); entry = nullptr;
        throw std::runtime_error("VST3 ARA factory provider does not match child component");
    }
}
VST3ARAEntry::~VST3ARAEntry() {
    if (entry2) entry2->release();
    if (entry) entry->release();
}
const ARA::ARAFactory* VST3ARAEntry::factory() const noexcept { return araFactory; }
std::shared_ptr<void> VST3ARAEntry::factoryLifetime() const { return independentFactoryLifetime; }
const ARA::ARAPlugInExtensionInstance* VST3ARAEntry::bind(ARA::ARADocumentControllerRef document,
    ARA::ARAPlugInInstanceRoleFlags assigned) {
    if (bindingAttempted || !document || !lifecycle.freshBeforeFirstBinding() || !inactive())
        throw std::runtime_error("refuse duplicate/late ARA binding");
    bindingAttempted = true;
    constexpr auto known = ARA::kARAPlaybackRendererRole | ARA::kARAEditorRendererRole | ARA::kARAEditorViewRole;
    if(lifecycle.verifyBorrowedNativeFreshnessImmediatelyBeforeBind
        && !lifecycle.verifyBorrowedNativeFreshnessImmediatelyBeforeBind())
        throw std::runtime_error("actual borrowed native lifecycle/owner freshness changed during ARA factory/document creation; refuse late bind");
    return entry2->bindToDocumentControllerWithRoles(document, known, assigned);
}
bool VST3ARAEntry::inactive() const noexcept {
    try { return lifecycle.inactive(); } catch (...) { return false; }
}
Bytes VST3ARAEntry::saveState() {
    if (!bindingAttempted || !inactive()) throw std::runtime_error("save needs bound inactive child");
    return lifecycle.saveState();
}
void VST3ARAEntry::restoreState(const Bytes& bytes) {
    if (!bindingAttempted || !inactive()) throw std::runtime_error("state restore must follow binding");
    lifecycle.restoreState(bytes);
}
} // namespace rysup::ara_lab
