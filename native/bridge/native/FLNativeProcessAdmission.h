#pragma once
#include <atomic>
#include <cstdint>
#include <utility>

namespace rysup::fl_ara::native_hooks {
// SDK-free policy. One word is the sole authority for permission, generation,
// and admitted calls. An entrant rejected by a closed gate never changes count.
// The template permits exhaustive small-limit offline overflow controls.
template<unsigned CountBits=20,unsigned GenerationBits=42>
class BasicNativeProcessAdmissionGate {
    static_assert(CountBits>0&&CountBits<=32&&GenerationBits>0&&CountBits+GenerationBits<=62);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,"RT admission must be lock-free");
    static constexpr std::uint64_t countMask=(std::uint64_t{1}<<CountBits)-1;
    static constexpr std::uint64_t generationMask=((std::uint64_t{1}<<GenerationBits)-1)<<CountBits;
    static constexpr std::uint64_t generationStep=std::uint64_t{1}<<CountBits;
    static constexpr std::uint64_t permanentBit=std::uint64_t{1}<<62;
    static constexpr std::uint64_t closedBit=std::uint64_t{1}<<63;
    std::atomic<std::uint64_t> word_{0};
    template<unsigned,unsigned> friend struct NativeProcessAdmissionTestAccess;
public:
    struct Snapshot {
        bool allowed;
        std::uint32_t activeCount;
        std::uint64_t generation;
        bool permanentlyClosed;
    };
    class Lease {
        BasicNativeProcessAdmissionGate* gate_=nullptr;
        explicit Lease(BasicNativeProcessAdmissionGate* gate)noexcept:gate_(gate){}
        friend class BasicNativeProcessAdmissionGate;
    public:
        Lease()noexcept=default;
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
        Lease(Lease&& other)noexcept:gate_(std::exchange(other.gate_,nullptr)){}
        Lease& operator=(Lease&& other)noexcept {
            if(this!=&other){release();gate_=std::exchange(other.gate_,nullptr);}return *this;
        }
        ~Lease(){release();}
        explicit operator bool()const noexcept{return gate_!=nullptr;}
        void release()noexcept {
            if(auto* gate=std::exchange(gate_,nullptr))gate->word_.fetch_sub(1,std::memory_order_seq_cst);
        }
    };
    BasicNativeProcessAdmissionGate()noexcept=default;
    BasicNativeProcessAdmissionGate(const BasicNativeProcessAdmissionGate&)=delete;
    BasicNativeProcessAdmissionGate& operator=(const BasicNativeProcessAdmissionGate&)=delete;
    Snapshot snapshot()const noexcept {
        const auto word=word_.load(std::memory_order_seq_cst);
        return {!(word&closedBit),static_cast<std::uint32_t>(word&countMask),
            (word&generationMask)>>CountBits,static_cast<bool>(word&permanentBit)};
    }
    bool isAllowed()const noexcept{return snapshot().allowed;}
    std::uint32_t activeCount()const noexcept{return snapshot().activeCount;}
    // NON-RT owner transition. The returned previous permission supports an
    // owner's conservative timeout rollback; active leases are never discarded.
    bool exchangeAllowed(bool allowed)noexcept {
        auto word=word_.load(std::memory_order_seq_cst);
        for(;;){
            const bool previous=!(word&closedBit);
            if(previous==allowed||(word&permanentBit))return previous;
            const bool exhausted=(word&generationMask)==generationMask;
            const auto next=exhausted?(word|closedBit|permanentBit):
                (((word+generationStep)&~closedBit)|(allowed?0:closedBit));
            if(word_.compare_exchange_weak(word,next,std::memory_order_seq_cst))return previous;
        }
    }
    void setAllowed(bool allowed)noexcept{(void)exchangeAllowed(allowed);}
    // Irreversible for this process-lifetime record. Existing calls still drain.
    void shutdown()noexcept{word_.fetch_or(closedBit|permanentBit,std::memory_order_seq_cst);}
    Lease tryEnter()noexcept{return tryEnterObserved(word_.load(std::memory_order_seq_cst));}
private:
    Lease tryEnterObserved(std::uint64_t word)noexcept {
        const auto observedGeneration=word&generationMask;
        // Fixed attempts, no spinning or waiting on the audio thread. Contention
        // may conservatively refuse; permission changes never get retried into
        // a new generation (including a close/drain/reopen ABA).
        for(unsigned attempt=0;attempt<4;++attempt){
            if((word&closedBit)||(word&generationMask)!=observedGeneration)return {};
            if((word&countMask)==countMask){shutdown();return {};}
            if(word_.compare_exchange_strong(word,word+1,std::memory_order_seq_cst))return Lease(this);
        }
        return {};
    }
};
using NativeProcessAdmissionGate=BasicNativeProcessAdmissionGate<>;
} // namespace rysup::fl_ara::native_hooks
