// Offline policy controls only. No SDK, DAW, device, plug-in or audio IO.
#include "FLNativeProcessAdmission.h"
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
namespace rysup::fl_ara::native_hooks {
template<unsigned C,unsigned G> struct NativeProcessAdmissionTestAccess {
    using Gate=BasicNativeProcessAdmissionGate<C,G>;
    static auto observe(const Gate& gate){return gate.word_.load();}
    static auto enterObserved(Gate& gate,std::uint64_t observed){return gate.tryEnterObserved(observed);}
};
}
using namespace rysup::fl_ara::native_hooks;
static unsigned checks=0;
#define CHECK(...) do{++checks;assert((__VA_ARGS__));}while(false)
struct Latch {
    std::mutex mutex;std::condition_variable condition;bool ready=false;
    void signal(){std::lock_guard<std::mutex> lock(mutex);ready=true;condition.notify_all();}
    void wait(){std::unique_lock<std::mutex> lock(mutex);condition.wait(lock,[&]{return ready;});}
};
int main(){
    using Gate=NativeProcessAdmissionGate;
    Gate gate;
    CHECK(gate.snapshot().allowed&&gate.activeCount()==0);
    {
        auto lease=gate.tryEnter();CHECK(lease&&gate.activeCount()==1);
        auto moved=std::move(lease);CHECK(!lease&&moved&&gate.activeCount()==1);
        CHECK(gate.exchangeAllowed(false));CHECK(!gate.isAllowed()&&gate.activeCount()==1);
        CHECK(!gate.tryEnter()&&gate.activeCount()==1);
        gate.setAllowed(true);CHECK(gate.isAllowed()&&gate.activeCount()==1);
        moved.release();moved.release();CHECK(gate.activeCount()==0);
    }
    // A refused callback can remain indefinitely in host-output silencing;
    // closing/draining is still stable because it never acquired an admission.
    gate.setAllowed(false);Latch rejected,finishRejected;
    std::atomic<unsigned> originals{0},owners{0};
    std::thread lateBlocked([&]{auto lease=gate.tryEnter();assert(!lease);rejected.signal();
        finishRejected.wait();if(lease){++originals;++owners;}});
    rejected.wait();
    for(unsigned i=0;i<10000;++i){const auto state=gate.snapshot();assert(!state.allowed&&state.activeCount==0);}
    CHECK(originals==0&&owners==0&&gate.activeCount()==0);
    finishRejected.signal();lateBlocked.join();
    // Genuine admission before close remains counted across BEFORE, native,
    // and AFTER callbacks. Owner cannot declare drained during any phase.
    gate.setAllowed(true);Latch before,original,after,continueBefore,continueOriginal,continueAfter;
    std::thread admitted([&]{auto lease=gate.tryEnter();assert(lease);++owners;before.signal();continueBefore.wait();
        ++originals;original.signal();continueOriginal.wait();++owners;after.signal();continueAfter.wait();});
    before.wait();CHECK(gate.exchangeAllowed(false)&&gate.activeCount()==1);
    continueBefore.signal();original.wait();CHECK(gate.activeCount()==1&&!gate.tryEnter());
    continueOriginal.signal();after.wait();CHECK(gate.activeCount()==1&&owners==2&&originals==1);
    continueAfter.signal();admitted.join();CHECK(gate.activeCount()==0&&!gate.isAllowed());
    // Reproduce the formerly vulnerable pre-read -> close/drain/reopen -> CAS
    // schedule deterministically. A stale generation cannot enter after reopen.
    gate.setAllowed(true);
    using Access=NativeProcessAdmissionTestAccess<20,42>;
    Latch observed,attempt;std::atomic<bool> staleAdmitted{false};
    std::thread stale([&]{const auto value=Access::observe(gate);observed.signal();attempt.wait();
        auto lease=Access::enterObserved(gate,value);staleAdmitted=static_cast<bool>(lease);});
    observed.wait();const auto generation=gate.snapshot().generation;
    gate.setAllowed(false);CHECK(gate.activeCount()==0);gate.setAllowed(true);
    CHECK(gate.snapshot().generation==generation+2);
    attempt.signal();stale.join();CHECK(!staleAdmitted&&gate.activeCount()==0);
    {auto fresh=gate.tryEnter();CHECK(fresh&&gate.activeCount()==1);}
    // Existing leases may cross an owner's conservative reopen after timeout;
    // their release must preserve the new generation and all other admissions.
    auto oldLease=gate.tryEnter();gate.setAllowed(false);gate.setAllowed(true);
    auto freshLease=gate.tryEnter();CHECK(oldLease&&freshLease&&gate.activeCount()==2);
    oldLease.release();CHECK(gate.activeCount()==1&&gate.isAllowed());freshLease.release();
    // Small limits exercise the actual production algorithm's saturation paths.
    BasicNativeProcessAdmissionGate<2,3> countLimit;
    std::array<decltype(countLimit)::Lease,3> leases;
    for(auto& lease:leases)lease=countLimit.tryEnter();
    CHECK(countLimit.activeCount()==3);
    CHECK(!countLimit.tryEnter()&&countLimit.snapshot().permanentlyClosed);
    CHECK(!countLimit.isAllowed()&&countLimit.activeCount()==3);
    countLimit.setAllowed(true);CHECK(!countLimit.isAllowed());
    for(auto& lease:leases)lease.release();CHECK(countLimit.activeCount()==0);
    BasicNativeProcessAdmissionGate<3,2> epochLimit;
    epochLimit.setAllowed(false);epochLimit.setAllowed(true);epochLimit.setAllowed(false);
    CHECK(epochLimit.snapshot().generation==3&&!epochLimit.snapshot().permanentlyClosed);
    epochLimit.setAllowed(true);CHECK(!epochLimit.isAllowed()&&epochLimit.snapshot().permanentlyClosed);
    CHECK(!epochLimit.tryEnter());
    auto outstanding=gate.tryEnter();CHECK(outstanding);gate.shutdown();
    CHECK(!gate.isAllowed()&&gate.snapshot().permanentlyClosed&&gate.activeCount()==1);
    gate.setAllowed(true);CHECK(!gate.tryEnter()&&gate.activeCount()==1);
    outstanding.release();CHECK(gate.activeCount()==0);
    // Threaded close/drain/reopen with actual admitted bodies and blocked spam.
    Gate stress;std::atomic<bool> stop{false};std::atomic<unsigned> bodies{0};
    std::atomic<std::uint64_t> completed{0};std::vector<std::thread> workers;
    for(unsigned w=0;w<6;++w)workers.emplace_back([&]{while(!stop.load()){
        auto lease=stress.tryEnter();if(!lease)continue;
        bodies.fetch_add(1);std::this_thread::yield();bodies.fetch_sub(1);++completed;
    }});
    for(unsigned round=0;round<3000;++round){
        stress.setAllowed(false);while(stress.activeCount())std::this_thread::yield();
        assert(bodies==0);
        for(unsigned repeat=0;repeat<8;++repeat){const auto state=stress.snapshot();assert(!state.allowed&&state.activeCount==0&&bodies==0);}
        stress.setAllowed(true);std::this_thread::yield();
    }
    stress.shutdown();while(stress.activeCount())std::this_thread::yield();stop=true;
    for(auto& worker:workers)worker.join();
    CHECK(bodies==0&&completed>0&&stress.activeCount()==0&&stress.snapshot().permanentlyClosed);
    std::printf("{\"pass\":true,\"checks\":%u,\"concurrentCloseDrainRounds\":3000,\"closedStableSnapshots\":24000,\"workers\":6,\"admittedBodies\":%llu}\n",checks,static_cast<unsigned long long>(completed.load()));
}
