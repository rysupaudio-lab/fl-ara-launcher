#pragma once
// Generic observation of genuine, already native-loaded VST3 instances in one
// independently admitted owned FL process. No factory enumeration, plug-in load,
// component reinitialization, ARA binding, product policy or audio-device code.
#include <CoreFoundation/CoreFoundation.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace rysup::fl_ara::native_hooks {
using ClassID = std::array<char,16>;

struct ThreadInfo {
    std::uintptr_t nativeThreadToken=0; // actual pthread_self(), never a queued substitute
    std::uint32_t hookDepth=0;
    bool isMainThread=false;
    bool insideNativeProcess=false;
    // Captured at the actual native lifecycle/state hook ENTRY, never at a
    // shared helper or owner callback. Zero means no caller authority supplied.
    // The owner resolves/seals this PC only off RT before archive operations.
    std::uintptr_t actualNativeCallerAddress=0;
};
struct ImageEvidence {
    std::string canonicalPath, executableSHA256;
    std::uintptr_t actualImageBase=0, actualAddress=0, imageOffset=0;
    bool resolved=false, stableRegularFileSHA256=false;
};
struct ModuleRecord {
    std::uint64_t identity=0;
    CFBundleRef actualBundle=nullptr; // retained by this library until process exit
    std::string canonicalBundlePath, canonicalExecutablePath, executableSHA256;
    ImageEvidence actualFactoryEntryImage;
    void* actualFactoryEntry=nullptr;
    bool stableExecutableSHA256=false;
    bool nativeCodeLeaseRetained=false; // RTLD_NOLOAD leases only; never loads code
    // This is source evidence, NOT a signature/trust approval. The owner must
    // independently validate the genuine bundle, signature and allowed process.
    bool signatureIndependentlyVerified=false;
};
struct FactoryRecord {
    std::uint64_t identity=0;
    const ModuleRecord* module=nullptr;
    Steinberg::IPluginFactory* actualFactory=nullptr; // borrowed native factory
    std::atomic<void*> ownerData{nullptr};
    std::atomic<bool> nativeAlive{true};
    // A single factory object returned by distinct actual bundles cannot be
    // assigned a module by creation order. Owner must refuse such provenance.
    std::atomic<bool> moduleAssociationAmbiguous{false};
};
enum class CreatedKind { Unknown, Component, Controller };
struct InstanceRecord {
    std::uint64_t identity=0;
    FactoryRecord* factory=nullptr;
    ClassID actualRequestedClassID{},actualRequestedInterfaceID{};
    CreatedKind createdKind=CreatedKind::Unknown;
    std::atomic<Steinberg::Vst::IComponent*> actualComponent{nullptr};
    std::atomic<Steinberg::Vst::IAudioProcessor*> actualProcessor{nullptr};
    std::atomic<Steinberg::Vst::IEditController*> actualController{nullptr};
    std::atomic<Steinberg::Vst::IComponentHandler*> actualNativeComponentHandler{nullptr};
    // Actual argument while the original setter is executing. This is routing
    // evidence for synchronous performEdit inside that call, NOT acceptance.
    // After failure the accepted handler above remains unchanged. Nested native
    // setters restore the outer pending argument; ordinary return clears it.
    std::atomic<Steinberg::Vst::IComponentHandler*> actualPendingNativeComponentHandler{nullptr};
    std::atomic<Steinberg::FUnknown*> actualInitializeContext{nullptr};
    // The owner allocates/frees its payload on a non-RT native/model boundary.
    // Process callbacks read this pointer directly; no registry lookup or lock.
    std::atomic<void*> ownerData{nullptr};
    // Separate opaque owner association for an actual controller. NEVER aliases
    // or transfers component ownerData/lifetime merely because records connect.
    // Owner publishes a verified controller association off RT; RT hooks/owner
    // callbacks may read only stable preallocated metadata/atomic fields.
    std::atomic<void*> ownerControllerData{nullptr};
    // Opaque owner policy only. Hooks never interpret/change these bits.
    std::atomic<std::uint32_t> ownerStateFlags{0};
    std::atomic<bool> stateInterceptionEnabled{false}; // opt-in only AFTER owner ARA admission
    // Owner closes this gate, then waits for actualNativeProcessCallsInFlight=0
    // before model/cache mutation (use seq_cst store/load, the default atomic
    // ordering, for this owner-side close/wait). A later entrant increments before testing
    // the closed gate and never calls native processing or owner callbacks.
    std::atomic<bool> nativeProcessAllowed{true};
    std::atomic<std::uint32_t> actualNativeProcessCallsInFlight{0};
    std::atomic<std::uint64_t> blockedProcessCallbacks{0};
    std::atomic<bool> nativeAlive{true},nativeInitialized{false},nativeTerminated{false};
    std::atomic<std::uint32_t> initializeCalls{0},terminateCalls{0},setStateCalls{0},getStateCalls{0};
    std::atomic<std::uint32_t> originalOnlyDiagnosticGetStateCalls{0};
    std::atomic<std::uint32_t> activationCalls{0},processingCalls{0},contextRequirementCalls{0},editorViewCalls{0};
};
enum class StateOperation { ComponentSetState, ComponentGetState, ControllerSetComponentState };
enum class ControllerHandlerPhase : std::uint8_t { BeforeOriginal, AfterOriginal };
enum class NativeProcessTelemetryPhase : std::uint8_t { BeforeOriginal, AfterOriginal };
// Test-only POD observation. ALL pointers (including nested SDK buffers) are
// borrowed for this synchronous callback only. No queued/deferred dereference.
// BeforeOriginal is immediately before the actual native call (after owner
// beforeProcess); AfterOriginal is immediately after it (before afterProcess).
// originalInvoked is false before and true only after that exact call returned.
// originalResult is meaningful only AfterOriginal; native failure is preserved.
struct NativeProcessTelemetry {
    NativeProcessTelemetryPhase phase;
    Steinberg::Vst::IAudioProcessor* actualNativeProcessor;
    const Steinberg::Vst::ProcessData* actualProcessData;
    bool originalInvoked;
    Steinberg::tresult originalResult;
};
class OriginalStateCall {
public:
    using Function=Steinberg::tresult (PLUGIN_API *)(void*,Steinberg::IBStream*);
    OriginalStateCall(void* actualSelf,Function original) noexcept : self(actualSelf),function(original) {}
    OriginalStateCall(const OriginalStateCall&)=delete;
    OriginalStateCall& operator=(const OriginalStateCall&)=delete;
    OriginalStateCall(OriginalStateCall&&)=delete;
    OriginalStateCall& operator=(OriginalStateCall&&)=delete;
    // Exactly one call is allowed. A second attempt returns kInternalError and
    // never invokes the plug-in again. The owner may supply its unwrapped stream.
    Steinberg::tresult operator()(Steinberg::IBStream*) noexcept;
    bool wasCalled()const noexcept{return calls!=0;}
    bool duplicateCallAttempted()const noexcept{return duplicate;}
    Steinberg::tresult actualResult()const noexcept{return result;}
    void* actualSelf()const noexcept{return self;}
    Function actualOriginal()const noexcept{return function;}
private:
    void* self=nullptr;Function function=nullptr;unsigned calls=0;bool duplicate=false;
    Steinberg::tresult result=Steinberg::kInternalError;
};
struct Callbacks {
    void (*moduleObserved)(const ModuleRecord&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*factoryObserved)(FactoryRecord&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*factoryInfoObserved)(FactoryRecord&,const Steinberg::PFactoryInfo&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*classInfoObserved)(FactoryRecord&,Steinberg::int32,const Steinberg::PClassInfo&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*instanceCreated)(InstanceRecord&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*interfaceObserved)(InstanceRecord&,const ClassID&,void* actualInterface,
                              const ImageEvidence& actualVTableImage,const ThreadInfo&,void*) noexcept=nullptr;
    // Native initialize is called ONCE first. This callback is synchronous on
    // that actual native thread; it decides whether a genuine ARA bind is valid.
    void (*initialized)(InstanceRecord&,void* actualInterface,Steinberg::FUnknown*,
                        Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    void (*beforeTerminate)(InstanceRecord&,void* actualInterface,const ThreadInfo&,void*) noexcept=nullptr;
    void (*terminated)(InstanceRecord&,void* actualInterface,Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    void (*setActiveObserved)(InstanceRecord&,Steinberg::TBool,Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    // Called only after stateInterceptionEnabled=true. The owner forwards the
    // original ordinary component bytes ONCE through OriginalStateCall, or
    // explicitly returns a failure before that call. Success without an original
    // call and duplicate original calls are rejected. No hidden fallback/retry.
    Steinberg::tresult (*state)(InstanceRecord&,StateOperation,Steinberg::IBStream*,
                               OriginalStateCall&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*setupProcessingObserved)(InstanceRecord&,const Steinberg::Vst::ProcessSetup&,
                                   Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    void (*setProcessingObserved)(InstanceRecord&,Steinberg::TBool,Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    void (*contextRequirementsObserved)(InstanceRecord&,Steinberg::uint32,const ThreadInfo&,void*) noexcept=nullptr;
    // RT: direct stable record/ownerData, no lookup, allocation, locks, CF/file,
    // model/SDK queries (except the optional queue count below), logging or
    // scheduling inside this library. Owner callbacks must obey that contract
    // and may only observe borrowed ProcessData.
    void (*beforeProcess)(InstanceRecord&,const Steinberg::Vst::ProcessData&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*afterProcess)(InstanceRecord&,const Steinberg::Vst::ProcessData&,
                         Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    // OPTIONAL test telemetry only; disabled by default. Same strict RT
    // contract as process callbacks: bounded numeric reads/caller-preallocated
    // POD or lock-free atomics only; NO allocation/file/SDK/map/mutex/logging,
    // model work, scheduling, buffer mutation or persistent borrowed pointers.
    // Never called for closed-gate, unregistered-object or owned-companion TLS
    // bypass calls. Native original is still invoked exactly once ordinarily.
    void (*nativeProcessTelemetry)(InstanceRecord&,const NativeProcessTelemetry&,
                                   const ThreadInfo&,void*) noexcept=nullptr;
    // Invalidate only pre-stored cache gates/dirty atomics when the actual native
    // inputParameterChanges queue reports >0 parameters, BEFORE beforeProcess
    // or original processing. The only SDK query in this path is its standard
    // realtime getParameterCount(), once when this observer is enabled and the
    // actual queue pointer is nonnull. Empty queues do NOT invalidate. The actual
    // count is supplied as POD; queue pointer is synchronous/borrowed only.
    // No parameter IDs/points/value calls, model calls, allocation or locks.
    void (*inputParameterChangesObserved)(InstanceRecord&,Steinberg::Vst::IParameterChanges*,
                                          Steinberg::int32 actualParameterCount,const ThreadInfo&,void*) noexcept=nullptr;
    void (*viewCreated)(InstanceRecord&,Steinberg::FIDString,Steinberg::IPlugView*,const ThreadInfo&,void*) noexcept=nullptr;
    // Actual successful native public parameter calls. These may be RT: owner
    // does ONLY pre-stored cache-gate invalidation/dirty atomics, never SDK,
    // lookup, allocation or logging. Values/results are never changed here.
    // A shared handler's first controller is diagnostic only, may be nullptr or
    // terminated, and MUST NOT be used to infer which insert was edited. Owner
    // scans only stable verified controller associations and matches the actual
    // handler against each accepted OR pending handler. Unrelated controllers
    // affect no ARA insert; a genuinely shared known handler affects only those
    // associated matching inserts. This library never guesses that ownership.
    void (*paramEditObserved)(InstanceRecord* diagnosticFirstController,
        Steinberg::Vst::IComponentHandler* actualNativeHandler,
        Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const ThreadInfo&,void*) noexcept=nullptr;
    void (*paramNormalizedObserved)(InstanceRecord&,Steinberg::Vst::IEditController*,
        Steinberg::Vst::ParamID,Steinberg::Vst::ParamValue,const ThreadInfo&,void*) noexcept=nullptr;
    // Exact native setter boundary, including failure/null/different handlers.
    // BeforeOriginal has pending actual argument already published and result
    // kInternalError (unavailable); AfterOriginal reports its actual result,
    // accepted handler changed only on success, pending restored/cleared. Owner
    // may publish/rollback an independently verified association here. These
    // callbacks can be non-main or inside processing: inspect actual ThreadInfo,
    // use only pre-stored atomics there; no SDK/VM/registry/alloc/lookup on RT.
    // Owned-companion TLS bypass suppresses callbacks and both handler stores.
    void (*controllerHandlerObserved)(InstanceRecord&,ControllerHandlerPhase,
        Steinberg::Vst::IEditController* actualController,
        Steinberg::Vst::IComponentHandler* actualPassedHandler,
        Steinberg::tresult,const ThreadInfo&,void*) noexcept=nullptr;
    // No borrowed self may be dereferenced after the final release. Records
    // retain identities/addresses for evidence, not native object ownership.
    void (*finalReleaseObserved)(InstanceRecord&,const ThreadInfo&,void*) noexcept=nullptr;
    void (*fault)(const char* fixedStage,const ModuleRecord*,const InstanceRecord*,
                  const ThreadInfo&,void*) noexcept=nullptr;
};
struct Config {
    std::string exactOwnedExecutablePath,exactOwnedExecutableSHA256;
    int exactOwnedProcessID=0;
    // Mandatory owner check, in addition to path/PID/SHA. It confirms this is
    // the already admitted owned normal FL process; the hook never admits apps.
    bool (*confirmAlreadyAdmittedOwnedProcess)(void*) noexcept=nullptr;
    Callbacks callbacks;
    void* callbackContext=nullptr;
};
struct InstallResult {bool installed=false;const char* reason="not attempted";};
// Model/native non-RT thread only; immutable config for the lifetime of process.
InstallResult install(const Config&) noexcept;
// Stop owner callbacks without detaching/invalidating live native objects. Native
// methods still forward their actual original once. Does not unload anything.
void disableCallbacks() noexcept;
bool installed() noexcept;
// Safe metadata lookup ONLY off RT, for owner diagnostics. Process hooks never
// call these. Returned records have process lifetime, native objects do not.
InstanceRecord* findInstance(void* actualNativeInterface) noexcept;
FactoryRecord* findFactory(Steinberg::IPluginFactory*) noexcept;
// Explicit OWN diagnostic, not a native save event. Owner must already hold its
// actual stopped/drained native boundary outside ARA operations. Main-thread,
// live initialized actual component only; calls its saved ordinary getState
// ONCE into the supplied stream and never invokes the owner state interceptor.
// No native-caller provenance is synthesized and native getStateCalls is not
// incremented. Failure of these guards invokes the original zero times.
Steinberg::tresult captureOriginalComponentState(InstanceRecord&,Steinberg::IBStream&) noexcept;
} // namespace rysup::fl_ara::native_hooks
