#pragma once
// SDK-free policy only. Root verifies actual Apple image/build/caller/callback
// provenance and holds a Sink lifecycle/start lease around native calls.
// This gate never invokes a native API, owns a device, or dereferences identities.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace rysup::fl_ara::test_output {
class FLUnitDeviceGate final {
public:
    using Unit=std::uintptr_t; using IOProc=std::uintptr_t;
    enum class Pair:std::uint32_t { None=0, AUHAL=1, Generic=2 };
    enum class Kind:std::uint32_t { CurrentDevice=1, Initialize=2, OutputStart=3 };
    enum class Reason:std::uint32_t { None=0, NullIdentity, InvalidPair, Busy, Poisoned, NoScope,
        WrongScope, NestedDifferentOwner, ReentrantStart, ScopeOverflow, UnitPoolFull,
        IOProcPoolFull, ConcurrentOwner, StaleGeneration, WrongDevice, UnknownIOProc,
        OriginalFailed, CallbackChanged, ValidatorFailed, ScopeOrder, InvalidScope,
        DestroyReserved, DestroyPoolFull, StaleBinding };
    struct Stats {
        std::uint64_t scopesBegun=0,scopeRefusals=0,createsCaptured=0,createRefusals=0;
        std::uint64_t startsAllowed=0,startRefusals=0,revocations=0;
        std::uint32_t lastReason=0,lastDevice=0,lastStage=0;
        bool poisoned=false;
    };
    using StillPrepared=bool(*)(void*) noexcept;
    class Scope final {
    public:
        Scope() noexcept=default; ~Scope();
        Scope(const Scope&)=delete; Scope& operator=(const Scope&)=delete;
        Scope(Scope&&) noexcept; Scope& operator=(Scope&&) noexcept;
        explicit operator bool()const noexcept{return gate_!=nullptr;}
        // Stable unit generation used for metadata binding, not Sink preparation.
        std::uint64_t generation()const noexcept{return generation_;}
        bool close() noexcept;
    private:
        friend class FLUnitDeviceGate;
        FLUnitDeviceGate* gate_=nullptr;std::uintptr_t thread_=0;Unit unit_=0;std::uint32_t device_=0;std::uint64_t token_=0,generation_=0;
    };
    class DestructionLease final {
    public:
        DestructionLease()noexcept=default;~DestructionLease();
        DestructionLease(const DestructionLease&)=delete;DestructionLease& operator=(const DestructionLease&)=delete;
        DestructionLease(DestructionLease&&)noexcept;DestructionLease& operator=(DestructionLease&&)noexcept;
        explicit operator bool()const noexcept{return gate_!=nullptr;}
        bool known()const noexcept{return known_;}
        Unit unit()const noexcept{return unit_;}
        std::uint64_t generation()const noexcept{return generation_;}
        // Root acquires the known unit's Sink Mutation lease (or verifies its
        // already-held SAME-THREAD mutation) before committing. Then call the
        // original Destroy once, holding BOTH leases until it returns.
        bool commitRevoke()noexcept;
        // Before commit: abandon without revoking the old live binding. After
        // commit: release reservation only; native failure never revives it.
        bool close()noexcept;
    private:
        friend class FLUnitDeviceGate;
        FLUnitDeviceGate* gate_=nullptr;Unit unit_=0;IOProc id_=0;std::uint32_t device_=0;
        std::uintptr_t thread_=0;std::uint64_t token_=0,generation_=0;bool known_=false;
    };
    static constexpr std::size_t maximumUnits=16,maximumIOProcs=32,maximumScopes=8,maximumDestructions=16;
    FLUnitDeviceGate() noexcept=default;
    FLUnitDeviceGate(const FLUnitDeviceGate&)=delete;
    FLUnitDeviceGate& operator=(const FLUnitDeviceGate&)=delete;
    // Non-RT metadata scopes can precede Sink callback registration. Revoke old
    // unit BEFORE device changes/uninitialize/dispose, then begin the new scope.
    Scope beginCapture(Kind,Unit,std::uint32_t device) noexcept;
    // Sink lease must stay held until original OutputStart returns AND this
    // scope closes. Validator/context must remain alive and nonblocking there.
    // preparedTicket is the Sink's nonzero immutable leased generation.
    Scope beginOutputStart(Unit,std::uint32_t device,std::uint64_t preparedTicket,
        StillPrepared,void* context) noexcept;
    // Root calls only after actual original CreateIOProcID exactly once.
    // Pair attests BOTH actual create caller and callback image/offset/build.
    bool recordCreate(std::uint32_t device,std::uintptr_t callbackIdentity,
        IOProc returned,std::int32_t actualResult,Pair) noexcept;
    // Root attests actual start caller against pair (AUHAL Start only;
    // Generic Start or StartAtTime), then calls original exactly once if true.
    bool approveDeviceStart(std::uint32_t device,IOProc,Pair,bool isAtTime) noexcept;
    // Immediately after an approved actual original DeviceStart/AtTime. Failure
    // revokes the ENTIRE current unit generation before any same-stack retry.
    // Success is acknowledgement only and cannot revive revoked metadata.
    bool deviceStartReturned(std::uint32_t device,IOProc,std::int32_t actualResult) noexcept;
    // Reserve first, including an unknown ID, BEFORE Root's owner/lease lookup.
    // No table claim spans the lease or any native API. Mismatched device or
    // another reservation refuses. A reserved ID cannot be captured or started.
    DestructionLease beginDestroy(std::uint32_t device,IOProc) noexcept;
    // BEFORE original DestroyIOProcID; a failed destroy stays revoked.
    bool revokeIOProc(std::uint32_t device,IOProc) noexcept;
    // BEFORE original disposal/uninitialize/device change. Sink-only callback/
    // format mutations invalidate its separate prepared ticket under its lease;
    // they need not erase a still-live device IOProc captured before registration.
    bool revokeUnit(Unit) noexcept;
    // Actual failed original start invalidates all unit-generation captures.
    bool originalStartReturned(const Scope&,std::int32_t actualResult) noexcept;
    Stats snapshot()const noexcept;
private:
    struct UnitRecord{Unit unit=0;std::uint32_t device=0;std::uint64_t generation=0;std::uintptr_t thread=0;std::uint32_t scopes=0;bool live=false;};
    struct IORecord{IOProc id=0;Unit unit=0;std::uintptr_t callback=0;std::uint32_t device=0;std::uint64_t generation=0,bindingSerial=0;Pair pair=Pair::None;bool live=false;};
    struct DestroyRecord{IOProc id=0;Unit unit=0;std::uint32_t device=0;std::uint64_t token=0,generation=0,bindingSerial=0;bool occupied=false,known=false,committed=false;};
    struct Frame{FLUnitDeviceGate* gate=nullptr;Unit unit=0;std::uint32_t device=0;std::uint64_t generation=0,token=0,preparedTicket=0;Kind kind=Kind::CurrentDevice;StillPrepared validator=nullptr;void* context=nullptr;};
    struct TLS{std::array<Frame,maximumScopes> frames{};std::size_t depth=0;std::uint64_t next=0;bool validating=false;};
    static thread_local TLS tls;
    Scope begin(Kind,Unit,std::uint32_t,std::uint64_t,StillPrepared,void*) noexcept;
    bool end(Scope&) noexcept;
    UnitRecord* findUnit(Unit) noexcept;
    IORecord* findIO(IOProc) noexcept;
    IORecord* findAnyIO(IOProc) noexcept;
    DestroyRecord* findDestroy(IOProc) noexcept;
    bool commitDestroy(DestructionLease&) noexcept;
    bool closeDestroy(DestructionLease&) noexcept;
    void revokeLocked(UnitRecord&) noexcept;
    void fail(Reason,std::uint32_t device,std::uint32_t stage) noexcept;
    bool scopeCurrent(const Frame&)const noexcept;
    static std::uintptr_t threadToken()noexcept;
    std::array<UnitRecord,maximumUnits> units{};
    std::array<IORecord,maximumIOProcs> io{};
    std::array<DestroyRecord,maximumDestructions> destroys{};
    std::uint64_t nextGeneration=0,nextBindingSerial=0,nextDestroyToken=0;
    std::atomic<bool> claimed{false},poisoned{false};
    std::array<std::atomic<std::uint64_t>,7> counts{};
    std::atomic<std::uint32_t> lastReason{0},lastDevice{0},lastStage{0};
};
} // namespace rysup::fl_ara::test_output
