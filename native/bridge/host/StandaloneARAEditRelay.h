#pragma once
// SDK-free actual ARA dirty-revision relay. Zero means manual invalidation,
// never an actual plug-in edit. It is safe to use from notification threads.
#include <atomic>
#include <cstdint>
namespace rysup::ara_lab {
class StandaloneARAEditRelay {
public:
    template<class QueueOnlyCallback>
    bool forwardActualRevision(std::uint64_t actualRevision,QueueOnlyCallback&& queue)noexcept{
        if(!actualRevision)return false;
        auto known=lastQueued.load(std::memory_order_acquire);
        while(actualRevision>known){
            if(lastQueued.compare_exchange_weak(known,actualRevision,std::memory_order_acq_rel)){
                queue(actualRevision);return true;
            }
        }
        return false;
    }
    std::uint64_t lastForwardedActualRevision()const noexcept{return lastQueued.load(std::memory_order_acquire);}
private:
    std::atomic<std::uint64_t> lastQueued{0};
};
} // namespace rysup::ara_lab
