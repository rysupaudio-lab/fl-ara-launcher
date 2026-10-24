#pragma once
// SDK-free real-time cache authorization shared by the coordinator and native
// source overlay. Publishing a batch never grants authority to an old ticket.
#include <atomic>
#include <cstdint>
#include <limits>
namespace rysup::ara_lab {
struct StandaloneCacheTicket {std::uint64_t epoch=0;};
class StandaloneCacheGate {
public:
    StandaloneCacheTicket invalidate()noexcept {
        auto old=state.load(std::memory_order_acquire);
        for(;;){
            if((old>>1)==(std::numeric_limits<std::uint64_t>::max()>>1)){alive=false;return {old>>1};}
            const auto next=((old>>1)+1)<<1;
            if(state.compare_exchange_weak(old,next,std::memory_order_acq_rel))return {next>>1};
        }
    }
    bool authorizes(StandaloneCacheTicket ticket)const noexcept {
        return ticket.epoch<=(std::numeric_limits<std::uint64_t>::max()>>1)
            && alive.load(std::memory_order_acquire) && state.load(std::memory_order_acquire)==((ticket.epoch<<1)|1);
    }
    bool publishIfUnchanged(StandaloneCacheTicket ticket)noexcept {
        if(ticket.epoch>(std::numeric_limits<std::uint64_t>::max()>>1) || !alive.load(std::memory_order_acquire))return false;
        auto expected=ticket.epoch<<1;
        return state.compare_exchange_strong(expected,expected|1,std::memory_order_acq_rel)
            && alive.load(std::memory_order_acquire);
    }
    StandaloneCacheTicket currentTicket()const noexcept{return {state.load(std::memory_order_acquire)>>1};}
    void closePermanently()noexcept{alive.store(false,std::memory_order_release);invalidate();}
private:
    std::atomic<std::uint64_t> state{0};
    std::atomic<bool> alive{true};
};
} // namespace rysup::ara_lab
