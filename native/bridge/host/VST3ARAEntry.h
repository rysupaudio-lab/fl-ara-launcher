#pragma once
#include "GenericARAHost.h"
#include <ARA_API/ARAVST3.h>

namespace rysup::ara_lab {
// Adapts an ALREADY CREATED, INITIALIZED, OTHERWISE UNUSED VST3 IComponent.
// Binary loading, class selection, IComponent initialization/termination,
// IAudioProcessor processing and IEditController UI remain the owner's job.
// No FL/native timeline mechanism is implemented here.
class VST3ARAEntry final : public Companion {
public:
    struct NativeLifecycle {
        // Must verify before any activation, state restore, process-context
        // requirements query or editor creation. A used child must be rejected.
        std::function<bool()> freshBeforeFirstBinding;
        std::function<bool()> inactive;
        // Optional actual borrowed-native hook/owner observation, evaluated as
        // the final callback immediately before the real entry2 bind. It must
        // not enter any native/SDK code or block. Own children may omit this.
        std::function<bool()> verifyBorrowedNativeFreshnessImmediatelyBeforeBind;
        // Exact ordinary companion state including editor-controller state if
        // used. The native owner must encode separate component/controller
        // chunks with its own bounded envelope; no state is fabricated here.
        std::function<Bytes()> saveState;
        std::function<void(const Bytes&)> restoreState;
    };
    VST3ARAEntry(Steinberg::FUnknown& component, ARA::IMainFactory& matchingFactoryProvider,
                 std::shared_ptr<void> moduleLifetime, NativeLifecycle);
    ~VST3ARAEntry() override;
    const ARA::ARAFactory* factory() const noexcept override;
    std::shared_ptr<void> factoryLifetime() const override;
    const ARA::ARAPlugInExtensionInstance* bind(ARA::ARADocumentControllerRef,
                                               ARA::ARAPlugInInstanceRoleFlags) override;
    bool inactive() const noexcept override;
    Bytes saveState() override;
    void restoreState(const Bytes&) override;
private:
    std::shared_ptr<void> moduleLifetime;
    std::shared_ptr<void> independentFactoryLifetime;
    const ARA::ARAFactory* araFactory = nullptr;
    NativeLifecycle lifecycle;
    ARA::IPlugInEntryPoint* entry = nullptr;
    ARA::IPlugInEntryPoint2* entry2 = nullptr;
    bool bindingAttempted = false;
};
} // namespace rysup::ara_lab
