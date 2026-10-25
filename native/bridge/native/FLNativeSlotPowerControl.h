#pragma once
// SDK-free observation/control ONLY. No vendor-memory read, SDK/native call,
// hook installation, allocation, wait, lock, cache publication or audio write.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace rysup::fl_ara::slot_power {
enum class Power : std::uint8_t { Unknown=0,Disabled=1,Enabled=2 };
inline constexpr std::intptr_t setEnabledID=11;
using Dispatcher=std::intptr_t(*)(void*,std::intptr_t,std::intptr_t,std::intptr_t);
static_assert(sizeof(std::intptr_t)==8,"qualified native Dispatcher requires the 64-bit scalar ABI");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"slot power word must be lock-free");

class Binding {
    static constexpr std::uint64_t powerMask=3,closedBit=4,overlapBit=8,seedUsedBit=16;
    static constexpr unsigned countShift=5,serialShift=21;
    static constexpr std::uint64_t countOne=1ull<<countShift,countMask=0xffffull<<countShift;
    static constexpr std::uint64_t serialOne=1ull<<serialShift,serialMask=~((1ull<<serialShift)-1);
    std::atomic<std::uint64_t> word{0};
    static Power normalized(Power value)noexcept{return value==Power::Disabled||value==Power::Enabled?value:Power::Unknown;}
    void complete(std::uint64_t serial,Power requested)noexcept {
        auto old=word.load(std::memory_order_acquire);
        for(unsigned attempt=0;attempt<16;++attempt){
            const auto count=(old&countMask)>>countShift;
            if(!count){closePermanently();return;}
            auto next=old-countOne;
            if(count==1){
                const bool uncontended=!(old&(closedBit|overlapBit))&&(old&serialMask)==serial;
                next&=~(powerMask|overlapBit);
                if(uncontended)next|=static_cast<std::uint64_t>(normalized(requested));
            }else next=(next&~powerMask)|overlapBit;
            if(word.compare_exchange_strong(old,next,std::memory_order_acq_rel,std::memory_order_acquire))return;
        }
        // Bounded RT work. Exceptional contention permanently revokes this
        // generation; it never spins or permits an old successful publication.
        closePermanently();
    }
public:
    const std::uintptr_t wrapper,actualComponent;
    const std::uint64_t generation;
    // Always starts Unknown. Numeric identities do not prove native ownership.
    Binding(std::uintptr_t exactWrapper,std::uintptr_t exactComponent,std::uint64_t exactGeneration)noexcept:
        wrapper(exactWrapper),actualComponent(exactComponent),generation(exactGeneration){
        word.store(wrapper&&actualComponent&&generation?0:closedBit,std::memory_order_relaxed);
    }
    Binding(const Binding&)=delete;Binding& operator=(const Binding&)=delete;
    class InitialSeedTicket {
        const Binding* owner=nullptr;std::uint64_t observed=0;
        friend class Binding;
        InitialSeedTicket(const Binding* binding,std::uint64_t value)noexcept:owner(binding),observed(value){}
    public:InitialSeedTicket()noexcept=default;
    };
    // Capture BEFORE Root's repeated native byte/backlink reads while holding
    // the verified stopped/drained HostLockMix boundary. Finish BEFORE source
    // publication/unlock. Only a fresh never-notified generation may seed once.
    InitialSeedTicket captureInitialSeedAtStoppedBoundary()const noexcept {
        return InitialSeedTicket(this,word.load(std::memory_order_acquire));
    }
    bool trySeedAtStoppedBoundary(const InitialSeedTicket& ticket,std::uint8_t repeatedProvenByte)noexcept {
        std::uint64_t expected=0;
        if(ticket.owner==this&&ticket.observed==0&&repeatedProvenByte<=1&&
            word.compare_exchange_strong(expected,seedUsedBit|static_cast<std::uint64_t>(repeatedProvenByte?Power::Enabled:Power::Disabled),
                std::memory_order_acq_rel,std::memory_order_acquire))return true;
        revokeToUnknown();return false;
    }
    // Stale/invalid native seed cannot overwrite a newer notification or revive
    // a closed generation. It revokes the observation instead; a subsequent
    // uncontended actual ID11 may recover a still-live generation.
    void revokeToUnknown()noexcept {
        auto old=word.load(std::memory_order_acquire);
        for(unsigned attempt=0;attempt<16;++attempt){
            if(old&closedBit)return;
            if((old&serialMask)==serialMask){closePermanently();return;}
            auto next=((old+serialOne)&~powerMask)|seedUsedBit;
            if(old&countMask)next|=overlapBit;
            if(word.compare_exchange_strong(old,next,std::memory_order_acq_rel,std::memory_order_acquire))return;
        }
        closePermanently();
    }
    class Pending {
        Binding* owner=nullptr;std::uint64_t serial=0;Power requested=Power::Unknown;
        friend class Binding;
        Pending(Binding* value,std::uint64_t sequence,Power power)noexcept:owner(value),serial(sequence),requested(power){}
    public:
        Pending()noexcept=default;
        Pending(const Pending&)=delete;Pending& operator=(const Pending&)=delete;
        Pending(Pending&& other)noexcept:owner(other.owner),serial(other.serial),requested(other.requested){other.owner=nullptr;}
        Pending& operator=(Pending&&)=delete;
        ~Pending(){if(owner)owner->complete(serial,Power::Unknown);}
        // Call ONLY after the exact original Dispatcher returns normally.
        // The return value is not a documented ID11 acceptance signal.
        void originalReturned()noexcept{if(auto* value=owner){owner=nullptr;value->complete(serial,requested);}}
        explicit operator bool()const noexcept{return owner!=nullptr;}
    };
    Pending beforeOriginal(std::intptr_t value)noexcept {
        const Power requested=value==0?Power::Disabled:value==1?Power::Enabled:Power::Unknown;
        auto old=word.load(std::memory_order_acquire);
        for(unsigned attempt=0;attempt<16;++attempt){
            if(old&closedBit)return {};
            if((old&countMask)==countMask||(old&serialMask)==serialMask){closePermanently();return {};}
            auto next=old+serialOne+countOne;
            if(old&countMask)next=(next&~powerMask)|overlapBit;
            if(word.compare_exchange_strong(old,next,std::memory_order_acq_rel,std::memory_order_acquire))
                return Pending(this,next&serialMask,requested);
        }
        closePermanently();return {};
    }
    Power snapshotForBlock()const noexcept {
        const auto value=word.load(std::memory_order_acquire);
        return value&(closedBit|overlapBit|countMask)?Power::Unknown:normalized(static_cast<Power>(value&powerMask));
    }
    // Permanently closes ONLY this generation, including already-issued Pending
    // observations. Its storage must still outlive all callbacks/source tables.
    void closePermanently()noexcept{word.fetch_or(closedBit,std::memory_order_acq_rel);}
    bool closed()const noexcept{return word.load(std::memory_order_acquire)&closedBit;}
    std::uint32_t callsInFlight()const noexcept{return static_cast<std::uint32_t>((word.load(std::memory_order_acquire)&countMask)>>countShift);}
};

