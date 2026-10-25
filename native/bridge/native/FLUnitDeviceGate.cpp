#include "FLUnitDeviceGate.h"
#include <utility>
#include <limits>
namespace rysup::fl_ara::test_output {
namespace {
struct Claim {
    std::atomic<bool>& flag;bool held=false;
    explicit Claim(std::atomic<bool>& value)noexcept:flag(value){bool expected=false;held=flag.compare_exchange_strong(expected,true,std::memory_order_acq_rel);}
    ~Claim(){if(held)flag.store(false,std::memory_order_release);}
};
bool validPair(FLUnitDeviceGate::Pair p)noexcept{return p==FLUnitDeviceGate::Pair::AUHAL||p==FLUnitDeviceGate::Pair::Generic;}
}
thread_local FLUnitDeviceGate::TLS FLUnitDeviceGate::tls{};
std::uintptr_t FLUnitDeviceGate::threadToken()noexcept{return reinterpret_cast<std::uintptr_t>(&tls);}
FLUnitDeviceGate::Scope::~Scope(){close();}
FLUnitDeviceGate::Scope::Scope(Scope&& other)noexcept{*this=std::move(other);}
FLUnitDeviceGate::Scope& FLUnitDeviceGate::Scope::operator=(Scope&& other)noexcept{
    if(this!=&other){close();gate_=std::exchange(other.gate_,nullptr);thread_=other.thread_;unit_=other.unit_;device_=other.device_;token_=other.token_;generation_=other.generation_;}return *this;
}
bool FLUnitDeviceGate::Scope::close()noexcept{return !gate_||gate_->end(*this);}
FLUnitDeviceGate::DestructionLease::~DestructionLease(){close();}
FLUnitDeviceGate::DestructionLease::DestructionLease(DestructionLease&& other)noexcept{*this=std::move(other);}
FLUnitDeviceGate::DestructionLease& FLUnitDeviceGate::DestructionLease::operator=(DestructionLease&& other)noexcept{
    if(this!=&other){close();gate_=std::exchange(other.gate_,nullptr);unit_=other.unit_;id_=other.id_;device_=other.device_;thread_=other.thread_;token_=other.token_;generation_=other.generation_;known_=other.known_;}return *this;
}
bool FLUnitDeviceGate::DestructionLease::commitRevoke()noexcept{return gate_&&gate_->commitDestroy(*this);}
bool FLUnitDeviceGate::DestructionLease::close()noexcept{return !gate_||gate_->closeDestroy(*this);}
void FLUnitDeviceGate::fail(Reason r,std::uint32_t d,std::uint32_t stage)noexcept{
    lastReason.store(static_cast<std::uint32_t>(r),std::memory_order_relaxed);lastDevice.store(d,std::memory_order_relaxed);lastStage.store(stage,std::memory_order_relaxed);
}
FLUnitDeviceGate::UnitRecord* FLUnitDeviceGate::findUnit(Unit unit)noexcept{
    for(auto& r:units)if(r.unit==unit&&unit)return &r;return nullptr;
}
FLUnitDeviceGate::IORecord* FLUnitDeviceGate::findIO(IOProc id)noexcept{
    auto* r=findAnyIO(id);return r&&r->live?r:nullptr;
}
FLUnitDeviceGate::IORecord* FLUnitDeviceGate::findAnyIO(IOProc id)noexcept{
    for(auto& r:io)if(r.id==id&&id)return &r;return nullptr;
}
FLUnitDeviceGate::DestroyRecord* FLUnitDeviceGate::findDestroy(IOProc id)noexcept{
    for(auto& r:destroys)if(r.occupied&&r.id==id&&id)return &r;return nullptr;
}
void FLUnitDeviceGate::revokeLocked(UnitRecord& r)noexcept{
    r.live=false;for(auto& p:io)if(p.unit==r.unit)p.live=false;counts[6].fetch_add(1,std::memory_order_relaxed);
}
bool FLUnitDeviceGate::scopeCurrent(const Frame& f)const noexcept{
    if(!tls.depth||tls.validating)return false;const auto& top=tls.frames[tls.depth-1];
    if(top.gate!=this||top.unit!=f.unit||top.device!=f.device||top.generation!=f.generation)return false;
    for(std::size_t i=0;i<tls.depth;++i)if(tls.frames[i].gate==this&&tls.frames[i].token==f.token)return true;
    return false;
}
FLUnitDeviceGate::Scope FLUnitDeviceGate::beginCapture(Kind kind,Unit unit,std::uint32_t device)noexcept{
    if(kind!=Kind::CurrentDevice&&kind!=Kind::Initialize){counts[1].fetch_add(1);fail(Reason::WrongScope,device,1);return{};}
    return begin(kind,unit,device,0,nullptr,nullptr);
}
FLUnitDeviceGate::Scope FLUnitDeviceGate::beginOutputStart(Unit unit,std::uint32_t device,std::uint64_t ticket,StillPrepared check,void* context)noexcept{
    return begin(Kind::OutputStart,unit,device,ticket,check,context);
}
FLUnitDeviceGate::Scope FLUnitDeviceGate::begin(Kind kind,Unit unit,std::uint32_t device,std::uint64_t ticket,StillPrepared check,void* context)noexcept{
    const auto refuse=[&](Reason reason){counts[1].fetch_add(1);fail(reason,device,1);return Scope{};};
    if(!unit||!device)return refuse(Reason::NullIdentity);
    if(poisoned.load(std::memory_order_acquire))return refuse(Reason::Poisoned);
    if(tls.validating)return refuse(Reason::WrongScope);
    if(kind==Kind::OutputStart&&(!ticket||!check))return refuse(Reason::InvalidScope);
    if(tls.depth>=maximumScopes)return refuse(Reason::ScopeOverflow);
    if(tls.depth){const auto& top=tls.frames[tls.depth-1];
        if(top.gate!=this||top.unit!=unit||top.device!=device)return refuse(Reason::NestedDifferentOwner);
        if(kind==Kind::OutputStart)for(std::size_t i=0;i<tls.depth;++i)if(tls.frames[i].kind==Kind::OutputStart)return refuse(Reason::ReentrantStart);
    }
    Claim claim(claimed);if(!claim.held)return refuse(Reason::Busy);
    auto* r=findUnit(unit);
    if(r&&r->scopes&&r->thread!=threadToken())return refuse(Reason::ConcurrentOwner);
    if(r&&r->live&&r->device!=device)return refuse(Reason::WrongDevice);
    if(r&&!r->live&&r->scopes)return refuse(Reason::StaleGeneration);
    if(!r){for(auto& candidate:units)if(!candidate.unit||(!candidate.live&&!candidate.scopes)){r=&candidate;break;}}
    if(!r)return refuse(Reason::UnitPoolFull);
    if(!r->live){
        if(nextGeneration==std::numeric_limits<std::uint64_t>::max()){poisoned=true;return refuse(Reason::Poisoned);}
        *r={unit,device,++nextGeneration,0,0,true};
    }
    if(tls.depth&&tls.frames[tls.depth-1].generation!=r->generation)return refuse(Reason::StaleGeneration);
    if(tls.next==std::numeric_limits<std::uint64_t>::max()){poisoned=true;return refuse(Reason::Poisoned);}
    const auto token=++tls.next;r->thread=threadToken();++r->scopes;
    tls.frames[tls.depth++]={this,unit,device,r->generation,token,ticket,kind,check,context};
    Scope result;result.gate_=this;result.thread_=threadToken();result.unit_=unit;result.device_=device;result.token_=token;result.generation_=r->generation;
    counts[0].fetch_add(1);fail(Reason::None,device,1);return result;
}
bool FLUnitDeviceGate::end(Scope& scope)noexcept{
    auto* owner=scope.gate_;scope.gate_=nullptr;if(owner!=this)return false;
    if(scope.thread_!=threadToken()){revokeUnit(scope.unit_);fail(Reason::WrongScope,scope.device_,5);return false;}
    std::size_t index=tls.depth;for(std::size_t i=0;i<tls.depth;++i)if(tls.frames[i].gate==this&&tls.frames[i].token==scope.token_){index=i;break;}
    if(index==tls.depth){fail(Reason::InvalidScope,scope.device_,5);return false;}
    const bool ordered=index+1==tls.depth;Claim claim(claimed);
    if(!claim.held){poisoned.store(true,std::memory_order_release);tls.depth=index;fail(Reason::Busy,scope.device_,5);return false;}
    for(std::size_t i=index;i<tls.depth;++i){const auto& f=tls.frames[i];auto* r=findUnit(f.unit);
        if(r&&r->generation==f.generation){if(!ordered)revokeLocked(*r);if(r->scopes)--r->scopes;if(!r->scopes)r->thread=0;}
    }
    tls.depth=index;fail(ordered?Reason::None:Reason::ScopeOrder,scope.device_,5);return ordered;
}
bool FLUnitDeviceGate::recordCreate(std::uint32_t device,std::uintptr_t callback,IOProc returned,std::int32_t result,Pair pair)noexcept{
    const auto refuse=[&](Reason reason){counts[3].fetch_add(1);fail(reason,device,2);return false;};
    if(!tls.depth||tls.validating||tls.frames[tls.depth-1].gate!=this)return refuse(Reason::NoScope);
    const auto frame=tls.frames[tls.depth-1];
    Claim claim(claimed);if(!claim.held){poisoned=true;return refuse(Reason::Busy);}
    auto* r=findUnit(frame.unit);
    if(poisoned.load()||!r||!r->live||r->generation!=frame.generation||r->thread!=threadToken())return refuse(Reason::StaleGeneration);
    const auto invalid=[&](Reason reason){revokeLocked(*r);return refuse(reason);};
    if(result)return invalid(Reason::OriginalFailed);
    if(!device||!callback||!returned)return invalid(Reason::NullIdentity);
    if(device!=frame.device||device!=r->device)return invalid(Reason::WrongDevice);
    if(!validPair(pair))return invalid(Reason::InvalidPair);
    if(findDestroy(returned))return invalid(Reason::DestroyReserved);
    auto* existing=findAnyIO(returned);
    if(existing&&existing->live){
        if(existing->unit!=r->unit||existing->generation!=r->generation||existing->device!=device||existing->callback!=callback||existing->pair!=pair){
            if(auto* other=findUnit(existing->unit))revokeLocked(*other);return invalid(Reason::CallbackChanged);
        }
    }
    // Retain dead identity/owner metadata for later native cleanup. Only a new
    // actual Create returning the SAME ID may replace that historical binding.
    IORecord* slot=existing;if(!slot)for(auto& candidate:io)if(!candidate.id){slot=&candidate;break;}
    if(!slot)return invalid(Reason::IOProcPoolFull);
    if(nextBindingSerial==UINT64_MAX){poisoned=true;return invalid(Reason::Poisoned);}
    *slot={returned,r->unit,callback,device,r->generation,++nextBindingSerial,pair,true};counts[2].fetch_add(1);fail(Reason::None,device,2);return true;
}
bool FLUnitDeviceGate::approveDeviceStart(std::uint32_t device,IOProc id,Pair pair,bool atTime)noexcept{
    const auto refuse=[&](Reason reason){counts[5].fetch_add(1);fail(reason,device,3);return false;};
    if(poisoned.load())return refuse(Reason::Poisoned);
    if(!device||!id)return refuse(Reason::NullIdentity);
    if(!validPair(pair)||(atTime&&pair!=Pair::Generic))return refuse(Reason::InvalidPair);
    if(!tls.depth||tls.validating)return refuse(Reason::NoScope);
    Frame frame;bool found=false;for(std::size_t i=tls.depth;i;--i)if(tls.frames[i-1].gate==this&&tls.frames[i-1].kind==Kind::OutputStart){frame=tls.frames[i-1];found=true;break;}
    if(!found||!scopeCurrent(frame)||!frame.preparedTicket||!frame.validator)return refuse(Reason::WrongScope);
    const auto validLocked=[&]()noexcept{
        auto* r=findUnit(frame.unit);auto* p=findIO(id);
        return !poisoned.load()&&r&&r->live&&r->generation==frame.generation&&r->device==device&&r->thread==threadToken()&&r->scopes
            &&p&&p->unit==frame.unit&&p->generation==frame.generation&&p->device==device&&p->pair==pair&&!findDestroy(id);
    };
    {Claim claim(claimed);if(!claim.held)return refuse(Reason::Busy);if(!validLocked())return refuse(Reason::UnknownIOProc);}
    // No table claim across this foreign nonblocking validator. Its owner holds
    // the independent Sink start lease; recheck our table after it returns.
    tls.validating=true;const bool prepared=frame.validator(frame.context);tls.validating=false;
    if(!prepared)return refuse(Reason::ValidatorFailed);
    Claim claim(claimed);if(!claim.held)return refuse(Reason::Busy);
    if(!scopeCurrent(frame)||!validLocked())return refuse(Reason::StaleGeneration);
    counts[4].fetch_add(1);fail(Reason::None,device,3);return true;
}
FLUnitDeviceGate::DestructionLease FLUnitDeviceGate::beginDestroy(std::uint32_t device,IOProc id)noexcept{
    const auto refuse=[&](Reason reason){fail(reason,device,8);return DestructionLease{};};
    if(!device||!id)return refuse(Reason::NullIdentity);
    if(poisoned.load())return refuse(Reason::Poisoned);
    Claim claim(claimed);if(!claim.held)return refuse(Reason::Busy);
    if(findDestroy(id))return refuse(Reason::DestroyReserved);
    auto* binding=findAnyIO(id);if(binding&&binding->device!=device)return refuse(Reason::WrongDevice);
    DestroyRecord* slot=nullptr;for(auto& candidate:destroys)if(!candidate.occupied){slot=&candidate;break;}
    if(!slot)return refuse(Reason::DestroyPoolFull);
    if(nextDestroyToken==UINT64_MAX){poisoned=true;return refuse(Reason::Poisoned);}
    *slot={id,binding?binding->unit:0,device,++nextDestroyToken,binding?binding->generation:0,binding?binding->bindingSerial:0,true,binding!=nullptr,false};
    DestructionLease result;result.gate_=this;result.unit_=slot->unit;result.id_=id;result.device_=device;result.thread_=threadToken();result.token_=slot->token;result.generation_=slot->generation;result.known_=slot->known;
    fail(Reason::None,device,8);return result;
}
bool FLUnitDeviceGate::commitDestroy(DestructionLease& lease)noexcept{
    if(lease.gate_!=this||lease.thread_!=threadToken()){fail(Reason::WrongScope,lease.device_,9);return false;}
    Claim claim(claimed);if(!claim.held){poisoned=true;fail(Reason::Busy,lease.device_,9);return false;}
    auto* reservation=findDestroy(lease.id_);
    if(!reservation||reservation->token!=lease.token_||reservation->device!=lease.device_||reservation->committed){fail(Reason::StaleBinding,lease.device_,9);return false;}
    auto* binding=findAnyIO(lease.id_);
    if(reservation->known){
        if(!binding||binding->unit!=reservation->unit||binding->device!=reservation->device||binding->generation!=reservation->generation||binding->bindingSerial!=reservation->bindingSerial){fail(Reason::StaleBinding,lease.device_,9);return false;}
        binding->live=false;
    }else if(binding){fail(Reason::StaleBinding,lease.device_,9);return false;}
    reservation->committed=true;counts[6].fetch_add(1);fail(Reason::None,lease.device_,9);return true;
}
bool FLUnitDeviceGate::closeDestroy(DestructionLease& lease)noexcept{
    if(lease.gate_!=this)return false;lease.gate_=nullptr;
    Claim claim(claimed);if(!claim.held){poisoned=true;fail(Reason::Busy,lease.device_,10);return false;}
    auto* reservation=findDestroy(lease.id_);
    if(!reservation||reservation->token!=lease.token_||reservation->device!=lease.device_){fail(Reason::StaleBinding,lease.device_,10);return false;}
    *reservation={};fail(Reason::None,lease.device_,10);return true;
}
bool FLUnitDeviceGate::deviceStartReturned(std::uint32_t device,IOProc id,std::int32_t result)noexcept{
    if(!device||!id){fail(Reason::NullIdentity,device,7);return false;}
    if(!tls.depth||tls.validating){fail(Reason::NoScope,device,7);return false;}
    Frame frame;bool found=false;for(std::size_t i=tls.depth;i;--i)if(tls.frames[i-1].gate==this&&tls.frames[i-1].kind==Kind::OutputStart){frame=tls.frames[i-1];found=true;break;}
    if(!found||!scopeCurrent(frame)||frame.device!=device){fail(Reason::WrongScope,device,7);return false;}
    Claim claim(claimed);if(!claim.held){poisoned=true;fail(Reason::Busy,device,7);return false;}
    auto* r=findUnit(frame.unit);
    if(!r||r->generation!=frame.generation||r->device!=device||r->thread!=threadToken()){
        fail(Reason::StaleGeneration,device,7);return false;
    }
    // The native owner invokes this only immediately after an APPROVED actual
    // original call. Even if that original destroyed its IOProc before failing,
    // its active scope still identifies the unit whose other captures must die.
    if(result){revokeLocked(*r);fail(Reason::OriginalFailed,device,7);return true;}
    auto* p=findIO(id);
    if(poisoned.load()||!r->live||!p||p->unit!=frame.unit||p->generation!=frame.generation||p->device!=device){
        fail(Reason::StaleGeneration,device,7);return false;
    }
    fail(Reason::None,device,7);return true;
}
bool FLUnitDeviceGate::revokeIOProc(std::uint32_t device,IOProc id)noexcept{
    if(!device||!id){fail(Reason::NullIdentity,device,4);return false;}
    Claim claim(claimed);if(!claim.held){poisoned=true;fail(Reason::Busy,device,4);return false;}
    auto* p=findIO(id);if(!p||p->device!=device){fail(Reason::UnknownIOProc,device,4);return false;}
    p->live=false;counts[6].fetch_add(1);fail(Reason::None,device,4);return true;
}
bool FLUnitDeviceGate::revokeUnit(Unit unit)noexcept{
    if(!unit){fail(Reason::NullIdentity,0,4);return false;}
    Claim claim(claimed);if(!claim.held){poisoned=true;fail(Reason::Busy,0,4);return false;}
    auto* r=findUnit(unit);if(!r){fail(Reason::InvalidScope,0,4);return false;}
    const auto device=r->device;revokeLocked(*r);fail(Reason::None,device,4);return true;
}
bool FLUnitDeviceGate::originalStartReturned(const Scope& scope,std::int32_t result)noexcept{
    if(scope.gate_!=this||scope.thread_!=threadToken()){fail(Reason::InvalidScope,scope.device_,6);return false;}
    if(!tls.depth||tls.frames[tls.depth-1].gate!=this||tls.frames[tls.depth-1].token!=scope.token_
        ||tls.frames[tls.depth-1].kind!=Kind::OutputStart){if(result)revokeUnit(scope.unit_);fail(Reason::InvalidScope,scope.device_,6);return false;}
    if(!result)return true;
    const bool revoked=revokeUnit(scope.unit_);fail(Reason::OriginalFailed,scope.device_,6);return revoked;
}
FLUnitDeviceGate::Stats FLUnitDeviceGate::snapshot()const noexcept{
    Stats s;s.scopesBegun=counts[0].load();s.scopeRefusals=counts[1].load();s.createsCaptured=counts[2].load();s.createRefusals=counts[3].load();s.startsAllowed=counts[4].load();s.startRefusals=counts[5].load();s.revocations=counts[6].load();
    s.lastReason=lastReason.load();s.lastDevice=lastDevice.load();s.lastStage=lastStage.load();s.poisoned=poisoned.load();return s;
}
} // namespace rysup::fl_ara::test_output
