#pragma once
#include "GenericARAHost.h"
#include <pluginterfaces/base/ipluginbase.h>
#include <array>

namespace rysup::ara_lab {
// Root's class-wide observer hooks must bypass native-insert attribution for
// this scope. It covers every OWN child factory/lifecycle/render call.
bool ownedPlaybackCompanionCallOnThisThread() noexcept;
struct OwnedVST3PlaybackFactoryContext {
    Steinberg::IPluginFactory* actualSameNativeFactory = nullptr;
    std::array<char,16> audioClassID {}, araClassID {};
    std::shared_ptr<void> alreadyLoadedModuleLifetime;
    std::shared_ptr<FactoryLease> initializedFactoryLease;
    std::uintptr_t forbiddenBorrowedNativeComponent = 0;
};
// Creates its own new component through the already-loaded SAME native factory.
// No CFBundle/dlopen/bundleEntry, native component adoption or UI. Explicit
// original-only ordinary parameter state may be applied after binding; ARA
// document/source state remains shared through the real document controller.
class OwnedVST3PlaybackCompanion final : public OwnedPlaybackCompanion {
public:
    static std::unique_ptr<OwnedVST3PlaybackCompanion> create(const OwnedVST3PlaybackFactoryContext&);
    ~OwnedVST3PlaybackCompanion() override;
    const ARA::ARAFactory* factory() const noexcept override;
    std::shared_ptr<void> factoryLifetime() const override;
    const ARA::ARAPlugInExtensionInstance* bind(ARA::ARADocumentControllerRef,ARA::ARAPlugInInstanceRoleFlags) override;
    bool inactive() const noexcept override;
    Bytes saveState() override;
    void restoreState(const Bytes&) override;
    std::uintptr_t ownedComponentAddress() const noexcept override;
    void startOwnedPlayback(double,int,int,bool) override;
    void renderOwnedPlaybackBlock(std::int64_t,int,float* const[]) override;
    std::uint32_t actualLatencySamples() const override;
    void stopOwnedPlayback() override;
    void disposeOwnedPlayback() override;
private:
    struct Impl;
    explicit OwnedVST3PlaybackCompanion(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> p;
};
} // namespace rysup::ara_lab