template<std::size_t Capacity=32>class Registry {
    static_assert(Capacity>0&&Capacity<=32,"bounded native insert registry");
    std::array<std::atomic<Binding*>,Capacity> slots{};
public:
    Registry()noexcept{for(auto& slot:slots)slot.store(nullptr,std::memory_order_relaxed);}
    Registry(const Registry&)=delete;Registry& operator=(const Registry&)=delete;
    // MODEL THREAD ONLY, with Root's actual stopped/drained admission proof.
    // Binding storage is process-lifetime stable. Never free/reset/reuse it.
    bool publishAtModelBoundary(std::size_t index,Binding& binding)noexcept {
        if(index>=Capacity||binding.closed()||binding.callsInFlight())return false;
        for(std::size_t i=0;i<Capacity;++i){auto* other=slots[i].load(std::memory_order_acquire);
            if(other&&(other==&binding||(!other->closed()&&other->wrapper==binding.wrapper)))return false;}
        auto* previous=slots[index].load(std::memory_order_acquire);
        if(previous&&!previous->closed())return false;
        if(previous&&binding.generation<=previous->generation)return false;
        slots[index].store(&binding,std::memory_order_release);return true;
    }
    void retireAtModelBoundary(std::size_t index)noexcept {
        if(index>=Capacity)return;
        if(auto* previous=slots[index].load(std::memory_order_acquire))previous->closePermanently();
        // Keep the retired pointer until replacement. A callback already holding
        // it is safe and cannot publish power into a replacement generation.
    }
    Binding::Pending beforeOriginal(void* self,std::intptr_t id,std::intptr_t value)const noexcept {
        if(id!=setEnabledID||!self)return {};
        const auto exact=reinterpret_cast<std::uintptr_t>(self);Binding* found=nullptr;
        for(const auto& slot:slots){auto* candidate=slot.load(std::memory_order_acquire);
            if(candidate&&candidate->wrapper==exact&&!candidate->closed()){
                if(found){found->closePermanently();candidate->closePermanently();return {};}
                found=candidate;}}
        return found?found->beforeOriginal(value):Binding::Pending{};
    }
};

// No ID/owner/filter can skip, duplicate or modify the original invocation or
// scalar result. Pending's destructor revokes on a C++ exception; it does not
// catch/translate the exception. Root must verify/pin a nonnull original first.
template<std::size_t Capacity,class Original>
std::intptr_t forwardOriginalExactlyOnce(const Registry<Capacity>& registry,Original&& original,
        void* self,std::intptr_t id,std::intptr_t index,std::intptr_t value)
        noexcept(noexcept(std::forward<Original>(original)(self,id,index,value))){
    auto pending=registry.beforeOriginal(self,id,value);
    const auto result=std::forward<Original>(original)(self,id,index,value);
    pending.originalReturned();return result;
}
} // namespace rysup::fl_ara::slot_power
